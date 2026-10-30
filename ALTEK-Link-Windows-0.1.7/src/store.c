#include "link.h"
#include <shlobj.h>
static void ini_path(WCHAR *p)
{
    path_join(p, app.dir, L"settings.ini");
}
static void ini_int(const WCHAR *key, int v, const WCHAR *p)
{
    WCHAR s[40];
    _snwprintf(s, 40, L"%d", v);
    WritePrivateProfileStringW(L"settings", key, s, p);
}
void settings_save(void)
{
    WCHAR p[MAX_PATH], s[256];
    EnterCriticalSection(&app.lock);
    ini_path(p);
    wide(app.name, s, 256);
    WritePrivateProfileStringW(L"settings", L"name", s, p);
    wide(app.avatar, s, 256);
    WritePrivateProfileStringW(L"settings", L"avatar", s, p);
    ini_int(L"camera", app.camera_index, p);
    ini_int(L"width", app.width, p);
    ini_int(L"height", app.height, p);
    ini_int(L"fps", app.fps, p);
    ini_int(L"bitrate", app.bitrate, p);
    ini_int(L"screen_width", app.screen_width, p);
    ini_int(L"screen_height", app.screen_height, p);
    ini_int(L"screen_fps", app.screen_fps, p);
    ini_int(L"screen_bitrate", app.screen_bitrate, p);
    ini_int(L"screen_quality", app.screen_quality, p);
    ini_int(L"screen_iframe", app.screen_iframe, p);
    ini_int(L"screen_threads", app.screen_threads, p);
    ini_int(L"screen_frame_skip", app.screen_frame_skip, p);
    ini_int(L"rx_rotate", app.rx_rotate, p);
    ini_int(L"theme", app.theme, p);
    ini_int(L"minimize_screen_share", app.minimize_screen_share, p);
    LeaveCriticalSection(&app.lock);
}
void peers_save(void)
{
    WCHAR path[MAX_PATH];
    FILE *f;
    int i;
    char *name;
    path_join(path, app.dir, L"peers.tmp");
    EnterCriticalSection(&app.lock);
    f = _wfopen(path, L"wb");
    if (!f)
    {
        LeaveCriticalSection(&app.lock);
        return;
    }
    for (i = 0; i < app.peer_count; i++)
    {
        struct peer *p = &app.peers[i];
        name = json_quote(p->name);
        if (name)
        {
            fprintf(f,
                    "{\"id\":\"%s\",\"name\":%s,\"ip\":\"%s\",\"avatar\":\"%s\",\"blocked\":%s}\n",
                    p->id, name, p->ip, p->avatar, p->blocked ? "true" : "false");
            free(name);
        }
    }
    if (!fclose(f))
    {
        WCHAR dest[MAX_PATH];
        path_join(dest, app.dir, L"peers.jsonl");
        MoveFileExW(path, dest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    LeaveCriticalSection(&app.lock);
}
int peer_seen(const char *id, const char *name, const char *ip, int authenticated)
{
    int i, fresh = 0;
    if (!valid_hex(id, 64) || !strcmp(id, app.id))
    {
        return -1;
    }
    EnterCriticalSection(&app.lock);
    for (i = 0; i < app.peer_count; i++)
    {
        if (!strcmp(app.peers[i].id, id))
        {
            break;
        }
    }
    if (i == app.peer_count)
    {
        if (i == MAX_PEERS)
        {
            LeaveCriticalSection(&app.lock);
            return -1;
        }
        app.peer_count++;
        fresh = 1;
        copy_s(app.peers[i].id, 65, id);
    }
    if (!app.peers[i].blocked)
    {
        copy_s(app.peers[i].name, 256, name);
        copy_s(app.peers[i].ip, 64, ip);
        app.peers[i].seen = GetTickCount64();
    }
    if (fresh || authenticated)
    {
        peers_save();
    }
    LeaveCriticalSection(&app.lock);
    changed();
    return i;
}
int peer_copy(const char *id, struct peer *out)
{
    int i, ok = 0;
    EnterCriticalSection(&app.lock);
    for (i = 0; i < app.peer_count; i++)
    {
        if (!strcmp(app.peers[i].id, id))
        {
            *out = app.peers[i];
            ok = 1;
            break;
        }
    }
    LeaveCriticalSection(&app.lock);
    return ok;
}
struct message *message_find(const char *id, const char *peer)
{
    int i;
    struct message *m = NULL;
    EnterCriticalSection(&app.lock);
    for (i = 0; i < app.message_count; i++)
    {
        if (!strcmp(app.messages[i]->id, id) && !strcmp(app.messages[i]->peer, peer))
        {
            m = app.messages[i];
            break;
        }
    }
    LeaveCriticalSection(&app.lock);
    return m;
}
int message_save(struct message *m)
{
    WCHAR path[MAX_PATH], leaf[140];
    char path8[1024], file8[768], *text, *qp, *qf, *out;
    size_t cap;
    int ok = 0;
    EnterCriticalSection(&app.lock);
    text = json_quote(m->text);
    utf8(m->path, path8, sizeof(path8));
    utf8(m->filename, file8, sizeof(file8));
    qp = json_quote(path8);
    qf = json_quote(file8);
    if (!text || !qp || !qf)
    {
        free(text);
        free(qp);
        free(qf);
        LeaveCriticalSection(&app.lock);
        return 0;
    }
    cap = strlen(text) + strlen(qp) + strlen(qf) + 1024;
    out = (char *)malloc(cap);
    if (out)
    {
        _snprintf(
            out, cap,
            "{\"id\":\"%s\",\"peer\":\"%s\",\"kind\":\"%s\",\"text\":%s,\"mine\":%s,\"state\":\"%"
            "s\",\"path\":%s,\"filename\":%s,\"size\":%lu,\"hash\":\"%s\",\"created\":\"%I64u\"}",
            m->id, m->peer, m->kind, text, m->mine ? "true" : "false", m->state, qp, qf, m->size,
            m->hash, m->created);
        _snwprintf(leaf, 140, L"messages\\%S_%S.json", m->peer, m->id);
        path_join(path, app.dir, leaf);
        ok = atomic_write(path, out, (DWORD)strlen(out));
        if (!ok)
        {
            log_event("STORE_ERROR", "message write failed %lu", GetLastError());
        }
        free(out);
    }
    free(text);
    free(qp);
    free(qf);
    LeaveCriticalSection(&app.lock);
    return ok;
}
struct message *message_add(const char *id, const char *peer, const char *kind, const char *text,
                            int mine)
{
    struct message *m = NULL;
    EnterCriticalSection(&app.lock);
    if (app.message_count < MAX_MESSAGES && valid_uuid(id) && valid_hex(peer, 64) &&
        !message_find(id, peer))
    {
        m = (struct message *)calloc(1, sizeof(*m));
        if (m)
        {
            m->text = _strdup(text);
            if (!m->text)
            {
                free(m);
                m = NULL;
            }
            else
            {
                copy_s(m->id, 40, id);
                copy_s(m->peer, 65, peer);
                copy_s(m->kind, 16, kind);
                m->mine = mine;
                {
                    FILETIME ft;
                    ULARGE_INTEGER v;
                    GetSystemTimeAsFileTime(&ft);
                    v.LowPart = ft.dwLowDateTime;
                    v.HighPart = ft.dwHighDateTime;
                    m->created = v.QuadPart;
                }
                copy_s(m->state, 40, mine ? "queued" : "delivered");
                if (message_save(m))
                {
                    app.messages[app.message_count++] = m;
                }
                else
                {
                    free(m->text);
                    free(m);
                    m = NULL;
                }
            }
        }
    }
    LeaveCriticalSection(&app.lock);
    return m;
}
void store_init(void)
{
    WCHAR p[MAX_PATH], w[256], host[128];
    DWORD hn = 128;
    FILE *f;
    char line[2048];
    struct json j;
    WIN32_FIND_DATAW fd;
    HANDLE find;
    unsigned char *b;
    DWORD n;
    int i;
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, p);
    path_join(app.dir, p, L"ALTEK-Link");
    CreateDirectoryW(app.dir, NULL);
    path_join(p, app.dir, L"messages");
    CreateDirectoryW(p, NULL);
    path_join(p, app.dir, L"sent");
    CreateDirectoryW(p, NULL);
    path_join(p, app.dir, L"received");
    CreateDirectoryW(p, NULL);
    ini_path(p);
    GetComputerNameW(host, &hn);
    GetPrivateProfileStringW(L"settings", L"name", host, w, 256, p);
    utf8(w, app.name, 256);
    GetPrivateProfileStringW(L"settings", L"avatar", L"", w, 256, p);
    utf8(w, app.avatar, 65);
    if (app.avatar[0] && !valid_hex(app.avatar, 64))
    {
        app.avatar[0] = 0;
    }
    app.camera_index = GetPrivateProfileIntW(L"settings", L"camera", 0, p);
    app.width = GetPrivateProfileIntW(L"settings", L"width", 320, p);
    app.height = GetPrivateProfileIntW(L"settings", L"height", 240, p);
    app.fps = GetPrivateProfileIntW(L"settings", L"fps", 15, p);
    app.bitrate = GetPrivateProfileIntW(L"settings", L"bitrate", 4000000, p);
    app.screen_width = GetPrivateProfileIntW(L"settings", L"screen_width", 0, p);
    app.screen_height = GetPrivateProfileIntW(L"settings", L"screen_height", 0, p);
    app.screen_fps = GetPrivateProfileIntW(L"settings", L"screen_fps", 4, p);
    app.screen_bitrate = GetPrivateProfileIntW(L"settings", L"screen_bitrate", 4000000, p);
    app.screen_quality = GetPrivateProfileIntW(L"settings", L"screen_quality", 1, p);
    app.screen_iframe =
        GetPrivateProfileIntW(L"settings", L"screen_iframe", app.screen_fps, p);
    app.screen_threads = GetPrivateProfileIntW(L"settings", L"screen_threads", 1, p);
    app.screen_frame_skip =
        GetPrivateProfileIntW(L"settings", L"screen_frame_skip", 1, p);
    app.rx_rotate = GetPrivateProfileIntW(L"settings", L"rx_rotate", 90, p);
    app.theme = GetPrivateProfileIntW(L"settings", L"theme", 0, p);
    app.minimize_screen_share =
        GetPrivateProfileIntW(L"settings", L"minimize_screen_share", 1, p) ? 1 : 0;
    if (app.theme < 0 || app.theme > 1)
    {
        app.theme = 0;
    }
    if (app.width < 16 || app.width > 1920 || app.height < 16 || app.height > 1080 ||
        app.width % 2 || app.height % 2)
    {
        app.width = 320;
        app.height = 240;
    }
    if (app.fps < 1 || app.fps > 60)
    {
        app.fps = 15;
    }
    if (app.bitrate < 50000 || app.bitrate > 20000000)
    {
        app.bitrate = 4000000;
    }
    if ((app.screen_width == 0) != (app.screen_height == 0) || app.screen_width < 0 ||
        app.screen_width > 7680 || app.screen_height < 0 || app.screen_height > 4320 ||
        (app.screen_width && (app.screen_width < 16 || app.screen_height < 16 ||
                              app.screen_width % 2 || app.screen_height % 2)))
    {
        app.screen_width = 0;
        app.screen_height = 0;
    }
    if (app.screen_fps < 1 || app.screen_fps > 60)
    {
        app.screen_fps = 4;
    }
    if (app.screen_bitrate < 50000 || app.screen_bitrate > 20000000)
    {
        app.screen_bitrate = 4000000;
    }
    if (app.screen_quality < 0 || app.screen_quality > 1)
    {
        app.screen_quality = 1;
    }
    if (app.screen_iframe < 1 || app.screen_iframe > 3600)
    {
        app.screen_iframe = app.screen_fps;
    }
    if (app.screen_threads < 0 || app.screen_threads > 16)
    {
        app.screen_threads = 1;
    }
    if (app.screen_frame_skip < 0 || app.screen_frame_skip > 1)
    {
        app.screen_frame_skip = 1;
    }
    /* Persist defaults and normalized values so all screen controls are visible in settings.ini. */
    settings_save();
    path_join(p, app.dir, L"peers.jsonl");
    f = _wfopen(p, L"rb");
    if (f)
    {
        while (fgets(line, sizeof(line), f) && app.peer_count < MAX_PEERS)
        {
            struct peer *q = &app.peers[app.peer_count];
            if (!json_parse(&j, line) || !json_get(&j, "id", q->id, 65) || !valid_hex(q->id, 64))
            {
                continue;
            }
            json_get(&j, "name", q->name, 256);
            json_get(&j, "ip", q->ip, 64);
            json_get(&j, "avatar", q->avatar, 65);
            q->blocked = json_bool(&j, "blocked");
            app.peer_count++;
        }
        fclose(f);
    }
    path_join(p, app.dir, L"messages\\*.json");
    find = FindFirstFileW(p, &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            WCHAR dir[MAX_PATH];
            struct message *m;
            char text[32768], temp[1024], id[40], peer[65], kind[16];
            path_join(dir, app.dir, L"messages");
            path_join(p, dir, fd.cFileName);
            if (!read_file(p, &b, &n, MAX_JSON * 4))
            {
                continue;
            }
            if (json_parse(&j, (char *)b) && json_get(&j, "id", id, 40) &&
                json_get(&j, "peer", peer, 65) && json_get(&j, "kind", kind, 16) &&
                json_get(&j, "text", text, sizeof(text)) && valid_uuid(id) && valid_hex(peer, 64) &&
                app.message_count < MAX_MESSAGES)
            {
                m = (struct message *)calloc(1, sizeof(*m));
                if (m)
                {
                    m->text = _strdup(text);
                    copy_s(m->id, 40, id);
                    copy_s(m->peer, 65, peer);
                    copy_s(m->kind, 16, kind);
                    m->mine = json_bool(&j, "mine");
                    json_get(&j, "state", m->state, 40);
                    json_get(&j, "path", temp, sizeof(temp));
                    wide(temp, m->path, MAX_PATH);
                    json_get(&j, "filename", temp, sizeof(temp));
                    wide(temp, m->filename, 180);
                    json_get(&j, "hash", m->hash, 65);
                    m->size = json_num(&j, "size", 0);
                    json_get(&j, "created", temp, sizeof(temp));
                    m->created = _strtoui64(temp, NULL, 10);
                    if (m->text)
                    {
                        app.messages[app.message_count++] = m;
                    }
                    else
                    {
                        free(m);
                    }
                }
            }
            free(b);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    for (i = 1; i < app.message_count; i++)
    {
        int k = i - 1;
        struct message *m = app.messages[i];
        while (k >= 0 && app.messages[k]->created > m->created)
        {
            app.messages[k + 1] = app.messages[k];
            k--;
        }
        app.messages[k + 1] = m;
    }
    for (i = 0; i < app.peer_count; i++)
    {
        app.peers[i].seen = 0;
    }
}
