/* player.h - CelluloidPlayer: native FFmpeg + Win32 video player
 * Plain C. No frameworks, no libmpv.
 *
 * Engine (ff.c):     demux/video/audio threads, packet queues, frame slots,
 *                     audio-master clock (waveOut), seeking, PNG export,
 *                     clipboard copy, frame step back/forward.
 * UI core (ui_core.c): window, input, hotkeys, menus, timer, playlist.
 * UI draw (ui_draw.c): dark chrome, GDI vector icons, native caption buttons.
 */
#ifndef PLAYER_H
#define PLAYER_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <timeapi.h>
#include <dwmapi.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CLASS_W  L"CelluloidPlayer"
#define APP_TITLE_W  L"Celluloid Player"

/* layout constants, 96-dpi pixels (scaled at runtime) */
enum { L_BAR_H = 44, L_EDGE = 6, L_ICON = 14, L_MIN_W = 420 };

/* resource ids (assets/app.rc) */
#define IDI_VIDEO  1
#define IDI_MUSIC  2

/* WM_COPYDATA id used to hand files to the running instance */
#define COPYDATA_OPEN  0x43504C31

/* theme (Adwaita dark) */
#define COL_VIDEO   RGB(0,0,0)
#define COL_CAPTION RGB(0,0,0)
#define COL_TEXT    RGB(238,238,236)
#define COL_DIM     RGB(160,160,158)
#define COL_ACCENT  RGB(53,132,228)

/* mouse hit ids */
enum {
    HT_NONE = 0,
    HT_CAP_FULL, HT_CAP_MENU,
    HT_BAR_PREV, HT_BAR_PLAY, HT_BAR_STOP, HT_BAR_NEXT,
    HT_SEEK, HT_TIME, HT_LOOP, HT_VOL, HT_VOL_TRACK, HT_VIDEO
};

/* ---------------- packet queue ---------------- */
typedef struct PktNode {
    AVPacket        pkt;        /* owns one reference */
    struct PktNode *next;
    int             serial;
} PktNode;

typedef struct {
    PktNode *first, *last;
    int      nb, serial;
    size_t   bytes;
} PktQ;

/* ---------------- decoded frame slots ---------------- */
typedef struct {
    AVFrame *bgra;      /* pre-allocated AV_PIX_FMT_BGRA frame */
    AVFrame *yuv;       /* the decoded frame (reference), moved to the history
                         * when the slot is displayed */
    double   pts;       /* seconds */
    int      serial;
    int      state;     /* 0 = free, 1 = owned (fifo / shown),
                         * 2 = being filled by the video thread,
                         * 3 = the reserved "view" slot (history display) */
} Slot;

#define SLOT_MAX 16

/* frame history for stepping back: decoded YUV frames, newest at yh_head */
#define YH_MAX 48
typedef struct { AVFrame *f; double pts; } YEnt;

/* ---------------- global player state ---------------- */
typedef struct Player {
    /* playlist */
    wchar_t (*pl)[1024];
    int     pl_n, pl_cur, pl_cap;

    /* ffmpeg */
    AVFormatContext *fmt;
    int      vs, as;            /* stream indexes, -1 = none */
    AVCodecContext *vctx, *actx;
    AVStream *vst, *ast;
    struct SwsContext *sws;
    struct SwrContext *swr;
    int      vw, vh;            /* video size, 0 = audio only */
    AVFrame *cover;             /* BGRA cover art of an audio file, or NULL */

    /* threads */
    HANDLE   th[3];             /* demux, video, audio */
    volatile int quit;

    /* sync / serial */
    int      serial_gen, cur_serial;
    volatile int seek_req;
    double   seek_target;
    volatile int seek_pending;  /* first frame of current seek not yet shown */
    volatile int eof;
    volatile int video_done, audio_done;
    double   dur;
    double   last_vpts;         /* video-thread only */

    /* audio master clock (waveOut sample position) */
    int      has_audio;
    HWAVEOUT wo;
    WAVEHDR  wh[10];
    uint8_t *whdata[10];
    int      wh_bytes;
    int      sample_rate;
    double   audio_base;
    volatile unsigned long long samples_written;
    volatile int audio_reset_pending;
    double   audio_target;
    int      audio_started;
    uint8_t *aubuf;
    int      aubuf_len, aubuf_cap;

    /* no-audio fallback clock (QPC) */
    double   sys_base;
    LONGLONG sys_t0_ticks;
    LARGE_INTEGER qpc_freq;
    volatile int paused;
    volatile int clock_sys;     /* audio finished: QPC clock takes over */
    double   pend_target;       /* seek target the ui is catching up to */

    /* queues and slots (guarded by lock) */
    SRWLOCK  lock;
    PktQ     pqv, pqa;

    Slot     slot[SLOT_MAX];
    int      slot_n;
    int      fifo[SLOT_MAX];            /* undisplayed slots, oldest at fifo_r */
    int      fifo_n, fifo_r;
    double  *kf;                        /* keyframe times from the index (sorted) */
    int      kf_n;
    YEnt     yh[YH_MAX];                /* history ring (decoded YUV frames) */
    int      yh_cap, yh_n, yh_head;
    int      yh_pos;                    /* frames stepped back from newest */
    int      view_slot;                 /* BGRA slot used to show history frames */
    struct SwsContext *sws_view;        /* ui-thread converter for the view slot */
    int      shown_slot;                /* -1 = nothing on screen */

    /* ui state (ui thread only) */
    HWND     hwnd;
    int      playing;
    int      loop_file;
    double   volume;                    /* 0..1 */
    int      muted;
    int      fullscreen;
    WINDOWPLACEMENT wp;
    DWORD    last_input;
    int      bars_visible, menu_open, mouse_tracked;
    int      hover, hover_down;
    int      dragging;                  /* 0 none, 1 seek, 2 volume */
    double   drag_pos;
    int      pre_click_paused;
    int      dpi;
    int      ui_last_cur, ui_last_playing, ui_last_q;
    wchar_t  time_txt[64];
    int      time_w;
    wchar_t  toast[192];
    DWORD    toast_until;

    HFONT    fnt_cap, fnt_bar, fnt_hint;

    /* video geometry / seek state added for the overlay ui */
    double   vdar, vfd;                 /* display aspect ratio, frame duration */
    volatile int media_gen;             /* ++ on every successful ff_open */
    volatile int seek_mode, pend_mode;  /* 0 = land on/after target, 1 = step back */
    int      pend_slot;                 /* newest catch-up frame while seeking */
    volatile int stepped;               /* user stepped frames while paused */

    int      ctl_hidden;                /* "Toggle Controls" */
    int      scrub_was_paused;
    DWORD    last_scrub;
    double   scrub_sent;
    int      last_mx, last_my;
    int      ui_last_gen;
    int      ui_last_n;
    LONG_PTR fs_style;

    /* native caption: height and left edge of the system buttons (client px) */
    int      cap_h, cap_btn_left;
    HICON    ico_video_big, ico_video_small, ico_music_big, ico_music_small;
    HICON    ico_small;                 /* icon currently shown in the caption */
} Player;

extern Player g_player;

/* ---------------- ff.c ---------------- */
int    ff_open(Player *p, const wchar_t *path);
void   ff_close(Player *p);             /* unload media, keep playlist */
void   ff_shutdown(Player *p);          /* unload + free playlist */
int    ff_seek(Player *p, double sec);
int    ff_scrub(Player *p, double sec);       /* exact frame for the seekbar drag */
double ff_clock(Player *p);
double ff_pos(Player *p);
void   ff_set_paused(Player *p, int pause);
void   ff_set_volume(Player *p);
int    ff_step_fwd(Player *p);          /* 1 = new frame shown */
int    ff_step_back(Player *p);         /* 1 = older frame shown */
int    ff_tick(Player *p);              /* 1 = new frame shown (repaint video) */
int    ff_eop(Player *p);               /* end of playback reached */
int    ff_at_end(Player *p);            /* everything played (even if paused) */
int    ff_save_frame_png(Player *p, wchar_t *out_name, int out_cap);
int    ff_copy_frame(Player *p);        /* clipboard CF_DIB */

/* ---------------- ui_draw.c ---------------- */
void   ui_init_fonts(Player *p);
void   ui_free_fonts(Player *p);
void   ui_layout(const Player *p, RECT *cap, RECT *video, RECT *bar);
void   ui_caption_update(Player *p);   /* re-read native caption metrics */
int    ui_cap_h(const Player *p);      /* caption strip height, client px */
int    ui_hit(Player *p, int x, int y);
void   ui_paint(HWND hwnd);
void   ui_invalidate(Player *p, int video, int chrome);
void   ui_fmt_time(wchar_t *o, int cap, double s);
void   ui_gfx_init(void);
void   ui_gfx_free(void);
double ui_seek_frac(const Player *p, int x);   /* mouse x -> 0..1 on the seek track */
double ui_vol_frac(const Player *p, int y);    /* mouse y -> 0..1 on the volume popup */
void   ui_menu_anchor(const Player *p, POINT *pt);  /* client coords, menu button */

/* ---------------- ui_core.c ---------------- */
void   ui_toast(Player *p, const wchar_t *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* PLAYER_H */
