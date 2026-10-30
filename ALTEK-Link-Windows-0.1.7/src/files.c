#include "link.h"
struct file_job
{
    char peer[65];
    WCHAR path[MAX_PATH];
};
static unsigned __stdcall stage_file(void *arg)
{
    struct file_job *job = (struct file_job *)arg;
    char id[40], hash[65], name[768];
    WCHAR path[MAX_PATH], leaf[80];
    struct message *m;
    const WCHAR *base;
    unsigned long size;
    uid(id);
    _snwprintf(leaf, 80, L"sent\\%S", id);
    path_join(path, app.dir, leaf);
    if (!CopyFileW(job->path, path, TRUE) || !digest_file(path, hash, &size))
    {
        DeleteFileW(path);
        notice("Could not stage file (limit 100 MB)");
        free(job);
        return 0;
    }
    base = wcsrchr(job->path, L'\\');
    base = base ? base + 1 : job->path;
    utf8(base, name, sizeof(name));
    EnterCriticalSection(&app.lock);
    m = message_add(id, job->peer, "file", name, 1);
    if (m)
    {
        copy_s(m->hash, 65, hash);
        wcsncpy(m->filename, base, 179);
        wcscpy(m->path, path);
        m->size = size;
        message_save(m);
    }
    LeaveCriticalSection(&app.lock);
    if (!m)
    {
        DeleteFileW(path);
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
    unsigned long left;
    DWORD written;
    unsigned char b[32768];
    path[0] = temp[0] = 0;
    if (!peer_copy(m->peer, &p) || p.blocked)
    {
        goto done;
    }
    t = tls_connect(p.ip, p.id);
    if (!t)
    {
        goto done;
    }
    _snprintf(fields, sizeof(fields), "\"id\":\"%s\"", m->id);
    req = envelope("download", fields);
    if (!req || !wire_json(t, req))
    {
        goto done;
    }
    response = (char *)wire_read(t, &n, MAX_JSON);
    if (!response || !json_parse(&j, response) || !json_bool(&j, "ok") || m->size > MAX_FILE)
    {
        goto done;
    }
    _snwprintf(leaf, 250, L"received\\%S_%s", m->id, m->filename);
    leaf[249] = 0;
    path_join(path, app.dir, leaf);
    _snwprintf(temp, MAX_PATH, L"%s.part", path);
    temp[MAX_PATH - 1] = 0;
    f = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
    {
        goto done;
    }
    left = m->size;
    while (left && app.running)
    {
        DWORD want = left > sizeof(b) ? sizeof(b) : left;
        if (!tls_read(t, b, want) || !WriteFile(f, b, want, &written, NULL) || written != want)
        {
            goto done;
        }
        left -= want;
        EnterCriticalSection(&app.lock);
        _snprintf(m->state, 40, "receiving %lu%%",
                  (m->size - left) * 100 / (m->size ? m->size : 1));
        LeaveCriticalSection(&app.lock);
        changed();
    }
    CloseHandle(f);
    f = INVALID_HANDLE_VALUE;
    if (left || !digest_file(temp, hash, NULL) || strcmp(hash, m->hash) ||
        !MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING))
    {
        goto done;
    }
    ok = 1;
    wire_json(t, "{\"ok\":true}");
done:
    if (f != INVALID_HANDLE_VALUE)
    {
        CloseHandle(f);
    }
    if (!ok && temp[0])
    {
        DeleteFileW(temp);
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
    log_event("FILE_RX", "ok=%d bytes=%lu", ok, m->size);
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
    unsigned long size;
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
