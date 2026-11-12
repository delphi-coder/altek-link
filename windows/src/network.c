#include "link.h"
#include <iphlpapi.h>
#include <mstcpip.h>

/*
 * Windows may report ICMP Port Unreachable errors from recvfrom() on a UDP
 * socket as WSAECONNRESET (10054). Some Windows 7 systems deliver this after
 * a discovery datagram is sent to a host that has no listener on that port.
 * Disable that behavior for the discovery socket. The receive loop below also
 * treats WSAECONNRESET as recoverable in case the option is unavailable.
 */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

static HANDLE listener_thread, discovery_thread, tick_thread;

char *envelope(const char *type, const char *fields)
{
    char *name, *out;
    size_t n;
    EnterCriticalSection(&app.lock);
    name = json_quote(app.name);
    LeaveCriticalSection(&app.lock);
    if (!name)
    {
        return NULL;
    }
    n = strlen(name) + strlen(fields) + 160;
    out = (char *)malloc(n);
    if (out)
    {
        _snprintf(out, n, "{\"v\":1,\"type\":\"%s\",\"name\":%s%s%s}", type, name,
                  fields[0] ? "," : "", fields);
        out[n - 1] = 0;
    }
    free(name);
    return out;
}

char *request(const char *peer, const char *body)
{
    struct peer p;
    struct tls *t;
    char *out = NULL;
    int n;
    struct json j;
    if (!body || !peer_copy(peer, &p) || p.blocked)
    {
        return NULL;
    }
    t = tls_connect(p.ip, p.id);
    if (!t)
    {
        return NULL;
    }
    if (wire_json(t, body))
    {
        out = (char *)wire_read(t, &n, MAX_JSON);
    }
    if (out && (!json_parse(&j, out) || !json_bool(&j, "ok")))
    {
        char reason[256] = "";
        if (json_parse(&j, out))
        {
            json_get(&j, "error", reason, sizeof(reason));
        }
        log_event("REFUSED", "%s", reason);
        free(out);
        out = NULL;
    }
    tls_close(t);
    return out;
}

static int reply(struct tls *t, int ok, const char *error)
{
    char *q = json_quote(error), b[512];
    int r;
    if (!q)
    {
        return 0;
    }
    _snprintf(b, sizeof(b), "{\"ok\":%s,\"error\":%s}", ok ? "true" : "false", q);
    b[sizeof(b) - 1] = 0;
    free(q);
    r = wire_json(t, b);
    return r;
}

static unsigned __stdcall handle(void *arg)
{
    SOCKET s = (SOCKET)(UINT_PTR)arg;
    struct tls *t = tls_accept(s);
    char *body = NULL;
    int n, index;
    struct json j;
    char type[32] = "", name[256] = "", id[40] = "", ip[64] = "";
    struct sockaddr_in a;
    int alen = sizeof(a);
    struct message *m;
    if (!t)
    {
        return 0;
    }
    getpeername(s, (struct sockaddr *)&a, &alen);
    copy_s(ip, 64, inet_ntoa(a.sin_addr));
    body = (char *)wire_read(t, &n, MAX_JSON);
    if (!body || !json_parse(&j, body) || json_num(&j, "v", 0) != 1 ||
        !json_get(&j, "type", type, 32))
    {
        goto done;
    }
    json_get(&j, "name", name, 256);
    index = peer_seen(t->peer, name, ip, 1);
    if (index < 0)
    {
        goto done;
    }
    if (!strcmp(type, "hello"))
    {
        char b[1024], *q = json_quote(app.name);
        if (q)
        {
            _snprintf(b, sizeof(b), "{\"ok\":true,\"name\":%s,\"id\":\"%s\",\"avatar\":\"%s\",\"file_stream\":true}", q,
                      app.id, app.avatar);
            b[sizeof(b) - 1] = 0;
            wire_json(t, b);
            free(q);
        }
        goto done;
    }
    if (app.peers[index].blocked)
    {
        reply(t, 0, "CONTACT_BLOCKED");
        goto done;
    }
    if (!strcmp(type, "profile"))
    {
        char known[65] = "", *r;
        json_get(&j, "known", known, 65);
        r = profile_response(known);
        if (r)
        {
            wire_json(t, r);
            free(r);
        }
        goto done;
    }
    json_get(&j, "id", id, 40);
    if (!valid_uuid(id))
    {
        reply(t, 0, "Invalid id");
        goto done;
    }
    if (!strcmp(type, "text"))
    {
        char text[32768];
        WCHAR w[8002];
        if (!json_get(&j, "text", text, sizeof(text)) ||
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, w, 8002) ||
            wcslen(w) > 8000)
        {
            reply(t, 0, "Text too long");
            goto done;
        }
        m = message_find(id, t->peer);
        if (!m)
        {
            m = message_add(id, t->peer, "text", text, 0);
            if (m)
            {
                EnterCriticalSection(&app.lock);
                app.peers[index].unread++;
                LeaveCriticalSection(&app.lock);
                notice("New message");
            }
        }
        reply(t, m != NULL, m ? "" : "Storage full");
        log_event("TEXT_RX", "peer=%.8s", t->peer);
        changed();
    }
    else if (!strcmp(type, "offer") || !strcmp(type, "offer_stream"))
    {
        char filename[768], hash[65] = "", kind[16] = "";
        uint64_t size;
        WCHAR w[180];
        int k;
        json_get(&j, "hash", hash, 65);
        json_get(&j, "filename", filename, sizeof(filename));
        json_get(&j, "kind", kind, 16);
        if (!json_u64(&j, "size", &size) || size > INT64_MAX || (!strcmp(type, "offer") && !valid_hex(hash, 64)))
        {
            reply(t, 0, "Invalid offer");
            goto done;
        }
        wide(filename, w, 180);
        for (k = 0; w[k]; k++)
        {
            if (w[k] < 32 || wcschr(L"<>:\"/\\|?*", w[k]))
            {
                w[k] = '_';
            }
        }
        if (!w[0])
        {
            wcscpy(w, L"attachment");
        }
        m = message_find(id, t->peer);
        if (!m)
        {
            m = message_add(id, t->peer, !strcmp(kind, "voice") ? "voice" : "file", filename, 0);
            if (m)
            {
                EnterCriticalSection(&app.lock);
                wcscpy(m->filename, w);
                copy_s(m->hash, 65, hash);
                m->size = size;
                copy_s(m->state, 40, "offered");
                message_save(m);
                app.peers[index].unread++;
                LeaveCriticalSection(&app.lock);
                notice("File offered - press Download in the message");
            }
        }
        reply(t, m != NULL, m ? "" : "Storage full");
        changed();
    }
    else if (!strcmp(type, "download"))
    {
        HANDLE f;
        DWORD got;
        unsigned char buf[262144];
        int ok = 1;
        uint64_t left;
        LARGE_INTEGER file_size, seek;
        uint64_t offset = 0, prefix;
        char offset_reply[128], stream_hash[65];
        HCRYPTPROV hp = 0;
        HCRYPTHASH hh = 0;
        int streaming;
        m = message_find(id, t->peer);
        if (!m || !m->mine || !strcmp(m->kind, "text"))
        {
            reply(t, 0, "File not authorized");
            goto done;
        }
        streaming = !m->hash[0];
        if (streaming && !json_bool(&j, "sha256_trailer"))
        {
            reply(t, 0, "Streaming digest required");
            goto done;
        }
        if (streaming && (!CryptAcquireContextW(&hp, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
                          !CryptCreateHash(hp, CALG_SHA_256, 0, 0, &hh)))
        {
            if (hp) CryptReleaseContext(hp, 0);
            reply(t, 0, "Digest unavailable");
            goto done;
        }
        f = CreateFileW(m->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE)
        {
            if (hh) CryptDestroyHash(hh);
            if (hp) CryptReleaseContext(hp, 0);
            reply(t, 0, "File missing");
            goto done;
        }
        left = m->size;
        if (!GetFileSizeEx(f, &file_size) || file_size.QuadPart < 0 ||
            (uint64_t)file_size.QuadPart != left)
        {
            CloseHandle(f);
            if (hh) CryptDestroyHash(hh);
            if (hp) CryptReleaseContext(hp, 0);
            reply(t, 0, "Source changed or missing");
            goto done;
        }
        if (!json_u64(&j, "offset", &offset)) offset = 0;
        if (offset > left)
        {
            CloseHandle(f);
            if (hh) CryptDestroyHash(hh);
            if (hp) CryptReleaseContext(hp, 0);
            reply(t, 0, "Invalid offset");
            goto done;
        }
        seek.QuadPart = streaming ? 0 : offset;
        _snprintf(offset_reply, sizeof(offset_reply), "{\"ok\":true,\"offset\":%I64u}", offset);
        if (!SetFilePointerEx(f, seek, NULL, FILE_BEGIN) || !wire_json(t, offset_reply))
        {
            CloseHandle(f);
            if (hh) CryptDestroyHash(hh);
            if (hp) CryptReleaseContext(hp, 0);
            goto done;
        }
        prefix = streaming ? offset : 0;
        while (prefix && app.running)
        {
            DWORD want = prefix > sizeof(buf) ? sizeof(buf) : (DWORD)prefix;
            if (!ReadFile(f, buf, want, &got, NULL) || got != want || !CryptHashData(hh, buf, got, 0)) { ok = 0; break; }
            prefix -= got;
        }
        left -= offset;
        while (ok && left && app.running)
        {
            DWORD want = left > sizeof(buf) ? sizeof(buf) : (DWORD)left;
            if (!ReadFile(f, buf, want, &got, NULL) || got != want ||
                (streaming && !CryptHashData(hh, buf, got, 0)) || !tls_write(t, buf, got))
            {
                ok = 0;
                break;
            }
            left -= got;
        }
        CloseHandle(f);
        if (streaming && ok && !left && !prefix)
        {
            if (!hash_finish(hh, stream_hash)) ok = 0;
            else
            {
                _snprintf(offset_reply, sizeof(offset_reply), "{\"hash\":\"%s\"}", stream_hash);
                if (!wire_json(t, offset_reply)) ok = 0;
            }
        }
        if (hh) CryptDestroyHash(hh);
        if (hp) CryptReleaseContext(hp, 0);
        if (ok && !left && !prefix)
        {
            DWORD ack_timeout = 4UL * 60UL * 60UL * 1000UL;
            char *ack;
            struct json aj;
            setsockopt(t->socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ack_timeout, sizeof(ack_timeout));
            ack = (char *)wire_read(t, &n, MAX_JSON);
            if (ack && json_parse(&aj, ack) && json_bool(&aj, "ok"))
            {
                EnterCriticalSection(&app.lock);
                copy_s(m->state, 40, "transferred");
                message_save(m);
                LeaveCriticalSection(&app.lock);
                changed();
            }
            free(ack);
        }
    }
    else if (call_request(t, &j, type))
    {
        free(body);
        return 0;
    }
done:
    free(body);
    tls_close(t);
    return 0;
}

static unsigned __stdcall listen_loop(void *arg)
{
    SOCKET s;
    (void)arg;
    while (app.running)
    {
        s = accept(app.listen_socket, NULL, NULL);
        if (s == INVALID_SOCKET)
        {
            break;
        }
        if (!start_job(handle, (void *)(UINT_PTR)s))
        {
            closesocket(s);
        }
    }
    return 0;
}

static unsigned __stdcall discovery_loop(void *arg)
{
    char b[2049], id[65], name[256], avatar[65], cert[1200], tag[32], ip[64];
    struct sockaddr_in a;
    int alen, n, i, wsa;
    struct json j;
    (void)arg;
    while (app.running)
    {
        alen = sizeof(a);
        n = recvfrom(app.discovery_socket, b, 2048, 0, (struct sockaddr *)&a, &alen);
        if (n == SOCKET_ERROR)
        {
            wsa = WSAGetLastError();
            if (!app.running)
            {
                break;
            }
            /* Expected on Windows when ICMP reports a UDP destination error. */
            if (wsa == WSAECONNRESET || wsa == WSAEINTR || wsa == WSAEWOULDBLOCK)
            {
                continue;
            }
            break;
        }
        if (n == 0)
        {
            continue;
        }
        b[n] = 0;
        copy_s(ip, sizeof(ip), inet_ntoa(a.sin_addr));
        if (!json_parse(&j, b) || json_num(&j, "v", 0) != 1 ||
            !json_get(&j, "app", tag, sizeof(tag)) || strcmp(tag, "ALTEK_LINK"))
        {
            continue;
        }
        id[0] = name[0] = avatar[0] = cert[0] = 0;
        json_get(&j, "id", id, sizeof(id));
        json_get(&j, "name", name, sizeof(name));
        json_get(&j, "avatar", avatar, sizeof(avatar));
        json_get(&j, "cert", cert, sizeof(cert));
        if (cert[0])
        {
            tls_trust_discovery_certificate(cert, id);
        }
        i = peer_seen(id, name, ip, 0);
        if (i < 0)
        {
            continue;
        }
        if (!avatar[0] || valid_hex(avatar, 64))
        {
            EnterCriticalSection(&app.lock);
            copy_s(app.peers[i].advertised, 65, avatar);
            LeaveCriticalSection(&app.lock);
            profile_sync(id);
        }
    }
    return 0;
}

static void announce(void)
{
    char b[2049], cert[1200], *q;
    char id[65], name[256], avatar[65], ips[MAX_PEERS][64];
    struct sockaddr_in a;
    ULONG bytes = 0;
    IP_ADAPTER_INFO *ad, *p;
    int i, ip_count = 0;
    int len;
    cert[0] = 0;
    tls_identity_certificate(cert, sizeof(cert));
    /* Never hold the global application lock while calling Winsock.  In
     * particular, UDP broadcast can occasionally block inside old Windows
     * network drivers.  UI painting also takes app.lock, so the old code
     * turned that network delay into a visible application/Desktop stall. */
    EnterCriticalSection(&app.lock);
    copy_s(id, sizeof(id), app.id);
    copy_s(name, sizeof(name), app.name);
    copy_s(avatar, sizeof(avatar), app.avatar);
    for (i = 0; i < app.peer_count && ip_count < MAX_PEERS; i++)
    {
        if (!app.peers[i].blocked)
        {
            copy_s(ips[ip_count++], sizeof(ips[0]), app.peers[i].ip);
        }
    }
    LeaveCriticalSection(&app.lock);
    q = json_quote(name);
    if (!q)
    {
        return;
    }
    _snprintf(
        b, sizeof(b),
        "{\"app\":\"ALTEK_LINK\",\"v\":1,\"id\":\"%s\",\"name\":%s,\"avatar\":\"%s\",\"cert\":\"%s\"}",
        id, q, avatar, cert);
    b[sizeof(b) - 1] = 0;
    free(q);
    len = (int)strlen(b);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(DISCOVERY_PORT);
    a.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(app.discovery_socket, b, len, 0, (struct sockaddr *)&a, sizeof(a));
    for (i = 0; i < ip_count; i++)
    {
        a.sin_addr.s_addr = inet_addr(ips[i]);
        sendto(app.discovery_socket, b, len, 0, (struct sockaddr *)&a, sizeof(a));
    }

    GetAdaptersInfo(NULL, &bytes);
    ad = (IP_ADAPTER_INFO *)malloc(bytes);
    if (ad && GetAdaptersInfo(ad, &bytes) == NO_ERROR)
    {
        for (p = ad; p; p = p->Next)
        {
            IP_ADDR_STRING *ip;
            for (ip = &p->IpAddressList; ip; ip = ip->Next)
            {
                unsigned long addr = inet_addr(ip->IpAddress.String),
                              mask = inet_addr(ip->IpMask.String);
                if (!addr || addr == INADDR_NONE)
                {
                    continue;
                }
                a.sin_addr.s_addr = addr | ~mask;
                sendto(app.discovery_socket, b, len, 0, (struct sockaddr *)&a, sizeof(a));
            }
            a.sin_addr.s_addr = inet_addr(p->GatewayList.IpAddress.String);
            if (a.sin_addr.s_addr && a.sin_addr.s_addr != INADDR_NONE)
            {
                sendto(app.discovery_socket, b, len, 0, (struct sockaddr *)&a, sizeof(a));
            }
        }
    }
    free(ad);
}

static unsigned __stdcall send_pending(void *arg)
{
    struct message *m = (struct message *)arg;
    char fields[MAX_JSON], *q, *body, *result;
    if (!strcmp(m->kind, "text"))
    {
        q = json_quote(m->text);
        if (!q)
        {
            goto done;
        }
        if (strlen(q) + 100 >= sizeof(fields))
        {
            free(q);
            goto done;
        }
        _snprintf(fields, sizeof(fields), "\"id\":\"%s\",\"text\":%s", m->id, q);
        fields[sizeof(fields) - 1] = 0;
        free(q);
        body = envelope("text", fields);
    }
    else
    {
        char fn[768];
        if (!m->hash[0])
        {
            struct json capabilities;
            char *hello_body = envelope("hello", ""), *hello_reply = request(m->peer, hello_body);
            free(hello_body);
            if (!hello_reply) goto done;
            if (!json_parse(&capabilities, hello_reply) || !json_bool(&capabilities, "file_stream"))
            {
                char legacy_hash[65];
                uint64_t size;
                free(hello_reply);
                if (!digest_file(m->path, legacy_hash, &size) || size != m->size) goto done;
                EnterCriticalSection(&app.lock);
                copy_s(m->hash, 65, legacy_hash);
                message_save(m);
                LeaveCriticalSection(&app.lock);
            }
            else free(hello_reply);
        }
        utf8(m->filename, fn, sizeof(fn));
        q = json_quote(fn);
        if (!q)
        {
            goto done;
        }
        _snprintf(fields, sizeof(fields),
                  "\"id\":\"%s\",\"kind\":\"%s\",\"filename\":%s,\"size\":%I64u,\"hash\":\"%s\","
                  "\"mime\":\"application/octet-stream\"",
                  m->id, m->kind, q, m->size, m->hash);
        fields[sizeof(fields) - 1] = 0;
        free(q);
        body = envelope(m->hash[0] ? "offer" : "offer_stream", fields);
    }
    if (!body)
    {
        goto done;
    }
    result = request(m->peer, body);
    free(body);
    if (result)
    {
        free(result);
        EnterCriticalSection(&app.lock);
        copy_s(m->state, 40, !strcmp(m->kind, "text") ? "delivered" : "offered");
        message_save(m);
        LeaveCriticalSection(&app.lock);
    }
done:
    EnterCriticalSection(&app.lock);
    m->busy = 0;
    LeaveCriticalSection(&app.lock);
    changed();
    return 0;
}

static void flush(void)
{
    int i;
    struct peer p;
    ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&app.lock);
    for (i = 0; i < app.message_count; i++)
    {
        struct message *m = app.messages[i];
        if (m->mine && !m->busy && !strcmp(m->state, "queued") && peer_copy(m->peer, &p) &&
            !p.blocked && p.seen && now - p.seen < 16000)
        {
            int k, previous = 0;
            for (k = 0; k < i; k++)
            {
                if (!strcmp(app.messages[k]->peer, m->peer) && app.messages[k]->mine &&
                    !strcmp(app.messages[k]->state, "queued"))
                {
                    previous = 1;
                    break;
                }
            }
            if (previous)
            {
                continue;
            }
            m->busy = 1;
            if (!start_job(send_pending, m))
            {
                m->busy = 0;
            }
        }
    }
    LeaveCriticalSection(&app.lock);
}

static unsigned __stdcall tick_loop(void *arg)
{
    int i;
    (void)arg;
    while (app.running)
    {
        announce();
        flush();
        call_tick();
        changed();
        for (i = 0; i < 20 && app.running; i++)
        {
            Sleep(200);
        }
    }
    return 0;
}

int network_start(void)
{
    struct sockaddr_in a;
    BOOL yes = TRUE;
    BOOL no = FALSE;
    DWORD returned = 0;
    app.listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    app.discovery_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (app.listen_socket == INVALID_SOCKET || app.discovery_socket == INVALID_SOCKET)
    {
        return 0;
    }
    setsockopt(app.listen_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes, sizeof(yes));
    setsockopt(app.discovery_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    setsockopt(app.discovery_socket, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));

    /* Prevent UDP ICMP errors from terminating discovery receive on Windows 7. */
    (void)WSAIoctl(app.discovery_socket, SIO_UDP_CONNRESET, &no, sizeof(no), NULL, 0,
                   &returned, NULL, NULL);

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    if (bind(app.listen_socket, (struct sockaddr *)&a, sizeof(a)) || listen(app.listen_socket, 16))
    {
        return 0;
    }
    a.sin_port = htons(DISCOVERY_PORT);
    if (bind(app.discovery_socket, (struct sockaddr *)&a, sizeof(a)))
    {
        return 0;
    }
    listener_thread = (HANDLE)_beginthreadex(NULL, 0, listen_loop, NULL, 0, NULL);
    discovery_thread = (HANDLE)_beginthreadex(NULL, 0, discovery_loop, NULL, 0, NULL);
    tick_thread = (HANDLE)_beginthreadex(NULL, 0, tick_loop, NULL, 0, NULL);
    if (!listener_thread || !discovery_thread || !tick_thread)
    {
        return 0;
    }
    return 1;
}

void network_stop(void)
{
    HANDLE threads[3];
    int i;
    app.running = 0;
    call_hangup();
    closesocket(app.listen_socket);
    closesocket(app.discovery_socket);
    tls_shutdown_all();
    threads[0] = listener_thread;
    threads[1] = discovery_thread;
    threads[2] = tick_thread;
    for (i = 0; i < 3; i++)
    {
        if (threads[i])
        {
            WaitForSingleObject(threads[i], 6000);
            CloseHandle(threads[i]);
        }
    }
    WaitForSingleObject(app.worker_event, 20000);
}

void send_text(const char *peer, const char *text)
{
    char id[40];
    WCHAR w[8002];
    if (!text[0] || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, w, 8002) ||
        wcslen(w) > 8000)
    {
        notice("Message exceeds 8000 characters");
        return;
    }
    uid(id);
    if (!message_add(id, peer, "text", text, 1))
    {
        notice("Could not save message");
    }
    changed();
    flush();
}

static unsigned __stdcall probe_job(void *arg)
{
    char *ip = (char *)arg, *req, *body;
    struct tls *t;
    int n;
    struct json j;
    char name[256] = "";
    t = tls_connect(ip, NULL);
    if (t)
    {
        req = envelope("hello", "");
        body = NULL;
        if (req && wire_json(t, req))
        {
            body = (char *)wire_read(t, &n, MAX_JSON);
        }
        if (body && json_parse(&j, body) && json_bool(&j, "ok"))
        {
            json_get(&j, "name", name, 256);
            peer_seen(t->peer, name, ip, 1);
        }
        else
        {
            notice("Peer did not answer");
        }
        free(req);
        free(body);
        tls_close(t);
    }
    else
    {
        notice("Could not connect to that IP");
    }
    free(ip);
    return 0;
}

void network_probe(const char *ip)
{
    char *s;
    if (inet_addr(ip) == INADDR_NONE)
    {
        notice("Enter a valid IPv4 address");
        return;
    }
    s = _strdup(ip);
    if (s && !start_job(probe_job, s))
    {
        free(s);
    }
}
