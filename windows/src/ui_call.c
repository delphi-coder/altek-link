#include "ui.h"
#include <mmsystem.h>
HWND ui_call;
static unsigned char *frames[2];
static int widths[2], heights[2];
static ULONGLONG ring_time;
static int screen_rot = 0;
static int down_px = -1, down_py = -1, dragging = 0;
static ULONGLONG down_time = 0;
/* Which remote button is currently held: 0=none, 1=left, 2=right. While a
 * button is held, absolute moves (op=6) are streamed so the remote sees a
 * real drag instead of a single down/up pair. */
static int remote_button = 0;
/* View mode: 0 = fit the frame inside the window, 1 = actual pixel size
 * (scrollable; when smaller than the area it is centered). `scr_*` is the
 * scroll offset of the frame in actual-size mode. */
static int view_mode = 0;
static int scr_x = 0, scr_y = 0;
static int controls_incoming = -1, controls_nav = -1, controls_fullscreen = -1;
static LONG remote_cursor = 1;
static int clipboard_mode;
static DWORD clipboard_suppress;

void ui_clipboard_packet(const unsigned char *data, int len)
{
    unsigned char *copy;
    if (len < 2 || len > 65538 || !ui_call) return;
    copy = (unsigned char *)malloc(len);
    if (!copy) return;
    memcpy(copy, data, len);
    if (!PostMessageW(ui_call, UI_CLIPBOARD_PACKET, (WPARAM)len, (LPARAM)copy)) free(copy);
}

static int clipboard_controller(void)
{
    return app.call && app.call->outgoing && !strcmp(app.call->kind, "screen");
}

static void clipboard_send(HWND hwnd)
{
    HANDLE h;
    const WCHAR *w;
    unsigned char *packet;
    int chars, bytes;
    if (!(clipboard_mode & (clipboard_controller() ? 1 : 2))) return;
    if (GetClipboardSequenceNumber() == clipboard_suppress) return;
    if (!OpenClipboard(hwnd)) return;
    h = GetClipboardData(CF_UNICODETEXT);
    w = h ? (const WCHAR *)GlobalLock(h) : NULL;
    if (w)
    {
        chars = (int)wcslen(w);
        bytes = WideCharToMultiByte(CP_UTF8, 0, w, chars, NULL, 0, NULL, NULL);
        if (bytes >= 0 && bytes <= 65536)
        {
            packet = (unsigned char *)malloc(bytes + 2);
            if (packet)
            {
                packet[0] = 3; packet[1] = 18;
                if (!bytes || WideCharToMultiByte(CP_UTF8, 0, w, chars, (char *)packet + 2, bytes, NULL, NULL))
                    screen_send_clipboard(packet, bytes + 2);
                free(packet);
            }
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
}

static void clipboard_receive(HWND hwnd, const unsigned char *packet, int len)
{
    int count;
    HGLOBAL mem;
    WCHAR *w;
    if (!(clipboard_mode & (clipboard_controller() ? 2 : 1)) || len < 2 || len > 65538) return;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)packet + 2, len - 2, NULL, 0);
    if (!count && len > 2) return;
    mem = GlobalAlloc(GMEM_MOVEABLE, (count + 1) * sizeof(WCHAR));
    if (!mem) return;
    w = (WCHAR *)GlobalLock(mem);
    if (!w) { GlobalFree(mem); return; }
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)packet + 2, len - 2, w, count);
    w[count] = 0;
    GlobalUnlock(mem);
    if (!OpenClipboard(hwnd)) { GlobalFree(mem); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    clipboard_suppress = GetClipboardSequenceNumber();
    CloseClipboard();
}


void ui_remote_cursor(int id)
{
    InterlockedExchange(&remote_cursor, id);
    if (ui_call)
    {
        PostMessageW(ui_call, UI_REMOTE_CURSOR, 0, 0);
    }
}

static void video_area(HWND hwnd, RECT *area)
{
    RECT r;
    GetClientRect(hwnd, &r);
    if (ui_screen_fullscreen_active(hwnd))
    {
        *area = r;
    }
    else
    {
        SetRect(area, 8, 38, r.right - 8, r.bottom - 66);
    }
}

static void call_controls(HWND hwnd, int incoming, int nav)
{
    int ids[10] = {ID_ANSWER, ID_MUTE, ID_SPEAKER, ID_ROTATE, ID_END, ID_BACK, ID_HOME,
                   ID_RECENTS, ID_VIEW, ID_FULLSCREEN};
    int i, fullscreen = ui_screen_fullscreen_active(hwnd);
    if (incoming == controls_incoming && nav == controls_nav &&
        fullscreen == controls_fullscreen)
    {
        return;
    }
    controls_incoming = incoming;
    controls_nav = nav;
    controls_fullscreen = fullscreen;
    if (fullscreen)
    {
        for (i = 0; i < 10; i++)
        {
            ShowWindow(GetDlgItem(hwnd, ids[i]), SW_HIDE);
        }
        return;
    }
    ShowWindow(GetDlgItem(hwnd, ID_ANSWER), incoming ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_MUTE), SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, ID_SPEAKER), SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, ID_ROTATE), SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, ID_END), SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, ID_BACK), nav ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_HOME), nav ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_RECENTS), nav ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_VIEW), nav ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_FULLSCREEN), nav ? SW_SHOW : SW_HIDE);
}

/* Current rotation applied to the main (first) preview stream. */
static int main_rotate(void)
{
    int rot;
    EnterCriticalSection(&app.lock);
    rot = (app.call && !strcmp(app.call->kind, "screen")) ? screen_rot : app.rx_rotate;
    LeaveCriticalSection(&app.lock);
    return rot;
}

/* Remote frame size after applying `rotate` (90/270 swap w/h). */
static void eff_size(int preview, int rotate, int *w, int *h)
{
    int fw = widths[preview], fh = heights[preview], r;
    r = ((rotate % 360) + 360) % 360;
    if (r == 90 || r == 270)
    {
        *w = fh;
        *h = fw;
    }
    else
    {
        *w = fw;
        *h = fh;
    }
}

/* Destination rectangle the frame is painted into. Fit mode scales to
 * contain; actual-size mode draws 1:1 pixels, centered when it fits and
 * offset by scr_x/scr_y when it does not (scr is clamped here). */
static void video_rect(RECT area, int preview, int rotate, RECT *out)
{
    int w, h, aw, ah, ox, oy;
    eff_size(preview, rotate, &w, &h);
    aw = area.right - area.left;
    ah = area.bottom - area.top;
    if (w < 1 || h < 1 || aw < 1 || ah < 1)
    {
        SetRectEmpty(out);
        return;
    }
    if (view_mode == 1 && preview == 0)
    {
        if (w <= aw)
        {
            ox = area.left + (aw - w) / 2;
            scr_x = 0;
        }
        else
        {
            if (scr_x < 0)
            {
                scr_x = 0;
            }
            if (scr_x > w - aw)
            {
                scr_x = w - aw;
            }
            ox = area.left - scr_x;
        }
        if (h <= ah)
        {
            oy = area.top + (ah - h) / 2;
            scr_y = 0;
        }
        else
        {
            if (scr_y < 0)
            {
                scr_y = 0;
            }
            if (scr_y > h - ah)
            {
                scr_y = h - ah;
            }
            oy = area.top - scr_y;
        }
        SetRect(out, ox, oy, ox + w, oy + h);
        return;
    }
    {
        float s = (float)aw / (float)w;
        if ((float)ah / (float)h < s)
        {
            s = (float)ah / (float)h;
        }
        w = (int)(w * s);
        h = (int)(h * s);
        ox = area.left + (aw - w) / 2;
        oy = area.top + (ah - h) / 2;
        SetRect(out, ox, oy, ox + w, oy + h);
    }
}

/* Maps a point over the call window to normalized 0..1 coords on the remote
 * screen, using the same rect the painter draws the frame into. */
static int video_norm(int mx, int my, float *nx, float *ny)
{
    RECT v, dr;
    if (!frames[0] || widths[0] < 1 || heights[0] < 1)
    {
        return 0;
    }
    video_area(ui_call, &v);
    if (v.right - v.left < 1 || v.bottom - v.top < 1)
    {
        return 0;
    }
    video_rect(v, 0, main_rotate(), &dr);
    if (IsRectEmpty(&dr) || mx < dr.left || my < dr.top || mx >= dr.right ||
        my >= dr.bottom)
    {
        return 0;
    }
    *nx = (float)(mx - dr.left) / (float)(dr.right - dr.left);
    *ny = (float)(my - dr.top) / (float)(dr.bottom - dr.top);
    if (*nx < 0.0f)
    {
        *nx = 0.0f;
    }
    if (*nx > 1.0f)
    {
        *nx = 1.0f;
    }
    if (*ny < 0.0f)
    {
        *ny = 0.0f;
    }
    if (*ny > 1.0f)
    {
        *ny = 1.0f;
    }
    return 1;
}
void video_present(const unsigned char *rgb, int w, int h, int preview)
{
    unsigned char *b;
    if (w < 1 || h < 1 || w > 1920 || h > 1920)
    {
        return;
    }
    EnterCriticalSection(&app.lock);
    if (w != widths[preview] || h != heights[preview])
    {
        if (preview == 0)
        {
            scr_x = 0;
            scr_y = 0;
        }
    }
    b = (unsigned char *)realloc(frames[preview], w * h * 4);
    if (b)
    {
        frames[preview] = b;
        memcpy(b, rgb, w * h * 4);
        widths[preview] = w;
        heights[preview] = h;
    }
    LeaveCriticalSection(&app.lock);
    /* UI repaints on a fixed timer; no unbounded queued frame messages. */
}
static void fitted(HDC dc, int preview, RECT area, int rotate)
{
    unsigned char *b = frames[preview], *rotated = NULL;
    int w = widths[preview], h = heights[preview], rw, rh, x, y;
    RECT r;
    if (!b)
    {
        return;
    }
    rotate = ((rotate % 360) + 360) % 360;
    if (rotate)
    {
        rotated = (unsigned char *)malloc(w * h * 4);
        if (!rotated)
        {
            return;
        }
        rw = (rotate == 90 || rotate == 270) ? h : w;
        rh = (rotate == 90 || rotate == 270) ? w : h;
        for (y = 0; y < h; y++)
        {
            for (x = 0; x < w; x++)
            {
                int dx = rotate == 90 ? h - 1 - y : rotate == 270 ? y : w - 1 - x;
                int dy = rotate == 90 ? x : rotate == 270 ? w - 1 - x : h - 1 - y;
                memcpy(rotated + 4 * (dy * rw + dx), b + 4 * (y * w + x), 4);
            }
        }
        b = rotated;
        w = rw;
        h = rh;
    }
    video_rect(area, preview, rotate, &r);
    if (preview == 0 && view_mode == 1)
    {
        int sv = SaveDC(dc);
        IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
        ui_draw_image(dc, b, w, h, r, 0);
        RestoreDC(dc, sv);
    }
    else
    {
        ui_draw_image(dc, b, w, h, r, 0);
    }
    free(rotated);
}
static LRESULT CALLBACK call_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CREATE)
    {
        controls_incoming = controls_nav = controls_fullscreen = -1;
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_ANSWER, 0, 0, 48, 42);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_MUTE, 0, 0, 48, 42);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_SPEAKER, 0, 0, 48, 42);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_ROTATE, 0, 0, 48, 42);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_END, 0, 0, 48, 42);
        /* Android navigation buttons sent to the remote phone: Back / Home / Recents. */
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_BACK, 0, 0, 48, 42);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_HOME, 0, 0, 48, 42);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_RECENTS, 0, 0, 48, 42);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_VIEW, 0, 0, 48, 42);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_FULLSCREEN, 0, 0, 48, 42);
        ui_control(hwnd, L"BUTTON", L"Clipboard", BS_PUSHBUTTON, ID_CLIPBOARD, 0, 0, 75, 28);
        AddClipboardFormatListener(hwnd);
        clipboard_mode = 0;
        SetTimer(hwnd, 1, 100, NULL);
        return 0;
    }
    if (msg == WM_SIZE)
    {
        /* Include the three remote Android nav buttons in the bottom row. */
        int ids[10] = {ID_ANSWER, ID_MUTE, ID_SPEAKER, ID_ROTATE, ID_END, ID_BACK, ID_HOME,
                       ID_RECENTS, ID_VIEW, ID_FULLSCREEN},
            i, x = (LOWORD(lp) - 10 * 58) / 2;
        for (i = 0; i < 10; i++)
        {
            MoveWindow(GetDlgItem(hwnd, ids[i]), x + i * 58, HIWORD(lp) - 56, 48, 42, TRUE);
        }
        MoveWindow(GetDlgItem(hwnd, ID_CLIPBOARD), LOWORD(lp) - 95, 5, 85, 28, TRUE);
        return 0;
    }
    if (msg == WM_CLIPBOARDUPDATE) { clipboard_send(hwnd); return 0; }
    if (msg == UI_CLIPBOARD_PACKET)
    {
        unsigned char *packet = (unsigned char *)lp;
        int len = (int)wp;
        if (len == 3 && packet[1] == 17 && !clipboard_controller())
        {
            clipboard_mode = packet[2] & 3;
            /* For two-way startup, the controller sends its current clipboard first.
               Do not race it with the remote device's older clipboard contents. */
            if (clipboard_mode == 3) clipboard_suppress = GetClipboardSequenceNumber();
            else clipboard_send(hwnd);
        }
        else if (len >= 2 && packet[1] == 18) clipboard_receive(hwnd, packet, len);
        free(packet);
        return 0;
    }
    if (msg == UI_REMOTE_CURSOR)
    {
        POINT pt;
        GetCursorPos(&pt);
        if (WindowFromPoint(pt) == hwnd)
        {
            SendMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        }
        return 0;
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && (HWND)wp == hwnd)
    {
        POINT pt;
        float nx = 0, ny = 0;
        int screen;
        EnterCriticalSection(&app.lock);
        screen = app.call && app.call->outgoing && app.call->media_started &&
                 !strcmp(app.call->kind, "screen");
        LeaveCriticalSection(&app.lock);
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (screen && video_norm(pt.x, pt.y, &nx, &ny))
        {
            int id = (int)InterlockedCompareExchange(&remote_cursor, 0, 0);
            LPCWSTR resource = screen_cursor_resource(id);
            SetCursor(resource ? LoadCursorW(NULL, resource) : NULL);
            return TRUE;
        }
    }
    if (msg == WM_TIMER)
    {
        int incoming = 0, nav = 0;
        EnterCriticalSection(&app.lock);
        if (app.call)
        {
            incoming = !app.call->outgoing && !app.call->accepted;
            nav = app.call->outgoing && !strcmp(app.call->kind, "screen");
            if (incoming && GetTickCount64() - ring_time > 2500)
            {
                ring_time = GetTickCount64();
                PlaySoundW(L"SystemExclamation", NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
            }
        }
        LeaveCriticalSection(&app.lock);
        /* Edge auto-scroll in actual-size mode: push the mouse into a border
         * band to keep panning the oversized remote frame. */
        if (view_mode == 1)
        {
            RECT r, v;
            POINT pt;
            int w, h, aw, ah, step = 36, m = 28, changed = 0;
            GetClientRect(hwnd, &r);
            video_area(hwnd, &v);
            eff_size(0, main_rotate(), &w, &h);
            aw = v.right - v.left;
            ah = v.bottom - v.top;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (pt.x >= v.left && pt.x < v.right && pt.y >= v.top && pt.y < v.bottom)
            {
                if (w > aw && pt.x < v.left + m)
                {
                    scr_x -= step;
                    changed = 1;
                }
                else if (w > aw && pt.x > v.right - m)
                {
                    scr_x += step;
                    changed = 1;
                }
                if (h > ah && pt.y < v.top + m)
                {
                    scr_y -= step;
                    changed = 1;
                }
                else if (h > ah && pt.y > v.bottom - m)
                {
                    scr_y += step;
                    changed = 1;
                }
                if (changed)
                {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        if (ui_screen_fullscreen_active(hwnd))
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            ui_screen_fullscreen_pointer(hwnd, pt.x, pt.y);
        }
        ShowWindow(GetDlgItem(hwnd, ID_CLIPBOARD), nav && !ui_screen_fullscreen_active(hwnd) ? SW_SHOW : SW_HIDE);
        call_controls(hwnd, incoming, nav);
        if (!IsIconic(hwnd))
        {
            RECT dirty;
            GetClientRect(hwnd, &dirty);
            if (!ui_screen_fullscreen_active(hwnd) && dirty.bottom > 60)
            {
                dirty.bottom -= 60;
            }
            InvalidateRect(hwnd, &dirty, FALSE);
        }
        return 0;
    }
    if (msg == WM_DRAWITEM)
    {
        ui_draw_button((DRAWITEMSTRUCT *)lp);
        return TRUE;
    }
    if (msg == WM_COMMAND)
    {
        switch (LOWORD(wp))
        {
        case ID_CLIPBOARD:
            if (clipboard_controller())
            {
                HMENU menu = CreatePopupMenu();
                POINT pt;
                UINT chosen;
                unsigned char packet[3];
                const WCHAR *labels[4] = {L"Disabled", L"Controller to remote", L"Remote to controller", L"Both directions"};
                int k;
                for (k = 0; k < 4; k++) AppendMenuW(menu, MF_STRING | (clipboard_mode == k ? MF_CHECKED : 0), k + 1, labels[k]);
                GetCursorPos(&pt);
                chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(menu);
                if (chosen)
                {
                    clipboard_mode = chosen - 1;
                    packet[0] = 3; packet[1] = 17; packet[2] = (unsigned char)clipboard_mode;
                    screen_send_clipboard(packet, 3);
                    clipboard_send(hwnd);
                }
            }
            break;
        case ID_ANSWER:
        {
            int minimize = 0;
            EnterCriticalSection(&app.lock);
            if (app.minimize_screen_share && app.call && !app.call->outgoing &&
                !strcmp(app.call->kind, "screen"))
            {
                minimize = 1;
            }
            LeaveCriticalSection(&app.lock);
            if (minimize)
            {
                ShowWindow(hwnd, SW_MINIMIZE);
                if (IsWindowVisible(app.window))
                {
                    ShowWindow(app.window, SW_MINIMIZE);
                }
            }
            call_answer();
        }
            break;
        case ID_END:
            call_hangup();
            break;
        case ID_ROTATE:
            if (app.call && !strcmp(app.call->kind, "screen"))
            {
                screen_rot = (screen_rot + 90) % 360;
            }
            else
            {
                app.rx_rotate = (app.rx_rotate + 90) % 360;
                settings_save();
            }
            scr_x = 0;
            scr_y = 0;
            break;
        case ID_VIEW:
            view_mode = view_mode ? 0 : 1;
            scr_x = 0;
            scr_y = 0;
            break;
        case ID_FULLSCREEN:
            if (app.call && app.call->outgoing && !strcmp(app.call->kind, "screen") &&
                app.call->media_started)
            {
                if (!ui_screen_fullscreen_enter(hwnd))
                {
                    notice("Full-screen keyboard capture unavailable");
                }
            }
            scr_x = 0;
            scr_y = 0;
            break;
        case ID_MUTE:
            EnterCriticalSection(&app.lock);
            if (app.call)
            {
                app.call->mute = !app.call->mute;
            }
            LeaveCriticalSection(&app.lock);
            break;
        case ID_SPEAKER:
            EnterCriticalSection(&app.lock);
            if (app.call)
            {
                app.call->speaker_mute = !app.call->speaker_mute;
            }
            LeaveCriticalSection(&app.lock);
            break;
        case ID_BACK:
            screen_send_key(1);
            break;
        case ID_HOME:
            screen_send_key(2);
            break;
        case ID_RECENTS:
            screen_send_key(3);
            break;
        }
        return 0;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE && app.call &&
        app.call->outgoing && !strcmp(app.call->kind, "screen"))
    {
        /* Buttons can otherwise retain keyboard focus when returning to the viewer. */
        SetFocus(hwnd);
    }
    if (msg == WM_GETDLGCODE)
    {
        return DLGC_WANTALLKEYS;
    }
    if (msg == WM_CHAR || msg == UI_FULLSCREEN_TEXT)
    {
        static WCHAR high_surrogate;
        WCHAR ch = (WCHAR)wp, pair[2];
        if (app.call && app.call->outgoing && !strcmp(app.call->kind, "screen") &&
            app.call->media_started && screen_remote_text_supported())
        {
            if (ch >= 0xd800 && ch <= 0xdbff) high_surrogate = ch;
            else if (ch >= 0xdc00 && ch <= 0xdfff && high_surrogate)
            {
                pair[0] = high_surrogate; pair[1] = ch;
                screen_send_text(pair, 2); high_surrogate = 0;
            }
            else
            {
                high_surrogate = 0;
                if (ch >= 32 && ch != 127) screen_send_text(&ch, 1);
            }
        }
        return 0;
    }
    if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
    {
        if (app.call && app.call->outgoing && !strcmp(app.call->kind, "screen") &&
            app.call->media_started)
        {
            if (!screen_remote_text_supported() || !ui_screen_text_key((int)wp) ||
                ((GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)))
            {
                if (!screen_send_keycode((int)wp, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN))
                    log_event("REMOTE_KEY", "send failed");
            }
            return 0;
        }
    }
    if (msg == UI_FULLSCREEN_KEY)
    {
        if (ui_screen_fullscreen_active(hwnd) && app.call && app.call->outgoing &&
            !strcmp(app.call->kind, "screen") && app.call->media_started)
        {
            if (!(lp & 2)) screen_send_keycode((int)wp, (lp & 1) ? 1 : 0);
        }
        return 0;
    }
    if (msg == WM_LBUTTONDOWN)
    {
        if (ui_screen_fullscreen_click(hwnd, (int)LOWORD(lp), (int)HIWORD(lp)))
        {
            scr_x = 0;
            scr_y = 0;
            /* The fixed frame timer deliberately excludes the bottom control
               strip to prevent flicker. After leaving full screen, reveal the
               controls immediately and perform one complete repaint so the
               former full-screen frame cannot remain behind them. */
            call_controls(hwnd, 0, 1);
            RedrawWindow(hwnd, NULL, NULL,
                         RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        }
        if (app.call && app.call->outgoing && !strcmp(app.call->kind, "screen") &&
            app.call->media_started)
        {
            float nx = 0, ny = 0;
            down_px = (int)LOWORD(lp);
            down_py = (int)HIWORD(lp);
            dragging = 0;
            down_time = GetTickCount64();
            SetCapture(hwnd);
            if (video_norm(down_px, down_py, &nx, &ny))
            {
                SetFocus(hwnd);
                screen_send_mouse(5, nx, ny);
            }
            remote_button = 1;
        }
        return 0;
    }
    if (msg == WM_RBUTTONDOWN)
    {
        if (app.call && app.call->outgoing && !strcmp(app.call->kind, "screen") &&
            app.call->media_started)
        {
            float nx = 0, ny = 0;
            int mx = (int)LOWORD(lp), my = (int)HIWORD(lp);
            SetCapture(hwnd);
            if (video_norm(mx, my, &nx, &ny))
            {
                SetFocus(hwnd);
                screen_send_mouse(8, nx, ny);
            }
            remote_button = 2;
        }
        return 0;
    }
    if (msg == WM_RBUTTONUP)
    {
        if (remote_button == 2)
        {
            float nx = 0, ny = 0;
            int mx = (int)LOWORD(lp), my = (int)HIWORD(lp);
            if (video_norm(mx, my, &nx, &ny))
            {
                screen_send_mouse(9, nx, ny);
            }
            remote_button = 0;
            ReleaseCapture();
        }
        return 0;
    }
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
    {
        if (app.call && app.call->outgoing && !strcmp(app.call->kind, "screen") &&
            app.call->media_started)
        {
            POINT pt;
            float nx = 0, ny = 0;
            int delta = (short)HIWORD(wp);
            pt.x = (short)LOWORD(lp);
            pt.y = (short)HIWORD(lp);
            ScreenToClient(hwnd, &pt);
            if (video_norm(pt.x, pt.y, &nx, &ny))
            {
                screen_send_wheel(nx, ny, delta, msg == WM_MOUSEHWHEEL ? 1 : 0);
            }
        }
        return 0;
    }
    if (msg == WM_MOUSEMOVE)
    {
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        ui_screen_fullscreen_pointer(hwnd, mx, my);
        if (down_px >= 0)
        {
            if (abs(mx - down_px) > 6 || abs(my - down_py) > 6)
            {
                dragging = 1;
            }
        }
        if (app.call && app.call->outgoing && !strcmp(app.call->kind, "screen") &&
            app.call->media_started)
        {
            float nx = 0, ny = 0;
            if (video_norm(mx, my, &nx, &ny))
            {
                screen_send_mouse(6, nx, ny);
            }
        }
        return 0;
    }
    if (msg == WM_LBUTTONUP)
    {
        int ok = 0;
        if (down_px >= 0)
        {
            int mx = (int)LOWORD(lp), my = (int)HIWORD(lp);
            if (remote_button == 1)
            {
                float nx = 0, ny = 0;
                if (video_norm(mx, my, &nx, &ny))
                {
                    screen_send_mouse(7, nx, ny);
                }
                remote_button = 0;
            }
            ReleaseCapture();
            down_px = -1;
            dragging = 0;
            ok = 1;
        }
        (void)ok;
        return 0;
    }
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mem;
        HBITMAP bmp;
        HGDIOBJ old;
        RECT r, video, preview;
        HBRUSH bg;
        WCHAR text[400], name[256];
        const WCHAR *status = L"Call ended";
        struct peer p;
        GetClientRect(hwnd, &r);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, r.right ? r.right : 1, r.bottom ? r.bottom : 1);
        old = SelectObject(mem, bmp);
        bg = CreateSolidBrush(RGB(20, 30, 34));
        FillRect(mem, &r, bg);
        DeleteObject(bg);
        EnterCriticalSection(&app.lock);
        name[0] = 0;
        if (app.call)
        {
            if (peer_copy(app.call->peer, &p))
            {
                wide(p.name, name, 256);
            }
            status = app.call->media_started ? L"Connected - TLS / H.264"
                     : app.call->accepted    ? L"Connecting..."
                     : app.call->outgoing    ? L"Calling..."
                                             : L"Incoming call";
        }
        _snwprintf(text, 400, L"%s   %s%s", name, status, view_mode ? L"   [1:1]" : L"   [Fit]");
        SelectObject(mem, ui_small);
        SetTextColor(mem, RGB(220, 235, 230));
        SetBkMode(mem, TRANSPARENT);
        TextOutW(mem, 16, 12, text, (int)wcslen(text));
        video_area(hwnd, &video);
        fitted(mem, 0, video,
               app.call && !strcmp(app.call->kind, "screen") ? screen_rot : app.rx_rotate);
        if (!ui_screen_fullscreen_active(hwnd))
        {
            SetRect(&preview, r.right - 174, 46, r.right - 14, 166);
            fitted(mem, 1, preview, 0);
        }
        LeaveCriticalSection(&app.lock);
        ui_screen_fullscreen_paint(hwnd, mem);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_CLOSE)
    {
        call_hangup();
        return 0;
    }
    if (msg == WM_DESTROY)
    {
        InterlockedExchange(&remote_cursor, 1);
        ui_screen_fullscreen_destroy(hwnd);
        KillTimer(hwnd, 1);
        RemoveClipboardFormatListener(hwnd);
        clipboard_mode = 0;
        ui_call = NULL;
        controls_incoming = controls_nav = controls_fullscreen = -1;
        return 0;
    }
    if (msg == WM_ERASEBKGND)
    {
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
void ui_call_register(void)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.hInstance = app.instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(app.instance, MAKEINTRESOURCEW(1));
    wc.lpfnWndProc = call_proc;
    wc.lpszClassName = L"AltekCall";
    RegisterClassW(&wc);
}
void ui_call_update(void)
{
    int active, incoming;
    EnterCriticalSection(&app.lock);
    active = app.call != NULL;
    incoming = active && !app.call->outgoing && !app.call->accepted;
    LeaveCriticalSection(&app.lock);
    if (active && !ui_call)
    {
        int i;
        EnterCriticalSection(&app.lock);
        for (i = 0; i < 2; i++)
        {
            free(frames[i]);
            frames[i] = NULL;
            widths[i] = heights[i] = 0;
        }
        LeaveCriticalSection(&app.lock);
        ui_call = CreateWindowExW(WS_EX_APPWINDOW, L"AltekCall", L"ALTEK Link - Call",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                  CW_USEDEFAULT, 760, 610, NULL, NULL, app.instance, NULL);
        if (ui_call && incoming && SetForegroundWindow(ui_call))
        {
            SetFocus(ui_call);
        }
        else if (ui_call && incoming)
        {
            FLASHWINFO flash;
            /* Respect the foreground lock while still making the incoming call visible. */
            SetWindowPos(ui_call, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetWindowPos(ui_call, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            memset(&flash, 0, sizeof(flash));
            flash.cbSize = sizeof(flash);
            flash.hwnd = ui_call;
            flash.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
            FlashWindowEx(&flash);
        }
    }
    if (!active && ui_call)
    {
        PlaySoundW(NULL, NULL, 0);
        DestroyWindow(ui_call);
    }
}
