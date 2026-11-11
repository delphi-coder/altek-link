#include "ui.h"
HFONT ui_font, ui_small, ui_bold;
char ui_peer[65];
HWND ui_chat, ui_input;
static HWND contacts, header;
static int order[MAX_PEERS], order_count;
static NOTIFYICONDATAW tray;
static WNDPROC input_original;
static HWND tooltip;
static HBRUSH bar_brush;
HWND ui_control(HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style, int id, int x, int y,
                int w, int h)
{
    HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                                 (HMENU)(INT_PTR)id, app.instance, NULL);
    if (child)
    {
        SendMessageW(child, WM_SETFONT, (WPARAM)ui_font, TRUE);
    }
    if (child && !wcscmp(cls, L"BUTTON") && (style & BS_OWNERDRAW) == BS_OWNERDRAW)
    {
        const WCHAR *tip = NULL;
        TOOLINFOW info;
        switch (id)
        {
        case ID_SEND:
            tip = L"Send (Enter)";
            break;
        case ID_ATTACH:
            tip = L"Attach file";
            break;
        case ID_EMOJI:
            tip = L"Emoji";
            break;
        case ID_AUDIO:
            tip = L"Audio call";
            break;
        case ID_VIDEO:
            tip = L"Video call";
            break;
        case ID_CAMERA:
            tip = L"View phone camera";
            break;
        case ID_SCREEN:
            tip = L"Screen share";
            break;
        case ID_SETTINGS:
            tip = L"Settings";
            break;
        case ID_MUTE:
            tip = L"Mute microphone";
            break;
        case ID_SPEAKER:
            tip = L"Mute playback";
            break;
        case ID_ROTATE:
            tip = L"Rotate received video";
            break;
        case ID_END:
            tip = L"End call";
            break;
        case ID_ANSWER:
            tip = L"Answer call";
            break;
        case ID_BACK:
            tip = L"Back (remote phone)";
            break;
        case ID_HOME:
            tip = L"Home (remote phone)";
            break;
        case ID_RECENTS:
            tip = L"Recents (remote phone)";
            break;
        }
        if (tip)
        {
            if (!tooltip)
            {
                tooltip =
                    CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP,
                                    0, 0, 0, 0, app.window, NULL, app.instance, NULL);
            }
            memset(&info, 0, sizeof(info));
            info.cbSize = sizeof(info);
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            info.hwnd = parent;
            info.uId = (UINT_PTR)child;
            info.lpszText = (LPWSTR)tip;
            SendMessageW(tooltip, TTM_ADDTOOLW, 0, (LPARAM)&info);
        }
    }
    return child;
}
static int online(struct peer *p)
{
    return !p->blocked && p->seen && GetTickCount64() - p->seen < 16000;
}
static void refresh_contacts(void)
{
    int i, j, n, sel = -1;
    LRESULT top = SendMessageW(contacts, LB_GETTOPINDEX, 0, 0);
    WCHAR name[256];
    static char shown_peer[65], shown_avatar[65];
    const char *avatar = "";
    unsigned __int64 sig = 1469598103934665603ULL;
    EnterCriticalSection(&app.lock);
    order_count = app.peer_count;
    for (i = 0; i < order_count; i++)
    {
        order[i] = i;
    }
    for (i = 1; i < order_count; i++)
    {
        n = order[i];
        j = i - 1;
        while (j >= 0 && (online(&app.peers[n]) > online(&app.peers[order[j]]) ||
                          (online(&app.peers[n]) == online(&app.peers[order[j]]) &&
                           strcmp(app.peers[n].name, app.peers[order[j]].name) < 0)))
        {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = n;
    }
    /* Only rebuild the listbox when something a user would see actually
     * changed (peer count, online status, names). Doing this on every packet
     * invalidated and repainted the whole list -> visible flicker. */
    for (i = 0; i < order_count; i++)
    {
        const char *p;
        struct peer *pp = &app.peers[order[i]];
        for (p = pp->id; *p; p++)
        {
            sig = (sig ^ (unsigned char)*p) * 1099511628211ULL;
        }
        sig = (sig ^ (unsigned char)online(pp)) * 1099511628211ULL;
        for (p = pp->name; *p; p++)
        {
            sig = (sig ^ (unsigned char)*p) * 1099511628211ULL;
        }
    }
    {
        const char *p;
        for (p = ui_peer; *p; p++)
        {
            sig = (sig ^ (unsigned char)*p) * 1099511628211ULL;
        }
    }
    {
        static unsigned __int64 last_sig = 0;
        if (sig == last_sig)
        {
            LeaveCriticalSection(&app.lock);
            return;
        }
        last_sig = sig;
    }
    SendMessageW(contacts, WM_SETREDRAW, FALSE, 0);
    SendMessageW(contacts, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < order_count; i++)
    {
        SendMessageW(contacts, LB_ADDSTRING, 0, (LPARAM)L"");
        if (!strcmp(ui_peer, app.peers[order[i]].id))
        {
            sel = i;
        }
    }
    SendMessageW(contacts, LB_SETCURSEL, sel, 0);
    if (top >= 0)
    {
        SendMessageW(contacts, LB_SETTOPINDEX, top, 0);
    }
    SendMessageW(contacts, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(contacts, NULL, TRUE);
    if (sel >= 0)
    {
        wide(app.peers[order[sel]].name, name, 256);
        SetWindowTextW(header, name);
        avatar = app.peers[order[sel]].avatar;
    }
    else
    {
        SetWindowTextW(header, L"ALTEK Link");
    }
    /* The header photo is an owner-drawn button: it only repaints when told to. */
    if (strcmp(shown_peer, ui_peer) || strcmp(shown_avatar, avatar))
    {
        copy_s(shown_peer, 65, ui_peer);
        copy_s(shown_avatar, 65, avatar);
        InvalidateRect(GetDlgItem(app.window, ID_HEADER_PHOTO), NULL, TRUE);
    }
    LeaveCriticalSection(&app.lock);
}
static void do_send(void)
{
    WCHAR w[8002];
    char s[32768];
    if (!ui_peer[0])
    {
        return;
    }
    GetWindowTextW(ui_input, w, 8002);
    utf8(w, s, sizeof(s));
    if (!s[0])
    {
        return;
    }
    send_text(ui_peer, s);
    SetWindowTextW(ui_input, L"");
    SetFocus(ui_input);
}
static void input_menu(HWND hwnd, LPARAM lp)
{
    HMENU menu = CreatePopupMenu();
    POINT p;
    DWORD from = 0, to = 0;
    int has_sel, can_paste, choice;
    if ((int)(short)LOWORD(lp) == -1 && (int)(short)HIWORD(lp) == -1)
    {
        /* Opened with the keyboard (Menu key / Shift+F10): place it at the text caret. */
        p.x = 12;
        p.y = 12;
        GetCaretPos(&p);
        ClientToScreen(hwnd, &p);
    }
    else
    {
        p.x = (short)LOWORD(lp);
        p.y = (short)HIWORD(lp);
    }
    SendMessageW(hwnd, EM_GETSEL, (WPARAM)&from, (LPARAM)&to);
    has_sel = to > from;
    can_paste = IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
    SetFocus(hwnd);
    AppendMenuW(menu, MF_STRING | (SendMessageW(hwnd, EM_CANUNDO, 0, 0) ? 0 : MF_GRAYED), 1,
                L"Undo\tCtrl+Z");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (has_sel ? 0 : MF_GRAYED), 2, L"Cut\tCtrl+X");
    AppendMenuW(menu, MF_STRING | (has_sel ? 0 : MF_GRAYED), 3, L"Copy\tCtrl+C");
    AppendMenuW(menu, MF_STRING | (can_paste ? 0 : MF_GRAYED), 4, L"Paste\tCtrl+V");
    AppendMenuW(menu, MF_STRING | (has_sel ? 0 : MF_GRAYED), 5, L"Delete\tDel");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (GetWindowTextLengthW(hwnd) > 0 ? 0 : MF_GRAYED), 6,
                L"Select all\tCtrl+A");
    choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    switch (choice)
    {
    case 1:
        SendMessageW(hwnd, EM_UNDO, 0, 0);
        break;
    case 2:
        SendMessageW(hwnd, WM_CUT, 0, 0);
        break;
    case 3:
        SendMessageW(hwnd, WM_COPY, 0, 0);
        break;
    case 4:
        /* Plain text only: never paste images or formatting into a chat message. */
        SendMessageW(hwnd, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
        break;
    case 5:
        SendMessageW(hwnd, WM_CLEAR, 0, 0);
        break;
    case 6:
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        break;
    }
}
static LRESULT CALLBACK input_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_RBUTTONUP)
    {
        /* Rich edit swallows the right click without raising WM_CONTEXTMENU, so handle it here. */
        POINT pt;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        ClientToScreen(hwnd, &pt);
        input_menu(hwnd, MAKELPARAM((WORD)pt.x, (WORD)pt.y));
        return 0;
    }
    if (msg == WM_CONTEXTMENU)
    {
        input_menu(hwnd, lp);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000))
    {
        do_send();
        return 0;
    }
    if (msg == WM_CHAR && wp == '\r' && !(GetKeyState(VK_SHIFT) & 0x8000))
    {
        return 0;
    }
    return CallWindowProcW(input_original, hwnd, msg, wp, lp);
}
static void choose_file(void)
{
    OPENFILENAMEW of;
    WCHAR path[MAX_PATH] = L"";
    if (!ui_peer[0])
    {
        return;
    }
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = app.window;
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.lpstrFilter = L"All files\0*.*\0\0";
    of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&of))
    {
        send_file(ui_peer, path);
    }
}
extern unsigned char *emoji[10];
extern int emoji_w[10], emoji_h[10];
/* Emoji in the popup menu: one master table. Edit glyph/name (and the matching
 * artwork in resources "#101".."#110") to add or change any emoji. */
static const WCHAR *emoji_glyphs[10] = {L"\xd83d\xde00", L"\xd83d\xde02",
                                        L"\xd83d\xde42", L"\xd83d\xde0d",
                                        L"\xd83d\xdc4d", L"\xd83d\xdc4b",
                                        L"\x2764",       L"\xd83c\xdf89",
                                        L"\xd83d\xde4f", L"\xd83d\xde22"};
static const WCHAR *emoji_names[10] = {L"Grin", L"Joy", L"Smile", L"Love", L"Thumbs up",
                                       L"Wave", L"Heart", L"Celebrate", L"Thanks", L"Cry"};
static void emoji_menu_draw(DRAWITEMSTRUCT *d)
{
    int i = (int)(UINT_PTR)d->itemData, t;
    RECT r = d->rcItem, ir;
    HBRUSH bg;
    WCHAR name[128];
    if (i < 0 || i >= 10)
    {
        return;
    }
    bg = CreateSolidBrush(d->itemState & ODS_SELECTED ? GetSysColor(COLOR_HIGHLIGHT)
                                                      : GetSysColor(COLOR_MENU));
    FillRect(d->hDC, &r, bg);
    DeleteObject(bg);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC,
                 d->itemState & ODS_SELECTED ? RGB(255, 255, 255) : GetSysColor(COLOR_MENUTEXT));
    ir = r;
    ir.left += 6;
    ir.right = ir.left + 26;
    t = (r.bottom - r.top - 26) / 2;
    ir.top += t;
    ir.bottom = ir.top + 26;
    if (emoji[i])
    {
        ui_draw_image(d->hDC, emoji[i], emoji_w[i], emoji_h[i], ir, 1);
    }
    else
    {
        TextOutW(d->hDC, ir.left, ir.top, emoji_glyphs[i], (int)wcslen(emoji_glyphs[i]));
    }
    wcsncpy(name, emoji_names[i], 127);
    name[127] = 0;
    TextOutW(d->hDC, r.left + 40, r.top + (r.bottom - r.top) / 2 - 8, name, (int)wcslen(name));
}
static void emoji_menu(void)
{
    HMENU m = CreatePopupMenu();
    RECT r;
    int i, n;
    WCHAR e[3];
    for (i = 0; i < 10; i++)
    {
        /* MF_OWNERDRAW so each item paints the actual emoji artwork instead of
         * a raw unicode codepoint (which a GDI menu cannot render correctly). */
        AppendMenuW(m, MF_OWNERDRAW, i + 1, (LPCTSTR)(UINT_PTR)i);
    }
    GetWindowRect(GetDlgItem(app.window, ID_EMOJI), &r);
    n = TrackPopupMenu(m, TPM_RETURNCMD | TPM_BOTTOMALIGN, r.left, r.top, 0, app.window, NULL);
    DestroyMenu(m);
    if (n > 0 && n <= 10)
    {
        const WCHAR *g = emoji_glyphs[n - 1];
        e[0] = g[0];
        e[1] = (e[0] >= 0xd800 && e[0] <= 0xdbff) ? g[1] : 0;
        e[2] = 0;
        SendMessageW(ui_input, EM_REPLACESEL, TRUE, (LPARAM)e);
        SetFocus(ui_input);
    }
}
static const struct theme themes[2] = {
    /* 0: green (original look) */
    {RGB(246, 250, 248), RGB(216, 227, 223), RGB(150, 184, 176), RGB(240, 245, 244),
     RGB(212, 241, 226), RGB(215, 237, 229), RGB(26, 108, 98), RGB(16, 82, 74), RGB(34, 126, 114),
     RGB(212, 230, 224), RGB(232, 242, 239), RGB(216, 234, 228), RGB(190, 218, 209),
     RGB(20, 90, 82), RGB(214, 232, 229), RGB(120, 190, 176), RGB(20, 70, 64), RGB(15, 120, 95)},
    /* 1: petrol blue */
    {RGB(244, 248, 250), RGB(214, 224, 230), RGB(140, 172, 190), RGB(238, 243, 246),
     RGB(205, 226, 236), RGB(208, 226, 236), RGB(18, 94, 122), RGB(10, 66, 88), RGB(30, 114, 144),
     RGB(208, 224, 232), RGB(228, 238, 243), RGB(212, 228, 236), RGB(186, 208, 220),
     RGB(14, 78, 102), RGB(212, 226, 234), RGB(116, 168, 190), RGB(12, 62, 82), RGB(14, 110, 140)}};
const struct theme *ui_theme(void)
{
    int t = app.theme;
    return &themes[t >= 0 && t < 2 ? t : 0];
}
/* Call after app.theme changes: rebuilds cached brushes and repaints everything. */
void ui_theme_apply(void)
{
    if (bar_brush)
    {
        DeleteObject(bar_brush);
    }
    bar_brush = CreateSolidBrush(ui_theme()->bar);
    if (app.window)
    {
        RedrawWindow(app.window, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}
static int button_icon(int id)
{
    switch (id)
    {
    case ID_AUDIO:
    case ID_ANSWER:
        return ICON_PHONE;
    case ID_END:
        return ICON_HANGUP;
    case ID_VIDEO:
        return ICON_VIDEO;
    case ID_CAMERA:
        return ICON_CAMERA;
    case ID_SCREEN:
        return ICON_SCREEN;
    case ID_ATTACH:
        return ICON_ATTACH;
    case ID_EMOJI:
        return ICON_EMOJI;
    case ID_SEND:
        return ICON_SEND;
    case ID_MUTE:
        return ICON_MIC;
    case ID_SPEAKER:
        return ICON_SPEAKER;
    case ID_ROTATE:
        return ICON_ROTATE;
    case ID_BACK:
        return ICON_BACK;
    case ID_HOME:
        return ICON_HOME;
    case ID_RECENTS:
        return ICON_RECENTS;
    case ID_VIEW:
        return ICON_FIT;
    case ID_FULLSCREEN:
        return ICON_FULL;
    case ID_SETTINGS:
        return ICON_SETTINGS;
    }
    return ICON_DISC;
}
void ui_draw_button(DRAWITEMSTRUCT *d)
{
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    int id = d->CtlID, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    int pressed = (d->itemState & ODS_SELECTED) != 0;
    int active = 0, in_call, disc, icon = button_icon(id);
    COLORREF page, tile, ink;
    HBRUSH bg;
    EnterCriticalSection(&app.lock);
    if (app.call)
    {
        if (id == ID_MUTE)
        {
            active = app.call->mute;
        }
        if (id == ID_SPEAKER)
        {
            active = app.call->speaker_mute;
        }
    }
    LeaveCriticalSection(&app.lock);
    in_call = id == ID_ANSWER || id == ID_END || id == ID_MUTE || id == ID_SPEAKER ||
              id == ID_ROTATE || id == ID_BACK || id == ID_HOME || id == ID_RECENTS ||
              id == ID_VIEW || id == ID_FULLSCREEN;
    page = in_call ? RGB(20, 30, 34) : UI_BAR;
    bg = CreateSolidBrush(page);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    if (in_call)
    {
        /* Round buttons on the dark call window. */
        disc = 40;
        tile = RGB(52, 68, 74);
        ink = RGB(236, 244, 241);
        if (id == ID_END)
        {
            tile = pressed ? RGB(168, 40, 50) : RGB(214, 62, 72);
            ink = RGB(255, 255, 255);
        }
        else if (id == ID_ANSWER)
        {
            tile = pressed ? RGB(28, 134, 76) : RGB(38, 166, 96);
            ink = RGB(255, 255, 255);
        }
        else if (active)
        {
            tile = RGB(236, 244, 241);
            ink = RGB(203, 57, 67);
        }
        else if (pressed)
        {
            tile = RGB(78, 98, 106);
        }
        ui_icon(dc, ICON_DISC, cx, cy, disc, tile, 0);
        ui_icon(dc, icon, cx, cy, 24, ink, active);
        return;
    }
    if (id == ID_SEND)
    {
        ui_icon(dc, ICON_DISC, cx, cy, 38, pressed ? ui_theme()->accent_dark : ui_theme()->accent, 0);
        ui_icon(dc, icon, cx + 1, cy, 22, RGB(255, 255, 255), 0);
        return;
    }
    if (pressed)
    {
        ui_icon(dc, ICON_DISC, cx, cy, 38, ui_theme()->press_disc, 0);
    }
    ui_icon(dc, icon, cx, cy, 26, ui_theme()->accent, 0);
}
static void layout(HWND hwnd)
{
    RECT r;
    int left = 262, cw;
    GetClientRect(hwnd, &r);
    cw = r.right - left;
    MoveWindow(contacts, 0, 58, left, r.bottom - 58, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_SETTINGS), left - 50, 8, 40, 40, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_HEADER_PHOTO), left + 10, 8, 44, 44, TRUE);
    MoveWindow(header, left + 64, 12, cw - 266, 36, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_AUDIO), r.right - 192, 10, 42, 40, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_VIDEO), r.right - 144, 10, 42, 40, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_CAMERA), r.right - 96, 10, 42, 40, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_SCREEN), r.right - 48, 10, 42, 40, TRUE);
    MoveWindow(ui_chat, left, 60, cw, r.bottom - 160, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_ATTACH), left + 8, r.bottom - 71, 40, 42, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_EMOJI), left + 52, r.bottom - 71, 40, 42, TRUE);
    MoveWindow(ui_input, left + 106, r.bottom - 86, cw - 172, 72, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_SEND), r.right - 52, r.bottom - 71, 44, 42, TRUE);
}
static void tray_menu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    POINT p;
    int n;
    AppendMenuW(m, MF_STRING, 1, L"Open ALTEK Link");
    AppendMenuW(m, MF_STRING, 2, L"Settings");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"Exit completely");
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);
    n = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (n == 1)
    {
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    }
    if (n == 2)
    {
        ui_settings(hwnd);
    }
    if (n == 3)
    {
        DestroyWindow(hwnd);
    }
}
static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CREATE)
    {
        app.window = hwnd;
        contacts = ui_control(hwnd, L"LISTBOX", L"",
                              LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL,
                              ID_CONTACTS, 0, 0, 260, 400);
        SendMessageW(contacts, LB_SETITEMHEIGHT, 0, 78);
        ui_control(hwnd, L"STATIC", L"ALTEK Link", SS_LEFT, 200, 18, 17, 180, 30);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_SETTINGS, 0, 0, 40, 40);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_HEADER_PHOTO, 0, 0, 44, 44);
        header = ui_control(hwnd, L"STATIC", L"ALTEK Link", SS_NOTIFY, ID_AVATAR, 0, 0, 300, 40);
        SendMessageW(header, WM_SETFONT, (WPARAM)ui_bold, TRUE);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_AUDIO, 0, 0, 40, 40);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_VIDEO, 0, 0, 40, 40);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_CAMERA, 0, 0, 40, 40);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_SCREEN, 0, 0, 40, 40);
        ui_chat = ui_control(hwnd, L"AltekChat", L"", WS_VSCROLL, 201, 0, 0, 400, 400);
        ui_input = ui_control(hwnd, MSFTEDIT_CLASS, L"",
                              ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, ID_INPUT, 0,
                              0, 400, 80);
        SendMessageW(ui_input, EM_EXLIMITTEXT, 0, 8000);
        SendMessageW(ui_input, EM_SETBKGNDCOLOR, 0, (LPARAM)RGB(255, 255, 255));
        bar_brush = CreateSolidBrush(UI_BAR);
        input_original = (WNDPROC)SetWindowLongPtrW(ui_input, GWLP_WNDPROC, (LONG_PTR)input_proc);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_SEND, 0, 0, 40, 40);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_ATTACH, 0, 0, 40, 40);
        ui_control(hwnd, L"BUTTON", L"", BS_OWNERDRAW, ID_EMOJI, 0, 0, 40, 40);
        memset(&tray, 0, sizeof(tray));
        tray.cbSize = sizeof(tray);
        tray.hWnd = hwnd;
        tray.uID = 1;
        tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        tray.uCallbackMessage = UI_TRAY;
        tray.hIcon = LoadIconW(app.instance, MAKEINTRESOURCEW(1));
        wcscpy(tray.szTip, L"ALTEK Link - Receiving local messages");
        Shell_NotifyIconW(NIM_ADD, &tray);
        SetTimer(hwnd, 1, 2000, NULL);
        return 0;
    }
    if (msg == WM_ERASEBKGND)
    {
        HDC dc = (HDC)wp;
        RECT r, box, strip;
        HBRUSH white = CreateSolidBrush(RGB(255, 255, 255)), bar = CreateSolidBrush(UI_BAR);
        HPEN line = CreatePen(PS_SOLID, 1, UI_BAR_LINE), frame = CreatePen(PS_SOLID, 1, UI_FRAME_LINE);
        HGDIOBJ old_pen, old_brush;
        GetClientRect(hwnd, &r);
        FillRect(dc, &r, white);
        SetRect(&strip, 0, 0, r.right, 58);
        FillRect(dc, &strip, bar);
        SetRect(&strip, 262, r.bottom - 100, r.right, r.bottom);
        FillRect(dc, &strip, bar);
        old_pen = SelectObject(dc, line);
        MoveToEx(dc, 0, 58, NULL);
        LineTo(dc, r.right, 58);
        MoveToEx(dc, 262, r.bottom - 100, NULL);
        LineTo(dc, r.right, r.bottom - 100);
        MoveToEx(dc, 261, 58, NULL);
        LineTo(dc, 261, r.bottom);
        if (ui_input && GetWindowRect(ui_input, &box))
        {
            MapWindowPoints(NULL, hwnd, (POINT *)&box, 2);
            InflateRect(&box, 7, 7);
            SelectObject(dc, frame);
            old_brush = SelectObject(dc, white);
            RoundRect(dc, box.left, box.top, box.right, box.bottom, 14, 14);
            SelectObject(dc, old_brush);
        }
        SelectObject(dc, old_pen);
        DeleteObject(line);
        DeleteObject(frame);
        DeleteObject(white);
        DeleteObject(bar);
        return 1;
    }
    if (msg == WM_CTLCOLORSTATIC)
    {
        SetBkColor((HDC)wp, UI_BAR);
        SetTextColor((HDC)wp, RGB(23, 35, 32));
        return (LRESULT)bar_brush;
    }
    if (msg == WM_LBUTTONDOWN)
    {
        RECT box;
        POINT p;
        p.x = (short)LOWORD(lp);
        p.y = (short)HIWORD(lp);
        if (ui_input && GetWindowRect(ui_input, &box))
        {
            MapWindowPoints(NULL, hwnd, (POINT *)&box, 2);
            InflateRect(&box, 7, 7);
            if (PtInRect(&box, p))
            {
                SetFocus(ui_input);
                return 0;
            }
        }
    }
    if (msg == WM_GETMINMAXINFO)
    {
        MINMAXINFO *m = (MINMAXINFO *)lp;
        m->ptMinTrackSize.x = 760;
        m->ptMinTrackSize.y = 480;
        return 0;
    }
    if (msg == WM_SIZE)
    {
        layout(hwnd);
        return 0;
    }
    if (msg == UI_UPDATE || msg == WM_TIMER)
    {
        refresh_contacts();
        ui_chat_refresh();
        ui_call_update();
        return 0;
    }
    if (msg == UI_NOTICE)
    {
        char *s = (char *)lp;
        tray.uFlags = NIF_INFO;
        wide(s, tray.szInfo, countof(tray.szInfo));
        wcscpy(tray.szInfoTitle, L"ALTEK Link");
        tray.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &tray);
        free(s);
        return 0;
    }
    if (msg == UI_TRAY)
    {
        if (lp == WM_LBUTTONUP || lp == NIN_BALLOONUSERCLICK)
        {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        else if (lp == WM_RBUTTONUP)
        {
            tray_menu(hwnd);
        }
        return 0;
    }
    if (msg == WM_MEASUREITEM)
    {
        MEASUREITEMSTRUCT *m = (MEASUREITEMSTRUCT *)lp;
        if (m->CtlType == ODT_MENU)
        {
            m->itemHeight = 40;
            m->itemWidth = 190;
            return TRUE;
        }
    }
    if (msg == WM_DRAWITEM)
    {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
        if (d->CtlType == ODT_MENU)
        {
            emoji_menu_draw(d);
            return TRUE;
        }
        if (d->CtlID == ID_HEADER_PHOTO)
        {
            struct peer p;
            RECT photo = d->rcItem;
            if (peer_copy(ui_peer, &p))
            {
                ui_avatar_draw(d->hDC, p.avatar, photo);
            }
            else
            {
                ui_avatar_draw(d->hDC, "", photo);
            }
            return TRUE;
        }
        if (d->CtlID == ID_CONTACTS)
        {
            RECT r = d->rcItem, a, txt;
            HBRUSH bg;
            WCHAR name[256], last[400];
            struct peer *p;
            int i;
            if (d->itemID == (UINT)-1 || d->itemID >= (UINT)order_count)
            {
                return TRUE;
            }
            EnterCriticalSection(&app.lock);
            p = &app.peers[order[d->itemID]];
            bg = CreateSolidBrush(d->itemState & ODS_SELECTED ? ui_theme()->select_bg
                                                              : RGB(255, 255, 255));
            FillRect(d->hDC, &r, bg);
            DeleteObject(bg);
            a = r;
            a.left += 12;
            a.top += 13;
            a.right = a.left + 46;
            a.bottom = a.top + 46;
            ui_avatar_draw(d->hDC, p->avatar, a);
            wide(p->name, name, 256);
            SetBkMode(d->hDC, TRANSPARENT);
            SetTextColor(d->hDC, online(p) ? RGB(23, 35, 32) : RGB(139, 145, 143));
            SelectObject(d->hDC, ui_bold);
            txt = r;
            txt.left += 72;
            txt.top += 12;
            txt.right -= 8;
            txt.bottom = txt.top + 25;
            DrawTextW(d->hDC, name, -1, &txt, DT_SINGLELINE | DT_END_ELLIPSIS);
            wcscpy(last, p->blocked ? L"Blocked" : online(p) ? L"Online" : L"Offline");
            for (i = app.message_count - 1; i >= 0; i--)
            {
                if (!strcmp(app.messages[i]->peer, p->id))
                {
                    WCHAR full[16002];
                    wide(app.messages[i]->text, full, 16002);
                    wcsncpy(last, full, 399);
                    last[399] = 0;
                    break;
                }
            }
            txt.top += 30;
            txt.bottom = txt.top + 23;
            SelectObject(d->hDC, ui_small);
            SetTextColor(d->hDC, RGB(113, 128, 121));
            DrawTextW(d->hDC, last, -1, &txt, DT_SINGLELINE | DT_END_ELLIPSIS);
            if (p->unread > 0)
            {
                WCHAR badge[16];
                RECT badge_rect = r;
                badge_rect.left = r.right - 38;
                badge_rect.right -= 6;
                badge_rect.top += 45;
                badge_rect.bottom = badge_rect.top + 22;
                _snwprintf(badge, 16, L"%d", p->unread);
                SetTextColor(d->hDC, ui_theme()->badge);
                SelectObject(d->hDC, ui_bold);
                DrawTextW(d->hDC, badge, -1, &badge_rect, DT_CENTER | DT_SINGLELINE);
            }
            LeaveCriticalSection(&app.lock);
            return TRUE;
        }
        ui_draw_button(d);
        return TRUE;
    }
    if (msg == WM_COMMAND)
    {
        switch (LOWORD(wp))
        {
        case ID_CONTACTS:
            if (HIWORD(wp) == LBN_SELCHANGE)
            {
                int sel = (int)SendMessageW(contacts, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < order_count)
                {
                    EnterCriticalSection(&app.lock);
                    copy_s(ui_peer, 65, app.peers[order[sel]].id);
                    app.peers[order[sel]].unread = 0;
                    LeaveCriticalSection(&app.lock);
                    refresh_contacts();
                    ui_chat_refresh();
                    SetFocus(ui_input);
                }
            }
            break;
        case ID_SEND:
            do_send();
            break;
        case ID_ATTACH:
            choose_file();
            break;
        case ID_EMOJI:
            emoji_menu();
            break;
        case ID_SETTINGS:
            ui_settings(hwnd);
            break;
        case ID_AUDIO:
            if (ui_peer[0])
            {
                call_dial(ui_peer, "audio");
            }
            break;
        case ID_VIDEO:
            if (ui_peer[0])
            {
                call_dial(ui_peer, "video");
            }
            break;
        case ID_CAMERA:
            if (ui_peer[0])
            {
                call_dial(ui_peer, "view");
            }
            break;
        case ID_SCREEN:
            if (ui_peer[0])
            {
                call_dial(ui_peer, "screen");
            }
            break;
        case ID_HEADER_PHOTO:
        case ID_AVATAR:
        {
            struct peer p;
            if (peer_copy(ui_peer, &p))
            {
                ui_avatar(hwnd, p.avatar);
            }
        }
        break;
        }
        return 0;
    }
    if (msg == WM_CONTEXTMENU && (HWND)wp == contacts)
    {
        HMENU menu = CreatePopupMenu();
        int result;
        struct peer p;
        if (!peer_copy(ui_peer, &p))
        {
            return 0;
        }
        AppendMenuW(menu, MF_STRING, 1, p.blocked ? L"Unblock contact" : L"Block contact");
        AppendMenuW(menu, MF_STRING, 2, L"Show profile picture");
        AppendMenuW(menu, MF_STRING, 3, L"Delete contact");
        result = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, (short)LOWORD(lp),
                                (short)HIWORD(lp), 0, hwnd, NULL);
        DestroyMenu(menu);
        if (result == 1)
        {
            int i;
            EnterCriticalSection(&app.lock);
            for (i = 0; i < app.peer_count; i++)
            {
                if (!strcmp(ui_peer, app.peers[i].id))
                {
                    app.peers[i].blocked = !app.peers[i].blocked;
                }
            }
            peers_save();
            LeaveCriticalSection(&app.lock);
            changed();
        }
        if (result == 2)
        {
            ui_avatar(hwnd, p.avatar);
        }
        if (result == 3)
        {
            if (MessageBoxW(hwnd, L"Permanently remove this contact?", L"Delete contact",
                            MB_YESNO | MB_ICONWARNING) == IDYES)
            {
                int i;
                EnterCriticalSection(&app.lock);
                for (i = 0; i < app.peer_count; i++)
                {
                    if (!strcmp(ui_peer, app.peers[i].id))
                    {
                        memmove(&app.peers[i], &app.peers[i + 1],
                                (app.peer_count - i - 1) * sizeof(struct peer));
                        app.peer_count--;
                        break;
                    }
                }
                peers_save();
                if (!strcmp(ui_peer, p.id))
                {
                    ui_peer[0] = 0;
                }
                LeaveCriticalSection(&app.lock);
                refresh_contacts();
                ui_chat_refresh();
            }
        }
        return 0;
    }
    if (msg == WM_CLOSE)
    {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    if (msg == WM_DESTROY)
    {
        KillTimer(hwnd, 1);
        if (ui_call)
        {
            DestroyWindow(ui_call);
        }
        if (bar_brush)
        {
            DeleteObject(bar_brush);
            bar_brush = NULL;
        }
        Shell_NotifyIconW(NIM_DELETE, &tray);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
int ui_run(HINSTANCE instance, int show)
{
    WNDCLASSW wc;
    MSG msg;
    INITCOMMONCONTROLSEX cc;
    WCHAR title[128];
    int i;
    app.instance = instance;
    LoadLibraryW(L"Msftedit.dll");
    cc.dwSize = sizeof(cc);
    cc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&cc);
    ui_font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                          L"Segoe UI");
    ui_small = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
    ui_bold = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, 0, L"Segoe UI");
    ui_chat_register();
    ui_call_register();
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hInstance = instance;
    wc.lpfnWndProc = main_proc;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"AltekLink";
    RegisterClassW(&wc);
    wide("ALTEK Link " APP_VERSION_BUILD, title, countof(title));
    app.window =
        CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                        CW_USEDEFAULT,
                        CW_USEDEFAULT, 1020, 700, NULL, NULL, instance, NULL);
    if (!app.window)
    {
        return 1;
    }
    ShowWindow(app.window, show);
    UpdateWindow(app.window);
    if (!tls_init() || !network_start())
    {
        MessageBoxW(
            app.window,
            L"Could not start local receiver. See debug.log in Settings > Open data folder.",
            L"ALTEK Link", MB_ICONERROR);
    }
    else
    {
        log_event("START", "Windows client %s", APP_VERSION_BUILD);
    }
    while ((i = GetMessageW(&msg, NULL, 0, 0)) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    app.window = NULL;
    network_stop();
    if (!app.workers)
    {
        tls_cleanup();
    }
    return 0;
}
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR cmd, int show)
{
    WSADATA wsa;
    WCHAR *slash;
    HANDLE mutex;
    int result;
    (void)previous;
    (void)cmd;
    memset(&app, 0, sizeof(app));
    InitializeCriticalSection(&app.lock);
    app.running = 1;
    app.listen_socket = app.discovery_socket = INVALID_SOCKET;
    app.worker_event = CreateEventW(NULL, TRUE, TRUE, NULL);
    mutex = CreateMutexW(NULL, FALSE, L"Local\\ALTEK-Link-Windows");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND other = FindWindowW(L"AltekLink", NULL);
        if (other)
        {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        if (mutex)
        {
            CloseHandle(mutex);
        }
        return 0;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
    {
        return 1;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GetModuleFileNameW(NULL, app.exe_dir, MAX_PATH);
    slash = wcsrchr(app.exe_dir, L'\\');
    if (slash)
    {
        *slash = 0;
    }
    store_init();
    result = ui_run(instance, strstr(cmd, "--tray") ? SW_HIDE : show);
    CoUninitialize();
    WSACleanup();
    if (mutex)
    {
        CloseHandle(mutex);
    }
    return result;
}
