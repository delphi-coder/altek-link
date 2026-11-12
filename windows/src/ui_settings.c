#include "ui.h"
#include <shlobj.h>
static HWND settings_window;
static unsigned char *profile_pixels;
static int profile_w, profile_h;
static unsigned char *crop_pixels;
static int crop_w, crop_h, crop_x, crop_y, crop_size, crop_drag, drag_x, drag_y, start_x, start_y,
    crop_saved;
static HWND crop_window;
static RECT crop_area;
static void crop_bounds(void)
{
    if (crop_size < 16)
    {
        crop_size = 16;
    }
    if (crop_size > crop_w)
    {
        crop_size = crop_w;
    }
    if (crop_size > crop_h)
    {
        crop_size = crop_h;
    }
    if (crop_x < 0)
    {
        crop_x = 0;
    }
    if (crop_y < 0)
    {
        crop_y = 0;
    }
    if (crop_x + crop_size > crop_w)
    {
        crop_x = crop_w - crop_size;
    }
    if (crop_y + crop_size > crop_h)
    {
        crop_y = crop_h - crop_size;
    }
}
static int save_crop(void)
{
    WCHAR path[MAX_PATH];
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER bi;
    unsigned char *b;
    int x, y;
    int ok = 0;
    b = (unsigned char *)malloc(sizeof(fh) + sizeof(bi) + 256 * 256 * 4);
    if (!b)
    {
        return 0;
    }
    memset(&fh, 0, sizeof(fh));
    memset(&bi, 0, sizeof(bi));
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = fh.bfOffBits + 256 * 256 * 4;
    bi.biSize = sizeof(bi);
    bi.biWidth = 256;
    bi.biHeight = -256;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    memcpy(b, &fh, sizeof(fh));
    memcpy(b + sizeof(fh), &bi, sizeof(bi));
    for (y = 0; y < 256; y++)
    {
        for (x = 0; x < 256; x++)
        {
            memcpy(b + fh.bfOffBits + 4 * (y * 256 + x),
                   crop_pixels +
                       4 * ((crop_y + y * crop_size / 256) * crop_w + crop_x + x * crop_size / 256),
                   4);
        }
    }
    path_join(path, app.dir, L"crop-temp.bmp");
    if (atomic_write(path, b, fh.bfSize))
    {
        ok = profile_set(path);
    }
    DeleteFileW(path);
    free(b);
    return ok;
}
static LRESULT CALLBACK crop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CREATE)
    {
        ui_control(hwnd, L"BUTTON", L"Use photo", BS_PUSHBUTTON, 1, 180, 470, 120, 34);
        return 0;
    }
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        int w, h;
        HPEN pen;
        HGDIOBJ old, brush;
        SetRect(&r, 12, 12, 468, 430);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        w = 456;
        h = w * crop_h / crop_w;
        if (h > 418)
        {
            h = 418;
            w = h * crop_w / crop_h;
        }
        SetRect(&crop_area, 12 + (456 - w) / 2, 12 + (418 - h) / 2, 12 + (456 + w) / 2,
                12 + (418 + h) / 2);
        ui_draw_image(dc, crop_pixels, crop_w, crop_h, crop_area, 0);
        pen = CreatePen(PS_SOLID, 3, RGB(20, 230, 180));
        old = SelectObject(dc, pen);
        brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, crop_area.left + crop_x * w / crop_w, crop_area.top + crop_y * h / crop_h,
                  crop_area.left + (crop_x + crop_size) * w / crop_w,
                  crop_area.top + (crop_y + crop_size) * h / crop_h);
        SelectObject(dc, old);
        SelectObject(dc, brush);
        DeleteObject(pen);
        SelectObject(dc, ui_small);
        SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, 28, 441, L"Drag square. Mouse wheel changes size.", 37);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN)
    {
        crop_drag = 1;
        drag_x = (short)LOWORD(lp);
        drag_y = (short)HIWORD(lp);
        start_x = crop_x;
        start_y = crop_y;
        SetCapture(hwnd);
        return 0;
    }
    if (msg == WM_LBUTTONUP)
    {
        crop_drag = 0;
        ReleaseCapture();
        return 0;
    }
    if (msg == WM_MOUSEMOVE && crop_drag)
    {
        int w = crop_area.right - crop_area.left, h = crop_area.bottom - crop_area.top;
        if (w && h)
        {
            crop_x = start_x + ((short)LOWORD(lp) - drag_x) * crop_w / w;
            crop_y = start_y + ((short)HIWORD(lp) - drag_y) * crop_h / h;
            crop_bounds();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }
    if (msg == WM_MOUSEWHEEL)
    {
        int old = crop_size;
        crop_size += (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1) * (crop_size / 10 + 1);
        crop_x += (old - crop_size) / 2;
        crop_y += (old - crop_size) / 2;
        crop_bounds();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == 1)
    {
        crop_saved = save_crop();
        if (crop_saved)
        {
            DestroyWindow(hwnd);
        }
        else
        {
            MessageBoxW(hwnd, L"Could not save the profile photo.", L"ALTEK Link", MB_ICONERROR);
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
    {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY)
    {
        crop_window = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static void choose_avatar(HWND parent)
{
    OPENFILENAMEW of;
    WCHAR file[MAX_PATH] = L"";
    WNDCLASSW wc;
    MSG msg;
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = parent;
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrFilter = L"Images\0*.jpg;*.jpeg;*.png;*.bmp\0\0";
    of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of))
    {
        return;
    }
    if (!image_load(file, &crop_pixels, &crop_w, &crop_h))
    {
        MessageBoxW(parent, L"Could not open that image.", L"ALTEK Link", MB_ICONERROR);
        return;
    }
    crop_size = crop_w < crop_h ? crop_w : crop_h;
    crop_x = (crop_w - crop_size) / 2;
    crop_y = (crop_h - crop_size) / 2;
    crop_drag = crop_saved = 0;
    memset(&wc, 0, sizeof(wc));
    wc.hInstance = app.instance;
    wc.lpfnWndProc = crop_proc;
    wc.hCursor = LoadCursor(NULL, IDC_SIZEALL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AltekCrop";
    RegisterClassW(&wc);
    EnableWindow(parent, FALSE);
    crop_window =
        CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Crop profile photo",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT,
                        CW_USEDEFAULT, 496, 552, parent, NULL, app.instance, NULL);
    while (crop_window && GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    free(crop_pixels);
    crop_pixels = NULL;
}
static LRESULT CALLBACK avatar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (profile_pixels)
        {
            int size = (r.right < r.bottom ? r.right : r.bottom) - 48;
            RECT p;
            SetRect(&p, (r.right - size) / 2, (r.bottom - size) / 2, (r.right + size) / 2,
                    (r.bottom + size) / 2);
            ui_draw_image(dc, profile_pixels, profile_w, profile_h, p, 0);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_LBUTTONUP || (msg == WM_KEYDOWN && wp == VK_ESCAPE))
    {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY)
    {
        free(profile_pixels);
        profile_pixels = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
void ui_avatar(HWND parent, const char *hash)
{
    WCHAR p[MAX_PATH], leaf[100];
    WNDCLASSW wc;
    HWND old = FindWindowW(L"AltekAvatar", NULL);
    if (old)
    {
        DestroyWindow(old);
    }
    if (!valid_hex(hash, 64))
    {
        MessageBoxW(parent, L"This contact has no profile photo.", L"ALTEK Link", MB_OK);
        return;
    }
    _snwprintf(leaf, 100, L"avatar_%S.jpg", hash);
    path_join(p, app.dir, leaf);
    if (!image_load(p, &profile_pixels, &profile_w, &profile_h))
    {
        return;
    }
    memset(&wc, 0, sizeof(wc));
    wc.hInstance = app.instance;
    wc.lpfnWndProc = avatar_proc;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"AltekAvatar";
    RegisterClassW(&wc);
    CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Profile photo - click or Escape to close",
                    WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 460, 490,
                    parent, NULL, app.instance, NULL);
}
static void label(HWND h, const WCHAR *s, int y)
{
    ui_control(h, L"STATIC", s, 0, 0, 20, y, 170, 26);
}
static int edit_int(HWND h, int id, int fallback)
{
    WCHAR s[40];
    int v;
    GetDlgItemTextW(h, id, s, 40);
    v = _wtoi(s);
    return v ? v : fallback;
}
static LRESULT CALLBACK settings_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CREATE)
    {
        WCHAR w[256];
        char version[80];
        HWND c;
        HKEY key;
        DWORD type, n = sizeof(w);
        int autostart = 0;
        label(h, L"Device name", 20);
        wide(app.name, w, 256);
        ui_control(h, L"EDIT", w, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 1, 195, 17, 270, 27);
        label(h, L"Camera", 63);
        c = ui_control(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 2, 195, 60,
                       270, 230);
        camera_list(c);
        label(h, L"Resolution", 106);
        c = ui_control(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 3, 195, 103,
                       270, 230);
        camera_modes(c, app.camera_index);
        label(h, L"Target FPS (1-60)", 149);
        _snwprintf(w, 256, L"%d", app.fps);
        ui_control(h, L"EDIT", w, WS_BORDER | ES_NUMBER | WS_TABSTOP, 4, 195, 146, 100, 27);
        label(h, L"Bitrate (kbit/s)", 192);
        _snwprintf(w, 256, L"%d", app.bitrate / 1000);
        ui_control(h, L"EDIT", w, WS_BORDER | ES_NUMBER | WS_TABSTOP, 5, 195, 189, 100, 27);
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                          0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
        {
            autostart =
                RegQueryValueExW(key, L"ALTEK Link", NULL, &type, (BYTE *)w, &n) == ERROR_SUCCESS;
            RegCloseKey(key);
        }
        c = ui_control(h, L"BUTTON", L"Start when I sign in", BS_AUTOCHECKBOX | WS_TABSTOP, 8, 245,
                       275, 225, 27);
        SendMessageW(c, BM_SETCHECK, autostart ? BST_CHECKED : BST_UNCHECKED, 0);
        label(h, L"Color theme", 318);
        c = ui_control(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 15, 195,
                       315, 190, 100);
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Green");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Petrol blue");
        SendMessageW(c, CB_SETCURSEL, app.theme, 0);
        ui_control(h, L"BUTTON", L"Choose / crop photo", BS_PUSHBUTTON, 9, 20, 360, 205, 32);
        ui_control(h, L"BUTTON", L"Remove photo", BS_PUSHBUTTON, 10, 245, 360, 205, 32);
        label(h, L"Add by IPv4", 412);
        ui_control(h, L"EDIT", L"192.168.100.", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 11, 195,
                   409, 170, 27);
        ui_control(h, L"BUTTON", L"Find", BS_PUSHBUTTON, 12, 375, 407, 90, 30);
        c = ui_control(h, L"BUTTON", L"Minimize before sharing this desktop",
                       BS_AUTOCHECKBOX | WS_TABSTOP, 16, 20, 451, 360, 27);
        SendMessageW(c, BM_SETCHECK, app.minimize_screen_share ? BST_CHECKED : BST_UNCHECKED, 0);
        label(h, L"Remote pinch zoom", 491);
        c = ui_control(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                       18, 195, 488, 270, 120);
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Enabled everywhere");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Disabled on desktop");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Disabled everywhere");
        SendMessageW(c, CB_SETCURSEL, app.remote_pinch_mode, 0);
        label(h, L"Save received files", 531);
        ui_control(h, L"EDIT", app.receive_dir, WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                   20, 195, 528, 115, 27);
        ui_control(h, L"BUTTON", L"Browse", BS_PUSHBUTTON, 19, 315, 527, 70, 30);
        ui_control(h, L"BUTTON", L"Default", BS_PUSHBUTTON, 21, 390, 527, 75, 30);
        ui_control(h, L"STATIC",
                   L"Calls use the default Windows microphone and speaker.\nUse a headset to avoid "
                   L"echo. Video changes apply to the next call.",
                   0, 0, 20, 570, 455, 42);
        _snprintf(version, sizeof(version) - 1, "Version %s", APP_VERSION_BUILD);
        version[sizeof(version) - 1] = 0;
        wide(version, w, countof(w));
        ui_control(h, L"STATIC", w, SS_CENTER, 0, 195, 622, 270, 22);
        ui_control(h, L"BUTTON", L"Open data folder", BS_PUSHBUTTON, 13, 20, 648, 160, 34);
        ui_control(h, L"BUTTON", L"Open settings.ini", BS_PUSHBUTTON, 17, 190, 648, 145, 34);
        ui_control(h, L"BUTTON", L"Save", BS_DEFPUSHBUTTON, 14, 345, 648, 120, 34);
        return 0;
    }
    if (msg == WM_COMMAND)
    {
        int id = LOWORD(wp);
        if (id == 2 && HIWORD(wp) == CBN_SELCHANGE)
        {
            camera_modes(GetDlgItem(h, 3), (int)SendDlgItemMessageW(h, 2, CB_GETCURSEL, 0, 0));
        }
        if (id == 9)
        {
            choose_avatar(h);
        }
        if (id == 10)
        {
            EnterCriticalSection(&app.lock);
            app.avatar[0] = 0;
            settings_save();
            LeaveCriticalSection(&app.lock);
        }
        if (id == 12)
        {
            WCHAR w[64];
            char s[64];
            GetDlgItemTextW(h, 11, w, 64);
            utf8(w, s, 64);
            network_probe(s);
        }
        if (id == 13)
        {
            ShellExecuteW(h, L"open", app.dir, NULL, NULL, SW_SHOWNORMAL);
        }
        if (id == 17)
        {
            WCHAR path[MAX_PATH];
            path_join(path, app.dir, L"settings.ini");
            if ((INT_PTR)ShellExecuteW(h, L"open", path, NULL, NULL, SW_SHOWNORMAL) <= 32)
            {
                MessageBoxW(h, L"Could not open settings.ini.", L"ALTEK Link", MB_ICONERROR);
            }
        }
        if (id == 21) SetDlgItemTextW(h, 20, L"");
        if (id == 19)
        {
            BROWSEINFOW bi;
            LPITEMIDLIST item;
            WCHAR folder[MAX_PATH];
            memset(&bi, 0, sizeof(bi));
            bi.hwndOwner = h;
            bi.lpszTitle = L"Choose a folder for received files";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            item = SHBrowseForFolderW(&bi);
            if (item)
            {
                if (SHGetPathFromIDListW(item, folder)) SetDlgItemTextW(h, 20, folder);
                CoTaskMemFree(item);
            }
        }
        if (id == 14)
        {
            WCHAR w[300];
            char name[256];
            int fps = edit_int(h, 4, 15), bitrate = edit_int(h, 5, 4000);
            LRESULT index, dimensions;
            HKEY key;
            GetDlgItemTextW(h, 1, w, 80);
            utf8(w, name, 256);
            if (!name[0] || fps < 1 || fps > 60 || bitrate < 50 || bitrate > 20000)
            {
                MessageBoxW(h, L"Check name, FPS (1-60), and bitrate (50-20000).",
                            L"ALTEK Link", MB_ICONWARNING);
                return 0;
            }
            EnterCriticalSection(&app.lock);
            GetDlgItemTextW(h, 20, app.receive_dir, MAX_PATH);
            copy_s(app.name, 256, name);
            app.fps = fps;
            app.bitrate = bitrate * 1000;
            app.minimize_screen_share =
                SendDlgItemMessageW(h, 16, BM_GETCHECK, 0, 0) == BST_CHECKED;
            index = SendDlgItemMessageW(h, 18, CB_GETCURSEL, 0, 0);
            if (index >= 0 && index <= 2)
            {
                app.remote_pinch_mode = (int)index;
            }
            index = SendDlgItemMessageW(h, 15, CB_GETCURSEL, 0, 0);
            if (index >= 0 && index < 2)
            {
                app.theme = (int)index;
            }
            index = SendDlgItemMessageW(h, 2, CB_GETCURSEL, 0, 0);
            if (index >= 0)
            {
                app.camera_index = (int)index;
            }
            index = SendDlgItemMessageW(h, 3, CB_GETCURSEL, 0, 0);
            if (index >= 0)
            {
                dimensions = SendDlgItemMessageW(h, 3, CB_GETITEMDATA, index, 0);
                app.width = LOWORD(dimensions);
                app.height = HIWORD(dimensions);
            }
            settings_save();
            LeaveCriticalSection(&app.lock);
            if (RegCreateKeyExW(HKEY_CURRENT_USER,
                                L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, NULL, 0,
                                KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS)
            {
                if (SendDlgItemMessageW(h, 8, BM_GETCHECK, 0, 0) == BST_CHECKED)
                {
                    WCHAR path[MAX_PATH], command[MAX_PATH + 20];
                    GetModuleFileNameW(NULL, path, MAX_PATH);
                    _snwprintf(command, countof(command), L"\"%s\" --tray", path);
                    RegSetValueExW(key, L"ALTEK Link", 0, REG_SZ, (BYTE *)command,
                                   (DWORD)((wcslen(command) + 1) * sizeof(WCHAR)));
                }
                else
                {
                    RegDeleteValueW(key, L"ALTEK Link");
                }
                RegCloseKey(key);
            }
            ui_theme_apply();
            changed();
            DestroyWindow(h);
        }
        return 0;
    }
    if (msg == WM_DESTROY)
    {
        settings_window = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
void ui_settings(HWND parent)
{
    WNDCLASSW wc;
    if (settings_window)
    {
        SetForegroundWindow(settings_window);
        return;
    }
    memset(&wc, 0, sizeof(wc));
    wc.hInstance = app.instance;
    wc.lpfnWndProc = settings_proc;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AltekSettings";
    RegisterClassW(&wc);
    settings_window =
        CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"ALTEK Link - Settings",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT,
                        CW_USEDEFAULT, 500, 734, parent, NULL, app.instance, NULL);
}
