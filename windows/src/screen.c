#include "link.h"
#define SCREEN_H264_DUMP 0
/* Remote desktop over the Android 0.1.6+ "screen" call. The call uses its own
 * Screen-media sub-protocol on the same media socket, different from the audio/video
 * media frames: each TLS record begins with a kind byte:
 *   0 keepalive; 1 = format (h264 flag, w, h, SPS/PPS); 2 = frame
 * (key flag, pts, access unit); 3 = control (tap/swipe/key).
 * Role: the side that dials is the controller (receives frames, sends gestures);
 * the side that answers is the shared side (sends desktop frames, applies gestures). */
static float beefloat(const unsigned char *b)
{
    unsigned char t[4];
    float f;
    t[0] = b[3];
    t[1] = b[2];
    t[2] = b[1];
    t[3] = b[0];
    memcpy(&f, t, 4);
    return f;
}
static void putfloat(unsigned char *b, float f)
{
    unsigned char t[4];
    memcpy(t, &f, 4);
    b[0] = t[3];
    b[1] = t[2];
    b[2] = t[1];
    b[3] = t[0];
}
static unsigned char clamp(int n)
{
    return (unsigned char)(n < 0 ? 0 : n > 255 ? 255 : n);
}
/* Tracks whether a mouse-down arrived without a matching up, so the session
 * cleanup can synthesize an up and avoid leaving the remote with a stuck
 * left button. */
static int remote_mouse_stuck = 0;
static int remote_right_stuck = 0;
/* 32-bit top-down RGB: byte order B,G,R,X which matches what rgb_i420 expects. */
static void rgb_i420(const unsigned char *rgb, int stride, int w, int h, unsigned char *out)
{
    int x, y, r, g, b;
    unsigned char *u = out + w * h, *v = u + w * h / 4;
    /* NOTE: identical algorithm to camera.c but defined here for screen capture. */
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

static unsigned __stdcall screen_keepalive(void *arg)
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

/* Wire IDs are independent of Windows resource IDs. Unknown custom cursors
 * fall back to the arrow; standard cursors follow the viewer's system theme. */
LPCWSTR screen_cursor_resource(int id)
{
    static const int resources[] = {0, 32512, 32513, 32514, 32515, 32516,
                                    32642, 32643, 32644, 32645, 32646,
                                    32648, 32649, 32650, 32651};
    if (id < 1 || id >= (int)countof(resources))
    {
        return NULL;
    }
    return MAKEINTRESOURCEW(resources[id]);
}

static unsigned __stdcall screen_cursor_watch(void *arg)
{
    struct call *c = (struct call *)arg;
    HCURSOR standard[15];
    int i, last = -1;
    for (i = 1; i < (int)countof(standard); i++)
    {
        standard[i] = LoadCursorW(NULL, screen_cursor_resource(i));
    }
    while (!c->stop && app.running)
    {
        CURSORINFO info;
        int id = 1;
        memset(&info, 0, sizeof(info));
        info.cbSize = sizeof(info);
        if (GetCursorInfo(&info))
        {
            if (!(info.flags & CURSOR_SHOWING))
            {
                id = 0;
            }
            else
            {
                for (i = 1; i < (int)countof(standard); i++)
                {
                    if (standard[i] && standard[i] == info.hCursor)
                    {
                        id = i;
                        break;
                    }
                }
            }
            if (id != last)
            {
                unsigned char packet[3];
                packet[0] = 3;
                packet[1] = 14;
                packet[2] = (unsigned char)id;
                if (!media_packet(c, packet, sizeof(packet)))
                {
                    break;
                }
                last = id;
            }
        }
        Sleep(33);
    }
    return 0;
}

#if SCREEN_H264_DUMP
static FILE *dump_h264_open(const WCHAR *leaf)
{
    char leafbuf[128];
    WCHAR path[MAX_PATH];
    FILE *fp;
    path_join(path, app.exe_dir, leaf);
    utf8(leaf, leafbuf, 128);
    fp = _wfopen(path, L"wb");
    if (fp)
    {
        log_event("DUMP", "writing %s", leafbuf);
    }
    else
    {
        log_event("DUMP", "cannot open %s", leafbuf);
    }
    return fp;
}
#endif

static unsigned __stdcall screen_capture(void *arg)
{
    struct call *c = (struct call *)arg;
    struct codec *enc = NULL;
    struct codec_encoder_config config;
    HDC hdc = NULL, tmem = NULL;
    HBITMAP tbm = NULL;
    HGDIOBJ old = NULL;
    int sw, sh, cw, ch, requested_w, requested_h;
    unsigned char *rgb = NULL, *i420 = NULL, *packet = NULL, *bytes = NULL;
    BITMAPINFOHEADER bi;
    void *tbits = NULL;
    ULONGLONG next = 0, last_tx = GetTickCount64();
    int fps, bitrate, len, key, frames_out = 0, lastSum = 0;
#if SCREEN_H264_DUMP
    FILE *dump = NULL;
#endif

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hdc = GetDC(NULL);
    if (!hdc)
    {
        goto done;
    }
    sw = GetDeviceCaps(hdc, HORZRES);
    sh = GetDeviceCaps(hdc, VERTRES);
    if (sw < 2 || sh < 2)
    {
        goto done;
    }
    EnterCriticalSection(&app.lock);
    requested_w = app.screen_width;
    requested_h = app.screen_height;
    fps = app.screen_fps;
    bitrate = app.screen_bitrate;
    config.screen_content = 1;
    config.quality_mode = app.screen_quality;
    config.intra_period = app.screen_iframe;
    config.threads = app.screen_threads;
    config.frame_skip = app.screen_frame_skip;
    LeaveCriticalSection(&app.lock);
    /* Zero means native monitor size; explicit values make StretchBlt scale the capture. */
    cw = requested_w ? requested_w : sw;
    ch = requested_h ? requested_h : sh;
    /* H264 wants even dimensions; keep the native size but clear the low bit. */
    ch &= ~1;
    cw &= ~1;
    if (ch < 2 || cw < 2)
    {
        ch = 240;
        cw = 320;
    }

    enc = codec_open(1, cw, ch, fps, bitrate, &config);
    if (!enc)
    {
        log_event("SCREEN_ERROR", "codec_open failed");
        notice("Video codec unavailable for screen share");
        goto done;
    }
#if SCREEN_H264_DUMP
    dump = dump_h264_open(L"screen.h264");
#endif
    tmem = CreateCompatibleDC(hdc);
    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(bi);
    bi.biWidth = cw;
    bi.biHeight = ch; /* positive height: a normal bottom-up DIB (also reliable for StretchBlt) */
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    tbm = CreateDIBSection(hdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &tbits, NULL, 0);
    if (!tmem || !tbm || !tbits)
    {
        goto done;
    }
    old = SelectObject(tmem, tbm);
    rgb = (unsigned char *)malloc(cw * ch * 4);
    i420 = (unsigned char *)malloc(cw * ch * 3 / 2);
    packet = (unsigned char *)malloc(MAX_MEDIA);
    if (!rgb || !i420 || !packet)
    {
        goto done;
    }
    /* The "screen" call kind has its own wire format (ScreenEngine.java on the Android side),
     * DIFFERENT from the audio/video call format camera.c/media.c use - same idea (kind byte,
     * then format/frame fields), different byte layout. Must match exactly:
     * kind=1, h264 flag (1 byte, always 1 here), width, height, then SPS/PPS = 10-byte header.
     * (This is also exactly what this file's own receive side, further down, already assumes
     * with its fixed `b + 10` offset - that side was correct; only this send side had drifted
     * to the other protocol's layout.) */
    if (codec_headers(enc, &bytes, &len))
    {
        packet[0] = 1;
        packet[1] = 1; /* h264 flag */
        put32(packet + 2, (unsigned long)cw); /* width */
        put32(packet + 6, (unsigned long)ch); /* height */
        memcpy(packet + 10, bytes, len);
#if SCREEN_H264_DUMP
        if (dump)
        {
            fwrite(bytes, 1, len, dump);
        }
#endif
        if (!media_packet(c, packet, len + 10))
        {
            goto done;
        }
    }
    log_event("SCREEN", "source=%dx%d encode=%dx%d fps=%d bitrate=%d rc=%s iframe=%d", sw,
              sh, cw, ch, fps, bitrate, config.quality_mode ? "quality" : "bitrate",
              config.intra_period);
    while (!c->stop && app.running)
    {
        if (GetTickCount64() < next)
        {
            Sleep(10);
            continue;
        }
        next = GetTickCount64() + 1000 / (fps > 0 ? fps : 10);
        if (StretchBlt(tmem, 0, 0, cw, ch, hdc, 0, 0, sw, sh, SRCCOPY))
        {
            int yy, x, sum = 0;
            /* The DIB is bottom-up; flip to a top-down RGB buffer. */
            for (yy = 0; yy < ch; yy++)
            {
                memcpy(rgb + yy * cw * 4, ((unsigned char *)tbits) + (ch - 1 - yy) * cw * 4, cw * 4);
            }
            for (yy = 0; yy < ch; yy += 8)
            {
                for (x = 0; x < cw; x += 8)
                {
                    unsigned char *p = rgb + (yy * cw + x) * 4;
                    sum += p[0] + p[1] + p[2];
                }
            }
            lastSum = sum;
            rgb_i420(rgb, cw * 4, cw, ch, i420);
            if (codec_encode(enc, i420, &bytes, &len, &key))
                {
                    /* Screen-call frame layout (see the format packet above for why this isn't
                     * camera.c's layout): kind=2, key flag (1 byte), pts (8 bytes) = 10-byte
                     * header, then the H.264 access unit. The previous version here wrote a
                     * 13-byte header (pts before the key flag, key flag as 4 bytes not 1) but
                     * still only sent len+10 bytes total - truncating the last 3 bytes of every
                     * single frame's H.264 data, on top of every field being misread as something
                     * else by the Android side. */
                    packet[0] = 2;
                    packet[1] = (unsigned char)(key ? 1 : 0);
                    put64(packet + 2, (uint64_t)GetTickCount64() * 1000);
                    memcpy(packet + 10, bytes, len);
#if SCREEN_H264_DUMP
                    if (dump)
                    {
                        fwrite(bytes, 1, len, dump);
                    }
#endif
                    if (!media_packet(c, packet, len + 10))
                    {
                        break;
                    }
                    frames_out++;
                }
        }
        if (GetTickCount64() - last_tx >= 5000)
        {
            log_event("SCREEN_TX", "frames5s=%d capSum=%d", frames_out, lastSum);
            frames_out = 0;
            last_tx = GetTickCount64();
        }
    }
done:
    if (old && tmem)
    {
        /* old is the previous object in tmem; unselect the DIB */
        SelectObject(tmem, old);
        old = NULL;
    }
    if (tbm)
    {
        DeleteObject(tbm);
    }
    if (tmem)
    {
        DeleteDC(tmem);
    }
    if (hdc)
    {
        ReleaseDC(NULL, hdc);
    }
    codec_close(enc);
#if SCREEN_H264_DUMP
    if (dump)
    {
        fclose(dump);
    }
#endif
    free(rgb);
    free(i420);
    free(packet);
    CoUninitialize();
    return 0;
}

/* Desktop icon view may be hosted by the shell's Progman or a WorkerW. Do not
 * classify Explorer folder windows or taskbar windows as the desktop. */
static int point_is_shell_desktop(POINT pt)
{
    HWND hit, root, shell;
    DWORD root_pid = 0, shell_pid = 0;
    WCHAR cls[64];
    hit = WindowFromPoint(pt);
    if (!hit)
    {
        return 0;
    }
    root = GetAncestor(hit, GA_ROOT);
    shell = GetShellWindow();
    if (!root || !shell)
    {
        return 0;
    }
    if (root == shell)
    {
        return 1;
    }
    GetWindowThreadProcessId(root, &root_pid);
    GetWindowThreadProcessId(shell, &shell_pid);
    if (!root_pid || root_pid != shell_pid || !GetClassNameW(root, cls, countof(cls)))
    {
        return 0;
    }
    return lstrcmpW(cls, L"WorkerW") == 0 || lstrcmpW(cls, L"Progman") == 0;
}

static void control_exec(int op, const unsigned char *b, int len)
{
    float x, y, x1, y1, x2, y2;
    int ms, action;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    INPUT in[3];
    switch (op)
    {
    case 1: /* tap */
        if (len < 10)
        {
            return;
        }
        x = beefloat(b + 2);
        y = beefloat(b + 6);
        /* Click via absolute MOVE + LEFTDOWN/LEFTUP: down and up must target the
         * same point, so a position move first ensures the cursor is there. */
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        in[0].mi.dx = (LONG)(x * 65535.0f);
        in[0].mi.dy = (LONG)(y * 65535.0f);
        SendInput(1, in, sizeof(INPUT));
        Sleep(10);
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTDOWN;
        in[0].mi.dx = (LONG)(x * 65535.0f);
        in[0].mi.dy = (LONG)(y * 65535.0f);
        SendInput(1, in, sizeof(INPUT));
        Sleep(30);
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTUP;
        in[0].mi.dx = (LONG)(x * 65535.0f);
        in[0].mi.dy = (LONG)(y * 65535.0f);
        SendInput(1, in, sizeof(INPUT));
        break;
    case 2: /* swipe = drag from start to end with the left button held */
        if (len < 22)
        {
            return;
        }
        x1 = beefloat(b + 2);
        y1 = beefloat(b + 6);
        x2 = beefloat(b + 10);
        y2 = beefloat(b + 14);
        ms = (int)((unsigned long)be32(b + 18));
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        in[0].mi.dx = (LONG)(x1 * 65535.0f);
        in[0].mi.dy = (LONG)(y1 * 65535.0f);
        SendInput(1, in, sizeof(INPUT));
        Sleep(10);
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTDOWN;
        in[0].mi.dx = (LONG)(x1 * 65535.0f);
        in[0].mi.dy = (LONG)(y1 * 65535.0f);
        SendInput(1, in, sizeof(INPUT));
        /* Stream intermediate positions so the target app sees real move
         * events while the button is held (text selection, icon drag, ...).
         * The previous version only SetCursorPos + LEFTUP, which the target
         * window never interpreted as a drag. */
        {
            int steps, i;
            steps = ms / 16;
            if (steps < 1)
            {
                steps = 1;
            }
            if (steps > 50)
            {
                steps = 50;
            }
            for (i = 1; i <= steps; i++)
            {
                float fx = x1 + (x2 - x1) * (float)i / (float)steps;
                float fy = y1 + (y2 - y1) * (float)i / (float)steps;
                memset(in, 0, sizeof(in));
                in[0].type = INPUT_MOUSE;
                in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
                in[0].mi.dx = (LONG)(fx * 65535.0f);
                in[0].mi.dy = (LONG)(fy * 65535.0f);
                SendInput(1, in, sizeof(INPUT));
                Sleep(ms / steps > 0 ? ms / steps : 1);
            }
        }
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTUP;
        in[0].mi.dx = (LONG)(x2 * 65535.0f);
        in[0].mi.dy = (LONG)(y2 * 65535.0f);
        SendInput(1, in, sizeof(INPUT));
        break;
    case 13: /* keyboard: [i32 vk][i32 down] */
        if (len < 10)
        {
            return;
        }
        {
            int vk = (int)(long)be32(b + 2);
            int down = (int)(long)be32(b + 6);
            unsigned int flags;
            switch (vk)
            {
            case VK_UP:
            case VK_DOWN:
            case VK_LEFT:
            case VK_RIGHT:
            case VK_HOME:
            case VK_END:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_INSERT:
            case VK_DELETE:
            case VK_DIVIDE:
            case VK_APPS:
            case VK_NUMLOCK:
            case VK_LWIN:
            case VK_RWIN:
            case VK_SNAPSHOT:
            case VK_RMENU:
            case VK_RCONTROL:
                flags = KEYEVENTF_EXTENDEDKEY;
                break;
            default:
                flags = 0;
            }
            if (!down)
            {
                flags |= KEYEVENTF_KEYUP;
            }
            memset(in, 0, sizeof(in));
            in[0].type = INPUT_KEYBOARD;
            in[0].ki.wVk = (WORD)vk;
            in[0].ki.dwFlags = flags;
            if (SendInput(1, in, sizeof(INPUT)) != 1)
                log_event("REMOTE_KEY", "SendInput failed error=%lu", (unsigned long)GetLastError());
        }
        break;
    case 5: /* mouse down: hold the left button at the normalized point */
        if (len < 10)
        {
            return;
        }
        log_event("CTRL_EXEC", "mouse down");
        x = beefloat(b + 2);
        y = beefloat(b + 6);
        remote_mouse_stuck = 1;
        /* Left-button events do not move the cursor; the button event only
         * happens at the current cursor location. So we first move to the
         * requested point, then press. */
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        in[0].mi.dx = (LONG)(x * 65535.0f);
        in[0].mi.dy = (LONG)(y * 65535.0f);
        SendInput(1, in, sizeof(INPUT));
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        SendInput(1, in, sizeof(INPUT));
        break;
    case 6: /* mouse move: while the button is held this is a drag */
        if (len < 10)
        {
            return;
        }
        x = beefloat(b + 2);
        y = beefloat(b + 6);
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        in[0].mi.dx = (LONG)(x * 65535.0f);
        in[0].mi.dy = (LONG)(y * 65535.0f);
        SendInput(1, in, sizeof(INPUT));
        break;
    case 7: /* mouse up */
        if (len < 10)
        {
            return;
        }
        log_event("CTRL_EXEC", "mouse up");
        x = beefloat(b + 2);
        y = beefloat(b + 6);
        remote_mouse_stuck = 0;
        /* Same subtlety as mouse down: a LEFTUP does not move the cursor before
         * it releases the button; we first place the cursor at the up point. */
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        in[0].mi.dx = (LONG)(x * 65535.0f);
        in[0].mi.dy = (LONG)(y * 65535.0f);
        SendInput(1, in, sizeof(INPUT));
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, in, sizeof(INPUT));
        break;
    case 8: /* right mouse down at the normalized point */
        if (len < 10)
        {
            return;
        }
        log_event("CTRL_EXEC", "right mouse down");
        x = beefloat(b + 2);
        y = beefloat(b + 6);
        remote_right_stuck = 1;
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        in[0].mi.dx = (LONG)(x * 65535.0f);
        in[0].mi.dy = (LONG)(y * 65535.0f);
        SendInput(1, in, sizeof(INPUT));
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        SendInput(1, in, sizeof(INPUT));
        break;
    case 9: /* right mouse up */
        if (len < 10)
        {
            return;
        }
        log_event("CTRL_EXEC", "right mouse up");
        x = beefloat(b + 2);
        y = beefloat(b + 6);
        remote_right_stuck = 0;
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        in[0].mi.dx = (LONG)(x * 65535.0f);
        in[0].mi.dy = (LONG)(y * 65535.0f);
        SendInput(1, in, sizeof(INPUT));
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        SendInput(1, in, sizeof(INPUT));
        break;
    case 10: /* vertical wheel scroll at the normalized point */
        if (len < 14)
        {
            return;
        }
        x = beefloat(b + 2);
        y = beefloat(b + 6);
        /* delta in WHEEL_DELTA units, + forward / - backward */
        {
            int delta = (int)be32(b + 10);
            memset(in, 0, sizeof(in));
            in[0].type = INPUT_MOUSE;
            in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
            in[0].mi.dx = (LONG)(x * 65535.0f);
            in[0].mi.dy = (LONG)(y * 65535.0f);
            SendInput(1, in, sizeof(INPUT));
            memset(in, 0, sizeof(in));
            in[0].type = INPUT_MOUSE;
            in[0].mi.dwFlags = MOUSEEVENTF_WHEEL;
            in[0].mi.mouseData = (DWORD)(short)delta;
            SendInput(1, in, sizeof(INPUT));
        }
        break;
    case 11: /* horizontal wheel scroll at the normalized point */
        if (len < 14)
        {
            return;
        }
        x = beefloat(b + 2);
        y = beefloat(b + 6);
        {
            int delta = (int)be32(b + 10);
            memset(in, 0, sizeof(in));
            in[0].type = INPUT_MOUSE;
            in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
            in[0].mi.dx = (LONG)(x * 65535.0f);
            in[0].mi.dy = (LONG)(y * 65535.0f);
            SendInput(1, in, sizeof(INPUT));
            memset(in, 0, sizeof(in));
            in[0].type = INPUT_MOUSE;
            in[0].mi.dwFlags = MOUSEEVENTF_HWHEEL;
            in[0].mi.mouseData = (DWORD)(short)delta;
            SendInput(1, in, sizeof(INPUT));
        }
        break;
    case 4: /* pinch: cx,cy,scale; translate to Ctrl+wheel at cx,cy */
        if (len < 14)
        {
            return;
        }
        x = beefloat(b + 2);
        y = beefloat(b + 6);
        {
            int mode;
            POINT pt;
            EnterCriticalSection(&app.lock);
            mode = app.remote_pinch_mode;
            LeaveCriticalSection(&app.lock);
            if (!(x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f))
            {
                return;
            }
            pt.x = (LONG)(x * (sw - 1));
            pt.y = (LONG)(y * (sh - 1));
            if (mode == 2 || (mode == 1 && point_is_shell_desktop(pt)))
            {
                return;
            }
        }
        /* scale parsed here only for direction */
        SetCursorPos((int)(x * sw), (int)(y * sh));
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = VK_LCONTROL;
        SendInput(1, in, sizeof(INPUT));
        Sleep(20);
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_WHEEL;
        in[0].mi.mouseData = (DWORD)(beefloat(b + 10) > 1.0f ? 120 : -120);
        SendInput(1, in, sizeof(INPUT));
        Sleep(20);
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = VK_LCONTROL;
        in[0].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, in, sizeof(INPUT));
        break;
    case 3: /* key: 1=back->Alt+Left, 2=home->Win, 3=recents->Alt+Tab */
        if (len < 6)
        {
            return;
        }
        action = (int)((unsigned long)be32(b + 2));
        memset(in, 0, sizeof(in));
        if (action == 1)
        {
            /* Browser/Explorer "Go back" is Alt+Left Arrow, not Esc. */
            INPUT seq[4];
            memset(seq, 0, sizeof(seq));
            seq[0].type = INPUT_KEYBOARD;
            seq[0].ki.wVk = VK_LMENU;
            seq[0].ki.dwFlags = 0;
            seq[1].type = INPUT_KEYBOARD;
            seq[1].ki.wVk = VK_LEFT;
            seq[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
            seq[2].type = INPUT_KEYBOARD;
            seq[2].ki.wVk = VK_LEFT;
            seq[2].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
            seq[3].type = INPUT_KEYBOARD;
            seq[3].ki.wVk = VK_LMENU;
            seq[3].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(4, seq, sizeof(INPUT));
        }
        else if (action == 2)
        {
            in[0].type = INPUT_KEYBOARD;
            in[0].ki.wVk = VK_LWIN;
            in[1].type = INPUT_KEYBOARD;
            in[1].ki.wVk = VK_LWIN;
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
        }
        else if (action == 3)
        {
            /* Recents/task-switcher: Alt down, Tab down, Tab up, Alt up. The
             * previous version sent only a spurious LMENU keyup, so nothing
             * ever happened. */
            INPUT seq[4];
            memset(seq, 0, sizeof(seq));
            seq[0].type = INPUT_KEYBOARD;
            seq[0].ki.wVk = VK_LMENU;
            seq[1].type = INPUT_KEYBOARD;
            seq[1].ki.wVk = VK_TAB;
            seq[2].type = INPUT_KEYBOARD;
            seq[2].ki.wVk = VK_TAB;
            seq[2].ki.dwFlags = KEYEVENTF_KEYUP;
            seq[3].type = INPUT_KEYBOARD;
            seq[3].ki.wVk = VK_LMENU;
            seq[3].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(4, seq, sizeof(INPUT));
        }
        break;
    default:
        break;
    }
}
static volatile LONG remote_text_capable;
int screen_remote_text_supported(void)
{
    return InterlockedCompareExchange(&remote_text_capable, 0, 0) != 0;
}

static int screen_send(const unsigned char *pkt, int len)
{
    struct call *c;
    int ok = 0;
    EnterCriticalSection(&app.lock);
    c = app.call;
    if (c && !strcmp(c->kind, "screen") && c->outgoing && c->tls)
    {
        ok = media_packet(c, pkt, len);
    }
    LeaveCriticalSection(&app.lock);
    return ok;
}
int screen_send_tap(float x, float y)
{
    unsigned char p[10];
    p[0] = 3;
    p[1] = 1;
    putfloat(p + 2, x);
    putfloat(p + 6, y);
    return screen_send(p, 10);
}
int screen_send_swipe(float x1, float y1, float x2, float y2, int ms)
{
    unsigned char p[22];
    p[0] = 3;
    p[1] = 2;
    putfloat(p + 2, x1);
    putfloat(p + 6, y1);
    putfloat(p + 10, x2);
    putfloat(p + 14, y2);
    put32(p + 18, (unsigned long)ms);
    return screen_send(p, 22);
}
int screen_send_key(int action)
{
    unsigned char p[6];
    p[0] = 3;
    p[1] = 3;
    put32(p + 2, (unsigned long)action);
    return screen_send(p, 6);
}
int screen_send_mouse(int op, float x, float y)
{
    unsigned char p[10];
    p[0] = 3;
    p[1] = (unsigned char)op;
    putfloat(p + 2, x);
    putfloat(p + 6, y);
    return screen_send(p, 10);
}
int screen_send_wheel(float x, float y, int delta, int horiz)
{
    unsigned char p[14];
    p[0] = 3;
    p[1] = (unsigned char)(horiz ? 11 : 10);
    putfloat(p + 2, x);
    putfloat(p + 6, y);
    put32(p + 10, (unsigned long)(long)delta);
    return screen_send(p, 14);
}
int screen_send_keycode(int vk, int down)
{
    unsigned char p[10];
    p[0] = 3;
    p[1] = 13;
    put32(p + 2, (unsigned long)vk);
    put32(p + 6, (unsigned long)(down ? 1 : 0));
    return screen_send(p, 10);
}

int screen_send_text(const WCHAR *text, int length)
{
    unsigned char p[66];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, (char *)p + 2, 64, NULL, NULL);
    if (bytes <= 0) return 0;
    p[0] = 3;
    p[1] = 15;
    return screen_send(p, bytes + 2);
}

void screen_run(struct call *c)
{
    HANDLE cap = NULL, ping = NULL, cursor = NULL;
    struct codec *dec = NULL;
    int timeout = 15000, len, bw, bh;
    ULONGLONG last = GetTickCount64();
    int frames = 0;
    unsigned char *b, *rgb;

    if (c->outgoing) InterlockedExchange(&remote_text_capable, 0);
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

    if (c->outgoing)
    {
        /* Controller: decode the remote desktop stream. */
        dec = codec_open(0, 0, 0, 0, 0, NULL);
    }
    else if (codec_available())
    {
        /* Shared side: capture this desktop and share it. */
        cap = (HANDLE)_beginthreadex(NULL, 0, screen_capture, c, 0, NULL);
        cursor = (HANDLE)_beginthreadex(NULL, 0, screen_cursor_watch, c, 0, NULL);
        if (!cap)
        {
            notice("Screen capture unavailable");
        }
    }
    ping = (HANDLE)_beginthreadex(NULL, 0, screen_keepalive, c, 0, NULL);
    changed();
    log_event("MEDIA_START", "kind=%s outgoing=%d", c->kind, c->outgoing);

    while (!c->stop && app.running)
    {
        b = wire_read(c->tls, &len, MAX_MEDIA);
        if (!b)
        {
            break;
        }
        switch (b[0])
        {
        case 0: /* keepalive */
            break;
        case 1: /* format: feed the SPS/PPS container to the decoder too */
            if (c->outgoing && dec && len >= 10)
            {
                int cw2, ch2;
                unsigned char *rgb2;
                /* OpenH264 needs the parameter sets fed before the access units
                 * decode, exactly as media.c feeds the video "format" packet. */
                codec_decode(dec, b + 10, len - 10, &rgb2, &cw2, &ch2);
            }
            break;
        case 2: /* frame */
            if (c->outgoing && dec && len > 10)
            {
                if (codec_decode(dec, b + 10, len - 10, &rgb, &bw, &bh))
                {
                    video_present(rgb, bw, bh, 0);
                    frames++;
                }
            }
            break;
        case 3: /* control */
            if (c->outgoing && len == 3 && b[1] == 16 && b[2] == 1)
                InterlockedExchange(&remote_text_capable, 1);
            if (c->outgoing && len == 3 && b[1] == 14 && b[2] <= 14)
            {
                ui_remote_cursor(b[2]);
            }
            if (!c->outgoing && len >= 2)
            {
                control_exec(b[1], b, len);
            }
            break;
        default:
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
    if (remote_mouse_stuck)
    {
        /* The remote held the left button when the session died; release it so
         * the desktop is not left in a dragging state. */
        INPUT in2;
        memset(&in2, 0, sizeof(in2));
        in2.type = INPUT_MOUSE;
        in2.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &in2, sizeof(INPUT));
        remote_mouse_stuck = 0;
        log_event("CTRL_EXEC", "safety-leftup on session close");
    }
    if (remote_right_stuck)
    {
        INPUT in3;
        memset(&in3, 0, sizeof(in3));
        in3.type = INPUT_MOUSE;
        in3.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        SendInput(1, &in3, sizeof(INPUT));
        remote_right_stuck = 0;
        log_event("CTRL_EXEC", "safety-rightup on session close");
    }
    if (cap)
    {
        WaitForSingleObject(cap, INFINITE);
        CloseHandle(cap);
    }
    if (ping)
    {
        WaitForSingleObject(ping, INFINITE);
        CloseHandle(ping);
    }
    if (cursor)
    {
        WaitForSingleObject(cursor, INFINITE);
        CloseHandle(cursor);
    }
    codec_close(dec);
    EnterCriticalSection(&app.lock);
    tls_close(c->tls);
    c->tls = NULL;
    LeaveCriticalSection(&app.lock);
    log_event("MEDIA_STOP", "closed");
}
