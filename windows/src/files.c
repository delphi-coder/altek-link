#include "link.h"
struct file_job
{
    char peer[65];
    WCHAR path[MAX_PATH];
};
static unsigned __stdcall stage_file(void *arg)
{
    struct file_job *job = (struct file_job *)arg;
    char id[40], name[768];
    struct message *m;
    const WCHAR *base;
    uint64_t size;
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    uid(id);
    if (!GetFileAttributesExW(job->path, GetFileExInfoStandard, &attrs) ||
        (attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        notice("Could not read the source file");
        free(job);
        return 0;
    }
    size = ((uint64_t)attrs.nFileSizeHigh << 32) | attrs.nFileSizeLow;
    base = wcsrchr(job->path, L'\\');
    base = base ? base + 1 : job->path;
    utf8(base, name, sizeof(name));
    EnterCriticalSection(&app.lock);
    m = message_add(id, job->peer, "file", name, 1);
    if (m)
    {
        m->hash[0] = 0;
        wcsncpy(m->filename, base, 179);
        wcsncpy(m->path, job->path, MAX_PATH - 1);
        m->size = size;
        message_save(m);
    }
    LeaveCriticalSection(&app.lock);
    if (!m)
    {
        notice("Message storage full");
    }
    free(job);
    changed();
    return 0;
}
void send_file(const char *peer, const WCHAR *path)
{
    struct file_job *j = (struct file_job *)calloc(1, sizeof(*j));
    if (!j)
    {
        return;
    }
    copy_s(j->peer, 65, peer);
    wcsncpy(j->path, path, MAX_PATH - 1);
    if (!start_job(stage_file, j))
    {
        free(j);
    }
}
static unsigned __stdcall download_job(void *arg)
{
    struct message *m = (struct message *)arg;
    struct peer p;
    struct tls *t = NULL;
    char fields[160], *req = NULL, *response = NULL, hash[65];
    int n, ok = 0;
    struct json j;
    WCHAR path[MAX_PATH], temp[MAX_PATH], leaf[250];
    HANDLE f = INVALID_HANDLE_VALUE;
    uint64_t left, offset = 0, acknowledged = 0;
    LARGE_INTEGER position;
    ULARGE_INTEGER available;
    WCHAR destination[MAX_PATH];
    DWORD written, got;
    HCRYPTPROV hp = 0;
    HCRYPTHASH hh = 0;
    unsigned char b[262144];
    DWORD last_update = 0;
    path[0] = temp[0] = 0;
    if (!peer_copy(m->peer, &p) || p.blocked)
    {
        goto done;
    }
    _snwprintf(leaf, 250, L"%S_%s", m->id, m->filename);
    leaf[249] = 0;
    EnterCriticalSection(&app.lock);
    if (m->receive_dir[0])
        wcsncpy(destination, m->receive_dir, MAX_PATH - 1);
    else if (app.receive_dir[0])
        wcsncpy(destination, app.receive_dir, MAX_PATH - 1);
    else
        path_join(destination, app.dir, L"received");
    destination[MAX_PATH - 1] = 0;
    if (!m->receive_dir[0])
    {
        wcsncpy(m->receive_dir, destination, MAX_PATH - 1);
        message_save(m);
    }
    LeaveCriticalSection(&app.lock);
    if (wcslen(destination) + wcslen(leaf) + 7 >= MAX_PATH)
    {
        notice("Receive folder path is too long");
        goto done;
    }
    path_join(path, destination, leaf);
    _snwprintf(temp, MAX_PATH, L"%s.part", path);
    temp[MAX_PATH - 1] = 0;
    f = CreateFileW(temp, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &position) || position.QuadPart < 0)
        goto done;
    offset = (uint64_t)position.QuadPart;
    if (offset > m->size) offset = 0;
    if (GetDiskFreeSpaceExW(destination, &available, NULL, NULL) &&
        available.QuadPart < m->size - offset)
    {
        notice("Not enough free space in the receive folder");
        goto done;
    }
    if (!CryptAcquireContextW(&hp, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(hp, CALG_SHA_256, 0, 0, &hh)) goto done;
    left = offset;
    position.QuadPart = 0;
    if (!SetFilePointerEx(f, position, NULL, FILE_BEGIN)) goto done;
    while (left)
    {
        DWORD want = left > sizeof(b) ? sizeof(b) : (DWORD)left;
        if (!ReadFile(f, b, want, &got, NULL) || got != want || !CryptHashData(hh, b, got, 0)) goto done;
        left -= got;
    }
    t = tls_connect(p.ip, p.id);
    if (!t) goto done;
    { DWORD timeout = 4UL * 60UL * 60UL * 1000UL; setsockopt(t->socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)); }
    _snprintf(fields, sizeof(fields), "\"id\":\"%s\",\"offset\":%I64u,\"sha256_trailer\":%s", m->id, offset, m->hash[0] ? "false" : "true");
    req = envelope("download", fields);
    if (!req || !wire_json(t, req)) goto done;
    response = (char *)wire_read(t, &n, MAX_JSON);
    if (!response || !json_parse(&j, response) || !json_bool(&j, "ok")) goto done;
    if (!json_u64(&j, "offset", &acknowledged)) acknowledged = 0;
    if (acknowledged != offset)
    {
        if (acknowledged != 0) goto done;
        offset = 0;
        CryptDestroyHash(hh); hh = 0;
        if (!CryptCreateHash(hp, CALG_SHA_256, 0, 0, &hh)) goto done;
        if (GetDiskFreeSpaceExW(destination, &available, NULL, NULL) && available.QuadPart < m->size)
        {
            notice("Not enough free space in the receive folder");
            goto done;
        }
    }
    position.QuadPart = offset;
    if (!SetFilePointerEx(f, position, NULL, FILE_BEGIN) || !SetEndOfFile(f)) goto done;
    left = m->size - offset;
    while (left && app.running)
    {
        DWORD want = left > sizeof(b) ? sizeof(b) : (DWORD)left;
        if (!tls_read(t, b, want) || !CryptHashData(hh, b, want, 0) || !WriteFile(f, b, want, &written, NULL) || written != want)
        {
            goto done;
        }
        left -= want;
        if (!last_update || GetTickCount() - last_update >= 1000 || !left)
        {
            EnterCriticalSection(&app.lock);
            _snprintf(m->state, 40, "receiving %lu%%",
                      (unsigned long)((double)(m->size - left) * 100.0 / (double)(m->size ? m->size : 1)));
            LeaveCriticalSection(&app.lock);
            changed();
            last_update = GetTickCount();
        }
    }
    CloseHandle(f);
    f = INVALID_HANDLE_VALUE;
    if (left || !hash_finish(hh, hash)) goto done;
    if (!m->hash[0])
    {
        free(response);
        response = (char *)wire_read(t, &n, MAX_JSON);
        if (!response || !json_parse(&j, response) || !json_get(&j, "hash", fields, sizeof(fields)) || !valid_hex(fields, 64)) goto done;
    }
    else copy_s(fields, sizeof(fields), m->hash);
    if (strcmp(hash, fields))
    {
        DeleteFileW(temp);
        notice("File checksum mismatch; retry from beginning");
        goto done;
    }
    {
        WCHAR base[180], ext[180], candidate[250];
        WCHAR *dot;
        unsigned int index;
        wcsncpy(base, m->filename, 179); base[179] = 0;
        ext[0] = 0;
        dot = wcsrchr(base, L'.');
        if (dot && dot != base) { wcscpy(ext, dot); *dot = 0; }
        for (index = 0; index < 100000; index++)
        {
            if (index) _snwprintf(candidate, 250, L"%s (%u)%s", base, index, ext);
            else _snwprintf(candidate, 250, L"%s%s", base, ext);
            candidate[249] = 0;
            if (wcslen(destination) + wcslen(candidate) + 2 >= MAX_PATH) goto done;
            path_join(path, destination, candidate);
            if (MoveFileExW(temp, path, 0)) break;
            if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) goto done;
        }
        if (index == 100000) goto done;
    }
    ok = 1;
    wire_json(t, "{\"ok\":true}");
done:
    if (hh) CryptDestroyHash(hh);
    if (hp) CryptReleaseContext(hp, 0);
    if (f != INVALID_HANDLE_VALUE)
    {
        CloseHandle(f);
    }
    tls_close(t);
    free(req);
    free(response);
    EnterCriticalSection(&app.lock);
    m->busy = 0;
    copy_s(m->state, 40, ok ? "downloaded" : "retry download");
    if (ok)
    {
        wcscpy(m->path, path);
    }
    message_save(m);
    LeaveCriticalSection(&app.lock);
    log_event("FILE_RX", "ok=%d bytes=%I64u", ok, m->size);
    if (!ok)
    {
        notice("File download failed; press Retry download in the message");
    }
    changed();
    return 0;
}
void receive_file(struct message *m)
{
    EnterCriticalSection(&app.lock);
    if (!m->busy && !m->mine)
    {
        m->busy = 1;
        if (!start_job(download_job, m))
        {
            m->busy = 0;
        }
    }
    LeaveCriticalSection(&app.lock);
}
static void avatar_path(WCHAR *out, const char *hash)
{
    WCHAR leaf[100];
    _snwprintf(leaf, 100, L"avatar_%S.jpg", hash);
    path_join(out, app.dir, leaf);
}
char *profile_response(const char *known)
{
    WCHAR p[MAX_PATH];
    char hash[65], *out;
    unsigned char *b = NULL;
    DWORD n = 0, size = 0;
    EnterCriticalSection(&app.lock);
    copy_s(hash, 65, app.avatar);
    LeaveCriticalSection(&app.lock);
    if (!hash[0] || !strcmp(hash, known))
    {
        out = (char *)malloc(120);
        if (out)
        {
            sprintf(out, "{\"ok\":true,\"hash\":\"%s\"}", hash);
        }
        return out;
    }
    avatar_path(p, hash);
    if (!read_file(p, &b, &n, 40000))
    {
        return _strdup("{\"ok\":false,\"error\":\"Profile missing\"}");
    }
    CryptBinaryToStringA(b, n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &size);
    out = (char *)malloc(size + 160);
    if (out)
    {
        int prefix = sprintf(out, "{\"ok\":true,\"hash\":\"%s\",\"image\":\"", hash);
        if (CryptBinaryToStringA(b, n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out + prefix,
                                 &size))
        {
            strcat(out, "\"}");
        }
        else
        {
            free(out);
            out = NULL;
        }
    }
    free(b);
    return out;
}
static unsigned __stdcall profile_job(void *arg)
{
    char *id = (char *)arg, fields[160], *req, *res, hash[65], *encoded;
    struct peer p;
    struct json j;
    int ok = 0, i;
    WCHAR path[MAX_PATH];
    DWORD n;
    unsigned char *bytes;
    char check[65];
    if (!peer_copy(id, &p))
    {
        goto done;
    }
    _snprintf(fields, sizeof(fields), "\"known\":\"%s\"", p.avatar);
    req = envelope("profile", fields);
    res = request(id, req);
    free(req);
    if (!res)
    {
        goto done;
    }
    if (!json_parse(&j, res) || !json_get(&j, "hash", hash, 65))
    {
        goto free_response;
    }
    if (!hash[0])
    {
        ok = 1;
    }
    else if (valid_hex(hash, 64))
    {
        avatar_path(path, hash);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        {
            ok = 1;
        }
        else
        {
            encoded = (char *)malloc(56000);
            bytes = (unsigned char *)malloc(40000);
            n = 40000;
            if (encoded && bytes && json_get(&j, "image", encoded, 56000) &&
                CryptStringToBinaryA(encoded, 0, CRYPT_STRING_BASE64, bytes, &n, NULL, NULL) &&
                n <= 40000 && digest(bytes, n, check) && !strcmp(hash, check))
            {
                unsigned char *decoded = NULL;
                int dw = 0, dh = 0;
                if (atomic_write(path, bytes, n))
                {
                    ok = image_load(path, &decoded, &dw, &dh) && dw == 256 && dh == 256;
                    free(decoded);
                    if (!ok)
                    {
                        DeleteFileW(path);
                    }
                }
            }
            free(encoded);
            free(bytes);
        }
    }
    if (ok)
    {
        EnterCriticalSection(&app.lock);
        for (i = 0; i < app.peer_count; i++)
        {
            if (!strcmp(app.peers[i].id, id))
            {
                copy_s(app.peers[i].avatar, 65, hash);
                break;
            }
        }
        peers_save();
        LeaveCriticalSection(&app.lock);
        changed();
    }
free_response:
    free(res);
done:
    EnterCriticalSection(&app.lock);
    for (i = 0; i < app.peer_count; i++)
    {
        if (!strcmp(app.peers[i].id, id))
        {
            app.peers[i].profile_busy = 0;
        }
    }
    LeaveCriticalSection(&app.lock);
    free(id);
    return 0;
}
void profile_sync(const char *id)
{
    int i;
    ULONGLONG now = GetTickCount64();
    WCHAR path[MAX_PATH];
    char *s;
    EnterCriticalSection(&app.lock);
    for (i = 0; i < app.peer_count; i++)
    {
        if (!strcmp(app.peers[i].id, id))
        {
            break;
        }
    }
    if (i < app.peer_count)
    {
        struct peer *p = &app.peers[i];
        avatar_path(path, p->avatar);
        if (!p->blocked && !p->profile_busy && (!p->profile_try || now - p->profile_try >= 15000) &&
            (strcmp(p->avatar, p->advertised) ||
             (p->avatar[0] && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)))
        {
            s = _strdup(id);
            if (s)
            {
                p->profile_try = now;
                p->profile_busy = 1;
                if (!start_job(profile_job, s))
                {
                    p->profile_busy = 0;
                    free(s);
                }
            }
        }
    }
    LeaveCriticalSection(&app.lock);
}
int profile_set(const WCHAR *source)
{
    WCHAR temp[MAX_PATH], path[MAX_PATH];
    char hash[65];
    uint64_t size;
    int ok = 0;
    path_join(temp, app.dir, L"avatar-new.jpg");
    if (image_avatar(source, temp) && digest_file(temp, hash, &size) && size <= 40000)
    {
        avatar_path(path, hash);
        if (MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING))
        {
            EnterCriticalSection(&app.lock);
            copy_s(app.avatar, 65, hash);
            settings_save();
            LeaveCriticalSection(&app.lock);
            ok = 1;
        }
    }
    DeleteFileW(temp);
    changed();
    return ok;
}
