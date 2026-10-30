#include "link.h"
static void release(struct call *c)
{
    if (c && !InterlockedDecrement(&c->refs))
    {
        free(c);
    }
}
static void clear(struct call *c)
{
    EnterCriticalSection(&app.lock);
    InterlockedExchange(&c->stop, 1);
    if (c->tls)
    {
        shutdown(c->tls->socket, SD_BOTH);
    }
    if (app.call == c)
    {
        app.call = NULL;
        release(c);
    }
    LeaveCriticalSection(&app.lock);
    changed();
}
void call_finished(struct call *c)
{
    clear(c);
}
static unsigned __stdcall run_media(void *arg)
{
    struct call *c = (struct call *)arg;
    if (!strcmp(c->kind, "screen"))
    {
        screen_run(c);
    }
    else
    {
        media_run(c);
    }
    clear(c);
    release(c);
    return 0;
}
static unsigned __stdcall connect_media(void *arg)
{
    struct call *c = (struct call *)arg;
    struct tls *t = NULL;
    char fields[256], *req = NULL, *res = NULL;
    int n;
    struct json j;
    if (c->stop)
    {
        goto done;
    }
    t = tls_connect(c->ip, c->peer);
    if (!t)
    {
        goto done;
    }
    _snprintf(fields, sizeof(fields), "\"id\":\"%s\",\"token\":\"%s\"", c->id, c->token);
    req = envelope("media", fields);
    if (!req || !wire_json(t, req))
    {
        goto done;
    }
    res = (char *)wire_read(t, &n, MAX_JSON);
    if (!res || !json_parse(&j, res) || !json_bool(&j, "ok"))
    {
        goto done;
    }
    EnterCriticalSection(&app.lock);
    if (!c->stop)
    {
        c->tls = t;
        t = NULL;
        c->media_started = 1;
    }
    LeaveCriticalSection(&app.lock);
    if (c->tls)
    {
        changed();
        if (!strcmp(c->kind, "screen"))
        {
            screen_run(c);
        }
        else
        {
            media_run(c);
        }
    }
done:
    if (!c->stop && !c->media_started)
    {
        notice("Media connection failed");
    }
    tls_close(t);
    free(req);
    free(res);
    clear(c);
    release(c);
    return 0;
}
static unsigned __stdcall signal_job(void *arg)
{
    struct call *c = (struct call *)arg;
    char fields[320], *req, *res;
    _snprintf(fields, sizeof(fields), "\"id\":\"%s\",\"kind\":\"%s\",\"token\":\"%s\"", c->id,
              c->kind, c->token);
    req = envelope(c->outgoing ? "call" : "accept", fields);
    res = request(c->peer, req);
    free(req);
    if (!res)
    {
        if (!c->stop)
        {
            notice("Call refused or connection failed");
        }
        clear(c);
    }
    free(res);
    release(c);
    return 0;
}
static unsigned __stdcall hangup_job(void *arg)
{
    struct call *c = (struct call *)arg;
    char fields[128], *req, *res;
    _snprintf(fields, sizeof(fields), "\"id\":\"%s\"", c->id);
    req = envelope("hangup", fields);
    res = request(c->peer, req);
    free(req);
    free(res);
    release(c);
    return 0;
}
void call_dial(const char *peer, const char *kind)
{
    struct peer p;
    struct call *c;
    if (!peer_copy(peer, &p) || p.blocked || !p.seen || GetTickCount64() - p.seen > 16000)
    {
        notice("Contact is offline or blocked");
        return;
    }
    if (strcmp(kind, "audio") && !codec_available())
    {
        notice("OpenH264 DLL is missing or incompatible");
        return;
    }
    EnterCriticalSection(&app.lock);
    if (app.call)
    {
        LeaveCriticalSection(&app.lock);
        notice("Another call is active");
        return;
    }
    c = (struct call *)calloc(1, sizeof(*c));
    if (!c)
    {
        LeaveCriticalSection(&app.lock);
        return;
    }
    c->refs = 2;
    uid(c->id);
    copy_s(c->peer, 65, peer);
    copy_s(c->ip, 64, p.ip);
    copy_s(c->kind, 16, kind);
    c->outgoing = 1;
    c->started = GetTickCount64();
    if (!random_hex(c->token, 32))
    {
        free(c);
        LeaveCriticalSection(&app.lock);
        return;
    }
    app.call = c;
    if (!start_job(signal_job, c))
    {
        app.call = NULL;
        free(c);
    }
    LeaveCriticalSection(&app.lock);
    changed();
}
void call_answer(void)
{
    struct call *c;
    EnterCriticalSection(&app.lock);
    c = app.call;
    if (c && !c->outgoing && !c->accepted)
    {
        c->accepted = 1;
        InterlockedIncrement(&c->refs);
        if (!start_job(signal_job, c))
        {
            release(c);
            clear(c);
        }
    }
    LeaveCriticalSection(&app.lock);
    changed();
}
void call_hangup(void)
{
    struct call *c;
    EnterCriticalSection(&app.lock);
    c = app.call;
    if (c)
    {
        InterlockedIncrement(&c->refs);
        clear(c);
        if (!start_job(hangup_job, c))
        {
            release(c);
        }
    }
    LeaveCriticalSection(&app.lock);
}
void call_tick(void)
{
    EnterCriticalSection(&app.lock);
    if (app.call && !app.call->media_started && GetTickCount64() - app.call->started > 45000)
    {
        call_hangup();
    }
    LeaveCriticalSection(&app.lock);
}
int call_request(struct tls *t, struct json *j, const char *type)
{
    char id[40], token[65], kind[16];
    struct call *c;
    int handoff = 0;
    const char *error = NULL;
    json_get(j, "id", id, 40);
    json_get(j, "token", token, 65);
    json_get(j, "kind", kind, 16);
    EnterCriticalSection(&app.lock);
    c = app.call;
    if (!strcmp(type, "call"))
    {
        if (c)
        {
            error = "Busy";
        }
        else if (!strcmp(kind, "view"))
        {
            error = "CAMERA_NOT_ARMED";
        }
        else if ((strcmp(kind, "audio") && strcmp(kind, "video") && strcmp(kind, "screen")) ||
                 !valid_hex(token, 64))
        {
            error = "Bad call";
        }
        else if ((!strcmp(kind, "video") || !strcmp(kind, "screen")) && !codec_available())
        {
            error = "Video codec unavailable";
        }
        else
        {
            struct peer p;
            peer_copy(t->peer, &p);
            c = (struct call *)calloc(1, sizeof(*c));
            if (!c)
            {
                error = "Out of memory";
            }
            else
            {
                c->refs = 1;
                copy_s(c->id, 40, id);
                copy_s(c->token, 65, token);
                copy_s(c->kind, 16, kind);
                copy_s(c->peer, 65, t->peer);
                copy_s(c->ip, 64, p.ip);
                c->started = GetTickCount64();
                app.call = c;
                notice("Incoming call");
            }
        }
    }
    else if (!strcmp(type, "hangup"))
    {
        if (c && !strcmp(c->peer, t->peer) && !strcmp(c->id, id))
        {
            clear(c);
        }
    }
    else if (!strcmp(type, "accept") || !strcmp(type, "media"))
    {
        if (!c || strcmp(c->peer, t->peer) || strcmp(c->id, id) || strcmp(c->token, token) ||
            c->stop)
        {
            error = "No matching call";
        }
        else if (!strcmp(type, "accept"))
        {
            if (!c->outgoing || c->accepted)
            {
                error = "Call already accepted";
            }
            else
            {
                c->accepted = 1;
                wire_json(t, "{\"ok\":true}");
                InterlockedIncrement(&c->refs);
                if (!start_job(connect_media, c))
                {
                    release(c);
                    clear(c);
                }
                LeaveCriticalSection(&app.lock);
                changed();
                return 0;
            }
        }
        else if (c->outgoing || !c->accepted || c->tls)
        {
            error = "Media not authorized";
        }
        else
        {
            if (wire_json(t, "{\"ok\":true}"))
            {
                c->tls = t;
                c->media_started = 1;
                InterlockedIncrement(&c->refs);
                if (start_job(run_media, c))
                {
                    handoff = 1;
                }
                else
                {
                    c->tls = NULL;
                    c->media_started = 0;
                    release(c);
                    clear(c);
                }
            }
            LeaveCriticalSection(&app.lock);
            changed();
            return handoff;
        }
    }
    else
    {
        error = "Unknown operation";
    }
    LeaveCriticalSection(&app.lock);
    if (error)
    {
        char buf[160];
        _snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", error);
        wire_json(t, buf);
        log_event("CALL_REFUSED", "%s", error);
    }
    else
    {
        wire_json(t, "{\"ok\":true}");
    }
    changed();
    return 0;
}
