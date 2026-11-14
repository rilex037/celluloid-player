/* ui_core.c - window, input handling, hotkeys, menus, timer, playlist */
#include "player.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

Player g_player;

static int S_(int v) { return MulDiv(v, g_player.dpi, 96); }

/* ---------------- playlist ---------------- */

static void pl_append(Player *p, const wchar_t *f)
{
    if (p->pl_n == p->pl_cap) {
        p->pl_cap = p->pl_cap ? p->pl_cap * 2 : 16;
        p->pl = (wchar_t (*)[1024])realloc(p->pl,
                (size_t)p->pl_cap * sizeof(wchar_t) * 1024);
    }
    if (!p->pl)
        return;
    lstrcpynW(p->pl[p->pl_n], f, 1024);
    p->pl_n++;
}

/* insert a file at position `at` (clamped), growing the list as needed */
static void pl_insert(Player *p, int at, const wchar_t *f)
{
    if (at < 0)
        at = 0;
    if (at > p->pl_n)
        at = p->pl_n;
    int n0 = p->pl_n;
    pl_append(p, f);                    /* grows the buffer, adds at the end */
    if (p->pl_n == n0)
        return;                         /* out of memory */
    if (at < p->pl_n - 1) {
        wchar_t tmp[1024];
        lstrcpynW(tmp, p->pl[p->pl_n - 1], 1024);
        memmove(p->pl[at + 1], p->pl[at],
                (size_t)(p->pl_n - 1 - at) * sizeof(wchar_t) * 1024);
        lstrcpynW(p->pl[at], tmp, 1024);
    }
}

static void set_title(Player *p)
{
    if (p->playing && p->pl_n > 0) {
        const wchar_t *path = p->pl[p->pl_cur];
        const wchar_t *f = wcsrchr(path, L'\\');
        if (f)
            f++;
        else {
            f = wcsrchr(path, L'/');
            if (f)
                f++;
            else
                f = path;
        }
        SetWindowTextW(p->hwnd, f);
    } else {
        SetWindowTextW(p->hwnd, APP_TITLE_W);
    }
}

/* music icon for audio-only files, video icon otherwise (and by default) */
static void set_app_icon(Player *p)
{
    int audio = (p->playing && p->vs < 0);
    HICON big = audio ? p->ico_music_big : p->ico_video_big;
    HICON small = audio ? p->ico_music_small : p->ico_video_small;
    if (small == p->ico_small)
        return;
    p->ico_small = small;
    if (big)
        SendMessageW(p->hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (small)
        SendMessageW(p->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
}

/* the caption strip is DWM frame: extend it so the system buttons show */
static void update_frame_margins(Player *p)
{
    static int last = -1;
    int h = p->fullscreen ? 0 : ui_cap_h(p);
    if (h == last)
        return;
    last = h;
    MARGINS m;
    ZeroMemory(&m, sizeof(m));
    m.cyTopHeight = h;
    DwmExtendFrameIntoClientArea(p->hwnd, &m);
}

static void caption_refresh(Player *p)
{
    if (!p->hwnd)
        return;
    ui_caption_update(p);
    update_frame_margins(p);
}

/* ---------------- small ui helpers ---------------- */

void ui_toast(Player *p, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(p->toast, 191, fmt, ap);
    p->toast[191] = 0;
    va_end(ap);
    p->toast_until = GetTickCount() + 2200;
    ui_invalidate(p, 2, 0);
}

static void toggle_pause(Player *p)
{
    if (p->paused && ff_at_end(p))
        ff_seek(p, 0);              /* play after the end = start over */
    ff_set_paused(p, !p->paused);
    ui_invalidate(p, 0, 2);
}

static void do_stop(Player *p)
{
    ff_seek(p, 0);
    ff_set_paused(p, 1);
    ui_invalidate(p, 1, 1);
}

static void toggle_loop(Player *p)
{
    p->loop_file = !p->loop_file;
    ui_invalidate(p, 0, 2);
}

static void set_volume(Player *p, double v)
{
    if (v < 0)
        v = 0;
    if (v > 1)
        v = 1;
    p->volume = v;
    if (v > 0 && p->muted)
        p->muted = 0;
    ff_set_volume(p);
    ui_invalidate(p, 0, 2);
}

static void toggle_mute(Player *p)
{
    p->muted = !p->muted;
    ff_set_volume(p);
    ui_invalidate(p, 0, 2);
}

static void do_save(Player *p)
{
    wchar_t name[96];
    if (ff_save_frame_png(p, name, 96) == 0)
        ui_toast(p, L"Saved to Pictures: %s", name);
    else
        ui_toast(p, L"Could not save frame");
}

static void do_copy(Player *p)
{
    if (ff_copy_frame(p) == 0)
        ui_toast(p, L"Frame copied to clipboard");
    else
        ui_toast(p, L"Copy failed");
}

static void step_frame(Player *p, int back)
{
    ff_set_paused(p, 1);
    if (back)
        ff_step_back(p);
    else
        ff_step_fwd(p);
    ui_invalidate(p, 1, 1);
}

/* show the frame under the knob (called on mouse move and from the timer so
 * the last position is rendered even when the mouse has stopped) */
static void scrub_to_knob(Player *p)
{
    DWORD now = GetTickCount();
    if (p->seek_req || p->seek_pending)
        return;
    if (now - p->last_scrub < 8 || fabs(p->drag_pos - p->scrub_sent) < 0.004)
        return;
    p->scrub_sent = p->drag_pos;
    p->last_scrub = now;
    if (ff_scrub(p, p->drag_pos))
        ui_invalidate(p, 1, 0);
}

static void seek_rel(Player *p, double d)
{
    if (p->playing)
        ff_seek(p, ff_pos(p) + d);
}

/* ---------------- fullscreen / window fitting ---------------- */

static void toggle_fullscreen(Player *p)
{
    HWND h = p->hwnd;
    if (!p->fullscreen) {
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        p->wp.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(h, &p->wp);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        p->fs_style = GetWindowLongPtrW(h, GWL_STYLE);
        p->fullscreen = 1;              /* before the style change */
        p->bars_visible = 1;
        p->last_input = GetTickCount();
        /* no WS_OVERLAPPEDWINDOW at all: no border, no drag, no resize */
        SetWindowLongPtrW(h, GWL_STYLE, p->fs_style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        p->fullscreen = 0;
        p->bars_visible = 1;
        SetWindowLongPtrW(h, GWL_STYLE, p->fs_style);
        SetWindowPlacement(h, &p->wp);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    caption_refresh(p);
    InvalidateRect(h, NULL, FALSE);
}

/* outer size minus client size: side + bottom frame (no caption) */
static void frame_extra(int *dw, int *dh)
{
    int pad = GetSystemMetrics(SM_CXPADDEDBORDER);
    *dw = 2 * (GetSystemMetrics(SM_CXSIZEFRAME) + pad);
    *dh = GetSystemMetrics(SM_CYSIZEFRAME) + pad;
}

/* resize the window so the video fills it exactly (no black bars) */
static void fit_window(Player *p)
{
    HWND h = p->hwnd;
    if (p->fullscreen || IsZoomed(h) || IsIconic(h) || p->vw <= 0 || p->vdar <= 0)
        return;
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    int ww = mi.rcWork.right - mi.rcWork.left;
    int wh = mi.rcWork.bottom - mi.rcWork.top;
    int cap = ui_cap_h(p);
    double dar = p->vdar;
    double vh = p->vh, vw = vh * dar;
    double maxw = ww * 0.85, maxh = wh * 0.85 - cap;
    double sc = 1.0;
    if (vw > maxw)
        sc = maxw / vw;
    if (vh * sc > maxh)
        sc = maxh / vh;
    vw *= sc;
    vh *= sc;
    if (vw < S_(640)) {                 /* small videos: show them larger */
        vw = S_(640);
        vh = vw / dar;
        if (vh > maxh) {
            vh = maxh;
            vw = vh * dar;
        }
    }
    if (vw < S_(L_MIN_W)) {
        vw = S_(L_MIN_W);
        vh = vw / dar;
    }
    int W = (int)(vw + 0.5), H = (int)(vh + 0.5) + cap;
    int dw, dh;
    frame_extra(&dw, &dh);
    W += dw;
    H += dh;
    RECT r;
    GetWindowRect(h, &r);
    int x = (r.left + r.right) / 2 - W / 2;
    int y = (r.top + r.bottom) / 2 - H / 2;
    if (x + W > mi.rcWork.right)  x = mi.rcWork.right - W;
    if (y + H > mi.rcWork.bottom) y = mi.rcWork.bottom - H;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top)  y = mi.rcWork.top;
    SetWindowPos(h, NULL, x, y, W, H, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---------------- file opening ---------------- */

static void play_open(Player *p, const wchar_t *path)
{
    if (ff_open(p, path) < 0)
        ui_toast(p, L"Could not open this file");
    set_title(p);
    set_app_icon(p);
    ui_invalidate(p, 1, 1);
}

/* Play the given files right now but keep what was played before: they are
 * inserted after the current entry, so Previous goes back to it and Next
 * continues with whatever was queued behind it. */
static void open_list(Player *p, const wchar_t (*list)[1024], int n)
{
    int i, at;
    if (n <= 0)
        return;
    if (n == 1 && p->pl_n > 0 && p->playing &&
        _wcsicmp(p->pl[p->pl_cur], list[0]) == 0) {
        ff_seek(p, 0);                  /* same file again: just restart it */
        ff_set_paused(p, 0);
        ui_invalidate(p, 1, 1);
        return;
    }
    at = (p->pl_n > 0) ? p->pl_cur + 1 : 0;
    for (i = 0; i < n; i++)
        pl_insert(p, at + i, list[i]);
    p->pl_cur = at;
    play_open(p, p->pl[at]);
}

static void open_dialog(Player *p)
{
    wchar_t *buf = (wchar_t *)calloc(1, 131072 + 2);
    if (!buf)
        return;
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = p->hwnd;
    ofn.lpstrFilter = L"Media files\0"
        L"*.mp4;*.m4v;*.mkv;*.webm;*.avi;*.mov;*.flv;*.wmv;*.ts;*.mts;"
        L"*.mpg;*.mpeg;*.3gp;*.ogv;*.m2ts;*.vob;"
        L"*.mp3;*.flac;*.wav;*.m4a;*.aac;*.opus;*.ogg;*.wma\0"
        L"Video\0"
        L"*.mp4;*.m4v;*.mkv;*.webm;*.avi;*.mov;*.flv;*.wmv;*.ts;*.mts;"
        L"*.mpg;*.mpeg;*.3gp;*.ogv;*.m2ts;*.vob\0"
        L"Audio\0*.mp3;*.flac;*.wav;*.m4a;*.aac;*.opus;*.ogg;*.wma\0"
        L"All files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 65536;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER |
                OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) {
        wchar_t (*list)[1024] = NULL;
        int n = 0;
        wchar_t *c = buf + wcslen(buf) + 1;
        if (*c == 0) {
            list = (wchar_t (*)[1024])calloc(1, sizeof(wchar_t) * 1024);
            if (list) {
                lstrcpynW(list[0], buf, 1024);
                n = 1;
            }
        } else {
            int cnt = 0;
            wchar_t *q;
            for (q = c; *q; q += wcslen(q) + 1)
                cnt++;
            list = (wchar_t (*)[1024])calloc((size_t)cnt, sizeof(wchar_t) * 1024);
            while (list && *c) {
                _snwprintf(list[n], 1023, L"%s\\%s", buf, c);
                list[n][1023] = 0;
                n++;
                c += wcslen(c) + 1;
            }
        }
        if (list) {
            open_list(p, (const wchar_t (*)[1024])list, n);
            free(list);
        }
    }
    free(buf);
}

/* previous / next entry of the played-files list; no wrap-around */
static int can_go(const Player *p, int dir)
{
    int t = p->pl_cur + dir;
    return p->pl_n > 0 && t >= 0 && t < p->pl_n;
}

static void go_next(Player *p, int dir)
{
    if (!can_go(p, dir))
        return;
    p->pl_cur += dir;
    play_open(p, p->pl[p->pl_cur]);
}

/* ---------------- menus ---------------- */

enum { M_COPY = 1, M_SAVE, M_PLAY, M_FFW, M_FBW, M_LOOP, M_MUTE, M_FULL,
       M_OPEN, M_PREV, M_NEXT, M_EXIT, M_CTRL };

static void do_menu(Player *p, int id)
{
    switch (id) {
    case M_COPY:  do_copy(p); break;
    case M_SAVE:  do_save(p); break;
    case M_PLAY:  toggle_pause(p); break;
    case M_FFW:   step_frame(p, 0); break;
    case M_FBW:   step_frame(p, 1); break;
    case M_LOOP:  toggle_loop(p); break;
    case M_MUTE:  toggle_mute(p); break;
    case M_FULL:  toggle_fullscreen(p); break;
    case M_OPEN:  open_dialog(p); break;
    case M_PREV:  go_next(p, -1); break;
    case M_NEXT:  go_next(p, 1); break;
    case M_CTRL:  p->ctl_hidden = !p->ctl_hidden; ui_invalidate(p, 1, 1); break;
    case M_EXIT:  DestroyWindow(p->hwnd); break;
    default: break;
    }
}

static void show_menu(Player *p, HWND hwnd, int x, int y, int context)
{
    HMENU m = CreatePopupMenu();
    int frame_ok = (p->playing && p->shown_slot >= 0);
    UINT fg = frame_ok ? 0 : MF_GRAYED;
    if (context) {
        AppendMenuW(m, MF_STRING | fg, M_COPY, L"Copy video frame\tCtrl+C");
        AppendMenuW(m, MF_STRING | fg, M_SAVE, L"Save frame to Pictures\tS");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (p->playing ? 0 : MF_GRAYED), M_PLAY,
                    p->paused ? L"Play\tSpace" : L"Pause\tSpace");
        AppendMenuW(m, MF_STRING | (p->playing ? 0 : MF_GRAYED),
                    M_FFW, L"Frame forward\t.");
        AppendMenuW(m, MF_STRING | (p->playing ? 0 : MF_GRAYED),
                    M_FBW, L"Frame back\t,");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (p->loop_file ? MF_CHECKED : 0),
                    M_LOOP, L"Loop this file");
        AppendMenuW(m, MF_STRING | (p->muted ? MF_CHECKED : 0),
                    M_MUTE, L"Mute\tM");
        AppendMenuW(m, MF_STRING | (p->fullscreen ? MF_CHECKED : 0),
                    M_FULL, L"Fullscreen\tF");
        AppendMenuW(m, MF_STRING | (p->ctl_hidden ? 0 : MF_CHECKED),
                    M_CTRL, L"Show controls");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, M_OPEN, L"Open file\x2026\tO");
        AppendMenuW(m, MF_STRING | (can_go(p, -1) ? 0 : MF_GRAYED),
                    M_PREV, L"Previous file");
        AppendMenuW(m, MF_STRING | (can_go(p, 1) ? 0 : MF_GRAYED),
                    M_NEXT, L"Next file");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, M_EXIT, L"Exit");
    } else {
        AppendMenuW(m, MF_STRING, M_OPEN, L"Open file\x2026\tO");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | fg, M_SAVE, L"Save frame to Pictures");
        AppendMenuW(m, MF_STRING | fg, M_COPY, L"Copy video frame");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (p->ctl_hidden ? 0 : MF_CHECKED),
                    M_CTRL, L"Show controls");
        AppendMenuW(m, MF_STRING | (p->loop_file ? MF_CHECKED : 0),
                    M_LOOP, L"Loop this file");
        AppendMenuW(m, MF_STRING | (p->fullscreen ? MF_CHECKED : 0),
                    M_FULL, L"Fullscreen\tF");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, M_EXIT, L"Exit");
    }
    p->menu_open = 1;
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON |
                               (context ? 0 : TPM_RIGHTALIGN),
                            x, y, 0, hwnd, NULL);
    p->menu_open = 0;
    DestroyMenu(m);
    p->last_input = GetTickCount();
    do_menu(p, id);
}

/* ---------------- timer ---------------- */

static void on_timer(Player *p)
{
    int rep = ff_tick(p);

    if (p->dragging == 1)
        scrub_to_knob(p);

    if (p->playing && !p->paused && ff_eop(p)) {
        if (p->loop_file)
            ff_seek(p, 0);
        else if (can_go(p, 1))
            go_next(p, 1);
        else
            ff_set_paused(p, 1);
        ui_invalidate(p, 1, 1);
    }

    if (p->media_gen != p->ui_last_gen) {       /* new media: fit the window */
        p->ui_last_gen = p->media_gen;
        set_title(p);
        set_app_icon(p);
        fit_window(p);
        ui_invalidate(p, 1, 1);
    }

    if (p->pl_cur != p->ui_last_cur || p->playing != p->ui_last_playing ||
        p->pl_n != p->ui_last_n) {
        p->ui_last_cur = p->pl_cur;
        p->ui_last_playing = p->playing;
        p->ui_last_n = p->pl_n;
        set_title(p);
        set_app_icon(p);
        ui_invalidate(p, 0, 2);         /* prev / next enabled state */
    }

    if (p->playing) {
        double cur = (p->dragging == 1) ? p->drag_pos : ff_pos(p);
        wchar_t a[32], b[32], tt[64];
        ui_fmt_time(a, 32, cur);
        if (p->dur > 0) {
            ui_fmt_time(b, 32, p->dur);
            _snwprintf(tt, 63, L"%s/%s", a, b);
        } else {
            _snwprintf(tt, 63, L"%s", a);
        }
        tt[63] = 0;
        int q = (int)(cur * 15.0);              /* keeps the seekbar smooth */
        if (wcscmp(tt, p->time_txt) != 0 || q != p->ui_last_q) {
            lstrcpynW(p->time_txt, tt, 64);
            p->ui_last_q = q;
            ui_invalidate(p, 0, 2);
        }
    }

    if (p->toast[0] && GetTickCount() > p->toast_until) {
        p->toast[0] = 0;
        ui_invalidate(p, 2, 0);
    }

    if (p->fullscreen && p->bars_visible && !p->menu_open && !p->dragging &&
        (p->hover == HT_NONE || p->hover == HT_VIDEO) &&
        GetTickCount() - p->last_input > 2500) {
        p->bars_visible = 0;
        ui_invalidate(p, 1, 1);
    }

    if (rep)
        ui_invalidate(p, 1, 0);
}

/* ---------------- window proc ---------------- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Player *p = &g_player;

    {   /* the native min / max / close buttons are handled by DWM */
        LRESULT dr = 0;
        if (DwmDefWindowProc(hwnd, msg, wp, lp, &dr))
            return dr;
    }

    switch (msg) {

    case WM_NCCALCSIZE:
        if (wp) {
            if (!p->fullscreen && !IsZoomed(hwnd)) {
                /* normal frame on left/right/bottom so the client lines up
                 * with the visible window; no caption: it is our strip */
                NCCALCSIZE_PARAMS *pr = (NCCALCSIZE_PARAMS *)lp;
                LONG top = pr->rgrc[0].top;
                LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
                pr->rgrc[0].top = top;
                return r;
            }
            if (IsZoomed(hwnd) && !p->fullscreen) {
                NCCALCSIZE_PARAMS *pr = (NCCALCSIZE_PARAMS *)lp;
                int fx = GetSystemMetrics(SM_CXSIZEFRAME) +
                         GetSystemMetrics(SM_CXPADDEDBORDER);
                int fy = GetSystemMetrics(SM_CYSIZEFRAME) +
                         GetSystemMetrics(SM_CXPADDEDBORDER);
                pr->rgrc[0].left += fx;
                pr->rgrc[0].right -= fx;
                pr->rgrc[0].top += fy;
                pr->rgrc[0].bottom -= fy;
            }
            return 0;   /* client area = whole window, DWM paints the buttons */
        }
        break;

    case WM_NCHITTEST: {
        LRESULT base = DefWindowProcW(hwnd, msg, wp, lp);
        if (base != HTCLIENT || p->fullscreen)
            return base;                /* fullscreen: never drag or resize */
        POINT pt;
        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
        ScreenToClient(hwnd, &pt);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (!IsZoomed(hwnd)) {          /* sides/bottom: native frame; top: ours */
            int e = S_(L_EDGE), e2 = e * 2;
            if (pt.y < e)
                return (pt.x < e2) ? HTTOPLEFT :
                       (pt.x > rc.right - e2) ? HTTOPRIGHT : HTTOP;
        }
        if (pt.y < ui_cap_h(p)) {
            int hit = ui_hit(p, pt.x, pt.y);
            if (hit == HT_CAP_FULL || hit == HT_CAP_MENU)
                return HTCLIENT;
            return HTCAPTION;
        }
        return base;
    }

    case WM_NCLBUTTONDBLCLK:
        if (!p->fullscreen && wp == HTCAPTION) {
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (p->fullscreen && !p->bars_visible && LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        int dw, dh;
        frame_extra(&dw, &dh);
        mm->ptMinTrackSize.x = S_(L_MIN_W) + dw;
        mm->ptMinTrackSize.y = ui_cap_h(p) + S_(120) + dh;
        return 0;
    }

    case WM_SIZE:
    case WM_DWMCOMPOSITIONCHANGED:
        caption_refresh(p);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_PAINT:
        ui_paint(hwnd);
        return 0;

    case WM_TIMER:
        if (wp == 1)
            on_timer(p);
        return 0;

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (!p->dragging && x == p->last_mx && y == p->last_my)
            return 0;                   /* not a real move (e.g. relayout) */
        p->last_mx = x;
        p->last_my = y;
        p->last_input = GetTickCount();
        if (p->fullscreen && !p->bars_visible) {
            p->bars_visible = 1;
            ui_invalidate(p, 1, 1);
        }
        if (!p->mouse_tracked) {
            TRACKMOUSEEVENT t;
            ZeroMemory(&t, sizeof(t));
            t.cbSize = sizeof(t);
            t.hwndTrack = hwnd;
            t.dwFlags = TME_LEAVE;
            TrackMouseEvent(&t);
            p->mouse_tracked = 1;
        }
        if (p->dragging == 1 && p->dur > 0) {
            p->drag_pos = ui_seek_frac(p, x) * p->dur;
            /* live scrubbing: follow the knob, one seek at a time */
            scrub_to_knob(p);
            ui_invalidate(p, 0, 2);
        } else if (p->dragging == 2) {
            set_volume(p, ui_vol_frac(p, y));
        } else {
            int h = ui_hit(p, x, y);
            if (h != p->hover) {
                p->hover = h;
                ui_invalidate(p, 0, 1);
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        p->mouse_tracked = 0;
        p->last_mx = p->last_my = -1;
        if (p->hover) {
            p->hover = HT_NONE;
            p->hover_down = HT_NONE;
            ui_invalidate(p, 0, 1);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        set_volume(p, p->volume + (d > 0 ? 0.05 : -0.05));
        p->last_input = GetTickCount();
        return 0;
    }

    case WM_LBUTTONDOWN: {
        p->last_input = GetTickCount();
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int hit = ui_hit(p, x, y);
        switch (hit) {
        case HT_SEEK:
            if (p->playing && p->dur > 0) {
                p->dragging = 1;
                p->scrub_was_paused = p->paused;
                ff_set_paused(p, 1);            /* silent while scrubbing */
                p->drag_pos = ui_seek_frac(p, x) * p->dur;
                p->scrub_sent = -1;
                scrub_to_knob(p);
                SetCapture(hwnd);
                ui_invalidate(p, 0, 2);
            }
            break;
        case HT_VOL_TRACK:
            p->dragging = 2;
            set_volume(p, ui_vol_frac(p, y));
            SetCapture(hwnd);
            break;
        case HT_VOL:
            toggle_mute(p);
            break;
        default:
            if (hit != HT_NONE && hit != HT_TIME) {
                p->hover_down = hit;
                ui_invalidate(p, 0, 1);
            }
            break;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        p->last_input = GetTickCount();
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int hit = ui_hit(p, x, y);
        if (p->dragging) {
            int was = p->dragging;
            p->dragging = 0;
            ReleaseCapture();
            if (was == 1) {
                ff_seek(p, p->drag_pos);        /* exact final position */
                if (!p->scrub_was_paused)
                    ff_set_paused(p, 0);
            }
            ui_invalidate(p, 0, 1);
        } else if (hit != HT_NONE && hit != HT_TIME && p->hover_down == hit) {
            switch (hit) {
            case HT_BAR_PREV: go_next(p, -1); break;
            case HT_BAR_PLAY: toggle_pause(p); break;
            case HT_BAR_STOP: do_stop(p); break;
            case HT_BAR_NEXT: go_next(p, 1); break;
            case HT_LOOP:     toggle_loop(p); break;
            case HT_CAP_FULL: toggle_fullscreen(p); break;
            case HT_CAP_MENU: {
                POINT pt;
                ui_menu_anchor(p, &pt);
                ClientToScreen(hwnd, &pt);
                show_menu(p, hwnd, pt.x, pt.y, 0);
                break;
            }
            default: break;
            }
        }
        p->hover_down = HT_NONE;
        ui_invalidate(p, 0, 1);
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (ui_hit(p, x, y) == HT_VIDEO)
            toggle_fullscreen(p);
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (p->dragging == 1 && !p->scrub_was_paused)
            ff_set_paused(p, 0);
        p->dragging = 0;
        return 0;

    case WM_CONTEXTMENU: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (x == -1 && y == -1) {   /* keyboard */
            RECT rw;
            GetWindowRect(hwnd, &rw);
            x = (rw.left + rw.right) / 2;
            y = (rw.top + rw.bottom) / 2;
        }
        POINT pt;
        pt.x = x;
        pt.y = y;
        ScreenToClient(hwnd, &pt);
        if (ui_hit(p, pt.x, pt.y) == HT_VIDEO)
            show_menu(p, hwnd, x, y, 1);
        return 0;
    }

    case WM_DROPFILES: {
        HDROP h = (HDROP)wp;
        int n = (int)DragQueryFileW(h, 0xFFFFFFFF, NULL, 0);
        int i;
        wchar_t (*list)[1024] = NULL;
        if (n > 0)
            list = (wchar_t (*)[1024])calloc((size_t)n, sizeof(wchar_t) * 1024);
        if (list)
            for (i = 0; i < n; i++)
                DragQueryFileW(h, (UINT)i, list[i], 1024);
        DragFinish(h);
        p->last_input = GetTickCount();
        if (list) {                         /* play right away, keep the history */
            SetForegroundWindow(hwnd);
            open_list(p, (const wchar_t (*)[1024])list, n);
            free(list);
        }
        return 0;
    }

    case WM_COPYDATA: {                     /* files from a second launch */
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        if (cd && cd->dwData == COPYDATA_OPEN && cd->lpData && cd->cbData >= 4) {
            const wchar_t *w = (const wchar_t *)cd->lpData;
            size_t total = cd->cbData / sizeof(wchar_t), pos = 0;
            int n = 0, k = 0;
            wchar_t (*list)[1024];
            while (pos < total && w[pos]) {         /* count */
                size_t l = wcsnlen(w + pos, total - pos);
                pos += l + 1;
                n++;
            }
            list = (n > 0) ? (wchar_t (*)[1024])calloc((size_t)n,
                                         sizeof(wchar_t) * 1024) : NULL;
            if (list) {
                pos = 0;
                while (pos < total && w[pos] && k < n) {
                    size_t l = wcsnlen(w + pos, total - pos);
                    lstrcpynW(list[k++], w + pos, 1024);
                    pos += l + 1;
                }
                open_list(p, (const wchar_t (*)[1024])list, k);
                free(list);
            }
        }
        return TRUE;
    }

    case WM_KEYDOWN: {
        p->last_input = GetTickCount();
        if (p->fullscreen && !p->bars_visible) {
            p->bars_visible = 1;
            ui_invalidate(p, 1, 1);
        }
        int fresh = !(lp & (1 << 30));    /* no auto-repeat */
        int ctrl = (GetKeyState(VK_CONTROL) < 0);
        switch ((int)wp) {
        case VK_SPACE:
        case 'K':
            if (fresh)
                toggle_pause(p);
            break;
        case VK_LEFT:  seek_rel(p, -5);  break;
        case VK_RIGHT: seek_rel(p, 5);   break;
        case 'J':      seek_rel(p, -10); break;
        case 'L':      seek_rel(p, 10);  break;
        case VK_UP:    set_volume(p, p->volume + 0.05); break;
        case VK_DOWN:  set_volume(p, p->volume - 0.05); break;
        case 'M':      if (fresh) toggle_mute(p); break;
        case 'F':      if (fresh) toggle_fullscreen(p); break;
        case 'S':      if (p->playing) do_save(p); break;
        case 'O':      if (fresh) open_dialog(p); break;
        case VK_HOME:  ff_seek(p, 0); break;
        case VK_END:   ff_seek(p, p->dur - 0.05); break;
        case VK_ESCAPE:
            if (p->fullscreen)
                toggle_fullscreen(p);
            break;
        case 'C':
            if (ctrl && p->playing)
                do_copy(p);
            break;
        default:
            if (wp >= '0' && wp <= '9' && p->dur > 0)
                ff_seek(p, p->dur * (double)(wp - '0') / 10.0);
            break;
        }
        return 0;
    }

    case WM_CHAR:
        /* frame stepping on the real characters (layout-safe). This is the
         * only place: handling the key in WM_KEYDOWN as well stepped twice
         * per press and ate the frame history in a few clicks. */
        if (wp == L',' || wp == L'.') {
            if (p->playing)
                step_frame(p, wp == L',');
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------- entry ---------------- */

/* make the native popup menus dark (Windows 10 1903+ / 11, undocumented) */
static void enable_dark_menus(void)
{
    typedef int  (WINAPI *PFN_MODE)(int);
    typedef void (WINAPI *PFN_FLUSH)(void);
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux)
        return;
    PFN_MODE mode = (PFN_MODE)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    PFN_FLUSH flush = (PFN_FLUSH)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (mode)
        mode(2);                        /* ForceDark */
    if (flush)
        flush();
}

/* command-line arguments as absolute paths; returns the count (list is
 * calloc'ed, caller frees) */
static int cmdline_files(wchar_t (**out)[1024])
{
    int argc = 0, i, n = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t (*list)[1024] = NULL;
    *out = NULL;
    if (!argv)
        return 0;
    if (argc > 1)
        list = (wchar_t (*)[1024])calloc((size_t)argc, sizeof(wchar_t) * 1024);
    if (list) {
        for (i = 1; i < argc; i++) {
            if (GetFullPathNameW(argv[i], 1024, list[n], NULL) == 0)
                lstrcpynW(list[n], argv[i], 1024);
            n++;
        }
    }
    LocalFree(argv);
    *out = list;
    return n;
}

/* a second launch hands its files to the running window and quits.
 * Returns 1 if this process should exit. */
static int forward_to_running(HANDLE mutex_owner_check)
{
    HWND other = NULL;
    int tries;
    wchar_t (*list)[1024];
    int n, i;
    (void)mutex_owner_check;

    for (tries = 0; tries < 60 && !other; tries++) {    /* it may still start up */
        other = FindWindowW(APP_CLASS_W, NULL);
        if (!other)
            Sleep(100);
    }
    if (!other)
        return 1;
    AllowSetForegroundWindow(ASFW_ANY);     /* let the player take focus */
    n = cmdline_files(&list);
    if (n > 0) {
        size_t chars = 1, pos = 0;
        wchar_t *buf;
        for (i = 0; i < n; i++)
            chars += wcslen(list[i]) + 1;
        buf = (wchar_t *)calloc(chars, sizeof(wchar_t));
        if (buf) {
            COPYDATASTRUCT cd;
            for (i = 0; i < n; i++) {
                size_t l = wcslen(list[i]);
                memcpy(buf + pos, list[i], l * sizeof(wchar_t));
                pos += l + 1;
            }
            cd.dwData = COPYDATA_OPEN;
            cd.cbData = (DWORD)(chars * sizeof(wchar_t));
            cd.lpData = buf;
            SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cd);
            free(buf);
        }
        free(list);
    }
    if (IsIconic(other))
        ShowWindow(other, SW_RESTORE);
    SetForegroundWindow(other);
    return 1;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int nShow)
{
    (void)hPrev;
    (void)cmd;

    /* only one player at a time */
    HANDLE single = CreateMutexW(NULL, TRUE, L"Local\\CelluloidPlayer.SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        forward_to_running(single);
        CloseHandle(single);
        return 0;
    }

    SetProcessDPIAware();
    timeBeginPeriod(1);
    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();
    enable_dark_menus();
    ui_gfx_init();

    Player *p = &g_player;
    QueryPerformanceFrequency(&p->qpc_freq);
    p->volume = 1.0;
    p->bars_visible = 1;
    p->hover = HT_NONE;
    p->hover_down = HT_NONE;
    p->shown_slot = -1;
    p->pend_slot = -1;
    p->vs = p->as = -1;
    p->last_mx = p->last_my = -1;
    {
        HDC dc = GetDC(NULL);
        p->dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
        if (p->dpi < 96)
            p->dpi = 96;
    }

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    {
        int bw = GetSystemMetrics(SM_CXICON), bh = GetSystemMetrics(SM_CYICON);
        int sw = GetSystemMetrics(SM_CXSMICON), sh = GetSystemMetrics(SM_CYSMICON);
        p->ico_video_big   = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_VIDEO),
                                IMAGE_ICON, bw, bh, LR_DEFAULTCOLOR);
        p->ico_video_small = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_VIDEO),
                                IMAGE_ICON, sw, sh, LR_DEFAULTCOLOR);
        p->ico_music_big   = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_MUSIC),
                                IMAGE_ICON, bw, bh, LR_DEFAULTCOLOR);
        p->ico_music_small = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_MUSIC),
                                IMAGE_ICON, sw, sh, LR_DEFAULTCOLOR);
        p->ico_small = p->ico_video_small;      /* video icon by default */
        wc.hIcon = p->ico_video_big ? p->ico_video_big :
                   LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    }
    wc.lpszClassName = APP_CLASS_W;
    RegisterClassW(&wc);

    int dw0, dh0;
    frame_extra(&dw0, &dh0);
    int W = S_(800) + dw0, H = ui_cap_h(p) + S_(450) + dh0;
    HWND hwnd = CreateWindowExW(0, APP_CLASS_W, APP_TITLE_W,
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, W, H,
                                NULL, NULL, hInst, NULL);
    if (!hwnd)
        return 1;
    p->hwnd = hwnd;
    {   /* dark native caption buttons (white glyphs) */
        BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
            DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }
    if (p->ico_video_big)
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)p->ico_video_big);
    if (p->ico_video_small)
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)p->ico_video_small);
    /* first WM_NCCALCSIZE: client area covers the caption, DWM keeps the buttons */
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    caption_refresh(p);
    ui_init_fonts(p);
    DragAcceptFiles(hwnd, TRUE);
    SetTimer(hwnd, 1, 10, NULL);

    ShowWindow(hwnd, nShow ? nShow : SW_SHOW);
    caption_refresh(p);

    /* command line -> played files */
    {
        wchar_t (*list)[1024];
        int n = cmdline_files(&list);
        if (n > 0) {
            open_list(p, (const wchar_t (*)[1024])list, n);
            free(list);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ff_shutdown(p);
    ui_free_fonts(p);
    ui_gfx_free();
    timeEndPeriod(1);
    if (single)
        CloseHandle(single);
    return (int)msg.wParam;
}
