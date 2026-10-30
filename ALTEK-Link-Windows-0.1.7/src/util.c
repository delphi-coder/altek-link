#include "link.h"
#include <stdarg.h>
#include <shlobj.h>
#ifdef ALTEK_DEBUG_UDP_LOG
#include "build_config.h"
#endif
struct app app;
void copy_s(char *out, size_t cap, const char *in)
{
    if (!cap)
    {
        return;
    }
    strncpy(out, in ? in : "", cap - 1);
    out[cap - 1] = 0;
}
void utf8(const WCHAR *in, char *out, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, in, -1, out, cap, NULL, NULL))
    {
        out[0] = 0;
    }
}
void wide(const char *in, WCHAR *out, int cap)
{
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, out, cap))
    {
        out[0] = 0;
    }
}
void path_join(WCHAR *out, const WCHAR *base, const WCHAR *leaf)
{
    _snwprintf(out, MAX_PATH - 1, L"%s\\%s", base, leaf);
    out[MAX_PATH - 1] = 0;
}
void uid(char *out)
{
    GUID g;
    WCHAR w[40];
    CoCreateGuid(&g);
    StringFromGUID2(&g, w, 40);
    w[37] = 0;
    utf8(w + 1, out, 40);
}
int random_hex(char *out, int bytes)
{
    HCRYPTPROV p = 0;
    unsigned char b[64];
    int i, ok = 0;
    if (bytes > 64)
    {
        return 0;
    }
    if (CryptAcquireContextW(&p, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
    {
        ok = CryptGenRandom(p, bytes, b);
        CryptReleaseContext(p, 0);
    }
    if (!ok)
    {
        return 0;
    }
    for (i = 0; i < bytes; i++)
    {
        sprintf(out + i * 2, "%02x", b[i]);
    }
    return 1;
}
int valid_hex(const char *s, int n)
{
    int i;
    if ((int)strlen(s) != n)
    {
        return 0;
    }
    for (i = 0; i < n; i++)
    {
        if (!strchr("0123456789abcdef", s[i]))
        {
            return 0;
        }
    }
    return 1;
}
int valid_uuid(const char *s)
{
    int i;
    if (strlen(s) != 36)
    {
        return 0;
    }
    for (i = 0; i < 36; i++)
    {
        if (!strchr("0123456789abcdefABCDEF-", s[i]))
        {
            return 0;
        }
    }
    return 1;
}
static int hash_finish(HCRYPTHASH h, char *hex)
{
    unsigned char b[32];
    DWORD n = 32;
    int i;
    if (!CryptGetHashParam(h, HP_HASHVAL, b, &n, 0))
    {
        return 0;
    }
    for (i = 0; i < 32; i++)
    {
        sprintf(hex + 2 * i, "%02x", b[i]);
    }
    return 1;
}
int digest(const void *data, DWORD size, char *hex)
{
    HCRYPTPROV p = 0;
    HCRYPTHASH h = 0;
    int ok = 0;
    if (CryptAcquireContextW(&p, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(p, CALG_SHA_256, 0, 0, &h) && CryptHashData(h, (const BYTE *)data, size, 0))
    {
        ok = hash_finish(h, hex);
    }
    if (h)
    {
        CryptDestroyHash(h);
    }
    if (p)
    {
        CryptReleaseContext(p, 0);
    }
    return ok;
}
int digest_file(const WCHAR *path, char *hex, unsigned long *size)
{
    HANDLE f;
    HCRYPTPROV p = 0;
    HCRYPTHASH h = 0;
    int ok = 0;
    DWORD n, total = 0;
    unsigned char b[32768];
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    if (!CryptAcquireContextW(&p, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(p, CALG_SHA_256, 0, 0, &h))
    {
        goto done;
    }
    for (;;)
    {
        if (!ReadFile(f, b, sizeof(b), &n, NULL))
        {
            goto done;
        }
        if (!n)
        {
            break;
        }
        if (n > MAX_FILE - total)
        {
            goto done;
        }
        total += n;
        if (!CryptHashData(h, b, n, 0))
        {
            goto done;
        }
    }
    ok = hash_finish(h, hex);
    if (size)
    {
        *size = total;
    }
done:
    CloseHandle(f);
    if (h)
    {
        CryptDestroyHash(h);
    }
    if (p)
    {
        CryptReleaseContext(p, 0);
    }
    return ok;
}
unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           p[3];
}
void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}
void put64(unsigned char *p, uint64_t v)
{
    put32(p, (unsigned long)(v >> 32));
    put32(p + 4, (unsigned long)v);
}
void changed(void)
{
    if (app.window)
    {
        PostMessageW(app.window, UI_UPDATE, 0, 0);
    }
}
void notice(const char *text)
{
    char *s = _strdup(text);
    log_event("NOTICE", "%s", text);
    if (!s)
    {
        return;
    }
    if (!app.window || !PostMessageW(app.window, UI_NOTICE, 0, (LPARAM)s))
    {
        free(s);
    }
}
void log_event(const char *tag, const char *fmt, ...)
{
#ifdef ALTEK_DEBUG_UDP_LOG
    char msg[1000], line[1400];
    WCHAR path[MAX_PATH];
    SYSTEMTIME st;
    va_list va;
    HANDLE f;
    DWORD n;
    va_start(va, fmt);
    _vsnprintf(msg, sizeof(msg) - 1, fmt, va);
    va_end(va);
    msg[sizeof(msg) - 1] = 0;
    GetLocalTime(&st);
    _snprintf(line, sizeof(line) - 1, "%02u:%02u:%02u.%03u [%.8s %s] %s %s\r\n", st.wHour,
              st.wMinute, st.wSecond, st.wMilliseconds, app.id, app.name, tag, msg);
    line[sizeof(line) - 1] = 0;
    OutputDebugStringA(line);
    EnterCriticalSection(&app.lock);
    path_join(path, app.dir, L"debug.log");
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE)
    {
        if (GetFileSize(f, NULL) > 2 * 1024 * 1024)
        {
            CloseHandle(f);
            f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        }
        if (f != INVALID_HANDLE_VALUE)
        {
            WriteFile(f, line, (DWORD)strlen(line), &n, NULL);
            CloseHandle(f);
        }
    }
    {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(LOG_PORT);
        a.sin_addr.s_addr = inet_addr(ALTEK_DEBUG_UDP_HOST);
        if (s != INVALID_SOCKET)
        {
            sendto(s, line, (int)strlen(line), 0, (struct sockaddr *)&a, sizeof(a));
            closesocket(s);
        }
    }
    LeaveCriticalSection(&app.lock);
#else
    (void)tag;
    (void)fmt;
#endif
}
struct job
{
    unsigned(__stdcall *fn)(void *);
    void *arg;
};
static unsigned __stdcall job_run(void *arg)
{
    struct job *j = (struct job *)arg;
    unsigned(__stdcall * fn)(void *) = j->fn;
    void *p = j->arg;
    free(j);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    fn(p);
    CoUninitialize();
    if (!InterlockedDecrement(&app.workers))
    {
        SetEvent(app.worker_event);
    }
    return 0;
}
int start_job(unsigned(__stdcall *fn)(void *), void *arg)
{
    struct job *j;
    uintptr_t h;
    if (!app.running)
    {
        return 0;
    }
    if (InterlockedIncrement(&app.workers) > 32)
    {
        InterlockedDecrement(&app.workers);
        return 0;
    }
    ResetEvent(app.worker_event);
    j = (struct job *)malloc(sizeof(*j));
    if (!j)
    {
        if (!InterlockedDecrement(&app.workers))
        {
            SetEvent(app.worker_event);
        }
        return 0;
    }
    j->fn = fn;
    j->arg = arg;
    h = _beginthreadex(NULL, 0, job_run, j, 0, NULL);
    if (!h)
    {
        free(j);
        if (!InterlockedDecrement(&app.workers))
        {
            SetEvent(app.worker_event);
        }
        return 0;
    }
    CloseHandle((HANDLE)h);
    return 1;
}
int atomic_write(const WCHAR *path, const void *data, DWORD len)
{
    WCHAR tmp[MAX_PATH];
    HANDLE f;
    DWORD n;
    int ok;
    _snwprintf(tmp, MAX_PATH, L"%s.tmp", path);
    tmp[MAX_PATH - 1] = 0;
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    ok = WriteFile(f, data, len, &n, NULL) && n == len && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok)
    {
        ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    if (!ok)
    {
        DeleteFileW(tmp);
    }
    return ok;
}
int read_file(const WCHAR *path, unsigned char **data, DWORD *len, DWORD max)
{
    HANDLE f;
    DWORD size, n;
    unsigned char *b;
    int ok;
    *data = NULL;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > max)
    {
        CloseHandle(f);
        return 0;
    }
    b = (unsigned char *)malloc(size + 1);
    if (!b)
    {
        CloseHandle(f);
        return 0;
    }
    ok = ReadFile(f, b, size, &n, NULL) && n == size;
    CloseHandle(f);
    if (!ok)
    {
        free(b);
        return 0;
    }
    b[size] = 0;
    *data = b;
    *len = size;
    return 1;
}
