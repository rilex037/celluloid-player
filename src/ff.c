/* ff.c - media engine: demux/decode threads, packet queues, frame slots,
 * audio-master clock via waveOut, seeking with serial flushes, PNG export,
 * clipboard frame copy, frame step back/forward.
 */
#include "player.h"
#include <stdio.h>
#include <process.h>

#define WH_COUNT      10          /* waveOut buffers */
#define WH_MS         40          /* per buffer */
#define PKTQ_MAX_V    (24 << 20)  /* video packet queue byte cap */
#define PKTQ_MAX_A    (6 << 20)   /* audio packet queue byte cap */
#define PKTQ_MAX_N    1200

/* ============================ packet queue ============================ */

static void pktq_init(PktQ *q)
{
    q->first = q->last = NULL;
    q->nb = 0;
    q->bytes = 0;
    q->serial = 1;
}

/* caller holds p->lock */
static void pktq_flush(PktQ *q, int new_serial)
{
    PktNode *n = q->first;
    while (n) {
        PktNode *t = n->next;
        av_packet_unref(&n->pkt);
        av_free(n);
        n = t;
    }
    q->first = q->last = NULL;
    q->nb = 0;
    q->bytes = 0;
    q->serial = new_serial;
}

/* caller holds p->lock; returns 0 on success */
static int pktq_push(PktQ *q, AVPacket *src, size_t max_bytes)
{
    if (q->nb >= PKTQ_MAX_N || q->bytes + (size_t)src->size > max_bytes)
        return -1;
    PktNode *n = av_mallocz(sizeof(PktNode));
    if (!n)
        return -1;
    if (av_packet_ref(&n->pkt, src) < 0) {
        av_free(n);
        return -1;
    }
    n->serial = q->serial;
    n->next = NULL;
    if (q->last)
        q->last->next = n;
    else
        q->first = n;
    q->last = n;
    q->nb++;
    q->bytes += (size_t)n->pkt.size;
    return 0;
}

/* caller holds p->lock; caller frees node */
static PktNode *pktq_pop(PktQ *q)
{
    PktNode *n = q->first;
    if (!n)
        return NULL;
    q->first = n->next;
    if (!q->first)
        q->last = NULL;
    q->nb--;
    q->bytes -= (size_t)n->pkt.size;
    return n;
}

/* ============================ slot / fifo / history ============================ */

#define SLOT_N      10                  /* BGRA slots (<= 1080p) */
#define SLOT_N_BIG  7                   /* BGRA slots (larger than 1080p) */
#define YH_BUDGET   (64 << 20)          /* bytes of decoded YUV kept for stepping */

/* The history stores decoded YUV frames (reference counted, about 3/8 of the
 * size of a BGRA frame), so it can reach back 30-48 frames for the same memory
 * that used to hold 8. Only the frame on screen needs a BGRA slot. */

static YEnt *yh_at(Player *p, int k)    /* k = 0 newest */
{
    return &p->yh[(p->yh_head - k + p->yh_cap * 2) % p->yh_cap];
}

/* caller holds p->lock; takes ownership of f */
static void yh_push(Player *p, AVFrame *f, double pts)
{
    if (!f)
        return;
    if (p->yh_cap <= 0) {
        av_frame_free(&f);
        return;
    }
    p->yh_head = (p->yh_head + 1) % p->yh_cap;
    if (p->yh_n == p->yh_cap)
        av_frame_free(&p->yh[p->yh_head].f);        /* oldest falls out */
    else
        p->yh_n++;
    p->yh[p->yh_head].f = f;
    p->yh[p->yh_head].pts = pts;
}

/* caller holds p->lock */
static void yh_clear(Player *p)
{
    int i;
    for (i = 0; i < YH_MAX; i++)
        av_frame_free(&p->yh[i].f);
    p->yh_n = p->yh_head = p->yh_pos = 0;
}

/* caller holds p->lock: give a slot back */
static void slot_free(Player *p, int i)
{
    av_frame_free(&p->slot[i].yuv);
    p->slot[i].state = 0;
}

/* caller holds p->lock: move a slot's decoded frame into the history */
static void slot_to_hist(Player *p, int i)
{
    yh_push(p, p->slot[i].yuv, p->slot[i].pts);
    p->slot[i].yuv = NULL;
}

/* YUV (any format) -> BGRA with the stream's real matrix / range */
static int convert_bgra(struct SwsContext **ctx, const AVFrame *src, AVFrame *dst)
{
    int w = src->width, h = src->height, cs = SWS_CS_DEFAULT;
    *ctx = sws_getCachedContext(*ctx, w, h, src->format,
                                dst->width, dst->height, AV_PIX_FMT_BGRA,
                                SWS_BILINEAR, NULL, NULL, NULL);
    if (!*ctx)
        return -1;
    if (src->colorspace == AVCOL_SPC_BT709)
        cs = SWS_CS_ITU709;
    else if (src->colorspace == AVCOL_SPC_BT2020_NCL ||
             src->colorspace == AVCOL_SPC_BT2020_CL)
        cs = SWS_CS_BT2020;
    else if (src->colorspace == AVCOL_SPC_SMPTE240M)
        cs = SWS_CS_SMPTE240M;
    else if (src->colorspace == AVCOL_SPC_BT470BG ||
             src->colorspace == AVCOL_SPC_SMPTE170M)
        cs = SWS_CS_ITU601;
    else if (h >= 720)
        cs = SWS_CS_ITU709;
    {
        const int *tab = sws_getCoefficients(cs);
        sws_setColorspaceDetails(*ctx, tab, src->color_range == AVCOL_RANGE_JPEG,
                                 sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                 0, 1 << 16, 1 << 16);
    }
    sws_scale(*ctx, (const uint8_t *const *)src->data, src->linesize, 0, h,
              dst->data, dst->linesize);
    return 0;
}

/* caller holds p->lock: put a decoded frame on screen and into the history */
static void display_slot(Player *p, int idx)
{
    int old = p->shown_slot;
    slot_to_hist(p, idx);
    p->shown_slot = idx;
    p->yh_pos = 0;
    if (old >= 0 && old != idx && old != p->view_slot)
        slot_free(p, old);              /* its YUV frame is already in the history */
}

/* caller holds p->lock: show history entry `pos` (0 = newest) */
static int show_hist(Player *p, int pos)
{
    int old;
    if (pos < 0 || pos >= p->yh_n || p->view_slot < 0)
        return 0;
    YEnt *e = yh_at(p, pos);
    Slot *v = &p->slot[p->view_slot];
    if (!v->bgra || convert_bgra(&p->sws_view, e->f, v->bgra) < 0)
        return 0;
    v->pts = e->pts;
    v->serial = p->cur_serial;
    old = p->shown_slot;
    p->shown_slot = p->view_slot;
    p->yh_pos = pos;
    if (old >= 0 && old != p->view_slot)
        slot_free(p, old);
    return 1;
}

/* caller holds p->lock */
static void fifo_push(Player *p, int idx)
{
    p->fifo[(p->fifo_r + p->fifo_n) % SLOT_MAX] = idx;
    p->fifo_n++;
}

static int fifo_pop(Player *p)
{
    int s = p->fifo[p->fifo_r];
    p->fifo_r = (p->fifo_r + 1) % SLOT_MAX;
    p->fifo_n--;
    return s;
}

/* caller holds p->lock: drop undisplayed frames of an older serial */
static void fifo_purge_stale(Player *p)
{
    while (p->fifo_n > 0) {
        int s = p->fifo[p->fifo_r];
        if (p->slot[s].serial == p->cur_serial)
            break;
        fifo_pop(p);
        slot_free(p, s);
    }
}

/* caller holds p->lock; (re)allocate BGRA slots + history for a frame size */
static int ensure_slots(Player *p, int w, int h, int fmt)
{
    int i, yb, cap;
    if (p->slot_n && p->vw == w && p->vh == h)
        return 1;
    for (i = 0; i < SLOT_MAX; i++) {
        av_frame_free(&p->slot[i].bgra);
        av_frame_free(&p->slot[i].yuv);
        p->slot[i].state = 0;
    }
    yh_clear(p);
    p->fifo_n = p->fifo_r = 0;
    p->shown_slot = -1;
    p->pend_slot = -1;
    yb = av_image_get_buffer_size((enum AVPixelFormat)fmt, w, h, 1);
    if (yb <= 0)
        yb = w * h * 2;
    cap = YH_BUDGET / yb;
    p->yh_cap = (cap < 4) ? 4 : (cap > YH_MAX) ? YH_MAX : cap;
    p->slot_n = ((int64_t)w * h > 1920 * 1080) ? SLOT_N_BIG : SLOT_N;
    for (i = 0; i < p->slot_n; i++) {
        Slot *s = &p->slot[i];
        s->state = 0;
        s->serial = 0;
        s->bgra = av_frame_alloc();
        if (!s->bgra)
            break;
        s->bgra->format = AV_PIX_FMT_BGRA;
        s->bgra->width = w;
        s->bgra->height = h;
        if (av_frame_get_buffer(s->bgra, 64) < 0) {
            av_frame_free(&s->bgra);
            break;
        }
    }
    p->slot_n = i;
    p->view_slot = -1;
    if (p->slot_n >= 3) {               /* last slot is reserved for history */
        p->view_slot = p->slot_n - 1;
        p->slot[p->view_slot].state = 3;
    }
    p->vw = w;
    p->vh = h;
    return p->slot_n >= 3;
}

/* ============================ audio helpers ============================ */

static int free_wh(Player *p)
{
    int i;
    for (i = 0; i < WH_COUNT; i++)
        if (p->wh[i].dwBufferLength == 0 || (p->wh[i].dwFlags & WHDR_DONE))
            return i;
    return -1;
}

/* decoder hit EOF and everything queued has been played */
static int audio_drained(const Player *p)
{
    int i;
    if (!p->audio_done || p->aubuf_len > 0)
        return 0;
    for (i = 0; i < WH_COUNT; i++)
        if (p->wh[i].dwBufferLength && !(p->wh[i].dwFlags & WHDR_DONE))
            return 0;
    return 1;
}

/* audio thread only: block until a waveOut buffer is free.
 * -1 = playback closed or a seek made this data stale. */
static int audio_wait_free(Player *p, int serial)
{
    for (;;) {
        int i = free_wh(p);
        if (i >= 0)
            return i;
        if (p->quit || serial != p->cur_serial)
            return -1;
        Sleep(4);
    }
}

/* audio thread only: send all complete chunks from aubuf to waveOut.
 * Waits for free buffers instead of dropping audio. 0 = aborted. */
static int audio_submit(Player *p, int serial)
{
    while (p->aubuf_len >= p->wh_bytes) {
        int i = audio_wait_free(p, serial);
        if (i < 0)
            return 0;
        memcpy(p->whdata[i], p->aubuf, p->wh_bytes);
        if (p->aubuf_len > p->wh_bytes)
            memmove(p->aubuf, p->aubuf + p->wh_bytes, p->aubuf_len - p->wh_bytes);
        p->aubuf_len -= p->wh_bytes;
        p->wh[i].dwBufferLength = (DWORD)p->wh_bytes;
        p->wh[i].dwFlags &= ~WHDR_DONE;
        if (waveOutWrite(p->wo, &p->wh[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
            p->samples_written += (unsigned long long)(p->wh_bytes / 4);
        else
            p->wh[i].dwBufferLength = 0;
    }
    return 1;
}

/* audio thread only */
static void audio_reset(Player *p)
{
    int i;
    waveOutReset(p->wo);
    if (p->paused)
        waveOutPause(p->wo);
    for (i = 0; i < WH_COUNT; i++)
        p->wh[i].dwFlags |= WHDR_DONE;
    p->samples_written = 0;
    p->audio_base = p->audio_target;
    p->audio_started = 0;       /* first frame at/after the target sets the base */
    p->aubuf_len = 0;
}

/* audio thread only: decode one frame -> resample -> queue to waveOut */
static void audio_out(Player *p, AVFrame *fr, int serial)
{
    if (!p->swr || serial != p->cur_serial)
        return;
    if (!p->audio_started) {
        double tb = av_q2d(p->ast->time_base);
        double pts = (fr->pts != AV_NOPTS_VALUE) ? fr->pts * tb : p->audio_target;
        double fd = (fr->sample_rate > 0) ?
                    (double)fr->nb_samples / (double)fr->sample_rate : 0.0;
        /* after a seek the demuxer restarts at the previous keyframe:
         * skip audio that lies before the requested position */
        if (p->audio_target > 0.0 && pts + fd <= p->audio_target)
            return;
        p->audio_base = pts;
        p->audio_started = 1;
    }
    int max_out = swr_get_out_samples(p->swr, fr->nb_samples);
    if (max_out < 256)
        max_out = 256;
    if ((size_t)p->aubuf_len + (size_t)max_out * 4 > (size_t)p->aubuf_cap) {
        if (!audio_submit(p, serial))
            return;
        if ((size_t)p->aubuf_len + (size_t)max_out * 4 > (size_t)p->aubuf_cap)
            return;
    }
    uint8_t *out = p->aubuf + p->aubuf_len;
    int got = swr_convert(p->swr, &out, max_out,
                          (const uint8_t **)fr->data, fr->nb_samples);
    if (got > 0)
        p->aubuf_len += got * 4;    /* stereo s16 = 4 bytes/sample */
    audio_submit(p, serial);
}

/* audio thread only: write final partial buffer at EOF */
static void audio_flush_tail(Player *p, int serial)
{
    if (p->aubuf_len <= 0)
        return;
    int i = audio_wait_free(p, serial);
    if (i < 0)
        return;
    int b = p->aubuf_len;
    if (b > p->wh_bytes)
        b = p->wh_bytes;
    memcpy(p->whdata[i], p->aubuf, b);
    p->aubuf_len = 0;
    p->wh[i].dwBufferLength = (DWORD)b;
    p->wh[i].dwFlags &= ~WHDR_DONE;
    if (waveOutWrite(p->wo, &p->wh[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
        p->samples_written += (unsigned long long)(b / 4);
    else
        p->wh[i].dwBufferLength = 0;
}

/* ============================ video helpers ============================ */

/* video thread only: convert frame to BGRA and append to fifo.
 * Blocks while all slots are busy (never drops decoded frames). */
static void push_display_frame(Player *p, AVFrame *fr, int serial)
{
    double tb = av_q2d(p->vst->time_base);
    int64_t ts = (fr->best_effort_timestamp != AV_NOPTS_VALUE) ?
                 fr->best_effort_timestamp : fr->pts;
    double pts = (ts != AV_NOPTS_VALUE) ? ts * tb : NAN;
    if (pts != pts)   /* isnan */
        pts = p->last_vpts + (fr->duration > 0 ? fr->duration * tb : 0.04);
    if (pts < 0)
        pts = 0;
    p->last_vpts = pts;
    int w = fr->width, h = fr->height, fmt = fr->format;
    int idx = -1, i;

    AcquireSRWLockExclusive(&p->lock);
    if (p->quit || serial != p->cur_serial || !ensure_slots(p, w, h, fmt)) {
        ReleaseSRWLockExclusive(&p->lock);
        return;
    }
    if (p->seek_pending) {
        /* Catch-up frames before the seek target are never shown. Skip the
         * (expensive) BGRA conversion and keep them only as YUV references in
         * the history, so stepping back right after a seek is instant. */
        int early = (p->pend_mode == 0) ? (pts + 0.04 < p->pend_target) :
                    (p->pend_mode == 1) ? (pts <= p->pend_target + 0.002) : 0;
        if (early) {
            yh_push(p, av_frame_clone(fr), pts);
            ReleaseSRWLockExclusive(&p->lock);
            return;
        }
    }
    ReleaseSRWLockExclusive(&p->lock);

    for (;;) {
        AcquireSRWLockExclusive(&p->lock);
        if (p->quit || serial != p->cur_serial || !ensure_slots(p, w, h, fmt)) {
            ReleaseSRWLockExclusive(&p->lock);
            return;
        }
        for (i = 0; i < p->slot_n; i++)
            if (p->slot[i].state == 0) {
                idx = i;
                p->slot[i].state = 2;       /* being filled */
                break;
            }
        ReleaseSRWLockExclusive(&p->lock);
        if (idx >= 0)
            break;
        Sleep(2);       /* ui hasn't consumed anything yet: wait */
    }

    Slot *s = &p->slot[idx];
    if (convert_bgra(&p->sws, fr, s->bgra) < 0) {
        AcquireSRWLockExclusive(&p->lock);
        s->state = 0;
        ReleaseSRWLockExclusive(&p->lock);
        return;
    }
    s->yuv = (p->yh_cap > 0) ? av_frame_clone(fr) : NULL;

    AcquireSRWLockExclusive(&p->lock);
    if (!p->quit && serial == p->cur_serial) {
        s->pts = pts;
        s->serial = serial;
        s->state = 1;
        fifo_push(p, idx);
    } else {
        slot_free(p, idx);      /* a seek happened while converting */
    }
    ReleaseSRWLockExclusive(&p->lock);
}

/* ============================ threads ============================ */

static void do_seek(Player *p)
{
    double t = p->seek_target;
    int st = (p->vs >= 0) ? p->vs : p->as;
    if (st < 0) {
        p->seek_req = 0;
        return;
    }
    AVStream *s = p->fmt->streams[st];
    int64_t us = (int64_t)(t * 1000000.0 + 0.5);
    int64_t ts = av_rescale_q(us, (AVRational){ 1, 1000000 }, s->time_base);
    int ok = (av_seek_frame(p->fmt, st, ts, AVSEEK_FLAG_BACKWARD) == 0);
    p->seek_req = 0;
    if (!ok)
        return;

    int ns = ++p->serial_gen, i;
    AcquireSRWLockExclusive(&p->lock);
    pktq_flush(&p->pqv, ns);
    pktq_flush(&p->pqa, ns);
    p->cur_serial = ns;
    p->fifo_n = p->fifo_r = 0;
    yh_clear(p);
    /* free every slot except the one on screen (freed on next display), the
     * view slot, and the one the video thread is filling right now */
    for (i = 0; i < p->slot_n; i++)
        if (p->slot[i].state == 1 && i != p->shown_slot)
            slot_free(p, i);
    p->eof = 0;
    p->video_done = 0;
    p->audio_done = 0;
    p->clock_sys = 0;
    p->pend_target = t;
    p->pend_mode = p->seek_mode;
    p->pend_slot = -1;
    p->seek_pending = (p->vs >= 0) ? 1 : 0;
    ReleaseSRWLockExclusive(&p->lock);

    p->sys_base = t;
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    p->sys_t0_ticks = li.QuadPart;
    if (p->has_audio) {
        p->audio_target = t;
        p->audio_reset_pending = 1;
    }
}

static unsigned __stdcall demux_thread(void *arg)
{
    Player *p = (Player *)arg;
    AVPacket *pkt = av_packet_alloc();
    while (!p->quit) {
        if (p->seek_req) {
            do_seek(p);
            continue;
        }
        int r = av_read_frame(p->fmt, pkt);
        if (r < 0) {
            p->eof = 1;
            Sleep(12);
            continue;
        }
        int si = pkt->stream_index;
        PktQ *q = (si == p->vs) ? &p->pqv :
                  (si == p->as && p->has_audio) ? &p->pqa : NULL;
        if (!q) {
            av_packet_unref(pkt);
            continue;
        }
        size_t cap = (q == &p->pqv) ? PKTQ_MAX_V : PKTQ_MAX_A;
        for (;;) {
            int pushed;
            AcquireSRWLockExclusive(&p->lock);
            pushed = (pktq_push(q, pkt, cap) == 0);
            ReleaseSRWLockExclusive(&p->lock);
            if (pushed)
                break;
            if (p->quit || p->seek_req)
                break;    /* drop; seek will flush anyway */
            Sleep(8);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return 0;
}

static unsigned __stdcall video_thread(void *arg)
{
    Player *p = (Player *)arg;
    AVFrame *fr = av_frame_alloc();
    int last_serial = -1;
    while (!p->quit) {
        AcquireSRWLockExclusive(&p->lock);
        PktNode *n = pktq_pop(&p->pqv);
        ReleaseSRWLockExclusive(&p->lock);

        if (!n) {
            if (p->eof && !p->video_done) {
                avcodec_send_packet(p->vctx, NULL);
                for (;;) {
                    int r = avcodec_receive_frame(p->vctx, fr);
                    if (r == 0) {
                        push_display_frame(p, fr, last_serial);
                        av_frame_unref(fr);
                    } else {
                        if (r == AVERROR_EOF && last_serial == p->cur_serial)
                            p->video_done = 1;
                        break;
                    }
                }
            } else {
                Sleep(8);
            }
            continue;
        }
        int serial = n->serial;
        if (serial != last_serial) {
            avcodec_flush_buffers(p->vctx);
            last_serial = serial;
        }
        avcodec_send_packet(p->vctx, &n->pkt);
        av_packet_unref(&n->pkt);
        av_free(n);
        for (;;) {
            int r = avcodec_receive_frame(p->vctx, fr);
            if (r == 0) {
                push_display_frame(p, fr, serial);
                av_frame_unref(fr);
            } else {
                break;
            }
        }
    }
    av_frame_free(&fr);
    return 0;
}

static unsigned __stdcall audio_thread(void *arg)
{
    Player *p = (Player *)arg;
    AVFrame *fr = av_frame_alloc();
    int last_serial = -1;
    while (!p->quit) {
        AcquireSRWLockExclusive(&p->lock);
        PktNode *n = pktq_pop(&p->pqa);
        ReleaseSRWLockExclusive(&p->lock);

        if (!n) {
            if (p->audio_reset_pending && p->eof) {
                avcodec_flush_buffers(p->actx);
                audio_reset(p);
                p->audio_reset_pending = 0;
                last_serial = p->cur_serial;
            }
            if (p->eof && !p->audio_done) {
                avcodec_send_packet(p->actx, NULL);
                for (;;) {
                    int r = avcodec_receive_frame(p->actx, fr);
                    if (r == 0) {
                        audio_out(p, fr, last_serial);
                        av_frame_unref(fr);
                    } else {
                        if (r == AVERROR_EOF) {
                            audio_flush_tail(p, last_serial);
                            if (last_serial == p->cur_serial)
                                p->audio_done = 1;
                        }
                        break;
                    }
                }
            } else {
                Sleep(8);
            }
            continue;
        }
        int serial = n->serial;
        if (serial != last_serial) {
            avcodec_flush_buffers(p->actx);
            if (p->audio_reset_pending)
                audio_reset(p);
            p->audio_reset_pending = 0;
            last_serial = serial;
        }
        avcodec_send_packet(p->actx, &n->pkt);
        av_packet_unref(&n->pkt);
        av_free(n);
        for (;;) {
            int r = avcodec_receive_frame(p->actx, fr);
            if (r == 0) {
                audio_out(p, fr, serial);
                av_frame_unref(fr);
            } else {
                break;
            }
        }
    }
    av_frame_free(&fr);
    return 0;
}

/* ============================ public API ============================ */

double ff_clock(Player *p)
{
    if (!p->has_audio || !p->wo || p->clock_sys) {
        if (p->paused)
            return p->sys_base;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - p->sys_t0_ticks) /
                    (double)p->qpc_freq.QuadPart;
        return p->sys_base + dt;
    }
    MMTIME mm;
    mm.wType = TIME_SAMPLES;
    unsigned long long pos;
    if (waveOutGetPosition(p->wo, &mm, sizeof(mm)) == MMSYSERR_NOERROR) {
        if (mm.wType == TIME_SAMPLES)
            pos = mm.u.sample;
        else if (mm.wType == TIME_BYTES)
            pos = mm.u.cb / 4;
        else
            pos = p->samples_written;
    } else {
        pos = p->samples_written;
    }
    if (pos > p->samples_written)     /* clamp driver quirks */
        pos = p->samples_written;
    double clk = p->audio_base + (double)pos / (double)p->sample_rate;
    if (p->dur > 0 && clk > p->dur)
        clk = p->dur;
    return clk;
}

double ff_pos(Player *p)
{
    if (!p->playing)
        return 0;
    double c;
    if (p->seek_req)
        c = p->seek_target;                 /* show where we are going, not */
    else if (p->seek_pending || (p->audio_reset_pending && !p->eof))
        c = p->pend_target;                 /* where the old clock still is */
    else if (p->paused && p->stepped && p->shown_slot >= 0)
        c = p->slot[p->shown_slot].pts;
    else
        c = ff_clock(p);
    if (c < 0)
        c = 0;
    if (p->dur > 0 && c > p->dur)
        c = p->dur;
    return c;
}

static int seek_common(Player *p, double sec, int mode)
{
    if (!p->playing || !p->fmt)
        return -1;
    if (sec < 0)
        sec = 0;
    if (p->dur > 0 && sec > p->dur - 0.05)
        sec = (p->dur > 0.1) ? p->dur - 0.05 : 0;
    p->seek_target = sec;
    p->seek_mode = mode;
    p->seek_req = 1;
    return 0;
}

int ff_seek(Player *p, double sec)
{
    p->stepped = 0;
    return seek_common(p, sec, 0);
}

/* latest keyframe at or before t, from the container index; -1 = unknown */
static double keyframe_before(const Player *p, double t)
{
    int lo = 0, hi = p->kf_n - 1, r = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (p->kf[mid] <= t + 1e-4) { r = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return (r >= 0) ? p->kf[r] : -1.0;
}

/* Seekbar drag: show the exact frame at t as fast as possible.
 *  1. a frame in the history covers t        -> show it, no decoding at all
 *  2. the decoder is already inside t's GOP  -> keep decoding forward, no restart
 *  3. otherwise                              -> real seek to the previous keyframe
 * Returns 1 when the picture changed right away. */
int ff_scrub(Player *p, double t)
{
    double fd = (p->vfd > 0) ? p->vfd : 0.04;
    int shown = 0, fwd = 0;
    if (!p->playing || !p->fmt)
        return 0;
    if (p->vs < 0) {                    /* audio only: plain seek */
        seek_common(p, t, 0);
        return 0;
    }
    if (p->seek_pending || p->seek_req)
        return 0;
    if (t < 0)
        t = 0;
    if (p->dur > 0 && t > p->dur - 0.05)
        t = (p->dur > 0.1) ? p->dur - 0.05 : 0;
    p->stepped = 0;

    AcquireSRWLockExclusive(&p->lock);
    if (p->yh_n > 0) {
        double newest = yh_at(p, 0)->pts, oldest = yh_at(p, p->yh_n - 1)->pts;
        if (t >= oldest - fd * 0.5 && t <= newest + fd * 0.5) {
            int k;
            for (k = 0; k < p->yh_n; k++)
                if (yh_at(p, k)->pts <= t + fd * 0.5)
                    break;
            if (k >= p->yh_n)
                k = p->yh_n - 1;
            if (p->shown_slot == p->view_slot && p->yh_pos == k)
                shown = 0;                      /* already on screen */
            else
                shown = show_hist(p, k);
            ReleaseSRWLockExclusive(&p->lock);
            return shown;
        }
    }
    {
        double newest = (p->yh_n > 0) ? yh_at(p, 0)->pts : -1.0;
        double kfp = keyframe_before(p, t);
        double pos = p->last_vpts;              /* newest frame the decoder produced */
        if (t > newest && !p->video_done) {
            if (kfp >= 0)
                fwd = (pos >= kfp - 1e-4);      /* already past t's keyframe */
            else
                fwd = (t - pos < 3.0);          /* no index: only short hops */
        }
        if (fwd) {
            p->pend_target = t;
            p->pend_mode = 0;
            p->seek_pending = 1;
        }
    }
    ReleaseSRWLockExclusive(&p->lock);
    if (!fwd)
        seek_common(p, t, 0);
    return 0;
}

void ff_set_paused(Player *p, int pause)
{
    if (!p->playing || pause == p->paused)
        return;
    if (pause) {
        if (p->wo)
            waveOutPause(p->wo);
        p->sys_base = ff_clock(p);
        p->paused = 1;
    } else {
        p->paused = 0;
        if (p->stepped && p->shown_slot >= 0) {
            double pts = p->slot[p->shown_slot].pts;   /* ui-owned slot */
            ff_seek(p, pts);                           /* clears stepped */
        }
        p->stepped = 0;
        if (p->wo)
            waveOutRestart(p->wo);
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        p->sys_t0_ticks = li.QuadPart;
    }
}

void ff_set_volume(Player *p)
{
    if (!p->wo)
        return;
    WORD v = p->muted ? 0 : (WORD)(p->volume * 65535.0 + 0.5);
    waveOutSetVolume(p->wo, (DWORD)v | ((DWORD)v << 16));
}

int ff_step_fwd(Player *p)
{
    int ok = 0;
    if (p->seek_pending || p->seek_req)
        return 0;
    AcquireSRWLockExclusive(&p->lock);
    fifo_purge_stale(p);
    if (p->yh_pos > 0) {
        ok = show_hist(p, p->yh_pos - 1);
    } else if (p->fifo_n > 0) {
        int s = p->fifo[p->fifo_r];
        if (p->slot[s].serial == p->cur_serial) {
            fifo_pop(p);
            display_slot(p, s);
            ok = 1;
        }
    }
    ReleaseSRWLockExclusive(&p->lock);
    if (ok)
        p->stepped = 1;
    return ok;
}

int ff_step_back(Player *p)
{
    int ok = 0;
    double target = -1.0;
    if (p->seek_pending || p->seek_req)
        return 0;
    AcquireSRWLockExclusive(&p->lock);
    if (p->shown_slot >= 0) {
        if (p->yh_pos + 1 < p->yh_n) {
            ok = show_hist(p, p->yh_pos + 1);
        } else {
            /* history used up: jump back to just before the shown frame; the
             * catch-up decode refills the history with the frames before it */
            double pts = p->slot[p->shown_slot].pts;
            double fd = (p->vfd > 0) ? p->vfd : 0.04;
            if (pts > fd * 0.5)
                target = pts - fd * 0.5;
        }
    }
    ReleaseSRWLockExclusive(&p->lock);
    if (target >= 0.0) {
        seek_common(p, target, 1);
        ok = 1;
    }
    if (ok)
        p->stepped = 1;
    return ok;
}

int ff_tick(Player *p)
{
    if (!p->playing)
        return 0;
    int shown_new = 0, cand = -1;
    AcquireSRWLockExclusive(&p->lock);
    fifo_purge_stale(p);
    if (p->seek_pending) {
        /* After a seek the decoder starts at the previous keyframe. Catch-up
         * frames are never shown (that looked like a fast-forward): the video
         * thread filed them in the history, the screen keeps the old picture
         * until the frame we are after is ready. */
        if (p->pend_mode == 1) {
            /* step back: everything before the target is in the history, the
             * first frame after it has arrived (or the stream ended) */
            if (p->fifo_n > 0 || p->video_done) {
                if (p->yh_n > 0 && show_hist(p, 0))
                    shown_new = 1;
                p->seek_pending = 0;
            }
        } else {
            /* exact: first frame at/after the target. Frames queued before it
             * (decoder was already running when the target moved forward) go
             * to the history. */
            while (p->fifo_n > 0) {
                int s = p->fifo[p->fifo_r];
                fifo_pop(p);
                if (p->slot[s].pts + 0.04 < p->pend_target) {
                    slot_to_hist(p, s);
                    slot_free(p, s);
                    continue;
                }
                cand = s;
                p->seek_pending = 0;
                break;
            }
            if (p->seek_pending && p->fifo_n == 0 && p->video_done) {
                /* target is past the last frame: show the last one we have */
                if (p->yh_n > 0 && show_hist(p, 0))
                    shown_new = 1;
                p->seek_pending = 0;
            }
        }
    } else if (!p->paused && !(p->audio_reset_pending && !p->eof)) {
        if (p->has_audio && !p->clock_sys && p->vs >= 0 && audio_drained(p)) {
            /* audio ended before the video: keep the clock running */
            LARGE_INTEGER li;
            p->sys_base = ff_clock(p);
            QueryPerformanceCounter(&li);
            p->sys_t0_ticks = li.QuadPart;
            p->clock_sys = 1;
        }
        double clk = ff_clock(p);
        while (p->fifo_n > 0) {
            int s = p->fifo[p->fifo_r];
            if (p->slot[s].pts > clk + 0.012)
                break;
            fifo_pop(p);
            if (cand >= 0) {            /* a newer frame is due too: the     */
                slot_to_hist(p, cand);  /* skipped one still goes to history */
                slot_free(p, cand);
            }
            cand = s;
        }
    }
    if (cand >= 0) {
        display_slot(p, cand);
        shown_new = 1;
    }
    ReleaseSRWLockExclusive(&p->lock);
    return shown_new;
}

int ff_at_end(Player *p)
{
    if (!p->playing)
        return 0;
    if (!p->eof || p->seek_req || p->seek_pending)
        return 0;
    if (!p->video_done || p->fifo_n > 0)
        return 0;
    if (p->has_audio && !audio_drained(p))
        return 0;
    return 1;
}

int ff_eop(Player *p)
{
    if (!p->playing || p->paused)
        return 0;
    return ff_at_end(p);
}

/* ---------------- frame export ---------------- */

int ff_save_frame_png(Player *p, wchar_t *out_name, int out_cap)
{
    int ret = -1;
    if (p->shown_slot < 0 || !p->slot[p->shown_slot].bgra)
        return -1;
    AVFrame *src = p->slot[p->shown_slot].bgra;
    int w = src->width, h = src->height;

    const AVCodec *c = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!c)
        return -2;
    AVCodecContext *ec = avcodec_alloc_context3(c);
    if (!ec)
        return -2;
    ec->width = w;
    ec->height = h;
    ec->pix_fmt = AV_PIX_FMT_RGB24;
    ec->time_base = (AVRational){ 1, 25 };
    if (avcodec_open2(ec, c, NULL) < 0) {
        avcodec_free_context(&ec);
        return -3;
    }
    AVFrame *fr = av_frame_alloc();
    if (!fr) {
        avcodec_free_context(&ec);
        return -3;
    }
    fr->format = AV_PIX_FMT_RGB24;
    fr->width = w;
    fr->height = h;
    if (av_frame_get_buffer(fr, 32) < 0) {
        av_frame_free(&fr);
        avcodec_free_context(&ec);
        return -3;
    }
    int y, x;
    for (y = 0; y < h; y++) {
        uint8_t *d = fr->data[0] + (size_t)y * fr->linesize[0];
        uint8_t *s = src->data[0] + (size_t)y * src->linesize[0];
        for (x = 0; x < w; x++) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d += 3;
            s += 4;
        }
    }
    fr->pts = 0;

    /* path: Pictures\celluloid_YYYYMMDD_HHMMSS_mmm.png */
    wchar_t dir[MAX_PATH];
    wchar_t name[96], full[MAX_PATH + 96];
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, 0, dir)))
        lstrcpynW(dir, L".", MAX_PATH);
    _snwprintf(name, 95, L"celluloid_%04u%02u%02u_%02u%02u%02u_%03u.png",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    name[95] = 0;
    _snwprintf(full, MAX_PATH + 95, L"%s\\%s", dir, name);
    full[MAX_PATH + 95] = 0;

    HANDLE hf = CreateFileW(full, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
        goto fail;

    avcodec_send_frame(ec, fr);
    avcodec_send_frame(ec, NULL);
    AVPacket *pkt = av_packet_alloc();
    for (;;) {
        int r = avcodec_receive_packet(ec, pkt);
        if (r != 0)
            break;
        DWORD wrote = 0;
        WriteFile(hf, pkt->data, (DWORD)pkt->size, &wrote, NULL);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    CloseHandle(hf);
    if (out_name && out_cap > 0)
        lstrcpynW(out_name, name, out_cap);
    ret = 0;
fail:
    av_frame_free(&fr);
    avcodec_free_context(&ec);
    return ret;
}

int ff_copy_frame(Player *p)
{
    if (p->shown_slot < 0 || !p->slot[p->shown_slot].bgra)
        return -1;
    AVFrame *src = p->slot[p->shown_slot].bgra;
    int w = src->width, h = src->height;

    size_t sz = 40 + (size_t)w * h * 4;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sz);
    if (!hg)
        return -2;
    uint8_t *base = (uint8_t *)GlobalLock(hg);
    if (!base) {
        GlobalFree(hg);
        return -2;
    }
    BITMAPINFOHEADER *bh = (BITMAPINFOHEADER *)base;
    bh->biSize = 40;
    bh->biWidth = w;
    bh->biHeight = h;            /* positive = bottom-up */
    bh->biPlanes = 1;
    bh->biBitCount = 32;
    bh->biCompression = BI_RGB;
    bh->biSizeImage = (DWORD)((size_t)w * h * 4);
    uint8_t *dst = base + 40;
    int y, x;
    for (y = 0; y < h; y++) {
        uint8_t *s = src->data[0] + (size_t)(h - 1 - y) * src->linesize[0];
        uint8_t *d = dst + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 0xFF;         /* opaque */
            d += 4;
            s += 4;
        }
    }
    GlobalUnlock(hg);
    if (!OpenClipboard(p->hwnd)) {
        GlobalFree(hg);
        return -3;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, hg)) {
        CloseClipboard();
        GlobalFree(hg);
        return -4;
    }
    CloseClipboard();
    return 0;
}

/* ---------------- open / close ---------------- */

/* decode the attached picture of `st` into p->cover (BGRA, max 2048 px) */
static void load_cover(Player *p, AVStream *st)
{
    const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *cc = NULL;
    AVFrame *fr = NULL, *out = NULL;
    struct SwsContext *sws = NULL;

    av_frame_free(&p->cover);
    if (!c || st->attached_pic.size <= 0)
        return;
    cc = avcodec_alloc_context3(c);
    fr = av_frame_alloc();
    if (!cc || !fr)
        goto done;
    if (avcodec_parameters_to_context(cc, st->codecpar) < 0 ||
        avcodec_open2(cc, c, NULL) < 0)
        goto done;
    if (avcodec_send_packet(cc, &st->attached_pic) < 0)
        goto done;
    if (avcodec_receive_frame(cc, fr) < 0) {
        avcodec_send_packet(cc, NULL);          /* flush: some decoders hold it */
        if (avcodec_receive_frame(cc, fr) < 0)
            goto done;
    }
    if (fr->width <= 0 || fr->height <= 0)
        goto done;
    {
        int w = fr->width, h = fr->height;
        if (w > 2048 || h > 2048) {
            double sc = 2048.0 / (w > h ? w : h);
            w = (int)(w * sc + 0.5);
            h = (int)(h * sc + 0.5);
            if (w < 1) w = 1;
            if (h < 1) h = 1;
        }
        out = av_frame_alloc();
        if (!out)
            goto done;
        out->format = AV_PIX_FMT_BGRA;
        out->width = w;
        out->height = h;
        if (av_frame_get_buffer(out, 32) < 0)
            goto done;
        if (convert_bgra(&sws, fr, out) < 0)
            goto done;
        p->cover = out;
        out = NULL;
    }
done:
    sws_freeContext(sws);
    av_frame_free(&out);
    av_frame_free(&fr);
    avcodec_free_context(&cc);
}

int ff_open(Player *p, const wchar_t *wpath)
{
    ff_close(p);

    char path8[4096];
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path8, sizeof(path8), NULL, NULL);

    AVFormatContext *f = avformat_alloc_context();
    if (!f)
        return -1;
    f->flags |= AVFMT_FLAG_GENPTS;
    if (avformat_open_input(&f, path8, NULL, NULL) < 0)
        return -1;
    if (avformat_find_stream_info(f, NULL) < 0) {
        avformat_close_input(&f);
        return -2;
    }
    p->fmt = f;

    /* pick streams: first video (skip attached art), first audio */
    unsigned i;
    int cs = -1;                        /* attached picture (cover art) stream */
    p->vs = p->as = -1;
    for (i = 0; i < f->nb_streams; i++) {
        AVStream *st = f->streams[i];
        AVCodecParameters *par = st->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
            !(st->disposition & AV_DISPOSITION_ATTACHED_PIC) && p->vs < 0)
            p->vs = (int)i;
        else if (par->codec_type == AVMEDIA_TYPE_AUDIO && p->as < 0)
            p->as = (int)i;
        else if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
                 (st->disposition & AV_DISPOSITION_ATTACHED_PIC) && cs < 0)
            cs = (int)i;
    }

    /* open decoders */
    if (p->vs >= 0) {
        AVStream *st = f->streams[p->vs];
        const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
        if (c) {
            p->vctx = avcodec_alloc_context3(c);
            if (p->vctx) {
                avcodec_parameters_to_context(p->vctx, st->codecpar);
                p->vctx->thread_count = 0;
                if (avcodec_open2(p->vctx, c, NULL) < 0) {
                    avcodec_free_context(&p->vctx);
                }
            }
        }
        if (!p->vctx)
            p->vs = -1;
    }
    if (p->as >= 0) {
        AVStream *st = f->streams[p->as];
        const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
        if (c) {
            p->actx = avcodec_alloc_context3(c);
            if (p->actx) {
                avcodec_parameters_to_context(p->actx, st->codecpar);
                p->actx->thread_count = 0;
                if (avcodec_open2(p->actx, c, NULL) < 0) {
                    avcodec_free_context(&p->actx);
                }
            }
        }
        if (!p->actx)
            p->as = -1;
    }
    if (p->vs < 0 && p->as < 0) {
        avformat_close_input(&p->fmt);
        return -3;
    }
    p->vst = (p->vs >= 0) ? f->streams[p->vs] : NULL;
    p->ast = (p->as >= 0) ? f->streams[p->as] : NULL;
    p->vw = p->vst ? p->vst->codecpar->width : 0;
    p->vh = p->vst ? p->vst->codecpar->height : 0;
    p->kf = NULL;
    p->kf_n = 0;
    if (p->vst) {                       /* keyframe times from the container index */
        int n = avformat_index_get_entries_count(p->vst), k = 0, e;
        if (n > 0 && n < (1 << 20)) {
            p->kf = (double *)av_malloc_array((size_t)n, sizeof(double));
            if (p->kf) {
                for (e = 0; e < n; e++) {
                    const AVIndexEntry *ie = avformat_index_get_entry(p->vst, e);
                    if (ie && (ie->flags & AVINDEX_KEYFRAME))
                        p->kf[k++] = (double)ie->timestamp * av_q2d(p->vst->time_base);
                }
                p->kf_n = k;
            }
        }
    }
    p->vdar = (p->vh > 0) ? (double)p->vw / (double)p->vh : 0.0;
    p->vfd = 0.04;
    if (p->vst) {
        AVRational sar = av_guess_sample_aspect_ratio(f, p->vst, NULL);
        AVRational fr = av_guess_frame_rate(f, p->vst, NULL);
        if (sar.num > 0 && sar.den > 0 && p->vh > 0)
            p->vdar = (double)p->vw * sar.num / ((double)p->vh * sar.den);
        if (fr.num > 0 && fr.den > 0)
            p->vfd = (double)fr.den / (double)fr.num;
    }

    /* duration */
    p->dur = (f->duration > 0) ? (double)f->duration / (double)AV_TIME_BASE : 0;
    if (p->dur <= 0 && p->vst && p->vst->duration > 0)
        p->dur = p->vst->duration * av_q2d(p->vst->time_base);

    /* audio: resampler to stereo s16 + waveOut */
    p->has_audio = 0;
    if (p->as >= 0) {
        if (p->actx->sample_rate <= 0)
            p->actx->sample_rate = 44100;
        if (p->actx->ch_layout.nb_channels == 0)
            av_channel_layout_default(&p->actx->ch_layout, 2);
        AVChannelLayout out;
        av_channel_layout_default(&out, 2);
        int rate = p->actx->sample_rate;
        if (rate < 8000 || rate > 384000)
            rate = 44100;
        if (swr_alloc_set_opts2(&p->swr, &out, AV_SAMPLE_FMT_S16, rate,
                                &p->actx->ch_layout, p->actx->sample_fmt,
                                p->actx->sample_rate, 0, NULL) == 0) {
            swr_init(p->swr);
            WAVEFORMATEX wfx;
            ZeroMemory(&wfx, sizeof(wfx));
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.nChannels = 2;
            wfx.nSamplesPerSec = (DWORD)rate;
            wfx.wBitsPerSample = 16;
            wfx.nBlockAlign = 4;
            wfx.nAvgBytesPerSec = (DWORD)rate * 4;
            if (waveOutOpen(&p->wo, WAVE_MAPPER, &wfx, 0, 0,
                            CALLBACK_NULL) == MMSYSERR_NOERROR) {
                p->sample_rate = rate;
                p->wh_bytes = (rate * 4 * WH_MS + 999) / 1000;
                int k;
                for (k = 0; k < WH_COUNT; k++) {
                    p->whdata[k] = av_mallocz((size_t)p->wh_bytes);
                    ZeroMemory(&p->wh[k], sizeof(WAVEHDR));
                    p->wh[k].lpData = (LPSTR)p->whdata[k];
                    p->wh[k].dwBufferLength = 0;
                    waveOutPrepareHeader(p->wo, &p->wh[k], sizeof(WAVEHDR));
                }
                p->aubuf_cap = p->wh_bytes * 4 + 65536;
                p->aubuf = av_mallocz((size_t)p->aubuf_cap);
                p->aubuf_len = 0;
                p->has_audio = 1;
            }
        }
        if (!p->has_audio) {
            swr_free(&p->swr);
            avcodec_free_context(&p->actx);
            p->as = -1;
            p->ast = NULL;
        }
    }
    if (p->vs < 0 && p->as < 0) {
        avformat_close_input(&p->fmt);
        return -3;
    }

    /* audio file with embedded cover art: decode it once for the main screen */
    if (p->vs < 0 && cs >= 0)
        load_cover(p, f->streams[cs]);

    /* state */
    p->serial_gen = 1;
    p->cur_serial = 1;
    pktq_init(&p->pqv);
    pktq_init(&p->pqa);
    p->slot_n = 0;
    p->fifo_n = p->fifo_r = 0;
    p->yh_cap = 0;
    p->yh_n = p->yh_head = p->yh_pos = 0;
    p->view_slot = -1;
    p->shown_slot = -1;
    p->eof = 0;
    p->video_done = (p->vs < 0) ? 1 : 0;
    p->audio_done = 0;
    p->seek_req = 0;
    p->seek_pending = 0;
    p->audio_base = 0;
    p->audio_started = 0;
    p->audio_reset_pending = 0;
    p->audio_target = 0;
    p->pend_slot = -1;
    p->stepped = 0;
    p->seek_mode = p->pend_mode = 0;
    p->clock_sys = 0;
    p->pend_target = 0;
    p->samples_written = 0;
    p->last_vpts = 0;
    p->sys_base = 0;
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    p->sys_t0_ticks = li.QuadPart;
    p->paused = 0;
    p->quit = 0;

    /* threads: demux, video, audio */
    p->th[0] = (HANDLE)_beginthreadex(NULL, 0, demux_thread, p, 0, NULL);
    if (p->vs >= 0)
        p->th[1] = (HANDLE)_beginthreadex(NULL, 0, video_thread, p, 0, NULL);
    if (p->has_audio)
        p->th[2] = (HANDLE)_beginthreadex(NULL, 0, audio_thread, p, 0, NULL);

    p->playing = 1;
    ff_set_volume(p);
    p->media_gen++;
    return 0;
}

void ff_close(Player *p)
{
    int i;
    if (!p->fmt)
        return;
    p->quit = 1;
    for (i = 0; i < 3; i++) {
        if (p->th[i]) {
            WaitForSingleObject(p->th[i], INFINITE);
            CloseHandle(p->th[i]);
            p->th[i] = NULL;
        }
    }
    if (p->wo) {
        waveOutReset(p->wo);
        for (i = 0; i < WH_COUNT; i++) {
            if (p->whdata[i]) {
                waveOutUnprepareHeader(p->wo, &p->wh[i], sizeof(WAVEHDR));
                av_freep(&p->whdata[i]);
            }
            p->wh[i].dwBufferLength = 0;
        }
        waveOutClose(p->wo);
        p->wo = NULL;
    }
    av_freep(&p->aubuf);
    p->aubuf_len = 0;
    swr_free(&p->swr);
    sws_freeContext(p->sws);
    p->sws = NULL;
    for (i = 0; i < SLOT_MAX; i++) {
        av_frame_free(&p->slot[i].bgra);
        av_frame_free(&p->slot[i].yuv);
        p->slot[i].state = 0;
    }
    av_frame_free(&p->cover);
    yh_clear(p);
    sws_freeContext(p->sws_view);
    p->sws_view = NULL;
    av_freep(&p->kf);
    p->kf_n = 0;
    p->view_slot = -1;
    p->slot_n = 0;
    AcquireSRWLockExclusive(&p->lock);
    pktq_flush(&p->pqv, 1);
    pktq_flush(&p->pqa, 1);
    ReleaseSRWLockExclusive(&p->lock);
    avcodec_free_context(&p->vctx);
    avcodec_free_context(&p->actx);
    avformat_close_input(&p->fmt);
    p->vs = p->as = -1;
    p->vst = p->ast = NULL;
    p->playing = 0;
    p->shown_slot = -1;
    p->pend_slot = -1;
    p->stepped = 0;
    p->vw = p->vh = 0;
    p->dur = 0;
    p->eof = 0;
    p->video_done = p->audio_done = 0;
    p->seek_req = 0;
    p->seek_pending = 0;
    p->paused = 0;
}

void ff_shutdown(Player *p)
{
    ff_close(p);
    if (p->pl) {
        free(p->pl);
        p->pl = NULL;
    }
    p->pl_n = p->pl_cap = 0;
}
