#ifndef ALTEK_UI_H
#define ALTEK_UI_H
#include "link.h"
#include "icons.h"
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>
#include <usp10.h>
#define ID_CONTACTS 100
#define ID_INPUT 101
#define ID_SEND 102
#define ID_ATTACH 103
#define ID_EMOJI 104
#define ID_AUDIO 105
#define ID_VIDEO 106
#define ID_CAMERA 107
#define ID_SETTINGS 108
#define ID_AVATAR 109
#define ID_ANSWER 110
#define ID_END 111
#define ID_MUTE 112
#define ID_SPEAKER 113
#define ID_ROTATE 114
#define ID_BLOCK 115
#define ID_EXIT 116
#define ID_PROBE 117
#define ID_HEADER_PHOTO 118
#define ID_SCREEN 119
#define ID_BACK 120
#define ID_HOME 121
#define ID_RECENTS 122

#define ID_VIEW 123

#define ID_FULLSCREEN 124
#define UI_FULLSCREEN_KEY (WM_APP + 5)
#define UI_FULLSCREEN_TEXT (WM_APP + 6)
int ui_screen_text_key(int vk);
struct theme
{
    COLORREF bar, bar_line, frame_line, chat_bg, mine_bg, select_bg;
    COLORREF accent, accent_dark, accent_hot, press_disc;
    COLORREF soft, soft_hot, soft_down, soft_ink;
    COLORREF track, track_fill, track_ink, badge;
};
const struct theme *ui_theme(void);
void ui_theme_apply(void);
#define UI_BAR (ui_theme()->bar)
#define UI_BAR_LINE (ui_theme()->bar_line)
#define UI_FRAME_LINE (ui_theme()->frame_line)
extern HFONT ui_font, ui_small, ui_bold;
extern char ui_peer[65];
extern HWND ui_chat, ui_input, ui_call;
void ui_settings(HWND parent);
void ui_avatar(HWND parent, const char *hash);
void ui_draw_button(DRAWITEMSTRUCT *d);
void ui_icon(HDC dc, int icon, int cx, int cy, int size, COLORREF color, int slash);
void ui_draw_image(HDC dc, const unsigned char *rgb, int w, int h, RECT target, int alpha);
void ui_avatar_draw(HDC dc, const char *hash, RECT r);
void ui_chat_register(void);
void ui_chat_refresh(void);
void ui_call_update(void);
void ui_call_register(void);
int ui_screen_fullscreen_active(HWND hwnd);
int ui_screen_fullscreen_enter(HWND hwnd);
void ui_screen_fullscreen_leave(HWND hwnd);
void ui_screen_fullscreen_destroy(HWND hwnd);
void ui_screen_fullscreen_pointer(HWND hwnd, int x, int y);
int ui_screen_fullscreen_click(HWND hwnd, int x, int y);
void ui_screen_fullscreen_paint(HWND hwnd, HDC dc);
HWND ui_control(HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style, int id, int x, int y,
                int w, int h);
#endif
