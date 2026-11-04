#ifndef ALTEK_LINK_H
#define ALTEK_LINK_H
#ifdef _MSC_VER
#pragma warning(disable : 4005)
#endif
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE
#define COBJMACROS
#define SECURITY_WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <process.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
/* Older MinGW-w64 headers (TDM-GCC) lack the TLS 1.1/1.2 Schannel protocol flags. */
#ifndef SP_PROT_TLS1_1_SERVER
#define SP_PROT_TLS1_1_SERVER 0x00000100
#endif
#ifndef SP_PROT_TLS1_1_CLIENT
#define SP_PROT_TLS1_1_CLIENT 0x00000200
#endif
#ifndef SP_PROT_TLS1_2_SERVER
#define SP_PROT_TLS1_2_SERVER 0x00000400
#endif
#ifndef SP_PROT_TLS1_2_CLIENT
#define SP_PROT_TLS1_2_CLIENT 0x00000800
#endif
#ifndef SCH_CRED_DISABLE_RECONNECTS
#define SCH_CRED_DISABLE_RECONNECTS 0x00000080
#endif
#if defined(ALTEK_EXPECT_X64) && !defined(_WIN64)
#error Wrong compiler environment: x64 was requested.
#endif
#if defined(ALTEK_EXPECT_X86) && defined(_WIN64)
#error Wrong compiler environment: x86 was requested.
#endif
#include "build_number.h"
#define APP_VERSION "0.1.7"
#define ALTEK_TEXT_INNER(value) #value
#define ALTEK_TEXT(value) ALTEK_TEXT_INNER(value)
#define APP_VERSION_BUILD APP_VERSION " (build " ALTEK_TEXT(APP_BUILD) ")"
#define PORT 45871
#define DISCOVERY_PORT 45872
#define LOG_PORT 45879
#define MAX_PEERS 200
#define MAX_MESSAGES 10000
#define MAX_JSON 65536
#define MAX_MEDIA (2 * 1024 * 1024)
#define MAX_FILE (100 * 1024 * 1024)
#define UI_UPDATE (WM_APP + 1)
#define UI_NOTICE (WM_APP + 2)
#define UI_FRAME (WM_APP + 3)
#define UI_TRAY (WM_APP + 4)
#define UI_REMOTE_CURSOR (WM_APP + 5)
#define countof(x) (sizeof(x) / sizeof((x)[0]))
struct peer
{
    char id[65], name[256], ip[64], avatar[65], advertised[65];
    ULONGLONG seen, profile_try;
    int blocked, unread, profile_busy;
};
struct message
{
    char id[40], peer[65], kind[16], state[40], hash[65];
    char *text;
    WCHAR path[MAX_PATH], filename[180];
    unsigned long size;
    uint64_t created;
    int mine, busy;
};
struct tls
{
    SOCKET socket;
    CtxtHandle context;
    int context_ok;
    SecPkgContext_StreamSizes sizes;
    unsigned char encrypted[131072], plain[65536];
    int enc_len, plain_pos, plain_len;
    CRITICAL_SECTION write_lock;
    char peer[65];
    struct tls *next;
};
struct call
{
    char id[40], peer[65], ip[64], token[65], kind[16];
    int outgoing, accepted, media_started;
    ULONGLONG started;
    volatile LONG stop, mute, speaker_mute, refs;
    struct tls *tls;
    void *camera_source;
};
struct app
{
    CRITICAL_SECTION lock;
    volatile LONG running, workers;
    HANDLE worker_event;
    HWND window;
    HINSTANCE instance;
    WCHAR dir[MAX_PATH], exe_dir[MAX_PATH];
    char name[256], id[65], avatar[65];
    int camera_index, width, height, fps, bitrate, rx_rotate, theme;
    int minimize_screen_share;
    int screen_width, screen_height, screen_fps, screen_bitrate;
    int screen_quality, screen_iframe, screen_threads, screen_frame_skip;
    struct peer peers[MAX_PEERS];
    int peer_count, message_count;
    struct message *messages[MAX_MESSAGES];
    struct call *call;
    SOCKET listen_socket, discovery_socket;
};
extern struct app app;
#include "json.h"
void copy_s(char *out, size_t cap, const char *in);
void utf8(const WCHAR *in, char *out, int cap);
void wide(const char *in, WCHAR *out, int cap);
void path_join(WCHAR *out, const WCHAR *base, const WCHAR *leaf);
void uid(char *out);
int random_hex(char *out, int bytes);
int valid_hex(const char *s, int n);
int valid_uuid(const char *s);
int digest(const void *data, DWORD size, char *hex);
int digest_file(const WCHAR *path, char *hex, unsigned long *size);
void log_event(const char *tag, const char *fmt, ...);
void notice(const char *text);
void changed(void);
int start_job(unsigned(__stdcall *fn)(void *), void *arg);
unsigned long be32(const unsigned char *p);
void put32(unsigned char *p, unsigned long v);
void put64(unsigned char *p, uint64_t v);
int atomic_write(const WCHAR *path, const void *data, DWORD len);
int read_file(const WCHAR *path, unsigned char **data, DWORD *len, DWORD max);
void store_init(void);
void settings_save(void);
void peers_save(void);
int peer_seen(const char *id, const char *name, const char *ip, int authenticated);
int peer_copy(const char *id, struct peer *out);
struct message *message_add(const char *id, const char *peer, const char *kind, const char *text,
                            int mine);
int message_save(struct message *m);
struct message *message_find(const char *id, const char *peer);
int tls_init(void);
void tls_cleanup(void);
int tls_identity_certificate(char *out, int cap);
int tls_trust_discovery_certificate(const char *encoded, const char *expected);
void tls_shutdown_all(void);
struct tls *tls_connect(const char *ip, const char *expected);
struct tls *tls_accept(SOCKET socket);
void tls_close(struct tls *t);
int tls_read(struct tls *t, void *data, int len);
int tls_write(struct tls *t, const void *data, int len);
int wire_send(struct tls *t, const void *data, int len);
int wire_json(struct tls *t, const char *text);
unsigned char *wire_read(struct tls *t, int *len, int max);
char *request(const char *peer, const char *body);
char *envelope(const char *type, const char *fields);
int network_start(void);
void network_stop(void);
void network_probe(const char *ip);
void send_text(const char *peer, const char *text);
void send_file(const char *peer, const WCHAR *path);
void receive_file(struct message *m);
void profile_sync(const char *peer);
char *profile_response(const char *known);
int profile_set(const WCHAR *path);
void call_dial(const char *peer, const char *kind);
void call_answer(void);
void call_hangup(void);
int call_request(struct tls *t, struct json *j, const char *type);
void call_tick(void);
void media_run(struct call *c);
void call_finished(struct call *c);
void screen_run(struct call *c);
int screen_send_tap(float x, float y);
int screen_send_swipe(float x1, float y1, float x2, float y2, int ms);
int screen_send_key(int action);
int screen_send_mouse(int op, float x, float y);
int screen_send_wheel(float x, float y, int delta, int horiz);

int screen_send_keycode(int vk, int down);
int media_packet(struct call *c, const void *data, int len);
void video_present(const unsigned char *rgb, int w, int h, int preview);
LPCWSTR screen_cursor_resource(int id);
void ui_remote_cursor(int id);
struct codec;
struct codec_encoder_config
{
    int screen_content;
    int quality_mode;
    int intra_period;
    int threads;
    int frame_skip;
};
struct codec *codec_open(int encode, int w, int h, int fps, int bitrate,
                         const struct codec_encoder_config *config);
void codec_close(struct codec *c);
int codec_encode(struct codec *c, const unsigned char *i420, unsigned char **bytes, int *len,
                 int *key);
int codec_headers(struct codec *c, unsigned char **bytes, int *len);
int codec_decode(struct codec *c, const unsigned char *bytes, int len, unsigned char **rgb, int *w,
                 int *h);
int codec_available(void);
unsigned __stdcall camera_thread(void *arg);
void camera_stop(struct call *c);
void camera_list(HWND combo);
void camera_modes(HWND combo, int index);
int image_load(const WCHAR *path, unsigned char **rgb, int *w, int *h);
int image_avatar(const WCHAR *source, const WCHAR *dest);
int ui_run(HINSTANCE instance, int show);
#endif
