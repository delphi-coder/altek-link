#ifndef ALTEK_ICONS_H
#define ALTEK_ICONS_H
/* Vector icons drawn on a 24 x 24 design grid, rendered with 4x4 supersampling. */
#define ICON_ATTACH 1
#define ICON_EMOJI 2
#define ICON_SEND 3
#define ICON_PHONE 4
#define ICON_HANGUP 5
#define ICON_VIDEO 6
#define ICON_CAMERA 7
#define ICON_MIC 8
#define ICON_SPEAKER 9
#define ICON_ROTATE 10
#define ICON_SETTINGS 11
#define ICON_DOWNLOAD 12
#define ICON_DISC 13
#define ICON_SCREEN 14
#define ICON_BACK 15
#define ICON_HOME 16
#define ICON_RECENTS 17
#define ICON_FIT 18
#define ICON_FULL 19
/* Fills mask (size * size bytes) with anti-aliased coverage 0..255. */
void icon_render(int icon, int size, int slash, unsigned char *mask);
#endif
