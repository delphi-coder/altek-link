#include "ui.h"

/*
 * Full-screen presentation and input capture for an outgoing screen-sharing
 * session. Keep visual effects, the reveal corner and keyboard redirection in
 * this module so the ordinary call window remains independent of them.
 */
static HWND full_window;
static HHOOK keyboard_hook;
static WINDOWPLACEMENT restore_placement;
static LONG restore_style, restore_ex_style;
static int full_active, exit_visible;

static void exit_rect(HWND hwnd, RECT *out)
{
    RECT r;
    GetClientRect(hwnd, &r);
    SetRect(out, r.right - 190, 12, r.right - 12, 56);
}

static LRESULT CALLBACK keyboard_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && full_active && full_window)
    {
        KBDLLHOOKSTRUCT *key = (KBDLLHOOKSTRUCT *)lp;
        int down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        int up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        if (down || up)
        {
            PostMessageW(full_window, UI_FULLSCREEN_KEY, (WPARAM)key->vkCode,
                         (LPARAM)(down ? 1 : 0));
            return 1;
        }
    }
    return CallNextHookEx(keyboard_hook, code, wp, lp);
}

int ui_screen_fullscreen_active(HWND hwnd)
{
    return full_active && full_window == hwnd;
}

int ui_screen_fullscreen_enter(HWND hwnd)
{
    MONITORINFO monitor;
    LONG style;
    if (full_active)
    {
        return full_window == hwnd;
    }
    full_window = hwnd;
    keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, app.instance, 0);
    if (!keyboard_hook)
    {
        full_window = NULL;
        return 0;
    }
    memset(&restore_placement, 0, sizeof(restore_placement));
    restore_placement.length = sizeof(restore_placement);
    GetWindowPlacement(hwnd, &restore_placement);
    restore_style = GetWindowLongW(hwnd, GWL_STYLE);
    restore_ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
    {
        UnhookWindowsHookEx(keyboard_hook);
        keyboard_hook = NULL;
        full_window = NULL;
        return 0;
    }
    full_active = 1;
    exit_visible = 0;
    style = WS_POPUP | WS_VISIBLE;
    SetWindowLongW(hwnd, GWL_STYLE, style);
    SetWindowLongW(hwnd, GWL_EXSTYLE,
                   restore_ex_style & ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME |
                                        WS_EX_STATICEDGE));
    SetWindowPos(hwnd, HWND_TOPMOST, monitor.rcMonitor.left, monitor.rcMonitor.top,
                 monitor.rcMonitor.right - monitor.rcMonitor.left,
                 monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    return 1;
}

void ui_screen_fullscreen_leave(HWND hwnd)
{
    if (!ui_screen_fullscreen_active(hwnd))
    {
        return;
    }
    full_active = 0;
    exit_visible = 0;
    if (keyboard_hook)
    {
        UnhookWindowsHookEx(keyboard_hook);
        keyboard_hook = NULL;
    }
    SetWindowLongW(hwnd, GWL_STYLE, restore_style);
    SetWindowLongW(hwnd, GWL_EXSTYLE, restore_ex_style);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetWindowPlacement(hwnd, &restore_placement);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    full_window = NULL;
}

void ui_screen_fullscreen_destroy(HWND hwnd)
{
    if (!ui_screen_fullscreen_active(hwnd))
    {
        return;
    }
    full_active = 0;
    exit_visible = 0;
    if (keyboard_hook)
    {
        UnhookWindowsHookEx(keyboard_hook);
        keyboard_hook = NULL;
    }
    full_window = NULL;
}

void ui_screen_fullscreen_pointer(HWND hwnd, int x, int y)
{
    RECT r, button, keep;
    POINT point;
    int visible;
    if (!ui_screen_fullscreen_active(hwnd))
    {
        return;
    }
    GetClientRect(hwnd, &r);
    exit_rect(hwnd, &button);
    keep = button;
    InflateRect(&keep, 16, 16);
    point.x = x;
    point.y = y;
    visible = (x >= r.right - 24 && y >= 0 && y < 24) ||
              (exit_visible && PtInRect(&keep, point));
    if (visible != exit_visible)
    {
        exit_visible = visible;
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

int ui_screen_fullscreen_click(HWND hwnd, int x, int y)
{
    RECT button;
    POINT point;
    if (!ui_screen_fullscreen_active(hwnd) || !exit_visible)
    {
        return 0;
    }
    exit_rect(hwnd, &button);
    point.x = x;
    point.y = y;
    if (!PtInRect(&button, point))
    {
        return 0;
    }
    ui_screen_fullscreen_leave(hwnd);
    return 1;
}

void ui_screen_fullscreen_paint(HWND hwnd, HDC dc)
{
    RECT button;
    HBRUSH fill;
    HPEN border, old_pen;
    HGDIOBJ old_brush, old_font;
    const WCHAR label[] = L"Exit full screen";
    if (!ui_screen_fullscreen_active(hwnd) || !exit_visible)
    {
        return;
    }
    exit_rect(hwnd, &button);
    fill = CreateSolidBrush(RGB(184, 44, 54));
    border = CreatePen(PS_SOLID, 1, RGB(255, 210, 214));
    old_brush = SelectObject(dc, fill);
    old_pen = (HPEN)SelectObject(dc, border);
    RoundRect(dc, button.left, button.top, button.right, button.bottom, 12, 12);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(border);
    DeleteObject(fill);
    old_font = SelectObject(dc, ui_bold);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, label, -1, &button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font);
}
