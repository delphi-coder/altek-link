#include "link.h"
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
static HRESULT devices(IMFActivate ***items, UINT32 *count)
{
    IMFAttributes *a = NULL;
    HRESULT hr;
    hr = MFCreateAttributes(&a, 1);
    if (SUCCEEDED(hr))
    {
        hr = IMFAttributes_SetGUID(a, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                   &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    }
    if (SUCCEEDED(hr))
    {
        hr = MFEnumDeviceSources(a, items, count);
    }
    if (a)
    {
        IMFAttributes_Release(a);
    }
    return hr;
}
void camera_list(HWND combo)
{
    IMFActivate **items = NULL;
    UINT32 n = 0, i;
    WCHAR *name;
    UINT32 len;
    HRESULT hr;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr))
    {
        return;
    }
    if (SUCCEEDED(devices(&items, &n)))
    {
        for (i = 0; i < n; i++)
        {
            name = NULL;
            len = 0;
            if (SUCCEEDED(IMFActivate_GetAllocatedString(
                    items[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len)))
            {
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)name);
                CoTaskMemFree(name);
            }
            else
            {
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Camera");
            }
            IMFActivate_Release(items[i]);
        }
        CoTaskMemFree(items);
    }
    MFShutdown();
    SendMessageW(combo, CB_SETCURSEL,
                 app.camera_index >= 0 && (UINT32)app.camera_index < n ? app.camera_index : 0, 0);
}
static HRESULT open_source(int index, IMFMediaSource **source)
{
    IMFActivate **items = NULL;
    UINT32 n = 0, i;
    HRESULT hr = devices(&items, &n);
    if (FAILED(hr))
    {
        return hr;
    }
    if (index < 0 || (UINT32)index >= n)
    {
        hr = E_INVALIDARG;
    }
    else
    {
        hr = IMFActivate_ActivateObject(items[index], &IID_IMFMediaSource, (void **)source);
    }
    for (i = 0; i < n; i++)
    {
        IMFActivate_Release(items[i]);
    }
    CoTaskMemFree(items);
    return hr;
}
static HRESULT reader_create(IMFMediaSource *source, IMFSourceReader **reader, HMODULE *dll)
{
    WCHAR system[MAX_PATH], path[MAX_PATH];
    IMFAttributes *attr = NULL;
    HRESULT hr;
    HRESULT(WINAPI * create)(IMFMediaSource *, IMFAttributes *, IMFSourceReader **);
    GetSystemDirectoryW(system, MAX_PATH);
    path_join(path, system, L"mfreadwrite.dll");
    *dll = LoadLibraryW(path);
    if (!*dll)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    create =
        (HRESULT(WINAPI *)(IMFMediaSource *, IMFAttributes *, IMFSourceReader **))GetProcAddress(
            *dll, "MFCreateSourceReaderFromMediaSource");
    if (!create)
    {
        return E_NOINTERFACE;
    }
    hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr))
    {
        hr = IMFAttributes_SetUINT32(attr, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    if (SUCCEEDED(hr))
    {
        hr = create(source, attr, reader);
    }
    if (attr)
    {
        IMFAttributes_Release(attr);
    }
    return hr;
}
static int dimensions(IMFMediaType *type, int *w, int *h)
{
    UINT64 value = 0;
    if (FAILED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &value)))
    {
        return 0;
    }
    *w = (int)(value >> 32);
    *h = (int)value;
    return *w > 0 && *h > 0 && *w <= 4096 && *h <= 4096;
}
void camera_modes(HWND combo, int index)
{
    IMFMediaSource *source = NULL;
    IMFSourceReader *r = NULL;
    IMFMediaType *t = NULL;
    HMODULE dll = NULL;
    DWORD i;
    int w, h;
    WCHAR label[96];
    HRESULT hr;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr))
    {
        return;
    }
    hr = open_source(index, &source);
    if (SUCCEEDED(hr))
    {
        hr = reader_create(source, &r, &dll);
    }
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < 256; i++)
        {
            if (FAILED(IMFSourceReader_GetNativeMediaType(r, MF_SOURCE_READER_FIRST_VIDEO_STREAM, i,
                                                          &t)))
            {
                break;
            }
            if (dimensions(t, &w, &h) && w <= 1920 && h <= 1080 && !(w % 2) && !(h % 2))
            {
                _snwprintf(label, 96, L"%d x %d", w, h);
                if (SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)label) == CB_ERR)
                {
                    LRESULT n = SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)label);
                    SendMessageW(combo, CB_SETITEMDATA, n, MAKELONG(w, h));
                }
            }
            IMFMediaType_Release(t);
            t = NULL;
        }
    }
    if (!SendMessageW(combo, CB_GETCOUNT, 0, 0))
    {
        LRESULT n = SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"320 x 240");
        SendMessageW(combo, CB_SETITEMDATA, n, MAKELONG(320, 240));
    }
    _snwprintf(label, 96, L"%d x %d", app.width, app.height);
    i = (DWORD)SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)label);
    SendMessageW(combo, CB_SETCURSEL, i == (DWORD)CB_ERR ? 0 : i, 0);
    if (r)
    {
        IMFSourceReader_Release(r);
    }
    if (source)
    {
        IMFMediaSource_Shutdown(source);
        IMFMediaSource_Release(source);
    }
    if (dll)
    {
        FreeLibrary(dll);
    }
    MFShutdown();
}
static unsigned char clamp(int n)
{
    return (unsigned char)(n < 0 ? 0 : n > 255 ? 255 : n);
}
static void rgb_i420(const unsigned char *rgb, int stride, int w, int h, unsigned char *out)
{
    int x, y, r, g, b;
    unsigned char *u = out + w * h, *v = u + w * h / 4;
    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            const unsigned char *p = rgb + y * stride + x * 4;
            b = p[0];
            g = p[1];
            r = p[2];
            out[y * w + x] = clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            if (!(y & 1) && !(x & 1))
            {
                u[(y / 2) * (w / 2) + x / 2] =
                    clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                v[(y / 2) * (w / 2) + x / 2] =
                    clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
    }
}
unsigned __stdcall camera_thread(void *arg)
{
    struct call *c = (struct call *)arg;
    IMFMediaSource *source = NULL;
    IMFSourceReader *r = NULL;
    IMFMediaType *type = NULL, *native = NULL;
    IMFSample *sample = NULL;
    IMFMediaBuffer *buffer = NULL;
    HMODULE dll = NULL;
    struct codec *enc = NULL;
    HRESULT hr;
    DWORD i, flags, stream, n = 0;
    LONGLONG ts;
    int w = 0, h = 0, tw, th, best = -1, score = 0x7fffffff, s, ow, oh, fps, bitrate, index, len,
        key;
    UINT32 rawstride = 0;
    LONG stride;
    unsigned char *i420 = NULL, *bytes = NULL, *packet = NULL, *raw = NULL, *rgb = NULL;
    ULONGLONG next = 0;
    int started = 0;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr))
    {
        goto done;
    }
    started = 1;
    EnterCriticalSection(&app.lock);
    index = app.camera_index;
    ow = app.width;
    oh = app.height;
    fps = app.fps;
    bitrate = app.bitrate;
    LeaveCriticalSection(&app.lock);
    hr = open_source(index, &source);
    if (FAILED(hr))
    {
        goto done;
    }
    EnterCriticalSection(&app.lock);
    c->camera_source = source;
    LeaveCriticalSection(&app.lock);
    hr = reader_create(source, &r, &dll);
    if (FAILED(hr))
    {
        goto done;
    }
    for (i = 0; i < 256; i++)
    {
        if (FAILED(IMFSourceReader_GetNativeMediaType(r, MF_SOURCE_READER_FIRST_VIDEO_STREAM, i,
                                                      &type)))
        {
            break;
        }
        if (dimensions(type, &tw, &th) && tw <= 1920 && th <= 1080 && !(tw % 2) && !(th % 2))
        {
            s = abs(tw - ow) + abs(th - oh);
            if (s < score)
            {
                score = s;
                best = (int)i;
            }
        }
        IMFMediaType_Release(type);
        type = NULL;
    }
    if (best < 0)
    {
        hr = MF_E_INVALIDMEDIATYPE;
        goto done;
    }
    hr = IMFSourceReader_GetNativeMediaType(r, MF_SOURCE_READER_FIRST_VIDEO_STREAM, best, &native);
    if (FAILED(hr))
    {
        goto done;
    }
    hr = IMFSourceReader_SetCurrentMediaType(r, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, native);
    if (FAILED(hr))
    {
        goto done;
    }
    hr = MFCreateMediaType(&type);
    if (FAILED(hr))
    {
        goto done;
    }
    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    hr = IMFSourceReader_SetCurrentMediaType(r, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, type);
    IMFMediaType_Release(type);
    type = NULL;
    if (FAILED(hr))
    {
        goto done;
    }
    hr = IMFSourceReader_GetCurrentMediaType(r, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type);
    if (FAILED(hr) || !dimensions(type, &w, &h))
    {
        goto done;
    }
    rawstride = w * 4;
    IMFMediaType_GetUINT32(type, &MF_MT_DEFAULT_STRIDE, &rawstride);
    stride = (LONG)rawstride;
    enc = codec_open(1, h, w, fps, bitrate, NULL);
    if (!enc)
    {
        hr = E_FAIL;
        goto done;
    }
    i420 = (unsigned char *)malloc(w * h * 3 / 2);
    rgb = (unsigned char *)malloc(w * h * 4);
    packet = (unsigned char *)malloc(MAX_MEDIA);
    if (!i420 || !rgb || !packet)
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!codec_headers(enc, &bytes, &len))
    {
        hr = E_FAIL;
        goto done;
    }
    packet[0] = 1;
    put32(packet + 1, h);
    put32(packet + 5, w);
    memcpy(packet + 9, bytes, len);
    if (!media_packet(c, packet, len + 9))
    {
        goto done;
    }
    log_event("CAMERA", "width=%d height=%d targetFPS=%d bitrate=%d", w, h, fps, bitrate);
    while (!c->stop && app.running)
    {
        hr = IMFSourceReader_ReadSample(r, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags,
                                        &ts, &sample);
        if (FAILED(hr) || flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            break;
        }
        if (!sample)
        {
            continue;
        }
        if (GetTickCount64() < next)
        {
            IMFSample_Release(sample);
            sample = NULL;
            continue;
        }
        next = GetTickCount64() + 1000 / fps;
        hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer);
        if (SUCCEEDED(hr))
        {
            hr = IMFMediaBuffer_Lock(buffer, &raw, NULL, &n);
        }
        if (SUCCEEDED(hr))
        {
            if (n >= (DWORD)(abs(stride) * h) && abs(stride) >= w * 4)
            {
                unsigned char *top = raw;
                int y;
                if (stride < 0)
                {
                    top = raw + (h - 1) * (-stride);
                }
                for (y = 0; y < h; y++)
                {
                    memcpy(rgb + y * w * 4, top + y * stride, w * 4);
                }
                video_present(rgb, w, h, 1);
                /* Android v1 rotates received frames 90 CW. Transmit 270 CW. */
                {
                    int x, yy;
                    unsigned char *rot = (unsigned char *)malloc(w * h * 4);
                    if (rot)
                    {
                        for (yy = 0; yy < h; yy++)
                        {
                            for (x = 0; x < w; x++)
                            {
                                memcpy(rot + 4 * ((w - 1 - x) * h + yy), rgb + 4 * (yy * w + x), 4);
                            }
                        }
                        rgb_i420(rot, h * 4, h, w, i420);
                        free(rot);
                    }
                    else
                    {
                        IMFMediaBuffer_Unlock(buffer);
                        raw = NULL;
                        hr = E_OUTOFMEMORY;
                        goto done;
                    }
                }
                if (codec_encode(enc, i420, &bytes, &len, &key))
                {
                    packet[0] = 2;
                    put64(packet + 1, (uint64_t)GetTickCount64() * 1000);
                    put32(packet + 9, key ? 1 : 0);
                    memcpy(packet + 13, bytes, len);
                    media_packet(c, packet, len + 13);
                }
            }
            IMFMediaBuffer_Unlock(buffer);
            raw = NULL;
        }
        if (buffer)
        {
            IMFMediaBuffer_Release(buffer);
            buffer = NULL;
        }
        IMFSample_Release(sample);
        sample = NULL;
    }
done:
    if (FAILED(hr) && !c->stop)
    {
        log_event("CAMERA_ERROR", "HRESULT=0x%08lx", hr);
        notice("Camera unavailable; call continues without outgoing video");
    }
    if (buffer)
    {
        IMFMediaBuffer_Release(buffer);
    }
    if (sample)
    {
        IMFSample_Release(sample);
    }
    codec_close(enc);
    free(i420);
    free(rgb);
    free(packet);
    EnterCriticalSection(&app.lock);
    c->camera_source = NULL;
    LeaveCriticalSection(&app.lock);
    if (type)
    {
        IMFMediaType_Release(type);
    }
    if (native)
    {
        IMFMediaType_Release(native);
    }
    if (r)
    {
        IMFSourceReader_Release(r);
    }
    if (source)
    {
        IMFMediaSource_Shutdown(source);
        IMFMediaSource_Release(source);
    }
    if (dll)
    {
        FreeLibrary(dll);
    }
    if (started)
    {
        MFShutdown();
    }
    CoUninitialize();
    return 0;
}

void camera_stop(struct call *c)
{
    IMFMediaSource *source;
    EnterCriticalSection(&app.lock);
    source = (IMFMediaSource *)c->camera_source;
    if (source)
    {
        IMFMediaSource_AddRef(source);
    }
    LeaveCriticalSection(&app.lock);
    if (source)
    {
        IMFMediaSource_Shutdown(source);
        IMFMediaSource_Release(source);
    }
}
