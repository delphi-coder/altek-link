#include "link.h"
#include <mmsystem.h>
int media_packet(struct call *c, const void *data, int len)
{
    int ok;
    if (c->stop || !c->tls)
    {
        return 0;
    }
    ok = wire_send(c->tls, data, len);
    if (!ok)
    {
        InterlockedExchange(&c->stop, 1);
        shutdown(c->tls->socket, SD_BOTH);
    }
    return ok;
}
static void audio_format(WAVEFORMATEX *f)
{
    memset(f, 0, sizeof(*f));
    f->wFormatTag = WAVE_FORMAT_PCM;
    f->nChannels = 1;
    f->nSamplesPerSec = 16000;
    f->wBitsPerSample = 16;
    f->nBlockAlign = 2;
    f->nAvgBytesPerSec = 32000;
}
static unsigned __stdcall audio_capture(void *arg)
{
    struct call *c = (struct call *)arg;
    HWAVEIN in = NULL;
    WAVEFORMATEX format;
    WAVEHDR h[6];
    unsigned char data[6][640], packet[641];
    int i, prepared = 0;
    MMRESULT result;
    audio_format(&format);
    memset(h, 0, sizeof(h));
    result = waveInOpen(&in, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR)
    {
        log_event("MIC_ERROR", "waveInOpen=%u", result);
        notice("Microphone unavailable; check Windows sound settings");
        return 0;
    }
    for (i = 0; i < 6; i++)
    {
        h[i].lpData = (LPSTR)data[i];
        h[i].dwBufferLength = 640;
        if (waveInPrepareHeader(in, &h[i], sizeof(WAVEHDR)))
        {
            goto done;
        }
        prepared++;
        if (waveInAddBuffer(in, &h[i], sizeof(WAVEHDR)))
        {
            goto done;
        }
    }
    if (waveInStart(in))
    {
        goto done;
    }
    while (!c->stop && app.running)
    {
        for (i = 0; i < 6; i++)
        {
            if (h[i].dwFlags & WHDR_DONE)
            {
                DWORD n = h[i].dwBytesRecorded;
                if (n > 640)
                {
                    n = 640;
                }
                n &= ~1U;
                packet[0] = 3;
                if (c->mute)
                {
                    memset(packet + 1, 0, n);
                }
                else
                {
                    memcpy(packet + 1, data[i], n);
                }
                if (n && !media_packet(c, packet, n + 1))
                {
                    break;
                }
                h[i].dwBytesRecorded = 0;
                if (waveInAddBuffer(in, &h[i], sizeof(WAVEHDR)))
                {
                    goto done;
                }
            }
        }
        Sleep(4);
    }
done:
    waveInStop(in);
    waveInReset(in);
    for (i = 0; i < prepared; i++)
    {
        waveInUnprepareHeader(in, &h[i], sizeof(WAVEHDR));
    }
    waveInClose(in);
    return 0;
}
static unsigned __stdcall keepalive(void *arg)
{
    struct call *c = (struct call *)arg;
    unsigned char zero = 0;
    int i;
    while (!c->stop && app.running)
    {
        if (!media_packet(c, &zero, 1))
        {
            break;
        }
        for (i = 0; i < 20 && !c->stop && app.running; i++)
        {
            Sleep(100);
        }
    }
    return 0;
}
void media_run(struct call *c)
{
    HANDLE mic = NULL, camera = NULL, ping = NULL;
    struct codec *decoder = NULL;
    HWAVEOUT out = NULL;
    WAVEFORMATEX format;
    WAVEHDR headers[12];
    unsigned char audio[12][4096];
    int i, slot = 0, prepared = 0, len, w, h;
    unsigned char *b, *rgb;
    int audio_on = strcmp(c->kind, "view") != 0;
    DWORD timeout = 15000;
    ULONGLONG last = GetTickCount64();
    int frames = 0;
    memset(headers, 0, sizeof(headers));
    if (!c->tls)
    {
        return;
    }
    if (c->stop)
    {
        EnterCriticalSection(&app.lock);
        tls_close(c->tls);
        c->tls = NULL;
        LeaveCriticalSection(&app.lock);
        return;
    }
    setsockopt(c->tls->socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    timeout = 4000;
    setsockopt(c->tls->socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    if (strcmp(c->kind, "audio"))
    {
        decoder = codec_open(0, 0, 0, 0, 0, NULL);
    }
    if (audio_on)
    {
        audio_format(&format);
        if (waveOutOpen(&out, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR)
        {
            for (i = 0; i < 12; i++)
            {
                headers[i].lpData = (LPSTR)audio[i];
                headers[i].dwBufferLength = 4096;
                if (waveOutPrepareHeader(out, &headers[i], sizeof(WAVEHDR)))
                {
                    break;
                }
                prepared++;
            }
        }
        else
        {
            out = NULL;
            notice("Audio playback device unavailable");
        }
        mic = (HANDLE)_beginthreadex(NULL, 0, audio_capture, c, 0, NULL);
    }
    if (!strcmp(c->kind, "video"))
    {
        camera = (HANDLE)_beginthreadex(NULL, 0, camera_thread, c, 0, NULL);
    }
    ping = (HANDLE)_beginthreadex(NULL, 0, keepalive, c, 0, NULL);
    changed();
    log_event("MEDIA_START", "kind=%s", c->kind);
    while (!c->stop && app.running)
    {
        b = wire_read(c->tls, &len, MAX_MEDIA);
        if (!b)
        {
            break;
        }
        if (b[0] == 1 && len > 9 && decoder)
        {
            w = be32(b + 1);
            h = be32(b + 5);
            if (w < 1 || h < 1 || w > 1920 || h > 1920)
            {
                free(b);
                break;
            }
            codec_decode(decoder, b + 9, len - 9, &rgb, &w, &h);
        }
        else if (b[0] == 2 && len > 13 && decoder)
        {
            if (codec_decode(decoder, b + 13, len - 13, &rgb, &w, &h))
            {
                video_present(rgb, w, h, 0);
                frames++;
            }
        }
        else if (b[0] == 3 && audio_on && out)
        {
            if (len > 4097 || ((len - 1) & 1))
            {
                free(b);
                break;
            }
            for (i = 0; i < prepared; i++)
            {
                int n = (slot + i) % prepared;
                if (!(headers[n].dwFlags & WHDR_INQUEUE))
                {
                    headers[n].dwBufferLength = len - 1;
                    if (c->speaker_mute)
                    {
                        memset(audio[n], 0, len - 1);
                    }
                    else
                    {
                        memcpy(audio[n], b + 1, len - 1);
                    }
                    if (len > 1)
                    {
                        waveOutWrite(out, &headers[n], sizeof(WAVEHDR));
                    }
                    slot = (n + 1) % prepared;
                    break;
                }
            }
        }
        else if (b[0] > 3)
        {
            free(b);
            break;
        }
        free(b);
        if (GetTickCount64() - last >= 5000)
        {
            log_event("VIDEO_RX", "frames5s=%d", frames);
            frames = 0;
            last = GetTickCount64();
        }
    }
    InterlockedExchange(&c->stop, 1);
    shutdown(c->tls->socket, SD_BOTH);
    camera_stop(c);
    if (camera)
    {
        WaitForSingleObject(camera, INFINITE);
        CloseHandle(camera);
    }
    if (mic)
    {
        WaitForSingleObject(mic, INFINITE);
        CloseHandle(mic);
    }
    if (ping)
    {
        WaitForSingleObject(ping, INFINITE);
        CloseHandle(ping);
    }
    if (out)
    {
        waveOutReset(out);
        for (i = 0; i < prepared; i++)
        {
            waveOutUnprepareHeader(out, &headers[i], sizeof(WAVEHDR));
        }
        waveOutClose(out);
    }
    codec_close(decoder);
    EnterCriticalSection(&app.lock);
    tls_close(c->tls);
    c->tls = NULL;
    LeaveCriticalSection(&app.lock);
    log_event("MEDIA_STOP", "closed");
}
