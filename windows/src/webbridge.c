#ifdef __MINGW32__
#define clock_getres webbridge_clock_getres
#define clock_gettime webbridge_clock_gettime
#define clock_settime webbridge_clock_settime
#endif
#include "ui.h"
#include <shlobj.h>
#include <iphlpapi.h>
#include <ctype.h>
#include <wctype.h>

/* Web sharing uses one worker and bounded buffers; content never enters RAM as a whole. */
static SOCKET listener = INVALID_SOCKET, client = INVALID_SOCKET;
static HANDLE worker;
static volatile LONG running;
static WCHAR share_root[MAX_PATH];
static char share_token[33], share_code[5], share_url[160];
static char shared_text[65537];
static volatile LONG text_guard, text_version;
static volatile LONG inline_images = 1, inline_video = 1, inline_html = 0;

int webbridge_inline_option(int group)
{
    volatile LONG *value = group == 0 ? &inline_images : group == 1 ? &inline_video : &inline_html;
    return InterlockedCompareExchange(value, 0, 0) != 0;
}

void webbridge_set_inline_option(int group, int enabled)
{
    static const WCHAR *keys[] = {L"images", L"video", L"html"};
    volatile LONG *value;
    WCHAR path[MAX_PATH];
    if (group < 0 || group > 2)
        return;
    value = group == 0 ? &inline_images : group == 1 ? &inline_video : &inline_html;
    InterlockedExchange(value, enabled ? 1 : 0);
    path_join(path, app.dir, L"settings.ini");
    WritePrivateProfileStringW(L"web_sharing", keys[group], enabled ? L"1" : L"0", path);
}

static void webbridge_load_options(void)
{
    WCHAR path[MAX_PATH];
    path_join(path, app.dir, L"settings.ini");
    InterlockedExchange(&inline_images,
                        GetPrivateProfileIntW(L"web_sharing", L"images", 1, path) != 0);
    InterlockedExchange(&inline_video,
                        GetPrivateProfileIntW(L"web_sharing", L"video", 1, path) != 0);
    InterlockedExchange(&inline_html, GetPrivateProfileIntW(L"web_sharing", L"html", 0, path) != 0);
}
void webbridge_window_open(HWND parent);
void webbridge_window_close(void);
const char *webbridge_address(void)
{
    return share_url;
}
const char *webbridge_code(void)
{
    return share_code;
}
const char *webbridge_token(void)
{
    return share_token;
}
LONG webbridge_text_version(void)
{
    return InterlockedCompareExchange(&text_version, 0, 0);
}
static void text_lock(void)
{
    while (InterlockedCompareExchange(&text_guard, 1, 0) != 0)
        Sleep(0);
}
void webbridge_copy_text(char *out)
{
    text_lock();
    memcpy(out, shared_text, strlen(shared_text) + 1);
    InterlockedExchange(&text_guard, 0);
}
void webbridge_set_text(const char *text)
{
    text_lock();
    strncpy(shared_text, text, 65536);
    shared_text[65536] = 0;
    InterlockedIncrement(&text_version);
    InterlockedExchange(&text_guard, 0);
}
static int failures;
static ULONGLONG blocked_until;

static int write_all(SOCKET s, const void *data, int size)
{
    const char *p = (const char *)data;
    while (size > 0)
    {
        int n = send(s, p, size, 0);
        if (n <= 0)
            return 0;
        p += n;
        size -= n;
    }
    return 1;
}
static void put(SOCKET s, const char *text)
{
    write_all(s, text, (int)strlen(text));
}
static void header(SOCKET s, const char *status, const char *type)
{
    char buf[512];
    _snprintf(buf, sizeof(buf),
              "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\nCache-Control: "
              "no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n\r\n",
              status, type);
    buf[sizeof(buf) - 1] = 0;
    put(s, buf);
}
static void reply(SOCKET s, const char *status, const char *message)
{
    header(s, status, "text/plain; charset=utf-8");
    put(s, message);
}
static int unhex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static int decode(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    int a, b;
    unsigned char c;
    while (*in && *in != '&')
    {
        c = (unsigned char)*in++;
        if (c == '%')
        {
            if (!in[0] || !in[1] || (a = unhex(in[0])) < 0 || (b = unhex(in[1])) < 0)
                return 0;
            c = (unsigned char)((a << 4) | b);
            in += 2;
        }
        else if (c == '+')
            c = ' ';
        if (!c || n + 1 >= cap)
            return 0;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return 1;
}
static int query(const char *q, const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    out[0] = 0;
    while (q && *q)
    {
        if (!strncmp(q, name, n) && q[n] == '=')
            return decode(q + n + 1, out, cap);
        q = strchr(q, '&');
        if (q)
            q++;
    }
    return 1;
}
static void html(SOCKET s, const char *p)
{
    const char *start = p;
    while (*p)
    {
        const char *rep = NULL;
        if (*p == '&')
            rep = "&amp;";
        else if (*p == '<')
            rep = "&lt;";
        else if (*p == '>')
            rep = "&gt;";
        else if (*p == '\"')
            rep = "&quot;";
        else if (*p == '\'')
            rep = "&#39;";
        if (rep)
        {
            write_all(s, start, (int)(p - start));
            put(s, rep);
            start = p + 1;
        }
        p++;
    }
    write_all(s, start, (int)(p - start));
}
static void urlpart(SOCKET s, const char *p)
{
    char b[4];
    static const char hex[] = "0123456789ABCDEF";
    unsigned char c;
    while (*p)
    {
        c = (unsigned char)*p++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            write_all(s, &c, 1);
        else
        {
            b[0] = '%';
            b[1] = hex[c >> 4];
            b[2] = hex[c & 15];
            write_all(s, b, 3);
        }
    }
}
/* Reject reparse points and all Windows path syntax outside simple path components. */
static int reserved_name(const WCHAR *s)
{
    WCHAR name[8];
    size_t i = 0;
    while (s[i] && s[i] != L'.' && i < 7)
    {
        name[i] = (WCHAR)towupper(s[i]);
        i++;
    }
    name[i] = 0;
    if (!wcscmp(name, L"CON") || !wcscmp(name, L"PRN") || !wcscmp(name, L"AUX") ||
        !wcscmp(name, L"NUL"))
        return 1;
    return i == 4 && (!wcsncmp(name, L"COM", 3) || !wcsncmp(name, L"LPT", 3)) && name[3] >= L'1' &&
           name[3] <= L'9';
}
static int resolve(const char *relative, WCHAR *out, int allow_missing_last)
{
    WCHAR w[1024], *p, *next;
    size_t len;
    DWORD attr;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, relative, -1, w, 1024))
        return 0;
    wcsncpy(out, share_root, MAX_PATH - 1);
    out[MAX_PATH - 1] = 0;
    p = w;
    while (*p)
    {
        next = wcschr(p, L'/');
        if (next)
            *next = 0;
        len = wcslen(p);
        if (!len || reserved_name(p) || !wcscmp(p, L".") || !wcscmp(p, L"..") ||
            p[len - 1] == L'.' || p[len - 1] == L' ' || wcspbrk(p, L"\\:*?\"<>|") ||
            wcslen(out) + len + 2 >= MAX_PATH)
            return 0;
        wcscat(out, L"\\");
        wcscat(out, p);
        attr = GetFileAttributesW(out);
        if (attr == INVALID_FILE_ATTRIBUTES)
        {
            if (!allow_missing_last || next)
                return 0;
        }
        else
        {
            if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
                return 0;
            if (next && !(attr & FILE_ATTRIBUTE_DIRECTORY))
                return 0;
        }
        if (!next)
            break;
        p = next + 1;
    }
    return 1;
}
static int combine(const char *folder, const char *name, char *out, size_t cap)
{
    if (!name[0] || strchr(name, '/') || strchr(name, '\\'))
        return 0;
    if (strlen(folder) + strlen(name) + 2 > cap)
        return 0;
    strcpy(out, folder);
    if (*folder)
        strcat(out, "/");
    strcat(out, name);
    return 1;
}
static void format_file_size(unsigned __int64 size, char *out, size_t capacity)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = (double)size;
    int unit = 0;
    while (value >= 1024.0 && unit < 5)
    {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) _snprintf(out, capacity, "%I64u B", size);
    else _snprintf(out, capacity, "%.1f %s", value, units[unit]);
    out[capacity - 1] = 0;
}

static void format_file_time(const FILETIME *time, char *out, size_t capacity)
{
    FILETIME local;
    SYSTEMTIME date;
    if (!FileTimeToLocalFileTime(time, &local) || !FileTimeToSystemTime(&local, &date))
    {
        strncpy(out, "-", capacity);
        out[capacity - 1] = 0;
        return;
    }
    _snprintf(out, capacity, "%04u-%02u-%02u %02u:%02u", date.wYear, date.wMonth,
              date.wDay, date.wHour, date.wMinute);
    out[capacity - 1] = 0;
}

static void listing(SOCKET s, const char *rel)
{
    WCHAR path[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE h;
    char name[2048], full[4096], size_text[48], time_text[48];
    char *snapshot;
    DWORD attr;
    if (!resolve(rel, path, 0) || (attr = GetFileAttributesW(path)) == INVALID_FILE_ATTRIBUTES ||
        !(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        reply(s, "404 Not Found", "Folder not found");
        return;
    }
    header(s, "200 OK", "text/html; charset=utf-8");
    put(s,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>ALTEK Link</title><style>"
        "*{box-sizing:border-box}body{margin:0;background:#f1f5f9;color:#263d51;"
        "font:16px Arial,sans-serif}header{background:#193782;color:white;padding:22px 16px;"
        "text-align:center}header h1{margin:0;font-size:26px}header p{margin:6px 0 0;opacity:.85}"
        "main{max-width:780px;margin:auto;padding:22px 16px}section{background:white;"
        "border-radius:10px;padding:18px;margin-bottom:16px;box-shadow:0 2px 10px #19378212}"
        "h2{font-size:18px;margin:0 0 14px}a{color:#244d95;text-decoration:none}"
        "a:hover{text-decoration:underline}ul{list-style:none;margin:0;padding:0}"
        "li a,.file-head{display:grid;grid-template-columns:minmax(0,1fr) 95px 145px;gap:12px;"
        "align-items:center;padding:12px;border-bottom:1px solid #edf1f5;overflow-wrap:anywhere}"
        ".file-head{color:#64748b;font-size:13px;font-weight:bold}.meta{white-space:nowrap}"
        "@media (orientation:portrait){li a,.file-head{grid-template-columns:minmax(0,1fr)}"
        ".meta{display:none}}"
        "li a:hover{background:#f5f8fd}input,textarea,button{font:inherit}"
        "input[type=text],input[type=file],textarea{width:100%;padding:10px;margin:4px 0 10px;"
        "border:1px solid #cbd5e1;border-radius:6px}button{background:#193782;color:white;"
        "border:0;border-radius:6px;padding:10px "
        "18px;cursor:pointer}button:hover{background:#28499b}"
        "progress{display:block;width:100%;margin-top:10px}textarea{min-height:110px;resize:"
        "vertical}"
        "</style><header><h1>ALTEK Link</h1><p>Local file sharing</p></header><main>"
        "<section><h2>Shared files</h2><p><a href='./'>Shared folder</a></p>"
        "<div class=file-head><span>Name</span><span class=meta>Size</span>"
        "<span class=meta>Last modified</span></div><ul>");
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", path);
    pattern[MAX_PATH - 1] = 0;
    h = FindFirstFileW(pattern, &data);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..") ||
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            if (!WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name, sizeof(name), NULL,
                                     NULL) ||
                !combine(rel, name, full, sizeof(full)))
                continue;
            put(s, "<li><a href='");
            put(s, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "?d=" : "file?p=");
            urlpart(s, full);
            put(s, "'><span>");
            html(s, name);
            put(s, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "/</span>" : "</span>");
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) strcpy(size_text, "-");
            else format_file_size(((unsigned __int64)data.nFileSizeHigh << 32) | data.nFileSizeLow,
                                  size_text, sizeof(size_text));
            format_file_time(&data.ftLastWriteTime, time_text, sizeof(time_text));
            put(s, "<span class=meta>"); put(s, size_text);
            put(s, "</span><span class=meta>"); put(s, time_text);
            put(s, "</span></a></li>");
        } while (FindNextFileW(h, &data));
        FindClose(h);
    }
    put(s, "</ul></section><section><h2>Upload files</h2><input id=f type=file multiple><button "
           "onclick=upload()>Upload</button><progress id=p max=100 value=0></progress><h2>Create "
           "folder</h2><input type=text id=folder placeholder='New folder'><button "
           "onclick=mkdir()>Create folder</button></section><section><h2>Shared text</h2><textarea "
           "id=t rows=5>");
    snapshot = (char *)malloc(65537);
    if (snapshot)
    {
        webbridge_copy_text(snapshot);
        html(s, snapshot);
        free(snapshot);
    }
    put(s,
        "</textarea><button onclick=sendtext()>Send text</button><p id=status></p></section></main>\n"
        "<script>var d = new URLSearchParams(location.search).get('d') || \"\";\n"
        "function post(url, body, done) {\n"
        "  var request = new XMLHttpRequest();\n"
        "  request.open('POST', url, true);\n"
        "  request.onload = function() {\n"
        "    if (request.status == 200) { if (done) done(); }\n"
        "    else alert(request.responseText);\n"
        "  };\n"
        "  request.onerror = function() { alert('Connection failed'); };\n"
        "  request.send(body);\n"
        "}\n"
        "function upload() {\n"
        "  var files = document.getElementById('f').files, index = 0;\n"
        "  function next() {\n"
        "    if (index >= files.length) { location.reload(); return; }\n"
        "    var file = files[index++], request = new XMLHttpRequest();\n"
        "    request.open('POST', 'upload?d=' + encodeURIComponent(d) + '&name=' + encodeURIComponent(file.name), true);\n"
        "    request.upload.onprogress = function(event) {\n"
        "      if (event.lengthComputable) document.getElementById('p').value = event.loaded / event.total * 100;\n"
        "    };\n"
        "    request.onload = function() { if (request.status == 200) next(); else alert(request.responseText); };\n"
        "    request.onerror = function() { alert('Upload failed'); };\n"
        "    request.send(file);\n"
        "  }\n"
        "  if (files.length) next();\n"
        "}\n"
        "function mkdir() {\n"
        "  post('mkdir?d=' + encodeURIComponent(d) + '&name=' + encodeURIComponent(document.getElementById('folder').value), '', function() { location.reload(); });\n"
        "}\n"
        "function sendtext() {\n"
        "  post('text?d=' + encodeURIComponent(d), document.getElementById('t').value, function() {\n"
        "    document.getElementById('status').textContent = 'Text sent';\n"
        "  });\n"
        "}\n"
        "</script></body></html>\n");
}
static const char *webbridge_file_type(const char *name, int *show_inline)
{
    const char *ext = strrchr(name, '.');
    *show_inline = 0;
    if (!ext)
        return "application/octet-stream";
    if (!_stricmp(ext, ".jpg") || !_stricmp(ext, ".jpeg"))
    {
        *show_inline = webbridge_inline_option(0);
        return "image/jpeg";
    }
    if (!_stricmp(ext, ".png"))
    {
        *show_inline = webbridge_inline_option(0);
        return "image/png";
    }
    if (!_stricmp(ext, ".mp4"))
    {
        *show_inline = webbridge_inline_option(1);
        return "video/mp4";
    }
    if (!_stricmp(ext, ".html") || !_stricmp(ext, ".htm"))
    {
        *show_inline = webbridge_inline_option(2);
        return "text/html; charset=utf-8";
    }
    return "application/octet-stream";
}
static void download(SOCKET s, const char *rel)
{
    WCHAR path[MAX_PATH];
    HANDLE f;
    LARGE_INTEGER size;
    char head[512], b[65536];
    DWORD n;
    int sent, show_inline;
    const char *type;
    if (!resolve(rel, path, 0))
    {
        reply(s, "404 Not Found", "File not found");
        return;
    }
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &size))
    {
        if (f != INVALID_HANDLE_VALUE)
            CloseHandle(f);
        reply(s, "404 Not Found", "File unavailable");
        return;
    }
    type = webbridge_file_type(rel, &show_inline);
    _snprintf(head, sizeof(head),
              "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: "
              "%I64u\r\nContent-Disposition: %s; filename*=UTF-8''",
              type, (unsigned __int64)size.QuadPart, show_inline ? "inline" : "attachment");
    head[sizeof(head) - 1] = 0;
    put(s, head);
    {
        const char *filename = strrchr(rel, '/');
        urlpart(s, filename ? filename + 1 : rel);
    }
    put(s, "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: "
           "sandbox\r\n\r\n");
    while (ReadFile(f, b, sizeof(b), &n, NULL) && n > 0)
    {
        sent = write_all(s, b, (int)n);
        if (!sent)
            break;
    }
    CloseHandle(f);
}
static void upload(SOCKET s, const char *folder, const char *name, unsigned __int64 size)
{
    WCHAR parent[MAX_PATH], path[MAX_PATH], wname[512], candidate[512], *dot;
    char rel[4096], buffer[65536];
    HANDLE f = INVALID_HANDLE_VALUE;
    DWORD written;
    int i, n;
    unsigned __int64 left = size;
    if (!resolve(folder, parent, 0) || !combine(folder, name, rel, sizeof(rel)) ||
        !resolve(rel, path, 1) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wname, 512))
    {
        reply(s, "400 Bad Request", "Invalid path");
        return;
    }
    for (i = 0; i < 10000; i++)
    {
        if (i == 0)
            wcsncpy(candidate, wname, 511);
        else
        {
            WCHAR stem[512], ext[512];
            wcsncpy(stem, wname, 511);
            stem[511] = 0;
            dot = wcsrchr(stem, L'.');
            ext[0] = 0;
            if (dot && dot != stem)
            {
                wcsncpy(ext, dot, 511);
                ext[511] = 0;
                *dot = 0;
            }
            _snwprintf(candidate, 511, L"%ls (%d)%ls", stem, i, ext);
        }
        candidate[511] = 0;
        if (wcslen(parent) + wcslen(candidate) + 2 >= MAX_PATH)
            break;
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", parent, candidate);
        path[MAX_PATH - 1] = 0;
        f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
    if (f == INVALID_HANDLE_VALUE)
    {
        reply(s, "500 Internal Server Error", "Cannot create file");
        return;
    }
    while (left > 0)
    {
        n = recv(s, buffer, (int)(left > sizeof(buffer) ? sizeof(buffer) : left), 0);
        if (n <= 0)
            break;
        if (!WriteFile(f, buffer, (DWORD)n, &written, NULL) || written != (DWORD)n)
            break;
        left -= (unsigned int)n;
    }
    CloseHandle(f);
    if (left)
    {
        DeleteFileW(path);
        reply(s, "500 Internal Server Error", "Upload interrupted");
    }
    else
        reply(s, "200 OK", "Uploaded");
}
static int read_exact(SOCKET s, char *b, int n)
{
    int got = 0, r;
    while (got < n)
    {
        r = recv(s, b + got, n - got, 0);
        if (r <= 0)
            return 0;
        got += r;
    }
    return 1;
}
static void serve(SOCKET s)
{
    char h[16384], method[16], uri[8192], *line, *next, *q, *op, code[128], dir[4096], name[2048],
        path[4096], prefix[48], tmp[512];
    int n = 0, has_length = 0, i;
    unsigned __int64 length = 0;
    WCHAR target[MAX_PATH];
    DWORD attr;
    char *incoming;
    while (n < (int)sizeof(h) - 1)
    {
        if (recv(s, h + n, 1, 0) != 1)
            return;
        n++;
        if (n >= 4 && !memcmp(h + n - 4, "\r\n\r\n", 4))
            break;
    }
    if (n < 4 || n >= (int)sizeof(h) - 1)
        return;
    h[n] = 0;
    if (sscanf(h, "%15s %8191s", method, uri) != 2)
        return;
    line = strstr(h, "\r\n");
    if (!line)
        return;
    line += 2;
    while (*line && (next = strstr(line, "\r\n")) != NULL && next != line)
    {
        *next = 0;
        if (!_strnicmp(line, "Content-Length:", 15))
        {
            char *end;
            if (has_length)
                return;
            line += 15;
            while (*line == ' ')
                line++;
            if (!*line || *line == '-')
                return;
            length = _strtoui64(line, &end, 10);
            if (*end)
                return;
            has_length = 1;
        }
        if (!_strnicmp(line, "Transfer-Encoding:", 18))
            return;
        line = next + 2;
    }
    q = strchr(uri, '?');
    if (q)
        *q++ = 0;
    _snprintf(prefix, sizeof(prefix), "/%s/", share_token);
    prefix[sizeof(prefix) - 1] = 0;
    if (!strcmp(uri, "/") && !strcmp(method, "GET"))
    {
        header(s, "200 OK", "text/html; charset=utf-8");
        put(s,
            "<!doctype html><meta charset=utf-8><meta name=viewport "
            "content='width=device-width,initial-scale=1'><title>ALTEK Link</title><body "
            "style='font:18px sans-serif;margin:3em'><h1>ALTEK Link</h1><p>Enter the four-letter "
            "code displayed on the sharing device.</p><form method=post action=/login><input "
            "name=code maxlength=4 autocomplete=off autocapitalize=characters required "
            "style='font-size:24px;text-transform:uppercase;width:5em'><button "
            "style='font-size:20px'>Connect</button></form>");
        return;
    }
    if (!strcmp(uri, "/login") && !strcmp(method, "POST"))
    {
        if (!has_length || length > 127 || !read_exact(s, code, (int)length))
        {
            reply(s, "400 Bad Request", "Invalid request");
            return;
        }
        code[(int)length] = 0;
        if (GetTickCount64() < blocked_until || strncmp(code, "code=", 5) ||
            _stricmp(code + 5, share_code))
        {
            if (GetTickCount64() >= blocked_until && ++failures >= 5)
            {
                failures = 0;
                blocked_until = GetTickCount64() + 30000;
            }
            reply(s, "403 Forbidden",
                  "Incorrect code or too many attempts. Please wait and try again.");
            return;
        }
        failures = 0;
        _snprintf(tmp, sizeof(tmp),
                  "HTTP/1.1 303 See Other\r\nLocation: %s\r\nConnection: close\r\nCache-Control: "
                  "no-store\r\nContent-Length: 0\r\n\r\n",
                  prefix);
        tmp[sizeof(tmp) - 1] = 0;
        put(s, tmp);
        return;
    }
    if (strncmp(uri, prefix, strlen(prefix)))
    {
        reply(s, "404 Not Found", "Not found");
        return;
    }
    op = uri + strlen(prefix);
    if (!query(q, "d", dir, sizeof(dir)) || !query(q, "name", name, sizeof(name)) ||
        !query(q, "p", path, sizeof(path)))
    {
        reply(s, "400 Bad Request", "Invalid query");
        return;
    }
    if (!*op && !strcmp(method, "GET"))
    {
        listing(s, dir);
        return;
    }
    if (!strcmp(op, "file") && !strcmp(method, "GET"))
    {
        download(s, path);
        return;
    }
    if (!strcmp(method, "POST") && !has_length)
    {
        reply(s, "411 Length Required", "Content-Length required");
        return;
    }
    if (!strcmp(op, "upload") && !strcmp(method, "POST"))
    {
        upload(s, dir, name, length);
        return;
    }
    if (!strcmp(op, "mkdir") && !strcmp(method, "POST"))
    {
        if (!combine(dir, name, path, sizeof(path)) || !resolve(path, target, 1))
        {
            reply(s, "400 Bad Request", "Invalid path");
            return;
        }
        attr = GetFileAttributesW(target);
        if (attr != INVALID_FILE_ATTRIBUTES)
        {
            reply(s, "409 Conflict", "Folder already exists");
            return;
        }
        if (!CreateDirectoryW(target, NULL))
        {
            reply(s, "500 Internal Server Error", "Cannot create folder");
            return;
        }
        reply(s, "200 OK", "Created");
        return;
    }
    if (!strcmp(op, "text") && !strcmp(method, "POST"))
    {
        if (length > 65536)
        {
            reply(s, "400 Bad Request", "Text too long");
            return;
        }
        incoming = (char *)malloc((size_t)length + 1);
        if (!incoming)
        {
            reply(s, "500 Internal Server Error", "Out of memory");
            return;
        }
        if (!read_exact(s, incoming, (int)length))
        {
            free(incoming);
            reply(s, "400 Bad Request", "Incomplete request");
            return;
        }
        incoming[(int)length] = 0;
        for (i = 0; i < (int)length; i++)
            if (!incoming[i])
                incoming[i] = ' ';
        webbridge_set_text(incoming);
        free(incoming);
        reply(s, "200 OK", "Text saved");
        return;
    }
    reply(s, "404 Not Found", "Not found");
}
static unsigned __stdcall serve_thread(void *arg)
{
    SOCKET c;
    int timeout = 30000;
    (void)arg;
    while (InterlockedCompareExchange(&running, 1, 1))
    {
        c = accept(listener, NULL, NULL);
        if (c == INVALID_SOCKET)
            break;
        client = c;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
        serve(c);
        if (client == c)
        {
            client = INVALID_SOCKET;
            closesocket(c);
        }
    }
    return 0;
}
static int local_address(char *ip, size_t cap)
{
    IP_ADAPTER_INFO *ad, *p;
    ULONG size = 0;
    int found = 0;
    char fallback[64] = "";
    GetAdaptersInfo(NULL, &size);
    ad = (IP_ADAPTER_INFO *)malloc(size);
    if (!ad)
        return 0;
    if (GetAdaptersInfo(ad, &size) == NO_ERROR)
        for (p = ad; p; p = p->Next)
        {
            const char *a = p->IpAddressList.IpAddress.String;
            if (!a[0] || !strcmp(a, "0.0.0.0") || !strncmp(a, "169.254.", 8) ||
                !strncmp(a, "127.", 4))
                continue;
            if (!fallback[0])
            {
                strncpy(fallback, a, sizeof(fallback) - 1);
                fallback[sizeof(fallback) - 1] = 0;
            }
            if (p->GatewayList.IpAddress.String[0] &&
                strcmp(p->GatewayList.IpAddress.String, "0.0.0.0"))
            {
                strncpy(ip, a, cap - 1);
                ip[cap - 1] = 0;
                found = 1;
                break;
            }
        }
    if (!found && fallback[0])
    {
        strncpy(ip, fallback, cap - 1);
        ip[cap - 1] = 0;
        found = 1;
    }
    free(ad);
    return found;
}
int web_sharing_is_active(void)
{
    return InterlockedCompareExchange(&running, 1, 1) != 0;
}
static void web_sharing_refresh_button(void)
{
    HWND button = app.window ? GetDlgItem(app.window, ID_WEB_START) : NULL;
    if (button)
        InvalidateRect(button, NULL, TRUE);
}
void web_sharing_stop(void)
{
    SOCKET s;
    webbridge_window_close();
    InterlockedExchange(&running, 0);
    s = listener;
    listener = INVALID_SOCKET;
    if (s != INVALID_SOCKET)
    {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
    s = client;
    client = INVALID_SOCKET;
    if (s != INVALID_SOCKET)
    {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
    if (worker)
    {
        WaitForSingleObject(worker, 35000);
        CloseHandle(worker);
        worker = NULL;
    }
    share_token[0] = 0;
    share_code[0] = 0;
    webbridge_set_text("");
    web_sharing_refresh_button();
}
void web_sharing_start(HWND parent)
{
    BROWSEINFOW bi;
    LPITEMIDLIST item;
    struct sockaddr_in sa;
    int len;
    char ip[64], rnd[9];
    unsigned int x;
    int i;
    const char *alpha = "ABCDEFGHJKLMNPQRSTUVWXYZ";
    if (InterlockedCompareExchange(&running, 1, 1))
    {
        webbridge_window_open(parent);
        return;
    }
    webbridge_load_options();
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = parent;
    bi.lpszTitle = L"Choose the folder to share in a browser";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    item = SHBrowseForFolderW(&bi);
    if (!item)
        return;
    if (!SHGetPathFromIDListW(item, share_root))
    {
        CoTaskMemFree(item);
        return;
    }
    CoTaskMemFree(item);
    if (!local_address(ip, sizeof(ip)) || !random_hex(share_token, 16) || !random_hex(rnd, 4))
    {
        MessageBoxW(parent, L"Could not find a local network address or generate a secure session.",
                    L"ALTEK Link", MB_ICONERROR);
        return;
    }
    sscanf(rnd, "%x", &x);
    for (i = 0; i < 4; i++)
    {
        share_code[i] = alpha[x % 24];
        x /= 24;
    }
    share_code[4] = 0;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
    {
        MessageBoxW(parent, L"Could not create the web server socket.", L"ALTEK Link",
                    MB_ICONERROR);
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR ||
        listen(listener, 8) == SOCKET_ERROR)
    {
        closesocket(listener);
        listener = INVALID_SOCKET;
        MessageBoxW(parent, L"Could not start web sharing.", L"ALTEK Link", MB_ICONERROR);
        return;
    }
    len = sizeof(sa);
    getsockname(listener, (struct sockaddr *)&sa, &len);
    _snprintf(share_url, sizeof(share_url), "http://%s:%u/", ip, (unsigned)ntohs(sa.sin_port));
    share_url[sizeof(share_url) - 1] = 0;
    failures = 0;
    blocked_until = 0;
    webbridge_set_text("");
    InterlockedExchange(&running, 1);
    worker = (HANDLE)_beginthreadex(NULL, 0, serve_thread, NULL, 0, NULL);
    if (!worker)
    {
        web_sharing_stop();
        MessageBoxW(parent, L"Could not start web sharing.", L"ALTEK Link", MB_ICONERROR);
        return;
    }
    web_sharing_refresh_button();
    webbridge_window_open(parent);
}
