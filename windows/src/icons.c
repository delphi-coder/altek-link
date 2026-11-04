#ifndef ALTEK_ICON_TEST
#include "ui.h"
#else
#include "icons.h"
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define SS 4
#define PI 3.14159265358979
struct canvas
{
    unsigned char *px;
    int n;
    double k;
};
static void seg(struct canvas *c, double x0, double y0, double x1, double y1, double w, int v)
{
    double r = w * 0.5 * c->k, ax = x0 * c->k, ay = y0 * c->k, bx = x1 * c->k, by = y1 * c->k;
    double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    int xmin = (int)floor((ax < bx ? ax : bx) - r - 1), xmax = (int)ceil((ax > bx ? ax : bx) + r + 1);
    int ymin = (int)floor((ay < by ? ay : by) - r - 1), ymax = (int)ceil((ay > by ? ay : by) + r + 1);
    int x, y;
    if (xmin < 0)
    {
        xmin = 0;
    }
    if (ymin < 0)
    {
        ymin = 0;
    }
    if (xmax > c->n - 1)
    {
        xmax = c->n - 1;
    }
    if (ymax > c->n - 1)
    {
        ymax = c->n - 1;
    }
    for (y = ymin; y <= ymax; y++)
    {
        for (x = xmin; x <= xmax; x++)
        {
            double px = x + 0.5 - ax, py = y + 0.5 - ay, t = 0, ex, ey;
            if (len2 > 0)
            {
                t = (px * dx + py * dy) / len2;
            }
            if (t < 0)
            {
                t = 0;
            }
            if (t > 1)
            {
                t = 1;
            }
            ex = px - t * dx;
            ey = py - t * dy;
            if (ex * ex + ey * ey <= r * r)
            {
                c->px[y * c->n + x] = (unsigned char)v;
            }
        }
    }
}
static void circle(struct canvas *c, double cx, double cy, double rad, int v)
{
    seg(c, cx, cy, cx, cy, rad * 2.0, v);
}
static void arc(struct canvas *c, double cx, double cy, double r, double a0, double a1, double w,
                int v)
{
    int steps = (int)(fabs(a1 - a0) / 3.0) + 1, i;
    double px = cx + r * cos(a0 * PI / 180.0), py = cy + r * sin(a0 * PI / 180.0);
    for (i = 1; i <= steps; i++)
    {
        double a = (a0 + (a1 - a0) * i / steps) * PI / 180.0;
        double x = cx + r * cos(a), y = cy + r * sin(a);
        seg(c, px, py, x, y, w, v);
        px = x;
        py = y;
    }
}
static void poly_fill(struct canvas *c, const double *p, int count, int v)
{
    int y, i, j, k, m;
    double xs[32];
    for (y = 0; y < c->n; y++)
    {
        double yc = y + 0.5;
        m = 0;
        for (i = 0; i < count; i++)
        {
            double x0 = p[i * 2] * c->k, y0 = p[i * 2 + 1] * c->k;
            double x1 = p[((i + 1) % count) * 2] * c->k, y1 = p[((i + 1) % count) * 2 + 1] * c->k;
            if (((y0 <= yc && y1 > yc) || (y1 <= yc && y0 > yc)) && m < 32)
            {
                xs[m++] = x0 + (yc - y0) * (x1 - x0) / (y1 - y0);
            }
        }
        for (i = 1; i < m; i++)
        {
            double t = xs[i];
            j = i - 1;
            while (j >= 0 && xs[j] > t)
            {
                xs[j + 1] = xs[j];
                j--;
            }
            xs[j + 1] = t;
        }
        for (i = 0; i + 1 < m; i += 2)
        {
            int a = (int)ceil(xs[i] - 0.5), b = (int)ceil(xs[i + 1] - 0.5);
            if (a < 0)
            {
                a = 0;
            }
            if (b > c->n)
            {
                b = c->n;
            }
            for (k = a; k < b; k++)
            {
                c->px[y * c->n + k] = (unsigned char)v;
            }
        }
    }
}
static void poly_line(struct canvas *c, const double *p, int count, int closed, double w, int v)
{
    int i, last = closed ? count : count - 1;
    for (i = 0; i < last; i++)
    {
        seg(c, p[i * 2], p[i * 2 + 1], p[((i + 1) % count) * 2], p[((i + 1) % count) * 2 + 1], w, v);
    }
}
static void rrect(struct canvas *c, double x0, double y0, double x1, double y1, double r, double w,
                  int v)
{
    seg(c, x0 + r, y0, x1 - r, y0, w, v);
    arc(c, x1 - r, y0 + r, r, -90, 0, w, v);
    seg(c, x1, y0 + r, x1, y1 - r, w, v);
    arc(c, x1 - r, y1 - r, r, 0, 90, w, v);
    seg(c, x1 - r, y1, x0 + r, y1, w, v);
    arc(c, x0 + r, y1 - r, r, 90, 180, w, v);
    seg(c, x0, y1 - r, x0, y0 + r, w, v);
    arc(c, x0 + r, y0 + r, r, 180, 270, w, v);
}
/* Filled handset shared by ICON_PHONE and ICON_HANGUP (traced artwork, 24 x 24 grid). */
static void phone_body(struct canvas *c)
{
    static const double p[84] = {
        4.44, 3.55, 3.05, 4.95, 2.58, 5.80, 2.19, 7.27,
        2.19, 8.82, 2.58, 10.45, 3.12, 11.53, 4.52, 13.78,
        5.76, 15.41, 8.24, 17.97, 9.33, 18.90, 11.03, 20.14,
        13.51, 21.53, 14.91, 21.92, 16.92, 21.92, 18.32, 21.53,
        18.86, 21.22, 20.57, 19.52, 20.64, 19.21, 20.57, 18.67,
        20.18, 17.97, 18.01, 15.72, 17.31, 15.33, 16.53, 15.33,
        16.15, 15.49, 14.83, 16.81, 14.13, 17.04, 13.74, 17.04,
        13.05, 16.81, 11.57, 15.57, 8.40, 12.31, 7.31, 10.99,
        7.16, 10.60, 7.08, 9.83, 7.31, 9.21, 8.63, 7.89,
        8.78, 7.58, 8.86, 6.81, 8.71, 6.34, 5.91, 3.55,
        5.29, 3.32, 4.91, 3.32};
    poly_fill(c, p, 42, 255);
}
static void draw_icon(struct canvas *c, int icon)
{
    int i;
    switch (icon)
    {
    case ICON_ATTACH:
        seg(c, 21.44, 11.05, 12.25, 20.24, 2.0, 255);
        arc(c, 8.005, 15.995, 6.0, 45, 225, 2.0, 255);
        seg(c, 3.76, 11.75, 12.95, 2.56, 2.0, 255);
        arc(c, 15.78, 5.39, 4.0, 225, 405, 2.0, 255);
        seg(c, 18.61, 8.22, 9.41, 17.41, 2.0, 255);
        arc(c, 8.0, 16.0, 2.0, 45, 225, 2.0, 255);
        seg(c, 6.59, 14.59, 15.07, 6.11, 2.0, 255);
        break;
    case ICON_EMOJI:
        arc(c, 12, 12, 9.4, 0, 360, 1.9, 255);
        circle(c, 8.9, 9.6, 1.4, 255);
        circle(c, 15.1, 9.6, 1.4, 255);
        arc(c, 12, 12.2, 5.3, 28, 152, 1.9, 255);
        break;
    case ICON_SEND:
    {
        static const double p[12] = {3.2, 4.0, 21.4, 12.0, 3.2, 20.0, 4.6, 14.0, 14.6, 12.0, 4.6, 10.0};
        poly_fill(c, p, 6, 255);
        poly_line(c, p, 6, 1, 1.8, 255);
    }
    break;
    case ICON_PHONE:
    {
        /* Handset outline traced from the reference artwork (filled), plus three
           sound waves: arcs around one centre, 24 x 24 grid. */
        phone_body(c);
        arc(c, 8.46, 15.61, 9.09, -63.0, -30.0, 1.3, 255);
        arc(c, 8.46, 15.61, 11.63, -65.0, -28.0, 1.3, 255);
        arc(c, 8.46, 15.61, 14.15, -66.5, -26.5, 1.3, 255);
    }
    break;
    case ICON_HANGUP:
        phone_body(c);
        seg(c, 13.9, 5.3, 18.5, 9.9, 1.6, 255);
        seg(c, 18.5, 5.3, 13.9, 9.9, 1.6, 255);
        break;
    case ICON_VIDEO:
    {
        static const double p[8] = {15.5, 10.9, 21.4, 7.4, 21.4, 16.6, 15.5, 13.1};
        rrect(c, 2.6, 6.6, 15.5, 17.4, 2.8, 1.9, 255);
        poly_line(c, p, 4, 1, 1.9, 255);
    }
    break;
    case ICON_CAMERA:
    {
        static const double p[8] = {8.0, 7.6, 9.4, 4.6, 14.6, 4.6, 16.0, 7.6};
        rrect(c, 2.6, 7.6, 21.4, 19.6, 3.0, 1.9, 255);
        poly_line(c, p, 4, 0, 1.9, 255);
        arc(c, 12, 13.6, 3.8, 0, 360, 1.9, 255);
        circle(c, 18.0, 10.9, 0.95, 255);
    }
    break;
    case ICON_MIC:
        seg(c, 12, 6.4, 12, 10.4, 6.4, 255);
        seg(c, 12, 6.4, 12, 10.4, 2.6, 0);
        arc(c, 12, 10.6, 6.8, 12, 168, 1.9, 255);
        seg(c, 12, 17.4, 12, 20.8, 1.9, 255);
        seg(c, 8.6, 20.8, 15.4, 20.8, 1.9, 255);
        break;
    case ICON_SPEAKER:
    {
        static const double p[12] = {3.0, 9.2, 7.0, 9.2, 12.0, 4.8, 12.0, 19.2, 7.0, 14.8, 3.0, 14.8};
        poly_fill(c, p, 6, 255);
        poly_line(c, p, 6, 1, 1.4, 255);
        arc(c, 12, 12, 5.4, -48, 48, 1.9, 255);
        arc(c, 12, 12, 8.8, -48, 48, 1.9, 255);
    }
    break;
    case ICON_ROTATE:
    {
        double a = 320.0 * PI / 180.0, ex = 12 + 7.2 * cos(a), ey = 12 + 7.2 * sin(a);
        double tx = -sin(a), ty = cos(a), nx = cos(a), ny = sin(a);
        double p[6];
        arc(c, 12, 12, 7.2, 30, 320, 1.9, 255);
        p[0] = ex + tx * 3.6;
        p[1] = ey + ty * 3.6;
        p[2] = ex - tx * 0.6 + nx * 3.4;
        p[3] = ey - ty * 0.6 + ny * 3.4;
        p[4] = ex - tx * 0.6 - nx * 3.4;
        p[5] = ey - ty * 0.6 - ny * 3.4;
        poly_fill(c, p, 3, 255);
        poly_line(c, p, 3, 1, 1.0, 255);
    }
    break;
    case ICON_SETTINGS:
        circle(c, 12, 12, 7.4, 255);
        for (i = 0; i < 8; i++)
        {
            double a = i * 45.0 * PI / 180.0;
            seg(c, 12 + 7.0 * cos(a), 12 + 7.0 * sin(a), 12 + 9.6 * cos(a), 12 + 9.6 * sin(a), 3.4, 255);
        }
        circle(c, 12, 12, 3.3, 0);
        break;
    case ICON_DOWNLOAD:
    {
        static const double p[6] = {6.8, 10.2, 12.0, 15.4, 17.2, 10.2};
        seg(c, 12, 3.5, 12, 15.0, 2.2, 255);
        poly_line(c, p, 3, 0, 2.2, 255);
        seg(c, 4.6, 20.0, 19.4, 20.0, 2.2, 255);
    }
    break;
    case ICON_DISC:
        circle(c, 12, 12, 12, 255);
        break;
    case ICON_BACK:
        seg(c, 19, 12, 5, 12, 1.9, 255);
        seg(c, 5, 12, 11, 6, 1.9, 255);
        seg(c, 5, 12, 11, 18, 1.9, 255);
        break;
    case ICON_HOME:
    {
        double w = 1.9;
        seg(c, 4, 12, 12, 4, w, 255);
        seg(c, 12, 4, 20, 12, w, 255);
        rrect(c, 6.5, 12, 17.5, 20, 1.0, w, 255);
        seg(c, 12, 20, 12, 15, w, 255);
    }
    break;
    case ICON_RECENTS:
        rrect(c, 4, 7, 15, 18, 2.0, 1.9, 255);
        rrect(c, 9, 3, 20, 14, 2.0, 1.9, 255);
        break;
    case ICON_SCREEN:
    {
        rrect(c, 2.6, 4.6, 21.4, 17.4, 2.0, 1.9, 255);
        seg(c, 12, 17.4, 12, 21.0, 1.9, 255);
        seg(c, 8.0, 21.0, 16.0, 21.0, 1.9, 255);
    }
    break;
    case ICON_FIT:
    {
        rrect(c, 3.0, 3.0, 21.0, 21.0, 2.0, 1.9, 255);
        rrect(c, 8.0, 8.0, 16.0, 16.0, 1.0, 1.9, 255);
    }
    break;
    case ICON_FULL:
    {
        seg(c, 4.0, 10.0, 4.0, 4.0, 1.9, 255);
        seg(c, 4.0, 4.0, 10.0, 4.0, 1.9, 255);
        seg(c, 20.0, 4.0, 14.0, 4.0, 1.9, 255);
        seg(c, 20.0, 10.0, 20.0, 4.0, 1.9, 255);
        seg(c, 4.0, 14.0, 4.0, 20.0, 1.9, 255);
        seg(c, 4.0, 20.0, 10.0, 20.0, 1.9, 255);
        seg(c, 20.0, 14.0, 20.0, 20.0, 1.9, 255);
        seg(c, 20.0, 20.0, 14.0, 20.0, 1.9, 255);
    }
    break;
    }
}
void icon_render(int icon, int size, int slash, unsigned char *mask)
{
    struct canvas c;
    int x, y, i, j, sum;
    if (size < 1 || size > 128)
    {
        return;
    }
    c.n = size * SS;
    c.k = c.n / 24.0;
    c.px = (unsigned char *)calloc((size_t)c.n * c.n, 1);
    if (!c.px)
    {
        memset(mask, 0, (size_t)size * size);
        return;
    }
    draw_icon(&c, icon);
    if (slash)
    {
        seg(&c, 4.6, 4.6, 19.4, 19.4, 4.6, 0);
        seg(&c, 4.6, 4.6, 19.4, 19.4, 2.0, 255);
    }
    for (y = 0; y < size; y++)
    {
        for (x = 0; x < size; x++)
        {
            sum = 0;
            for (j = 0; j < SS; j++)
            {
                for (i = 0; i < SS; i++)
                {
                    sum += c.px[(y * SS + j) * c.n + x * SS + i];
                }
            }
            mask[y * size + x] = (unsigned char)(sum / (SS * SS));
        }
    }
    free(c.px);
}
#ifndef ALTEK_ICON_TEST
void ui_icon(HDC dc, int icon, int cx, int cy, int size, COLORREF color, int slash)
{
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bmp;
    HDC mem;
    HGDIOBJ old;
    unsigned char *mask, *dst;
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    int i, r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
    if (size < 1 || size > 128)
    {
        return;
    }
    mask = (unsigned char *)malloc((size_t)size * size);
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    mem = CreateCompatibleDC(dc);
    if (mask && bmp && mem && bits)
    {
        icon_render(icon, size, slash, mask);
        dst = (unsigned char *)bits;
        for (i = 0; i < size * size; i++)
        {
            int a = mask[i];
            dst[i * 4] = (unsigned char)(b * a / 255);
            dst[i * 4 + 1] = (unsigned char)(g * a / 255);
            dst[i * 4 + 2] = (unsigned char)(r * a / 255);
            dst[i * 4 + 3] = (unsigned char)a;
        }
        old = SelectObject(mem, bmp);
        AlphaBlend(dc, cx - size / 2, cy - size / 2, size, size, mem, 0, 0, size, size, blend);
        SelectObject(mem, old);
    }
    if (mem)
    {
        DeleteDC(mem);
    }
    if (bmp)
    {
        DeleteObject(bmp);
    }
    free(mask);
}
#endif
