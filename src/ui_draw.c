/* ui_draw.c - Celluloid-style chrome.
 *
 * The scaled video frame lives in a cache bitmap that is only re-rendered when
 * the frame or the window size changes. Every paint copies the cache into a
 * back buffer and draws the header, the overlay control bar, popups and toast
 * on top with GDI+ (anti-aliased). The control bar is an overlay: it never
 * changes the size of the video area.
 */
#include "player.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846

static int   S_(int v)    { return MulDiv(v, g_player.dpi, 96); }
static float SF(double v) { return (float)(v * g_player.dpi / 96.0); }

#define PIN(r, x, y) ((x) >= (r).left && (x) < (r).right && \
                      (y) >= (r).top  && (y) < (r).bottom)

/* ---------------- GDI+ flat API (declared here, no C++ headers) ---------------- */

typedef struct { float x, y; } PTF;
typedef struct { UINT32 ver; void *cb; BOOL nobg; BOOL noext; } GPSI;

int  WINAPI GdiplusStartup(ULONG_PTR *tok, const GPSI *in, void *out);
void WINAPI GdiplusShutdown(ULONG_PTR tok);
int  WINAPI GdipCreateFromHDC(HDC dc, void **g);
int  WINAPI GdipDeleteGraphics(void *g);
int  WINAPI GdipFlush(void *g, int intention);
int  WINAPI GdipSetSmoothingMode(void *g, int m);
int  WINAPI GdipSetPixelOffsetMode(void *g, int m);
int  WINAPI GdipCreateSolidFill(UINT32 c, void **b);
int  WINAPI GdipSetSolidFillColor(void *b, UINT32 c);
int  WINAPI GdipDeleteBrush(void *b);
int  WINAPI GdipCreatePen1(UINT32 c, float w, int unit, void **p);
int  WINAPI GdipSetPenColor(void *p, UINT32 c);
int  WINAPI GdipSetPenWidth(void *p, float w);
int  WINAPI GdipSetPenLineCap197819(void *p, int s, int e, int d);
int  WINAPI GdipSetPenLineJoin(void *p, int j);
int  WINAPI GdipDeletePen(void *p);
int  WINAPI GdipDrawLine(void *g, void *p, float x1, float y1, float x2, float y2);
int  WINAPI GdipDrawArc(void *g, void *p, float x, float y, float w, float h,
                        float a0, float sw);
int  WINAPI GdipFillEllipse(void *g, void *b, float x, float y, float w, float h);
int  WINAPI GdipFillPolygon2(void *g, void *b, const PTF *pts, int n);
int  WINAPI GdipDrawPolygon(void *g, void *p, const PTF *pts, int n);
int  WINAPI GdipCreatePath(int fill, void **path);
int  WINAPI GdipDeletePath(void *path);
int  WINAPI GdipAddPathArc(void *path, float x, float y, float w, float h,
                           float a0, float sw);
int  WINAPI GdipClosePathFigure(void *path);
int  WINAPI GdipFillPath(void *g, void *b, void *path);
int  WINAPI GdipDrawPath(void *g, void *p, void *path);

#define ARGB(a, r, g, b) ((UINT32)(((UINT32)(a) << 24) | ((UINT32)(r) << 16) | \
                                   ((UINT32)(g) << 8) | (UINT32)(b)))
#define A_TEXT    ARGB(255, 238, 238, 236)
#define A_DIM     ARGB(255, 170, 170, 168)
#define A_ACCENT  ARGB(255, 53, 132, 228)
#define A_HOVER   ARGB(40, 255, 255, 255)
#define A_OFF     ARGB(80, 238, 238, 236)       /* disabled button */

static ULONG_PTR g_tok;
static void *G, *BR, *PEN;      /* graphics of the current paint, shared brush/pen */

void ui_gfx_init(void)
{
    GPSI in;
    in.ver = 1; in.cb = NULL; in.nobg = FALSE; in.noext = FALSE;
    GdiplusStartup(&g_tok, &in, NULL);
    GdipCreateSolidFill(0xFFFFFFFF, &BR);
    GdipCreatePen1(0xFFFFFFFF, 1.0f, 2, &PEN);
    GdipSetPenLineCap197819(PEN, 2, 2, 0);     /* round caps */
    GdipSetPenLineJoin(PEN, 2);                /* round joins */
}

/* ---------------- buffers ---------------- */

static HDC g_cdc, g_bdc;                 /* video cache, back buffer */
static HBITMAP g_cbmp, g_bbmp, g_cold, g_bold;
static void *g_cbits, *g_bbits;
static int g_bw, g_bh;
static int g_cdirty = 1;
static struct SwsContext *g_usws;      /* frame -> display size scaler */

static int make_dib(HDC ref, int w, int h, HDC *dc, HBITMAP *bmp,
                    HBITMAP *old, void **bits)
{
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    *dc = CreateCompatibleDC(ref);
    *bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, bits, NULL, 0);
    if (!*dc || !*bmp)
        return 0;
    *old = (HBITMAP)SelectObject(*dc, *bmp);
    SetBkMode(*dc, TRANSPARENT);
    return 1;
}

static void free_dib(HDC *dc, HBITMAP *bmp, HBITMAP *old)
{
    if (*dc) {
        SelectObject(*dc, *old);
        DeleteDC(*dc);
    }
    if (*bmp)
        DeleteObject(*bmp);
    *dc = NULL; *bmp = NULL; *old = NULL;
}

static void ensure_buffers(HDC ref, int w, int h)
{
    if (g_cdc && g_bdc && w == g_bw && h == g_bh)
        return;
    free_dib(&g_cdc, &g_cbmp, &g_cold);
    free_dib(&g_bdc, &g_bbmp, &g_bold);
    make_dib(ref, w, h, &g_cdc, &g_cbmp, &g_cold, &g_cbits);
    make_dib(ref, w, h, &g_bdc, &g_bbmp, &g_bold, &g_bbits);
    g_bw = w;
    g_bh = h;
    g_cdirty = 1;
}

void ui_gfx_free(void)
{
    free_dib(&g_cdc, &g_cbmp, &g_cold);
    free_dib(&g_bdc, &g_bbmp, &g_bold);
    sws_freeContext(g_usws);
    g_usws = NULL;
    if (PEN) GdipDeletePen(PEN);
    if (BR)  GdipDeleteBrush(BR);
    PEN = BR = NULL;
    GdiplusShutdown(g_tok);
}

/* ---------------- layout ---------------- */

typedef struct {
    RECT cap, vid, bar, ov;
    int  has_cap, has_bar;
    RECT c_full, c_menu;
    RECT b_prev, b_play, b_stop, b_next;
    RECT seek, seek_hit, time, b_loop, b_vol;
    RECT pop, pop_hit, pop_track;
} BL;

static int g_tw_short, g_tw_long;       /* time label widths, measured once */

/* ---------------- native caption metrics ---------------- */

#define DWMWA_CAPTION_BUTTON_BOUNDS_ 5

/* frame thickness that a maximized window hangs over the monitor */
static void frame_over(int *fx, int *fy)
{
    int pad = GetSystemMetrics(SM_CXPADDEDBORDER);
    *fx = GetSystemMetrics(SM_CXSIZEFRAME) + pad;
    *fy = GetSystemMetrics(SM_CYSIZEFRAME) + pad;
}

/* The system caption buttons are drawn by DWM. Their bounds are reported
 * relative to the visible frame (window rect minus the invisible resize
 * border), so convert through screen coordinates into our client space. */
void ui_caption_update(Player *p)
{
    RECT rb, vis, wr, rc;
    POINT o = { 0, 0 };
    int W;

    p->cap_h = p->cap_btn_left = p->cap_btn_top = 0;
    if (!p->hwnd)
        return;
    GetClientRect(p->hwnd, &rc);
    W = rc.right;
    if (!p->fullscreen &&
        SUCCEEDED(DwmGetWindowAttribute(p->hwnd, DWMWA_CAPTION_BUTTON_BOUNDS_,
                                        &rb, sizeof(rb))) &&
        SUCCEEDED(DwmGetWindowAttribute(p->hwnd, 9 /* EXTENDED_FRAME_BOUNDS */,
                                        &vis, sizeof(vis))) &&
        rb.right > rb.left && rb.bottom > rb.top) {
        int visW = vis.right - vis.left;
        int winW;
        int bx, by;
        GetWindowRect(p->hwnd, &wr);
        ClientToScreen(p->hwnd, &o);
        winW = wr.right - wr.left;
        if (abs(rb.right - visW) <= abs(rb.right - winW)) {
            bx = vis.left;  by = vis.top;       /* relative to visible frame */
        } else {
            bx = wr.left;   by = wr.top;        /* relative to window rect */
        }
        p->cap_btn_left = bx + rb.left - o.x;
        p->cap_btn_top = by + rb.top - o.y;
        p->cap_h = by + rb.bottom - o.y;
        if (p->cap_btn_top < 0)
            p->cap_btn_top = 0;
    }
    if (p->cap_h < S_(24) || p->cap_h > S_(64) ||
        p->cap_btn_left < S_(80) || p->cap_btn_left > W) {
        int fx, fy;
        frame_over(&fx, &fy);
        p->cap_h = GetSystemMetrics(SM_CYCAPTION) +
                   (IsZoomed(p->hwnd) ? 0 : fy);
        p->cap_btn_left = W - S_(46) * 3;
        p->cap_btn_top = 0;
    }
}

int ui_cap_h(const Player *p)
{
    if (p->cap_h > 0)
        return p->cap_h;
    return GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYSIZEFRAME) +
           GetSystemMetrics(SM_CXPADDEDBORDER);
}

static void bar_calc(const Player *p, BL *L)
{
    RECT rc;
    GetClientRect(p->hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int fs = p->fullscreen;
    int cap_h = fs ? 0 : ui_cap_h(p);
    int bar_h = S_(L_BAR_H);
    int show = fs ? p->bars_visible : !p->ctl_hidden;

    memset(L, 0, sizeof(*L));
    L->has_cap = !fs;
    L->has_bar = show;
    SetRect(&L->cap, 0, 0, W, cap_h);
    SetRect(&L->vid, 0, cap_h, W, H);
    if (show) {
        int ot = H - bar_h - S_(36);
        if (ot < cap_h)
            ot = cap_h;
        SetRect(&L->bar, 0, H - bar_h, W, H);
        SetRect(&L->ov, 0, ot, W, H);
    } else {
        SetRect(&L->bar, 0, H, W, H);
        SetRect(&L->ov, 0, H, W, H);
    }

    if (!fs) {                                  /* header: custom buttons sit */
        int bw = S_(46);                        /* left of the native ones,   */
        int x = p->cap_btn_left;                /* same size as they are      */
        if (x <= 0 || x > W)
            x = W - bw * 3;
        int ty = p->cap_btn_top;                /* same top/bottom as native */
        SetRect(&L->c_full, x - bw, ty, x, cap_h);
        SetRect(&L->c_menu, x - bw * 2, ty, x - bw, cap_h);
    }

    if (show) {                                 /* control bar */
        int bs = S_(30), bhh = S_(28);
        int by = L->bar.top + (bar_h - bhh) / 2;
        int cy = L->bar.top + bar_h / 2;
        int gap = S_(2), x = S_(8), last;

#define BTN(r) do { SetRect(&L->r, x, by, x + bs, by + bhh); x += bs + gap; } while (0)
        BTN(b_prev);
        BTN(b_play);
        BTN(b_stop);
        BTN(b_next);
#undef BTN
        last = x - gap;

        int xr = W - S_(8);
        SetRect(&L->b_vol, xr - bs, by, xr, by + bhh);
        xr -= bs + gap;
        SetRect(&L->b_loop, xr - bs, by, xr, by + bhh);
        xr -= bs + S_(8);
        int tw = (p->dur >= 3600) ? g_tw_long : g_tw_short;
        if (tw <= 0)
            tw = S_(84);
        SetRect(&L->time, xr - tw, L->bar.top, xr, L->bar.bottom);
        xr -= tw + S_(12);

        int sx1 = last + S_(12), sx2 = xr;
        if (sx2 < sx1 + S_(24))
            sx2 = sx1 + S_(24);
        SetRect(&L->seek, sx1, cy - S_(2), sx2, cy - S_(2) + S_(3));
        SetRect(&L->seek_hit, sx1 - S_(6), cy - S_(14), sx2 + S_(6), cy + S_(14));

        /* vertical volume popup above the speaker */
        int pcx = (L->b_vol.left + L->b_vol.right) / 2;
        int pw = S_(30), ph = S_(112);
        int pb = L->b_vol.top - S_(6);
        if (pcx - pw / 2 < S_(4))
            pcx = S_(4) + pw / 2;
        SetRect(&L->pop, pcx - pw / 2, pb - ph, pcx + pw / 2, pb);
        SetRect(&L->pop_hit, L->pop.left, L->pop.top, L->pop.right, L->b_vol.top);
        SetRect(&L->pop_track, pcx - S_(2), L->pop.top + S_(12),
                pcx + S_(2), L->pop.bottom - S_(12));
    }
}

static int vol_visible(const Player *p)
{
    return p->hover == HT_VOL || p->hover == HT_VOL_TRACK || p->dragging == 2 ||
           (p->vol_show_until &&
            (LONG)(p->vol_show_until - GetTickCount()) > 0);
}

void ui_layout(const Player *p, RECT *cap, RECT *video, RECT *bar)
{
    BL L;
    bar_calc(p, &L);
    *cap = L.cap;
    *video = L.vid;
    *bar = L.bar;
}

void ui_invalidate(Player *p, int video, int chrome)
{
    if (!p->hwnd)
        return;
    BL L;
    bar_calc(p, &L);
    if (video) {                    /* 1 = new frame, 2 = repaint only */
        if (video == 1)
            g_cdirty = 1;
        InvalidateRect(p->hwnd, &L.vid, FALSE);
    }
    if (chrome) {                   /* 1 = header + bar, 2 = bar only */
        if (L.has_cap && chrome != 2)
            InvalidateRect(p->hwnd, &L.cap, FALSE);
        if (L.has_bar) {
            InvalidateRect(p->hwnd, &L.ov, FALSE);
            InvalidateRect(p->hwnd, &L.pop, FALSE);
        }
    }
}

int ui_hit(Player *p, int x, int y)
{
    BL L;
    bar_calc(p, &L);
    if (L.has_cap && y < L.cap.bottom) {
        if (PIN(L.c_full, x, y))  return HT_CAP_FULL;
        if (PIN(L.c_menu, x, y))  return HT_CAP_MENU;
        return HT_NONE;
    }
    if (L.has_bar) {
        if (vol_visible(p) && PIN(L.pop_hit, x, y))
            return HT_VOL_TRACK;
        if (PIN(L.bar, x, y)) {
            if (PIN(L.b_prev, x, y)) return HT_BAR_PREV;
            if (PIN(L.b_play, x, y)) return HT_BAR_PLAY;
            if (PIN(L.b_stop, x, y)) return HT_BAR_STOP;
            if (PIN(L.b_next, x, y)) return HT_BAR_NEXT;
            if (PIN(L.b_vol, x, y))  return HT_VOL;
            if (PIN(L.b_loop, x, y)) return HT_LOOP;
            if (PIN(L.seek_hit, x, y)) return HT_SEEK;
            if (PIN(L.time, x, y))   return HT_TIME;
            return HT_NONE;
        }
    }
    return HT_VIDEO;
}

double ui_seek_frac(const Player *p, int x)
{
    BL L;
    bar_calc(p, &L);
    int w = L.seek.right - L.seek.left;
    if (w <= 0)
        return 0;
    double f = (double)(x - L.seek.left) / (double)w;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

double ui_vol_frac(const Player *p, int y)
{
    BL L;
    bar_calc(p, &L);
    int h = L.pop_track.bottom - L.pop_track.top;
    if (h <= 0)
        return 0;
    double f = (double)(L.pop_track.bottom - y) / (double)h;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

void ui_menu_anchor(const Player *p, POINT *pt)
{
    BL L;
    bar_calc(p, &L);
    pt->x = L.c_menu.right;
    pt->y = L.cap.bottom;
}

/* ---------------- fonts / time ---------------- */

static HFONT mkfont(int px, int weight, const wchar_t *face, int dpi)
{
    return CreateFontW(-MulDiv(px, dpi, 96), 0, 0, 0, weight, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

void ui_init_fonts(Player *p)
{
    p->fnt_cap  = mkfont(12, FW_NORMAL, L"Segoe UI", p->dpi);
    p->fnt_bar  = mkfont(12, FW_NORMAL, L"Segoe UI", p->dpi);
    p->fnt_hint = mkfont(15, FW_NORMAL, L"Segoe UI", p->dpi);

    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ of = SelectObject(dc, p->fnt_bar);
    SIZE a, b;
    GetTextExtentPoint32W(dc, L"88:88/88:88", 11, &a);
    GetTextExtentPoint32W(dc, L"88:88:88/88:88:88", 17, &b);
    g_tw_short = a.cx + S_(6);
    g_tw_long = b.cx + S_(6);
    SelectObject(dc, of);
    DeleteDC(dc);
}

void ui_free_fonts(Player *p)
{
    if (p->fnt_cap)  DeleteObject(p->fnt_cap);
    if (p->fnt_bar)  DeleteObject(p->fnt_bar);
    if (p->fnt_hint) DeleteObject(p->fnt_hint);
    p->fnt_cap = p->fnt_bar = p->fnt_hint = NULL;
}

void ui_fmt_time(wchar_t *o, int cap, double s)
{
    if (s < 0)
        s = 0;
    int t = (int)(s + 0.5);
    int h = t / 3600, m = (t % 3600) / 60, sec = t % 60;
    if (h)
        _snwprintf(o, cap - 1, L"%02d:%02d:%02d", h, m, sec);
    else
        _snwprintf(o, cap - 1, L"%02d:%02d", m, sec);
    o[cap - 1] = 0;
}

/* ---------------- anti-aliased primitives (GDI+) ---------------- */

static void gfill_poly(const PTF *p, int n, UINT32 c)
{
    GdipSetSolidFillColor(BR, c);
    GdipFillPolygon2(G, BR, p, n);
}

static void gline(float x1, float y1, float x2, float y2, float w, UINT32 c)
{
    GdipSetPenColor(PEN, c);
    GdipSetPenWidth(PEN, w);
    GdipDrawLine(G, PEN, x1, y1, x2, y2);
}

static void gcircle(float cx, float cy, float r, UINT32 c)
{
    GdipSetSolidFillColor(BR, c);
    GdipFillEllipse(G, BR, cx - r, cy - r, 2 * r, 2 * r);
}

static void garc(float cx, float cy, float rx, float ry, float a0, float sw,
                 float w, UINT32 c)
{
    GdipSetPenColor(PEN, c);
    GdipSetPenWidth(PEN, w);
    GdipDrawArc(G, PEN, cx - rx, cy - ry, 2 * rx, 2 * ry, a0, sw);
}

static void *rrpath(float x, float y, float w, float h, float r)
{
    void *path = NULL;
    float d = 2 * r;
    if (d > w) d = w;
    if (d > h) d = h;
    GdipCreatePath(0, &path);
    GdipAddPathArc(path, x, y, d, d, 180, 90);
    GdipAddPathArc(path, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(path, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(path, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(path);
    return path;
}

static void grrect(float x, float y, float w, float h, float r, UINT32 c)
{
    void *path = rrpath(x, y, w, h, r);
    GdipSetSolidFillColor(BR, c);
    GdipFillPath(G, BR, path);
    GdipDeletePath(path);
}

static void grrect_r(const RECT *r, float rad, UINT32 c)
{
    grrect((float)r->left, (float)r->top, (float)(r->right - r->left),
           (float)(r->bottom - r->top), rad, c);
}

static void gtri(float x1, float y1, float x2, float y2, float x3, float y3,
                 float soften, UINT32 c)
{
    PTF p[3];
    p[0].x = x1; p[0].y = y1;
    p[1].x = x2; p[1].y = y2;
    p[2].x = x3; p[2].y = y3;
    gfill_poly(p, 3, c);
    if (soften > 0) {                       /* rounded corners */
        GdipSetPenColor(PEN, c);
        GdipSetPenWidth(PEN, soften);
        GdipDrawPolygon(G, PEN, p, 3);
    }
}

/* ---------------- glyphs (cx, cy = centre, s = icon size in px) ---------------- */

static void glyph_play(float cx, float cy, float s, UINT32 c)
{
    gtri(cx - s * 0.34f, cy - s * 0.46f, cx - s * 0.34f, cy + s * 0.46f,
         cx + s * 0.46f, cy, s * 0.12f, c);
}

static void glyph_pause(float cx, float cy, float s, UINT32 c)
{
    float w = s * 0.26f, h = s * 0.92f;
    grrect(cx - s * 0.40f, cy - h / 2, w, h, s * 0.06f, c);
    grrect(cx + s * 0.14f, cy - h / 2, w, h, s * 0.06f, c);
}

static void glyph_stop(float cx, float cy, float s, UINT32 c)
{
    float w = s * 0.74f;
    grrect(cx - w / 2, cy - w / 2, w, w, s * 0.10f, c);
}

static void glyph_prev(float cx, float cy, float s, UINT32 c)
{
    grrect(cx - s * 0.52f, cy - s * 0.46f, s * 0.15f, s * 0.92f, s * 0.05f, c);
    gtri(cx + s * 0.50f, cy - s * 0.46f, cx + s * 0.50f, cy + s * 0.46f,
         cx - s * 0.28f, cy, s * 0.10f, c);
}

static void glyph_next(float cx, float cy, float s, UINT32 c)
{
    grrect(cx + s * 0.37f, cy - s * 0.46f, s * 0.15f, s * 0.92f, s * 0.05f, c);
    gtri(cx - s * 0.50f, cy - s * 0.46f, cx - s * 0.50f, cy + s * 0.46f,
         cx + s * 0.28f, cy, s * 0.10f, c);
}

static void glyph_loop(float cx, float cy, float s, UINT32 c)
{
    float pw = fmaxf(1.5f, s * 0.11f);
    float y0 = cy - s * 0.30f, y1 = cy + s * 0.30f, r = (y1 - y0) / 2;
    float half = s * 0.44f, xr = cx + half, xl = cx - half;
    gline(xl, y0, xr - s * 0.22f, y0, pw, c);
    gline(xr, y1, xl + s * 0.22f, y1, pw, c);
    garc(xr, cy, r, r, -90, 180, pw, c);
    garc(xl, cy, r, r, 90, 180, pw, c);
    gtri(xr - s * 0.26f, y0 - s * 0.17f, xr - s * 0.26f, y0 + s * 0.17f,
         xr + s * 0.06f, y0, 0, c);
    gtri(xl + s * 0.26f, y1 - s * 0.17f, xl + s * 0.26f, y1 + s * 0.17f,
         xl - s * 0.06f, y1, 0, c);
}

static void glyph_speaker(float cx, float cy, float s, UINT32 c, double vol, int muted)
{
    PTF p[6];
    float pw = fmaxf(1.4f, s * 0.10f);
    p[0].x = cx - s * 0.56f; p[0].y = cy - s * 0.20f;
    p[1].x = cx - s * 0.30f; p[1].y = cy - s * 0.20f;
    p[2].x = cx + s * 0.02f; p[2].y = cy - s * 0.48f;
    p[3].x = cx + s * 0.02f; p[3].y = cy + s * 0.48f;
    p[4].x = cx - s * 0.30f; p[4].y = cy + s * 0.20f;
    p[5].x = cx - s * 0.56f; p[5].y = cy + s * 0.20f;
    gfill_poly(p, 6, c);
    if (muted) {
        gline(cx + s * 0.18f, cy - s * 0.24f, cx + s * 0.52f, cy + s * 0.24f, pw, c);
        gline(cx + s * 0.18f, cy + s * 0.24f, cx + s * 0.52f, cy - s * 0.24f, pw, c);
    } else {
        if (vol > 0.0)
            garc(cx + s * 0.04f, cy, s * 0.28f, s * 0.28f, -48, 96, pw, c);
        if (vol > 0.45)
            garc(cx + s * 0.04f, cy, s * 0.52f, s * 0.52f, -48, 96, pw, c);
    }
}

static void glyph_fs(float cx, float cy, float s, UINT32 c, int exit)
{
    float pw = fmaxf(1.0f, SF(1.0));
    float m = exit ? s * 0.28f : s * 0.46f, ln = s * 0.30f;
    gline(cx - m, cy - m + ln, cx - m, cy - m, pw, c);
    gline(cx - m, cy - m, cx - m + ln, cy - m, pw, c);
    gline(cx + m - ln, cy - m, cx + m, cy - m, pw, c);
    gline(cx + m, cy - m, cx + m, cy - m + ln, pw, c);
    gline(cx + m, cy + m - ln, cx + m, cy + m, pw, c);
    gline(cx + m, cy + m, cx + m - ln, cy + m, pw, c);
    gline(cx - m + ln, cy + m, cx - m, cy + m, pw, c);
    gline(cx - m, cy + m, cx - m, cy + m - ln, pw, c);
}

static void glyph_menu(float cx, float cy, float s, UINT32 c)
{
    float pw = fmaxf(1.0f, SF(1.0)), hw = s * 0.46f;
    gline(cx - hw, cy - s * 0.30f, cx + hw, cy - s * 0.30f, pw, c);
    gline(cx - hw, cy, cx + hw, cy, pw, c);
    gline(cx - hw, cy + s * 0.30f, cx + hw, cy + s * 0.30f, pw, c);
}

/* ---------------- video cache ---------------- */

static RECT fit_rect(double dar, const RECT *a)
{
    RECT o;
    SetRect(&o, 0, 0, 0, 0);
    int dw = a->right - a->left, dh = a->bottom - a->top;
    if (dar <= 0 || dw <= 0 || dh <= 0)
        return o;
    int w = dw, h = (int)(dw / dar + 0.5);
    if (h > dh) {
        h = dh;
        w = (int)(dh * dar + 0.5);
    }
    if (abs(dw - w) <= 2) w = dw;       /* no 1-2 px black slivers */
    if (abs(dh - h) <= 2) h = dh;
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    o.left = a->left + (dw - w) / 2;
    o.top = a->top + (dh - h) / 2;
    o.right = o.left + w;
    o.bottom = o.top + h;
    return o;
}

static void fill_black(const RECT *r)
{
    if (r->right > r->left && r->bottom > r->top) {
        HBRUSH br = CreateSolidBrush(COL_VIDEO);
        FillRect(g_cdc, r, br);
        DeleteObject(br);
    }
}

static void render_cache(Player *p, const BL *L)
{
    RECT all;
    SetRect(&all, 0, 0, g_bw, g_bh);

    AVFrame *fr = NULL;
    double dar = 0;
    if (p->shown_slot >= 0 && p->slot[p->shown_slot].bgra) {
        fr = p->slot[p->shown_slot].bgra;
        dar = (p->vdar > 0) ? p->vdar : (double)fr->width / fr->height;
    } else if (p->playing && p->vs < 0 && p->cover && p->cover->height > 0) {
        fr = p->cover;                      /* audio file: show its cover art */
        dar = (double)fr->width / fr->height;
    }
    if (fr) {
        RECT f = fit_rect(dar, &L->vid);
        int dw = f.right - f.left, dh = f.bottom - f.top;
        if (dw > 0 && dh > 0) {
            /* black bars only where the picture doesn't reach */
            RECT t = all, b = all, l = all, r = all;
            t.bottom = f.top;
            b.top = f.bottom;
            l.top = f.top; l.bottom = f.bottom; l.right = f.left;
            r.top = f.top; r.bottom = f.bottom; r.left = f.right;
            fill_black(&t); fill_black(&b); fill_black(&l); fill_black(&r);

            /* scale straight into the cache bitmap with swscale: much faster
             * than GDI's HALFTONE stretch and it runs on SIMD code */
            g_usws = sws_getCachedContext(g_usws, fr->width, fr->height,
                         AV_PIX_FMT_BGRA, dw, dh, AV_PIX_FMT_BGRA,
                         (dw < fr->width) ? SWS_AREA : SWS_BILINEAR,
                         NULL, NULL, NULL);
            if (g_usws) {
                uint8_t *dst[4] = { (uint8_t *)g_cbits +
                                    (size_t)f.top * g_bw * 4 + (size_t)f.left * 4,
                                    NULL, NULL, NULL };
                int dls[4] = { g_bw * 4, 0, 0, 0 };
                sws_scale(g_usws, (const uint8_t *const *)fr->data, fr->linesize,
                          0, fr->height, dst, dls);
                return;
            }
        }
    }
    fill_black(&all);
}

/* ---------------- overlay pieces ---------------- */

static void paint_gradient(const RECT *clip, const RECT *ov)
{
    RECT r;
    int oh = ov->bottom - ov->top, x, y;
    if (oh <= 0 || !IntersectRect(&r, clip, ov))
        return;
    for (y = r.top; y < r.bottom; y++) {
        double t = (double)(y - ov->top + 1) / (double)oh;
        int a = (int)(256.0 * 0.84 * pow(t, 1.3) + 0.5);
        int inv = 256 - a;
        UINT32 *px = (UINT32 *)g_bbits + (size_t)y * g_bw + r.left;
        for (x = r.left; x < r.right; x++, px++) {
            UINT32 c = *px;
            UINT32 b = ((c & 255) * inv + 12 * a) >> 8;
            UINT32 g = (((c >> 8) & 255) * inv + 12 * a) >> 8;
            UINT32 rr = (((c >> 16) & 255) * inv + 14 * a) >> 8;
            *px = (rr << 16) | (g << 8) | b;
        }
    }
}

static void text_in(const wchar_t *s, RECT r, UINT fmt, HFONT f, COLORREF c)
{
    HGDIOBJ of = SelectObject(g_bdc, f);
    SetTextColor(g_bdc, c);
    DrawTextW(g_bdc, s, -1, &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(g_bdc, of);
}

static void rect_center(const RECT *r, float *cx, float *cy)
{
    *cx = (r->left + r->right) / 2.0f;
    *cy = (r->top + r->bottom) / 2.0f;
}

static void fill_rgb(const RECT *r, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    FillRect(g_bdc, r, br);
    DeleteObject(br);
}

/* caption strip: black like the dark native one, icon + title on the left,
 * two flat buttons (fullscreen, options) right before the system buttons */
static void draw_header(Player *p, const BL *L)
{
    float cx, cy, s = SF(10);
    int cap_h = L->cap.bottom - L->cap.top;

    fill_rgb(&L->cap, RGB(0, 0, 0));    /* black + alpha 0 = DWM frame shows */

    if (p->hover == HT_CAP_FULL)
        fill_rgb(&L->c_full, p->hover_down == HT_CAP_FULL ? RGB(46, 46, 46)
                                                          : RGB(28, 28, 28));
    if (p->hover == HT_CAP_MENU)
        fill_rgb(&L->c_menu, p->hover_down == HT_CAP_MENU ? RGB(46, 46, 46)
                                                          : RGB(28, 28, 28));
    rect_center(&L->c_full, &cx, &cy);
    glyph_fs(cx, cy, s, A_TEXT, p->fullscreen);
    rect_center(&L->c_menu, &cx, &cy);
    glyph_menu(cx, cy, s, A_TEXT);
    GdipFlush(G, 1);

    /* app icon + title, left aligned like a native title bar */
    {
        int isz = GetSystemMetrics(SM_CXSMICON);
        int ix = S_(10), iy = (cap_h - isz) / 2;
        wchar_t title[512];
        RECT tr;
        if (p->ico_small)
            DrawIconEx(g_bdc, ix, iy, p->ico_small, isz, isz, 0, NULL, DI_NORMAL);
        GetWindowTextW(p->hwnd, title, 512);
        SetRect(&tr, ix + isz + S_(8), L->cap.top, L->c_menu.left - S_(8),
                L->cap.bottom);
        if (tr.right > tr.left + 8)
            text_in(title, tr, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS,
                    p->fnt_cap, COL_TEXT);
    }
}

static void hbg(const RECT *r, int on)
{
    if (on)
        grrect_r(r, SF(5), A_HOVER);
}

static void draw_bar(Player *p, const BL *L)
{
    float cx, cy, s = SF(L_ICON);

    int can_prev = p->pl_cur > 0 && p->pl_n > 0;
    int can_next = p->pl_cur + 1 < p->pl_n;

    rect_center(&L->b_prev, &cx, &cy);
    hbg(&L->b_prev, can_prev && p->hover == HT_BAR_PREV);
    glyph_prev(cx, cy, s, can_prev ? A_TEXT : A_OFF);

    rect_center(&L->b_play, &cx, &cy);
    hbg(&L->b_play, p->hover == HT_BAR_PLAY);
    if (p->paused || !p->playing)
        glyph_play(cx, cy, s, A_TEXT);
    else
        glyph_pause(cx, cy, s, A_TEXT);

    rect_center(&L->b_stop, &cx, &cy);
    hbg(&L->b_stop, p->hover == HT_BAR_STOP);
    glyph_stop(cx, cy, s, A_TEXT);

    rect_center(&L->b_next, &cx, &cy);
    hbg(&L->b_next, can_next && p->hover == HT_BAR_NEXT);
    glyph_next(cx, cy, s, can_next ? A_TEXT : A_OFF);

    rect_center(&L->b_loop, &cx, &cy);
    hbg(&L->b_loop, p->hover == HT_LOOP);
    glyph_loop(cx, cy, s, p->loop_file ? A_ACCENT : A_TEXT);

    rect_center(&L->b_vol, &cx, &cy);
    hbg(&L->b_vol, p->hover == HT_VOL);
    glyph_speaker(cx, cy, s, A_TEXT, p->volume, p->muted);

    /* seek track: thin line, white fill, white knob */
    {
        float x1 = (float)L->seek.left, x2 = (float)L->seek.right;
        float ty = (float)L->seek.top, th = (float)(L->seek.bottom - L->seek.top);
        float ky = ty + th / 2.0f;
        grrect(x1, ty, x2 - x1, th, th / 2.0f, ARGB(90, 255, 255, 255));
        if (p->playing && p->dur > 0) {
            double pos = (p->dragging == 1) ? p->drag_pos : ff_pos(p);
            double fr = pos / p->dur;
            if (fr < 0) fr = 0;
            if (fr > 1) fr = 1;
            float fx = x1 + (float)((x2 - x1) * fr);
            if (fx > x1 + 1)
                grrect(x1, ty, fx - x1, th, th / 2.0f, A_TEXT);
            gcircle(fx, ky, (p->hover == HT_SEEK || p->dragging == 1) ?
                    SF(6.5) : SF(5.5), A_TEXT);
        }
    }
    GdipFlush(G, 1);

    if (p->playing && p->time_txt[0])
        text_in(p->time_txt, L->time, DT_RIGHT | DT_VCENTER, p->fnt_bar, COL_TEXT);
}

static void draw_popup(Player *p, const BL *L)
{
    if (!vol_visible(p))
        return;
    grrect_r(&L->pop, SF(8), ARGB(255, 40, 40, 42));
    float cx = (L->pop_track.left + L->pop_track.right) / 2.0f;
    float y0 = (float)L->pop_track.top, y1 = (float)L->pop_track.bottom;
    float v = p->muted ? 0.0f : (float)p->volume;
    float ky = y1 - (y1 - y0) * v;
    grrect(cx - SF(1.5), y0, SF(3), y1 - y0, SF(1.5), ARGB(90, 255, 255, 255));
    if (y1 - ky > 1)
        grrect(cx - SF(1.5), ky, SF(3), y1 - ky, SF(1.5), A_TEXT);
    gcircle(cx, ky, SF(5.5), A_TEXT);
    GdipFlush(G, 1);
}

static void draw_toast(Player *p, const BL *L)
{
    if (!p->toast[0] || GetTickCount() >= p->toast_until)
        return;
    HGDIOBJ of = SelectObject(g_bdc, p->fnt_bar);
    SIZE sz;
    GetTextExtentPoint32W(g_bdc, p->toast, (int)wcslen(p->toast), &sz);
    SelectObject(g_bdc, of);
    RECT tr;
    tr.left = L->vid.left + S_(14);
    tr.right = tr.left + sz.cx + S_(24);
    tr.bottom = (L->has_bar ? L->bar.top : L->vid.bottom) - S_(10);
    tr.top = tr.bottom - sz.cy - S_(14);
    grrect_r(&tr, SF(8), ARGB(215, 20, 20, 22));
    GdipFlush(G, 1);
    InflateRect(&tr, -S_(10), -S_(6));
    text_in(p->toast, tr, DT_CENTER | DT_VCENTER, p->fnt_bar, COL_TEXT);
}

/* ---------------- paint ---------------- */

void ui_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc)
        return;
    Player *p = &g_player;
    RECT rc, pr;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0 ||
        !IntersectRect(&pr, &ps.rcPaint, &rc)) {
        EndPaint(hwnd, &ps);
        return;
    }
    ensure_buffers(hdc, rc.right, rc.bottom);
    if (!g_cdc || !g_bdc) {
        EndPaint(hwnd, &ps);
        return;
    }

    BL L;
    bar_calc(p, &L);
    if (g_cdirty) {
        render_cache(p, &L);
        g_cdirty = 0;
    }
    int w = pr.right - pr.left, h = pr.bottom - pr.top;
    BitBlt(g_bdc, pr.left, pr.top, w, h, g_cdc, pr.left, pr.top, SRCCOPY);

    GdipCreateFromHDC(g_bdc, &G);
    GdipSetSmoothingMode(G, 4);
    GdipSetPixelOffsetMode(G, 2);

    if (!p->playing) {
        RECT vr = L.vid;
        text_in(L"Drop a file here, or press O to open", vr,
                DT_CENTER | DT_VCENTER, p->fnt_hint, COL_DIM);
    }
    if (L.has_bar)
        paint_gradient(&pr, &L.ov);
    if (L.has_cap)
        draw_header(p, &L);
    if (L.has_bar) {
        draw_bar(p, &L);
        draw_popup(p, &L);
    }
    draw_toast(p, &L);

    GdipFlush(G, 1);
    GdipDeleteGraphics(G);
    G = NULL;

    if (L.has_cap) {
        /* the caption strip is DWM "glass": pixels with alpha 0 would show
         * the system frame colour instead of ours, so make them opaque */
        RECT cr = L.cap, ir;
        if (IntersectRect(&ir, &cr, &pr)) {
            int x, y, split = p->cap_btn_left;
            for (y = ir.top; y < ir.bottom; y++) {
                UINT32 *px = (UINT32 *)g_bbits + (size_t)y * g_bw + ir.left;
                for (x = ir.left; x < ir.right; x++, px++) {
                    if (x < split)
                        *px &= 0x00FFFFFFu;     /* frame colour behind ours... */
                    else
                        *px = 0;                /* ...and behind the system buttons */
                }
            }
        }
    }

    BitBlt(hdc, pr.left, pr.top, w, h, g_bdc, pr.left, pr.top, SRCCOPY);
    EndPaint(hwnd, &ps);
}
