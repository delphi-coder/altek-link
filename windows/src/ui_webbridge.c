#include "ui.h"

#define WEB_TEXT 701
#define WEB_SEND 702
#define WEB_STOP 703
#define WEB_CLOSE 704
#define WEB_QR 705
#define WEB_IMAGES 707
#define WEB_VIDEO 708
#define WEB_HTML 709
#define WEB_WARNING 710

int webbridge_make_qr(const char *text, unsigned char *modules, int capacity);
const char *webbridge_address(void);
const char *webbridge_code(void);
const char *webbridge_token(void);
LONG webbridge_text_version(void);
void webbridge_copy_text(char *out);
void webbridge_set_text(const char *text);
int webbridge_inline_option(int group);
void webbridge_set_inline_option(int group, int enabled);

static HWND web_window;
static unsigned char qr_modules[177 * 177];
static int qr_size;
static LONG shown_version = -1;
static HFONT web_title_font;

static LRESULT CALLBACK qr_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PAINTSTRUCT ps;
    HDC dc;
    RECT r, module;
    int x, y, scale, left, top;
    (void)wp; (void)lp;
    if (msg != WM_PAINT)
        return DefWindowProcW(hwnd, msg, wp, lp);
    dc = BeginPaint(hwnd, &ps);
    GetClientRect(hwnd, &r);
    FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    if (qr_size > 0)
    {
        scale = (r.right < r.bottom ? r.right : r.bottom) / (qr_size + 8);
        if (scale < 1) scale = 1;
        left = (r.right - qr_size * scale) / 2;
        top = (r.bottom - qr_size * scale) / 2;
        for (y = 0; y < qr_size; y++)
            for (x = 0; x < qr_size; x++)
                if (qr_modules[y * qr_size + x])
                {
                    SetRect(&module, left + x * scale, top + y * scale,
                            left + (x + 1) * scale, top + (y + 1) * scale);
                    FillRect(dc, &module, (HBRUSH)GetStockObject(BLACK_BRUSH));
                }
    }
    EndPaint(hwnd, &ps);
    return 0;
}

static HWND control(HWND parent, const WCHAR *cls, const WCHAR *caption, DWORD style,
                    int id, int x, int y, int width, int height)
{
    HWND h = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, caption,
                             WS_CHILD | WS_VISIBLE | style, x, y, width, height, parent,
                             (HMENU)(INT_PTR)id, app.instance, NULL);
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return h;
}

static void refresh_text(HWND hwnd, int force)
{
    LONG current = webbridge_text_version();
    char *text;
    WCHAR *wide, *display;
    size_t i, j;
    if (!force && shown_version == current) return;
    text = (char *)malloc(65537);
    wide = (WCHAR *)malloc(65537 * sizeof(WCHAR));
    display = (WCHAR *)malloc(131073 * sizeof(WCHAR));
    if (text && wide && display)
    {
        webbridge_copy_text(text);
        if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 65537)) wide[0] = 0;
        for (i = 0, j = 0; wide[i] && j < 131070; i++)
        {
            if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r')) display[j++] = L'\r';
            display[j++] = wide[i];
        }
        display[j] = 0;
        SetWindowTextW(GetDlgItem(hwnd, WEB_TEXT), display);
        shown_version = current;
    }
    free(text);
    free(wide);
    free(display);
}

static void submit_text(HWND hwnd)
{
    HWND edit = GetDlgItem(hwnd, WEB_TEXT);
    int length = GetWindowTextLengthW(edit);
    WCHAR *wide;
    char *text;
    int i, j;
    if (length > 131072)
    {
        MessageBoxW(hwnd, L"The shared text is too long (maximum 64 KB UTF-8).", L"ALTEK Link", MB_ICONWARNING);
        return;
    }
    wide = (WCHAR *)malloc((length + 1) * sizeof(WCHAR));
    text = (char *)malloc(65537);
    if (!wide || !text)
    {
        free(wide); free(text);
        MessageBoxW(hwnd, L"Not enough memory.", L"ALTEK Link", MB_ICONERROR);
        return;
    }
    GetWindowTextW(edit, wide, length + 1);
    for (i = 0, j = 0; wide[i]; i++)
    {
        if (wide[i] == L'\r' && wide[i + 1] == L'\n') continue;
        wide[j++] = wide[i];
    }
    wide[j] = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, 65537, NULL, NULL))
        MessageBoxW(hwnd, L"The shared text is too long (maximum 64 KB UTF-8).", L"ALTEK Link", MB_ICONWARNING);
    else
    {
        webbridge_set_text(text);
        shown_version = webbridge_text_version();
        SetDlgItemTextW(hwnd, 706, L"Text shared. Other devices can refresh their page.");
    }
    free(wide); free(text);
}

static LRESULT CALLBACK web_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WCHAR address[180], code[32];
    HWND edit, title;
    LOGFONTW title_font;
    if (msg == WM_CREATE)
    {
        MultiByteToWideChar(CP_UTF8, 0, webbridge_address(), -1, address, 180);
        MultiByteToWideChar(CP_UTF8, 0, webbridge_code(), -1, code, 32);
        title = control(hwnd, L"STATIC", L"Scan to connect", SS_CENTER, 0, 20, 10, 410, 24);
        if (GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(title_font), &title_font))
        {
            title_font.lfWeight = FW_BOLD;
            title_font.lfHeight = title_font.lfHeight < 0 ? title_font.lfHeight - 2 : title_font.lfHeight + 2;
            web_title_font = CreateFontIndirectW(&title_font);
            if (web_title_font) SendMessageW(title, WM_SETFONT, (WPARAM)web_title_font, TRUE);
        }
        control(hwnd, L"AltekWebQR", L"", 0, WEB_QR, 125, 32, 210, 210);
        control(hwnd, L"STATIC", L"Address:", 0, 0, 20, 252, 75, 20);
        edit = control(hwnd, L"EDIT", address, ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, 0, 95, 247, 335, 26);
        (void)edit;
        control(hwnd, L"STATIC", L"Access code:", 0, 0, 20, 286, 90, 20);
        control(hwnd, L"EDIT", code, ES_READONLY | WS_TABSTOP, 0, 110, 281, 90, 26);
        control(hwnd, L"STATIC", L"Shared text:", 0, 0, 20, 319, 150, 20);
        control(hwnd, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, WEB_TEXT, 20, 340, 410, 128);
        SendDlgItemMessageW(hwnd, WEB_TEXT, EM_SETLIMITTEXT, 131072, 0);
        control(hwnd, L"BUTTON", L"Share text", WS_TABSTOP, WEB_SEND, 20, 479, 108, 28);
        control(hwnd, L"STATIC", L"", 0, 706, 140, 484, 290, 20);
        control(hwnd, L"STATIC", L"Open in browser (unchecked: download):", 0, 0, 20, 515, 410, 20);
        control(hwnd, L"BUTTON", L"Images (JPG, PNG)", BS_AUTOCHECKBOX | WS_TABSTOP, WEB_IMAGES, 20, 538, 160, 22);
        control(hwnd, L"BUTTON", L"Video (MP4)", BS_AUTOCHECKBOX | WS_TABSTOP, WEB_VIDEO, 185, 538, 115, 22);
        control(hwnd, L"BUTTON", L"HTML", BS_AUTOCHECKBOX | WS_TABSTOP, WEB_HTML, 305, 538, 80, 22);
        CheckDlgButton(hwnd, WEB_IMAGES, webbridge_inline_option(0) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, WEB_VIDEO, webbridge_inline_option(1) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, WEB_HTML, webbridge_inline_option(2) ? BST_CHECKED : BST_UNCHECKED);
        control(hwnd, L"STATIC", L"HTTP is not encrypted. Use a trusted network.", 0, WEB_WARNING, 20, 571, 410, 20);
        control(hwnd, L"BUTTON", L"Stop sharing", WS_TABSTOP, WEB_STOP, 198, 601, 110, 30);
        control(hwnd, L"BUTTON", L"Close", WS_TABSTOP, WEB_CLOSE, 320, 601, 110, 30);
        shown_version = -1;
        refresh_text(hwnd, 1);
        SetTimer(hwnd, 1, 300, NULL);
        return 0;
    }
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN)
    {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        if (GetDlgCtrlID((HWND)lp) == WEB_WARNING) SetTextColor(dc, RGB(180, 35, 35));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    if (msg == WM_TIMER && wp == 1)
    {
        refresh_text(hwnd, 0);
        return 0;
    }
    if (msg == WM_COMMAND)
    {
        switch (LOWORD(wp))
        {
        case WEB_SEND: submit_text(hwnd); return 0;
        case WEB_IMAGES: case WEB_VIDEO: case WEB_HTML:
            if (HIWORD(wp) == BN_CLICKED)
                webbridge_set_inline_option(LOWORD(wp) - WEB_IMAGES,
                    IsDlgButtonChecked(hwnd, LOWORD(wp)) == BST_CHECKED);
            return 0;
        case WEB_STOP: web_sharing_stop(); return 0;
        case WEB_CLOSE: DestroyWindow(hwnd); return 0;
        }
    }
    if (msg == WM_CLOSE)
    {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY)
    {
        KillTimer(hwnd, 1);
        if (web_title_font) { DeleteObject(web_title_font); web_title_font = NULL; }
        web_window = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void webbridge_window_close(void)
{
    if (web_window) DestroyWindow(web_window);
}

void webbridge_window_open(HWND parent)
{
    WNDCLASSW wc;
    char link[220];
    RECT rect;
    if (web_window)
    {
        ShowWindow(web_window, SW_RESTORE);
        SetForegroundWindow(web_window);
        return;
    }
    _snprintf(link, sizeof(link), "%s%s/", webbridge_address(), webbridge_token());
    link[sizeof(link) - 1] = 0;
    qr_size = webbridge_make_qr(link, qr_modules, sizeof(qr_modules));
    if (!qr_size)
    {
        MessageBoxW(parent, L"Could not generate the QR code.", L"ALTEK Link", MB_ICONERROR);
        return;
    }
    memset(&wc, 0, sizeof(wc));
    wc.hInstance = app.instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpfnWndProc = qr_proc;
    wc.lpszClassName = L"AltekWebQR";
    RegisterClassW(&wc);
    wc.lpfnWndProc = web_proc;
    wc.lpszClassName = L"AltekWebSharing";
    RegisterClassW(&wc);
    SetRect(&rect, 0, 0, 450, 650);
    AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    web_window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AltekWebSharing", L"ALTEK Link - Web sharing",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                                 rect.bottom - rect.top, parent, NULL, app.instance, NULL);
    if (!web_window)
    {
        MessageBoxW(parent, L"Could not open the web sharing window.", L"ALTEK Link", MB_ICONERROR);
        return;
    }
    ShowWindow(web_window, SW_SHOW);
    SetForegroundWindow(web_window);
}
