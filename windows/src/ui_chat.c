#include "ui.h"
#define LINE_H 25
#define PAD 14
#define FILE_ROW 40
#define ACT_DOWNLOAD 1
#define ACT_OPEN 2
#define ACT_FOLDER 3
#define TIMER_SCROLL 77
#define SEL_END 1000000
struct item
{
    struct message *m;
    int index, top, height, left, width, text_h;
};
struct tl
{
    int draw;
    int sel0, sel1;
    int hit, hit_x, hit_y;
    int hit_char;
    int length;
};
struct textbuf
{
    WCHAR w[16002], orig[16002];
    int which[16002], src[16002];
};
static int scroll_pos, total_height, old_count;
static char old_peer[65];
static struct item *items;
static int item_count, item_cap;
static int sel_on, sel_am, sel_ac, sel_cm, sel_cc, dragging;
static int press_item = -1, press_btn = -1, hover_item = -1, hover_btn = -1, tracking;
unsigned char *emoji[10];
int emoji_w[10], emoji_h[10];
static const unsigned int codes[10] = {0x1f600, 0x1f602, 0x1f642, 0x1f60d, 0x1f44d,
                                       0x1f44b, 0x2764,  0x1f389, 0x1f64f, 0x1f622};
void ui_draw_image(HDC dc, const unsigned char *rgb, int w, int h, RECT r, int alpha)
{
    BITMAPINFO bi;
    int rw = r.right - r.left, rh = r.bottom - r.top;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    if (!rgb || w < 1 || h < 1 || rw < 1 || rh < 1)
    {
        return;
    }
    if (alpha)
    {
        void *bits;
        HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old;
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (bitmap && mem)
        {
            memcpy(bits, rgb, w * h * 4);
            old = SelectObject(mem, bitmap);
            AlphaBlend(dc, r.left, r.top, rw, rh, mem, 0, 0, w, h, blend);
            SelectObject(mem, old);
        }
        if (mem)
        {
            DeleteDC(mem);
        }
        if (bitmap)
        {
            DeleteObject(bitmap);
        }
    }
    else
    {
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, r.left, r.top, rw, rh, 0, 0, w, h, rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
    }
}
#define AVATAR_SCALE 0.80 /* figure size relative to the avatar square; 1.0 = fills it */
/* Default avatar (head + shoulders), traced from the reference artwork on a 512 x 512 grid.
   Returns 1 when the point (u, v) is part of the figure. */
static int avatar_inside(double u, double v)
{
    double dx = u - 256.0, dy = v - 124.0, ex, ey;
    if (dx * dx + dy * dy <= 124.3 * 124.3)
    {
        return 1; /* head */
    }
    if (u < 35.0 || u > 477.0 || v < 255.0 || v > 512.0)
    {
        return 0;
    }
    if (dx * dx + dy * dy <= 168.0 * 168.0)
    {
        return 0; /* gap between head and shoulders */
    }
    if (v < 255.0 + 188.5 && (u < 35.0 + 130.0 || u > 477.0 - 130.0))
    {
        ex = (u < 256.0 ? u - (35.0 + 130.0) : u - (477.0 - 130.0)) / 130.0;
        ey = (v - (255.0 + 188.5)) / 188.5;
        if (ex * ex + ey * ey > 1.0)
        {
            return 0; /* rounded shoulders */
        }
    }
    if (v > 512.0 - 50.0 && (u < 35.0 + 50.0 || u > 477.0 - 50.0))
    {
        ex = u < 256.0 ? u - (35.0 + 50.0) : u - (477.0 - 50.0);
        ey = v - (512.0 - 50.0);
        if (ex * ex + ey * ey > 50.0 * 50.0)
        {
            return 0; /* bottom corners */
        }
    }
    return 1;
}
void ui_avatar_draw(HDC dc, const char *hash, RECT r)
{
    static struct
    {
        char hash[65];
        unsigned char *pixels;
        int w, h;
    } cache[24];
    static int next;
    WCHAR leaf[100], p[MAX_PATH];
    const struct theme *theme;
    HBRUSH brush;
    int i, w, h, size, left, top;
    if (valid_hex(hash, 64))
    {
        for (i = 0; i < 24; i++)
        {
            if (!strcmp(cache[i].hash, hash))
            {
                break;
            }
        }
        if (i == 24)
        {
            i = next;
            next = (next + 1) % 24;
            free(cache[i].pixels);
            cache[i].pixels = NULL;
            cache[i].hash[0] = 0;
            _snwprintf(leaf, 100, L"avatar_%S.jpg", hash);
            path_join(p, app.dir, leaf);
            if (image_load(p, &cache[i].pixels, &cache[i].w, &cache[i].h))
            {
                copy_s(cache[i].hash, 65, hash);
            }
        }
        if (cache[i].pixels)
        {
            ui_draw_image(dc, cache[i].pixels, cache[i].w, cache[i].h, r, 0);
            return;
        }
    }
    theme = ui_theme();
    brush = CreateSolidBrush(theme->soft);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
    w = r.right - r.left;
    h = r.bottom - r.top;
    size = w < h ? w : h;
    if (size < 8)
    {
        return;
    }
    left = r.left + (w - size) / 2;
    top = r.top + (h - size) / 2;
    {
        /* Anti-aliased figure: 4 x 4 samples per pixel, accent colour over theme->soft. */
        BITMAPINFO bi;
        void *bits = NULL;
        HBITMAP bitmap;
        HDC mem;
        HGDIOBJ old;
        unsigned char *dst;
        int x, y, sx, sy;
        int fr = GetRValue(theme->accent), fg = GetGValue(theme->accent), fb = GetBValue(theme->accent);
        int br = GetRValue(theme->soft), bg = GetGValue(theme->soft), bb = GetBValue(theme->soft);
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        mem = CreateCompatibleDC(dc);
        if (bitmap && mem && bits)
        {
            dst = (unsigned char *)bits;
            for (y = 0; y < size; y++)
            {
                for (x = 0; x < size; x++)
                {
                    int hits = 0, p = (y * size + x) * 4;
                    for (sy = 0; sy < 4; sy++)
                    {
                        for (sx = 0; sx < 4; sx++)
                        {
                            hits += avatar_inside(
                                256.0 + ((x + (sx + 0.5) / 4.0) * 512.0 / size - 256.0) / AVATAR_SCALE,
                                256.0 + ((y + (sy + 0.5) / 4.0) * 512.0 / size - 256.0) / AVATAR_SCALE);
                        }
                    }
                    dst[p] = (unsigned char)((fb * hits + bb * (16 - hits)) / 16);
                    dst[p + 1] = (unsigned char)((fg * hits + bg * (16 - hits)) / 16);
                    dst[p + 2] = (unsigned char)((fr * hits + br * (16 - hits)) / 16);
                    dst[p + 3] = 255;
                }
            }
            old = SelectObject(mem, bitmap);
            BitBlt(dc, left, top, size, size, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
        }
        if (mem)
        {
            DeleteDC(mem);
        }
        if (bitmap)
        {
            DeleteObject(bitmap);
        }
    }
}
/* Converts UTF-8 to the laid-out text: emoji that have artwork become one placeholder character.
   src[k] is the index in the plain wide string that produced laid-out character k. */
static int make_text(const char *input, WCHAR *out, int *which, int *src, int cap)
{
    WCHAR text[16002];
    int i = 0, n = 0, k;
    unsigned int cp;
    wide(input, text, countof(text));
    while (text[i] && n < cap - 1)
    {
        int used = 1;
        cp = text[i];
        which[n] = -1;
        src[n] = i;
        if (cp >= 0xd800 && cp <= 0xdbff && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff)
        {
            cp = 0x10000 + ((cp - 0xd800) << 10) + text[i + 1] - 0xdc00;
            used = 2;
        }
        for (k = 0; k < 10; k++)
        {
            if (codes[k] == cp)
            {
                break;
            }
        }
        if (k < 10 && text[i + used] != 0x200d &&
            !(text[i + used] == 0xd83c && text[i + used + 1] >= 0xdffb &&
              text[i + used + 1] <= 0xdfff) &&
            !(i > 0 && text[i - 1] == 0x200d))
        {
            out[n] = 0x3000;
            which[n] = k;
            n++;
            i += used;
            if (text[i] == 0xfe0f)
            {
                i++;
            }
        }
        else if (text[i] == '\r')
        {
            /* Rich edit controls separate lines with CR; the wire format uses LF. */
            if (text[i + 1] != '\n')
            {
                out[n++] = '\n';
            }
            i++;
        }
        else
        {
            out[n++] = text[i++];
        }
    }
    out[n] = 0;
    src[n] = i;
    return n;
}
static int text_layout(HDC dc, const char *text, int x, int y, int width, struct tl *q)
{
    WCHAR w[16002];
    int which[16002], src[16002], n, pos = 0, rows = 0, fit, last, i, end, len, cx, rtl, found = 0;
    SIZE size;
    SCRIPT_STRING_ANALYSIS ssa;
    DWORD flags;
    struct tl none;
    if (!q)
    {
        memset(&none, 0, sizeof(none));
        q = &none;
    }
    n = make_text(text, w, which, src, 16002);
    q->length = n;
    q->hit_char = 0;
    SelectObject(dc, ui_font);
    SetBkMode(dc, TRANSPARENT);
    while (pos < n)
    {
        int row_top = y + rows * LINE_H;
        end = pos;
        while (end < n && w[end] != '\n')
        {
            end++;
        }
        if (end == pos)
        {
            if (q->hit && !found && q->hit_y < row_top + LINE_H)
            {
                found = 1;
                q->hit_char = pos;
            }
            pos++;
            rows++;
            continue;
        }
        fit = 0;
        GetTextExtentExPointW(dc, w + pos, end - pos, width, &fit, NULL, &size);
        if (fit < 1)
        {
            fit = 1;
        }
        if (pos + fit < end)
        {
            if (w[pos + fit - 1] >= 0xd800 && w[pos + fit - 1] <= 0xdbff && fit > 1)
            {
                fit--;
            }
            last = fit;
            for (i = fit - 1; i > 0; i--)
            {
                if (w[pos + i] == ' ')
                {
                    last = i + 1;
                    break;
                }
            }
            fit = last;
        }
        len = fit;
        rtl = 0;
        for (i = 0; i < len; i++)
        {
            WCHAR ch = w[pos + i];
            if (ch >= 0x590 && ch <= 0x8ff)
            {
                rtl = 1;
                break;
            }
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
            {
                break;
            }
        }
        flags = SSA_GLYPHS | SSA_FALLBACK | SSA_LINK;
        if (rtl)
        {
            flags |= SSA_RTL;
        }
        ssa = NULL;
        if (SUCCEEDED(ScriptStringAnalyse(dc, w + pos, len, 0, -1, flags, 0, NULL, NULL, NULL, NULL,
                                          NULL, &ssa)))
        {
            const SIZE *sz = ScriptString_pSize(ssa);
            int left = rtl ? x + width - sz->cx : x;
            if (q->hit && !found && q->hit_y < row_top + LINE_H)
            {
                int hc = 0, tr = 0;
                found = 1;
                q->hit_char = pos;
                if (SUCCEEDED(ScriptStringXtoCP(ssa, q->hit_x - left, &hc, &tr)))
                {
                    if (hc < 0)
                    {
                        hc = 0;
                        tr = 0;
                    }
                    if (hc >= len)
                    {
                        hc = len;
                        tr = 0;
                    }
                    q->hit_char = pos + hc + tr;
                    if (q->hit_char > pos + len)
                    {
                        q->hit_char = pos + len;
                    }
                }
            }
            if (q->draw)
            {
                if (q->sel1 > q->sel0)
                {
                    int a = q->sel0 > pos ? q->sel0 : pos;
                    int b = q->sel1 < pos + len ? q->sel1 : pos + len;
                    if (a < b)
                    {
                        int xa = 0, xb = 0;
                        RECT hr;
                        a -= pos;
                        b -= pos;
                        ScriptStringCPtoX(ssa, a, FALSE, &xa);
                        if (b >= len)
                        {
                            ScriptStringCPtoX(ssa, len - 1, TRUE, &xb);
                        }
                        else
                        {
                            ScriptStringCPtoX(ssa, b, FALSE, &xb);
                        }
                        hr.left = left + (xa < xb ? xa : xb);
                        hr.right = left + (xa < xb ? xb : xa);
                        hr.top = row_top;
                        hr.bottom = row_top + LINE_H - 1;
                        SetDCBrushColor(dc, RGB(163, 205, 249));
                        FillRect(dc, &hr, (HBRUSH)GetStockObject(DC_BRUSH));
                    }
                }
                ScriptStringOut(ssa, left, row_top, 0, NULL, 0, 0, FALSE);
                for (i = 0; i < len; i++)
                {
                    if (which[pos + i] >= 0)
                    {
                        int a, b, k = which[pos + i];
                        RECT r;
                        ScriptStringCPtoX(ssa, i, FALSE, &a);
                        ScriptStringCPtoX(ssa, i, TRUE, &b);
                        cx = a < b ? a : b;
                        SetRect(&r, left + cx, row_top + 2, left + cx + 21, row_top + 23);
                        if (emoji[k])
                        {
                            ui_draw_image(dc, emoji[k], emoji_w[k], emoji_h[k], r, 1);
                        }
                    }
                }
            }
            ScriptStringFree(&ssa);
        }
        else
        {
            if (q->draw)
            {
                TextOutW(dc, x, row_top, w + pos, len);
            }
            if (q->hit && !found && q->hit_y < row_top + LINE_H)
            {
                found = 1;
                q->hit_char = pos;
            }
        }
        pos += len;
        if (pos < n && w[pos] == '\n')
        {
            pos++;
        }
        rows++;
    }
    if (q->hit && !found)
    {
        q->hit_char = n;
    }
    return (rows ? rows : 1) * LINE_H;
}
static int is_space(WCHAR c)
{
    return c == ' ' || c == '\n' || c == '\t';
}
/* Finds the word (or run of blanks) around laid-out character ch. */
static void word_at(const char *text, int ch, int *a, int *b)
{
    struct textbuf *t = (struct textbuf *)malloc(sizeof(*t));
    int n, s, e, blank;
    *a = *b = ch;
    if (!t)
    {
        return;
    }
    n = make_text(text, t->w, t->which, t->src, 16002);
    if (n > 0)
    {
        if (ch >= n)
        {
            ch = n - 1;
        }
        if (ch < 0)
        {
            ch = 0;
        }
        blank = is_space(t->w[ch]);
        s = ch;
        while (s > 0 && is_space(t->w[s - 1]) == blank)
        {
            s--;
        }
        e = ch + 1;
        while (e < n && is_space(t->w[e]) == blank)
        {
            e++;
        }
        *a = s;
        *b = e;
    }
    free(t);
}
static int is_file(const struct message *m)
{
    return strcmp(m->kind, "text") != 0;
}
/* Rebuilds the bubble geometry in document coordinates. Caller holds app.lock. */
static void build_items(HDC dc, int client_w)
{
    int i, y = 16, width;
    item_count = 0;
    for (i = 0; i < app.message_count; i++)
    {
        struct message *m = app.messages[i];
        struct item *it;
        if (strcmp(m->peer, ui_peer))
        {
            continue;
        }
        if (item_count == item_cap)
        {
            int cap = item_cap ? item_cap * 2 : 64;
            struct item *grown = (struct item *)realloc(items, cap * sizeof(*items));
            if (!grown)
            {
                break;
            }
            items = grown;
            item_cap = cap;
        }
        width = client_w - 70;
        if (width > 570)
        {
            width = 570;
        }
        if (width < 100)
        {
            width = 100;
        }
        it = &items[item_count++];
        it->m = m;
        it->index = i;
        it->width = width;
        it->left = m->mine ? client_w - width - 18 : 18;
        it->text_h = text_layout(dc, m->text, 0, 0, width - 2 * PAD, NULL);
        it->top = y;
        it->height = it->text_h + 43 + (is_file(m) && !m->mine ? FILE_ROW : 0);
        y += it->height + 12;
    }
    total_height = y + 10;
}
static int pos_cmp(int m1, int c1, int m2, int c2)
{
    if (m1 != m2)
    {
        return m1 < m2 ? -1 : 1;
    }
    if (c1 != c2)
    {
        return c1 < c2 ? -1 : 1;
    }
    return 0;
}
static int selection_range(int *lm, int *lc, int *hm, int *hc)
{
    if (!sel_on)
    {
        return 0;
    }
    if (pos_cmp(sel_am, sel_ac, sel_cm, sel_cc) <= 0)
    {
        *lm = sel_am;
        *lc = sel_ac;
        *hm = sel_cm;
        *hc = sel_cc;
    }
    else
    {
        *lm = sel_cm;
        *lc = sel_cc;
        *hm = sel_am;
        *hc = sel_ac;
    }
    return pos_cmp(*lm, *lc, *hm, *hc) != 0;
}
/* Selected laid-out characters [s0, s1) of the message with global index `index`. */
static int item_sel(int index, int *s0, int *s1)
{
    int lm, lc, hm, hc;
    if (!selection_range(&lm, &lc, &hm, &hc) || index < lm || index > hm)
    {
        return 0;
    }
    *s0 = index == lm ? lc : 0;
    *s1 = index == hm ? hc : SEL_END;
    return *s1 > *s0;
}
static void size_text(unsigned long size, WCHAR *out, int cap)
{
    if (size >= 1048576UL)
    {
        unsigned long t = (size * 10UL) / 1048576UL;
        _snwprintf(out, cap, L"%lu.%lu MB", t / 10, t % 10);
    }
    else if (size >= 1024UL)
    {
        _snwprintf(out, cap, L"%lu KB", size / 1024UL);
    }
    else
    {
        _snwprintf(out, cap, L"%lu B", size);
    }
    out[cap - 1] = 0;
}
/* Buttons shown inside a received file bubble. Rectangles are in client coordinates. */
static int item_actions(const struct item *it, int *acts, RECT *rc)
{
    struct message *m = it->m;
    int x = it->left + PAD, y = it->top - scroll_pos + 10 + it->text_h + 6;
    if (!is_file(m) || m->mine)
    {
        return 0;
    }
    if (m->path[0])
    {
        acts[0] = ACT_OPEN;
        SetRect(&rc[0], x, y, x + 96, y + 28);
        acts[1] = ACT_FOLDER;
        SetRect(&rc[1], x + 104, y, x + 244, y + 28);
        return 2;
    }
    if (m->busy)
    {
        return 0;
    }
    acts[0] = ACT_DOWNLOAD;
    SetRect(&rc[0], x, y, x + 190, y + 28);
    return 1;
}
static int item_at(POINT p)
{
    int i;
    for (i = 0; i < item_count; i++)
    {
        int top = items[i].top - scroll_pos;
        if (p.y >= top && p.y < top + items[i].height && p.x >= items[i].left &&
            p.x < items[i].left + items[i].width)
        {
            return i;
        }
    }
    return -1;
}
static int button_at(POINT p, int *slot, int *btn, int *act)
{
    int i, n, k, acts[2];
    RECT rc[2];
    i = item_at(p);
    if (i < 0)
    {
        return 0;
    }
    EnterCriticalSection(&app.lock);
    n = item_actions(&items[i], acts, rc);
    LeaveCriticalSection(&app.lock);
    for (k = 0; k < n; k++)
    {
        if (PtInRect(&rc[k], p))
        {
            *slot = i;
            *btn = k;
            *act = acts[k];
            return 1;
        }
    }
    return 0;
}
/* Maps a client point to (message index, laid-out character). Returns the item slot or -1. */
static int text_hit(HWND hwnd, POINT p, int *msg_index, int *ch)
{
    HDC dc;
    int i, k, top;
    struct tl q;
    if (!item_count)
    {
        return -1;
    }
    k = item_count - 1;
    for (i = 0; i < item_count; i++)
    {
        if (p.y < items[i].top - scroll_pos + items[i].height + 6)
        {
            k = i;
            break;
        }
    }
    top = items[k].top - scroll_pos;
    memset(&q, 0, sizeof(q));
    q.hit = 1;
    q.hit_x = p.x;
    q.hit_y = p.y;
    dc = GetDC(hwnd);
    text_layout(dc, items[k].m->text, items[k].left + PAD, top + 10, items[k].width - 2 * PAD, &q);
    ReleaseDC(hwnd, dc);
    if (p.y < top)
    {
        q.hit_char = 0;
    }
    *msg_index = items[k].index;
    *ch = q.hit_char;
    return k;
}
static void clipboard_wide(HWND hwnd, const WCHAR *text, size_t chars)
{
    HGLOBAL memory;
    WCHAR *p;
    if (!OpenClipboard(hwnd))
    {
        return;
    }
    memory = GlobalAlloc(GMEM_MOVEABLE, (chars + 1) * sizeof(WCHAR));
    if (memory)
    {
        p = (WCHAR *)GlobalLock(memory);
        memcpy(p, text, chars * sizeof(WCHAR));
        p[chars] = 0;
        GlobalUnlock(memory);
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, memory))
        {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}
static void clipboard_text(HWND hwnd, const char *text)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    WCHAR *w;
    if (n < 1)
    {
        return;
    }
    w = (WCHAR *)malloc(n * sizeof(WCHAR));
    if (w)
    {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, w, n);
        clipboard_wide(hwnd, w, wcslen(w));
        free(w);
    }
}
static int append_wide(WCHAR **buf, size_t *len, size_t *cap, const WCHAR *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
    {
        size_t need = *len + 3;
        if (need > *cap)
        {
            size_t grown = *cap ? *cap * 2 : 512;
            WCHAR *p;
            while (grown < need)
            {
                grown *= 2;
            }
            p = (WCHAR *)realloc(*buf, grown * sizeof(WCHAR));
            if (!p)
            {
                return 0;
            }
            *buf = p;
            *cap = grown;
        }
        if (s[i] == '\n')
        {
            (*buf)[(*len)++] = '\r';
        }
        (*buf)[(*len)++] = s[i];
    }
    return 1;
}
static void copy_selection(HWND hwnd)
{
    struct textbuf *t = (struct textbuf *)malloc(sizeof(*t));
    WCHAR *out = NULL;
    size_t len = 0, cap = 0;
    int i, any = 0;
    if (!t)
    {
        return;
    }
    EnterCriticalSection(&app.lock);
    for (i = 0; i < item_count; i++)
    {
        int s0, s1, n, from, to, olen;
        if (!item_sel(items[i].index, &s0, &s1))
        {
            continue;
        }
        wide(items[i].m->text, t->orig, 16002);
        olen = (int)wcslen(t->orig);
        n = make_text(items[i].m->text, t->w, t->which, t->src, 16002);
        if (s0 > n)
        {
            s0 = n;
        }
        if (s1 > n)
        {
            s1 = n;
        }
        from = s0 < n ? t->src[s0] : olen;
        to = s1 < n ? t->src[s1] : olen;
        if (to > from)
        {
            if (any)
            {
                append_wide(&out, &len, &cap, L"\n", 1);
            }
            any = 1;
            if (!append_wide(&out, &len, &cap, t->orig + from, (size_t)(to - from)))
            {
                break;
            }
        }
    }
    LeaveCriticalSection(&app.lock);
    if (out && len)
    {
        clipboard_wide(hwnd, out, len);
    }
    free(out);
    free(t);
}
static void select_all(void)
{
    if (!item_count)
    {
        return;
    }
    sel_am = items[0].index;
    sel_ac = 0;
    sel_cm = items[item_count - 1].index;
    sel_cc = SEL_END;
    sel_on = 1;
}
static int dangerous_file(const WCHAR *path)
{
    static const WCHAR *bad[] = {L".exe", L".bat", L".cmd", L".com", L".scr", L".msi", L".ps1",
                                 L".vbs", L".vbe", L".js",  L".jse", L".wsf", L".wsh", L".hta",
                                 L".jar", L".reg", L".lnk", L".dll", L".cpl", L".pif", L".msc"};
    const WCHAR *dot = wcsrchr(path, L'.');
    size_t i;
    if (!dot)
    {
        return 0;
    }
    for (i = 0; i < countof(bad); i++)
    {
        if (!_wcsicmp(dot, bad[i]))
        {
            return 1;
        }
    }
    return 0;
}
static void run_action(HWND hwnd, struct message *m, int act)
{
    WCHAR path[MAX_PATH], args[MAX_PATH + 16];
    EnterCriticalSection(&app.lock);
    wcscpy(path, m->path);
    LeaveCriticalSection(&app.lock);
    if (act == ACT_DOWNLOAD)
    {
        receive_file(m);
    }
    else if (!path[0])
    {
        return;
    }
    else if (act == ACT_OPEN)
    {
        if (dangerous_file(path) &&
            MessageBoxW(hwnd, L"This file type can run programs on your computer.\nOpen it anyway?",
                        L"ALTEK Link", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        {
            return;
        }
        ShellExecuteW(hwnd, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    }
    else if (act == ACT_FOLDER)
    {
        _snwprintf(args, MAX_PATH + 16, L"/select,\"%s\"", path);
        args[MAX_PATH + 15] = 0;
        ShellExecuteW(hwnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    }
}
static int find_url(const char *text, WCHAR *out, int cap)
{
    const char *p = strstr(text, "https://"), *q = strstr(text, "http://");
    char url[2048];
    int n = 0;
    if (!p || (q && q < p))
    {
        p = q;
    }
    if (!p)
    {
        return 0;
    }
    while (p[n] && p[n] != ' ' && p[n] != '\n' && p[n] != '\r' && p[n] != '\t' && n < 2000)
    {
        url[n] = p[n];
        n++;
    }
    url[n] = 0;
    wide(url, out, cap);
    return 1;
}
static void draw_button(HDC dc, RECT r, const WCHAR *label, int primary, int hot, int down,
                        int icon)
{
    COLORREF fill, ink;
    RECT t = r;
    if (primary)
    {
        fill = down ? ui_theme()->accent_dark : hot ? ui_theme()->accent_hot : ui_theme()->accent;
        ink = RGB(255, 255, 255);
    }
    else
    {
        fill = down ? ui_theme()->soft_down : hot ? ui_theme()->soft_hot : ui_theme()->soft;
        ink = ui_theme()->soft_ink;
    }
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, fill);
    SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, 12, 12);
    if (icon)
    {
        ui_icon(dc, icon, r.left + 18, (r.top + r.bottom) / 2, 16, ink, 0);
        t.left += 32;
    }
    SelectObject(dc, ui_small);
    SetTextColor(dc, ink);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, label, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
static void paint_file_row(HDC dc, int slot, const struct item *it, struct message *m)
{
    RECT rc[2], bar;
    int acts[2], n, k, percent = 0;
    WCHAR label[80], sz[40];
    if (!is_file(m) || m->mine)
    {
        return;
    }
    n = item_actions(it, acts, rc);
    size_text(m->size, sz, 40);
    for (k = 0; k < n; k++)
    {
        int hot = hover_item == slot && hover_btn == k;
        int down = hot && press_item == slot && press_btn == k;
        if (acts[k] == ACT_DOWNLOAD)
        {
            if (!strcmp(m->state, "retry download"))
            {
                _snwprintf(label, 80, L"Retry download (%s)", sz);
            }
            else
            {
                _snwprintf(label, 80, L"Download (%s)", sz);
            }
            label[79] = 0;
            draw_button(dc, rc[k], label, 1, hot, down, ICON_DOWNLOAD);
        }
        else if (acts[k] == ACT_OPEN)
        {
            draw_button(dc, rc[k], L"Open", 1, hot, down, 0);
        }
        else
        {
            draw_button(dc, rc[k], L"Show in folder", 0, hot, down, 0);
        }
    }
    if (!n && !m->path[0])
    {
        /* Transfer in progress: progress bar with percentage. */
        if (!strncmp(m->state, "receiving ", 10))
        {
            percent = atoi(m->state + 10);
        }
        if (percent < 0)
        {
            percent = 0;
        }
        if (percent > 100)
        {
            percent = 100;
        }
        SetRect(&bar, it->left + PAD, it->top - scroll_pos + 10 + it->text_h + 6,
                it->left + PAD + 190, it->top - scroll_pos + 10 + it->text_h + 34);
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(NULL_PEN));
        SetDCBrushColor(dc, ui_theme()->track);
        RoundRect(dc, bar.left, bar.top, bar.right + 1, bar.bottom + 1, 12, 12);
        if (percent > 0)
        {
            RECT fill = bar;
            fill.right = bar.left + (bar.right - bar.left) * percent / 100;
            SetDCBrushColor(dc, ui_theme()->track_fill);
            RoundRect(dc, fill.left, fill.top, fill.right + 1, fill.bottom + 1, 12, 12);
        }
        if (percent > 0)
        {
            _snwprintf(label, 80, L"Receiving %d%%", percent);
        }
        else
        {
            _snwprintf(label, 80, L"Connecting...");
        }
        label[79] = 0;
        SelectObject(dc, ui_small);
        SetTextColor(dc, ui_theme()->track_ink);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, label, -1, &bar, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}
static void paint_chat(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps), mem;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT client, r, txt;
    HBRUSH brush;
    int i, max;
    char label[256];
    SCROLLINFO si;
    GetClientRect(hwnd, &client);
    mem = CreateCompatibleDC(dc);
    bitmap = CreateCompatibleBitmap(dc, client.right ? client.right : 1,
                                    client.bottom ? client.bottom : 1);
    old = SelectObject(mem, bitmap);
    brush = CreateSolidBrush(ui_theme()->chat_bg);
    FillRect(mem, &client, brush);
    DeleteObject(brush);
    SetBkMode(mem, TRANSPARENT);
    EnterCriticalSection(&app.lock);
    build_items(mem, client.right);
    max = total_height - client.bottom;
    if (max < 0)
    {
        max = 0;
    }
    if (scroll_pos > max)
    {
        scroll_pos = max;
    }
    if (scroll_pos < 0)
    {
        scroll_pos = 0;
    }
    if (!ui_peer[0])
    {
        r = client;
        SelectObject(mem, ui_font);
        SetTextColor(mem, RGB(100, 119, 116));
        DrawTextW(mem, L"Select a contact to start a conversation", -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    for (i = 0; i < item_count; i++)
    {
        struct item *it = &items[i];
        struct message *m = it->m;
        struct tl q;
        HBRUSH bg;
        int s0, s1;
        r.left = it->left;
        r.right = r.left + it->width;
        r.top = it->top - scroll_pos;
        r.bottom = r.top + it->height;
        if (r.bottom <= 0)
        {
            continue;
        }
        if (r.top >= client.bottom)
        {
            break;
        }
        bg = CreateSolidBrush(m->mine ? ui_theme()->mine_bg : RGB(255, 255, 255));
        SelectObject(mem, bg);
        SelectObject(mem, GetStockObject(NULL_PEN));
        RoundRect(mem, r.left, r.top, r.right, r.bottom, 16, 16);
        SelectObject(mem, GetStockObject(WHITE_BRUSH));
        DeleteObject(bg);
        memset(&q, 0, sizeof(q));
        q.draw = 1;
        if (item_sel(it->index, &s0, &s1))
        {
            q.sel0 = s0;
            q.sel1 = s1;
        }
        SetTextColor(mem, RGB(30, 46, 43));
        text_layout(mem, m->text, r.left + PAD, r.top + 10, it->width - 2 * PAD, &q);
        paint_file_row(mem, i, it, m);
        SelectObject(mem, ui_small);
        SetTextColor(mem, RGB(97, 122, 115));
        txt = r;
        txt.left += PAD;
        txt.top = r.bottom - 24;
        txt.right -= 12;
        _snprintf(label, sizeof(label), "%s%s", is_file(m) ? "File - " : "", m->state);
        label[sizeof(label) - 1] = 0;
        {
            WCHAR w[256];
            wide(label, w, 256);
            DrawTextW(mem, w, -1, &txt, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    LeaveCriticalSection(&app.lock);
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMax = total_height;
    si.nPage = client.bottom;
    si.nPos = scroll_pos;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}
static void scroll_clamp(HWND hwnd)
{
    RECT r;
    int max;
    GetClientRect(hwnd, &r);
    max = total_height - r.bottom;
    if (max < 0)
    {
        max = 0;
    }
    if (scroll_pos > max)
    {
        scroll_pos = max;
    }
    if (scroll_pos < 0)
    {
        scroll_pos = 0;
    }
}
static void drag_to(HWND hwnd, POINT p)
{
    RECT r;
    int mi, ch;
    GetClientRect(hwnd, &r);
    if (p.y < 0)
    {
        p.y = 0;
    }
    if (p.y >= r.bottom)
    {
        p.y = r.bottom - 1;
    }
    if (text_hit(hwnd, p, &mi, &ch) >= 0)
    {
        sel_cm = mi;
        sel_cc = ch;
        sel_on = sel_am != sel_cm || sel_ac != sel_cc;
    }
    InvalidateRect(hwnd, NULL, FALSE);
}
static void autoscroll(HWND hwnd)
{
    POINT p;
    RECT r;
    int d = 0;
    GetCursorPos(&p);
    ScreenToClient(hwnd, &p);
    GetClientRect(hwnd, &r);
    if (p.y < 0)
    {
        d = p.y / 2 - 6;
    }
    else if (p.y >= r.bottom)
    {
        d = (p.y - r.bottom) / 2 + 6;
    }
    if (d)
    {
        scroll_pos += d;
        scroll_clamp(hwnd);
    }
    drag_to(hwnd, p);
}
static void mouse_down(HWND hwnd, LPARAM lp, int dbl)
{
    POINT p;
    int slot, btn, act, mi, ch;
    p.x = (short)LOWORD(lp);
    p.y = (short)HIWORD(lp);
    SetFocus(hwnd);
    if (button_at(p, &slot, &btn, &act))
    {
        press_item = slot;
        press_btn = btn;
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    if (text_hit(hwnd, p, &mi, &ch) < 0)
    {
        return;
    }
    if (dbl)
    {
        int a, b, k = text_hit(hwnd, p, &mi, &ch);
        word_at(items[k].m->text, ch, &a, &b);
        sel_am = sel_cm = mi;
        sel_ac = a;
        sel_cc = b;
        sel_on = b > a;
        dragging = 0;
    }
    else
    {
        sel_am = sel_cm = mi;
        sel_ac = sel_cc = ch;
        sel_on = 0;
        dragging = 1;
        SetCapture(hwnd);
        SetTimer(hwnd, TIMER_SCROLL, 40, NULL);
    }
    InvalidateRect(hwnd, NULL, FALSE);
}
static void mouse_move(HWND hwnd, LPARAM lp)
{
    POINT p;
    int slot = -1, btn = -1, act = 0, hot;
    p.x = (short)LOWORD(lp);
    p.y = (short)HIWORD(lp);
    if (!tracking)
    {
        TRACKMOUSEEVENT t;
        t.cbSize = sizeof(t);
        t.dwFlags = TME_LEAVE;
        t.hwndTrack = hwnd;
        t.dwHoverTime = 0;
        if (TrackMouseEvent(&t))
        {
            tracking = 1;
        }
    }
    if (dragging)
    {
        drag_to(hwnd, p);
        return;
    }
    hot = button_at(p, &slot, &btn, &act);
    if (!hot)
    {
        slot = btn = -1;
    }
    if (slot != hover_item || btn != hover_btn)
    {
        hover_item = slot;
        hover_btn = btn;
        InvalidateRect(hwnd, NULL, FALSE);
    }
}
static void mouse_up(HWND hwnd, LPARAM lp)
{
    POINT p;
    p.x = (short)LOWORD(lp);
    p.y = (short)HIWORD(lp);
    if (press_item >= 0)
    {
        int slot = -1, btn = -1, act = 0, was_item = press_item, was_btn = press_btn;
        press_item = press_btn = -1;
        if (GetCapture() == hwnd)
        {
            ReleaseCapture();
        }
        if (button_at(p, &slot, &btn, &act) && slot == was_item && btn == was_btn)
        {
            run_action(hwnd, items[slot].m, act);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    if (dragging)
    {
        dragging = 0;
        KillTimer(hwnd, TIMER_SCROLL);
        if (GetCapture() == hwnd)
        {
            ReleaseCapture();
        }
    }
}
static void context_menu(HWND hwnd, LPARAM lp)
{
    POINT p, screen;
    HMENU menu = CreatePopupMenu();
    int slot, choice, has_sel, url_ok = 0, acts[2], n = 0, k;
    RECT rc[2];
    WCHAR url[2048];
    struct message *m = NULL;
    p.x = (short)LOWORD(lp);
    p.y = (short)HIWORD(lp);
    screen = p;
    slot = item_at(p);
    if (slot >= 0)
    {
        m = items[slot].m;
        url_ok = find_url(m->text, url, 2048);
        EnterCriticalSection(&app.lock);
        n = item_actions(&items[slot], acts, rc);
        LeaveCriticalSection(&app.lock);
    }
    has_sel = sel_on && (sel_am != sel_cm || sel_ac != sel_cc);
    AppendMenuW(menu, MF_STRING | (has_sel ? 0 : MF_GRAYED), 1, L"Copy\tCtrl+C");
    if (m)
    {
        AppendMenuW(menu, MF_STRING, 2, L"Copy whole message");
    }
    AppendMenuW(menu, MF_STRING, 3, L"Select all\tCtrl+A");
    if (url_ok)
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, 4, L"Open link");
    }
    for (k = 0; k < n; k++)
    {
        if (k == 0)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        }
        AppendMenuW(menu, MF_STRING, 10 + acts[k],
                    acts[k] == ACT_DOWNLOAD ? L"Download file"
                    : acts[k] == ACT_OPEN   ? L"Open file"
                                            : L"Show in folder");
    }
    ClientToScreen(hwnd, &screen);
    choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    if (choice == 1)
    {
        copy_selection(hwnd);
    }
    else if (choice == 2 && m)
    {
        clipboard_text(hwnd, m->text);
    }
    else if (choice == 3)
    {
        select_all();
        InvalidateRect(hwnd, NULL, FALSE);
    }
    else if (choice == 4 && url_ok)
    {
        ShellExecuteW(hwnd, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    }
    else if (choice > 10 && m)
    {
        run_action(hwnd, m, choice - 10);
    }
}
static LRESULT CALLBACK chat_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT)
    {
        paint_chat(hwnd);
        return 0;
    }
    if (msg == WM_MOUSEWHEEL || msg == WM_VSCROLL)
    {
        RECT r;
        SCROLLINFO si;
        GetClientRect(hwnd, &r);
        if (msg == WM_MOUSEWHEEL)
        {
            scroll_pos -= GET_WHEEL_DELTA_WPARAM(wp) * 75 / WHEEL_DELTA;
        }
        else
        {
            switch (LOWORD(wp))
            {
            case SB_LINEUP:
                scroll_pos -= 25;
                break;
            case SB_LINEDOWN:
                scroll_pos += 25;
                break;
            case SB_PAGEUP:
                scroll_pos -= r.bottom;
                break;
            case SB_PAGEDOWN:
                scroll_pos += r.bottom;
                break;
            case SB_THUMBTRACK:
                memset(&si, 0, sizeof(si));
                si.cbSize = sizeof(si);
                si.fMask = SIF_TRACKPOS;
                GetScrollInfo(hwnd, SB_VERT, &si);
                scroll_pos = si.nTrackPos;
                break;
            }
        }
        scroll_clamp(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)
    {
        mouse_down(hwnd, lp, msg == WM_LBUTTONDBLCLK);
        return 0;
    }
    if (msg == WM_MOUSEMOVE)
    {
        mouse_move(hwnd, lp);
        return 0;
    }
    if (msg == WM_LBUTTONUP)
    {
        mouse_up(hwnd, lp);
        return 0;
    }
    if (msg == WM_CAPTURECHANGED)
    {
        if (dragging)
        {
            dragging = 0;
            KillTimer(hwnd, TIMER_SCROLL);
        }
        if (press_item >= 0)
        {
            press_item = press_btn = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    if (msg == WM_MOUSELEAVE)
    {
        tracking = 0;
        if (hover_item >= 0)
        {
            hover_item = hover_btn = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    if (msg == WM_TIMER && wp == TIMER_SCROLL)
    {
        if (dragging)
        {
            autoscroll(hwnd);
        }
        return 0;
    }
    if (msg == WM_RBUTTONUP)
    {
        SetFocus(hwnd);
        context_menu(hwnd, lp);
        return 0;
    }
    if (msg == WM_KEYDOWN)
    {
        int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && (wp == 'C' || wp == VK_INSERT))
        {
            copy_selection(hwnd);
            return 0;
        }
        if (ctrl && wp == 'A')
        {
            select_all();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (wp == VK_ESCAPE)
        {
            sel_on = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT)
    {
        POINT p;
        int slot, btn, act;
        LPCWSTR cursor = IDC_ARROW;
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        if (button_at(p, &slot, &btn, &act))
        {
            cursor = IDC_HAND;
        }
        else if (item_at(p) >= 0)
        {
            cursor = IDC_IBEAM;
        }
        SetCursor(LoadCursorW(NULL, cursor));
        return TRUE;
    }
    if (msg == WM_ERASEBKGND)
    {
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
void ui_chat_register(void)
{
    WNDCLASSW wc;
    int i;
    WCHAR p[30];
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.hInstance = app.instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"AltekChat";
    wc.lpfnWndProc = chat_proc;
    RegisterClassW(&wc);
    for (i = 0; i < 10; i++)
    {
        _snwprintf(p, 30, L"#%d", 101 + i);
        image_load(p, &emoji[i], &emoji_w[i], &emoji_h[i]);
    }
}
void ui_chat_refresh(void)
{
    static unsigned __int64 old_sig;
    unsigned __int64 sig = 1469598103934665603ULL;
    int count, i;
    int peer_changed = strcmp(old_peer, ui_peer) != 0;
    const char *p;
    EnterCriticalSection(&app.lock);
    count = app.message_count;
    for (i = 0; i < count; i++)
    {
        struct message *m = app.messages[i];
        if (strcmp(m->peer, ui_peer))
        {
            continue;
        }
        for (p = m->id; *p; p++)
        {
            sig = (sig ^ (unsigned char)*p) * 1099511628211ULL;
        }
        for (p = m->state; *p; p++)
        {
            sig = (sig ^ (unsigned char)*p) * 1099511628211ULL;
        }
    }
    LeaveCriticalSection(&app.lock);
    if (old_count != count || peer_changed || old_sig != sig)
    {
        RECT r;
        int at_bottom;
        GetClientRect(ui_chat, &r);
        at_bottom = scroll_pos >= total_height - r.bottom - 40;
        if (peer_changed)
        {
            sel_on = 0;
            dragging = 0;
            press_item = press_btn = hover_item = hover_btn = -1;
        }
        copy_s(old_peer, 65, ui_peer);
        old_count = count;
        old_sig = sig;
        InvalidateRect(ui_chat, NULL, FALSE);
        UpdateWindow(ui_chat);
        if (peer_changed || at_bottom)
        {
            scroll_pos = total_height - r.bottom;
            if (scroll_pos < 0)
            {
                scroll_pos = 0;
            }
        }
    }
}
