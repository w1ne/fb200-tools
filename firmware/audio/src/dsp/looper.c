/* Looper, the audio side: see looper.h. looper_process and what it calls
 * are on the audio path (ITCM); the control functions are COLD (flash). */
#include <string.h>
#include "cold.h"
#include "looper.h"
#include "loopstore/loopstore.h"

#define HB_N (4 * LOOPER_HB_SIDE - 2)  /* decimator history: taps - 1 (26) */
#define IH_N (2 * LOOPER_HB_SIDE - 1)  /* interpolator history (13) */
#define CHUNK_SAMPLES (LS_FPC * LC_N)  /* 6368: 289 ms */
#define MIN_LEN ((uint32_t)(LOOPER_MIN_MS * LOOPER_FS / 1000.0f))
#define NO_CHUNK 0xFFFFFFFFu

/* Half-band FIR (27 taps, Kaiser beta 6): h[13] = 0.5, h[13 +- (2j+1)] =
 * kHb[j], the even offsets are 0. Sum of kHb = 0.25 (unity at DC). Flat
 * (+-0.03 dB) to 8 kHz, -6 dB at 11.025 kHz, <= -47 dB from 14 kHz and
 * <= -64 dB from 15 kHz. */
static const float kHb[LOOPER_HB_SIDE] = {
    3.132591042e-01f, -9.156855253e-02f, 4.186601721e-02f, -1.937165627e-02f,
    7.920118734e-03f, -2.469291685e-03f, 3.642603099e-04f,
};

static inline float ramp(float g, float t, float st)
{
    float e = t - g;
    return e > st ? g + st : e < -st ? g - st : t;
}

/* The flash side has a slot for one more chunk (after the ones started
 * and not yet given one), res kept back. */
static inline int room(const looper_t *lp, uint32_t extra)
{
    const loopio_t *io = lp->io;
    return lp->started - io->alloc + extra < io->pool;
}

/* a marker waiting for its slot goes first: 1 when none is left */
static inline int flush_mark(looper_t *lp)
{
    if (lp->mark && lio_wr_mark(lp->io, lp->mark)) lp->mark = 0;
    return lp->mark == 0;
}

static inline void end_session(looper_t *lp)
{
    lp->sess = 0;
    if (!flush_mark(lp) || !lio_wr_mark(lp->io, LIO_END)) lp->mark = LIO_END;
}

/* REC closes: the loop is pos samples long. What plays next (the tail)
 * fades out while the start fades in: a new version of the first frames
 * (the next write session). */
__attribute__((noinline)) static void close_rec(looper_t *lp)
{
    lp->len = lp->pos;
    lp->nfr = (lp->len + LC_N - 1u) / LC_N;
    lp->io->nfr = lp->nfr;
    if (lp->sess) end_session(lp);
    lp->state = LOOPER_PLAY;
    lp->pos = 0;
    lp->xfade = LOOPER_XFADE;
    lp->alt = lp->redo = 0;
    lp->passes = 0;
}

/* the flash is full: the dub ends (fades out), the loop plays on */
static void cut_dub(looper_t *lp)
{
    lp->cut++;
    lp->dub_t = 0.0f;
    if (lp->state == LOOPER_DUB) lp->state = LOOPER_PLAY;
}

/* frame f is done (filled samples of it belong to the loop): send it if it
 * is new, else end the write session */
__attribute__((noinline)) static void frame_end(looper_t *lp, uint32_t f, uint32_t filled)
{
    for (uint32_t j = filled; j < LC_N; j++) lp->enc[j] = 0.0f;
    if (!lp->touched) {
        if (lp->sess) end_session(lp);
        return;
    }
    if (!flush_mark(lp)) {                 /* order: the marker first */
        lp->io->wr_over++;
        return;
    }
    if (!lp->sess) {
        lp->sess = 1;
        lp->fresh = lp->state == LOOPER_REC;
        lp->w_lc = NO_CHUNK;
    }
    uint32_t lc = f / LS_FPC;
    if (lc != lp->w_lc) {                  /* a new chunk: the flash side needs a slot */
        if (!room(lp, lp->fresh ? 1u : 0u)) {
            cut_dub(lp);
            end_session(lp);
            return;
        }
        lp->started++;
        lp->w_lc = lc;
    }
    uint8_t *slot = lio_wr_slot(lp->io);
    if (!slot) return;                     /* counted: the flash side fills the gap */
    lc_encode(lp->enc, slot);
    lio_wr_push(lp->io, f | (lp->fresh ? LIO_FRESH : 0u));
}

/* a frame starts at pos: the old frame from the read stream */
__attribute__((noinline)) static void frame_begin(looper_t *lp)
{
    uint32_t f = lp->pos / LC_N;
    lp->touched = 0;
    if (lp->state == LOOPER_REC) {
        /* a new chunk, and none left for it (one is kept for the close) */
        if (f != 0u && f % LS_FPC == 0u && !room(lp, 1u)) {
            lp->cut++;
            close_rec(lp);
            frame_begin(lp);
        }
        return;
    }
    const uint8_t *p = lio_rd_take(lp->io);
    if (p) lc_decode(p, lp->dec);
    else memset(lp->dec, 0, sizeof lp->dec);
    lio_rd_done(lp->io);
    lp->rd_f = lp->rd_f + 1u >= lp->nfr ? 0u : lp->rd_f + 1u;
    /* dubbing near a chunk end, and no slot for the next chunk: fade out
     * now (LOOPER_RAMP_MS, 3.4 frames), inside this chunk */
    uint32_t lc = f / LS_FPC, first = lc * LS_FPC;
    uint32_t in_chunk = lp->nfr - first < LS_FPC ? lp->nfr - first : LS_FPC;
    if (lp->dub_t > 0.0f && f - first + 8u >= in_chunk) {
        uint32_t need = lp->sess && lp->w_lc == lc ? 0u : 1u;
        if (!room(lp, need)) cut_dub(lp);
    }
}

/* k samples at 22.05 kHz: x in, y out (the loop, level and fades in). The
 * fades step linearly across the call (to the value a per-sample ramp
 * reaches). */
static void run(looper_t *lp, const float *x, float *y, unsigned k)
{
    if (lp->state == LOOPER_STOP) {
        for (unsigned i = 0; i < k; i++) y[i] = 0.0f;
        return;
    }
    const float rk = lp->ramp * (float)k, ik = 1.0f / (float)k;
    const float lv = lp->level;
    float dg = lp->dub_g, og = lp->out_g;
    const float dg1 = ramp(dg, lp->dub_t, rk), og1 = ramp(og, lp->out_t, rk);
    const float ddg = (dg1 - dg) * ik, dog = (og1 - og) * ik;
    for (unsigned i = 0; i < k; i++) {
        if (lp->pos % LC_N == 0u) frame_begin(lp);
        uint32_t o = lp->pos % LC_N;
        int rec = lp->state == LOOPER_REC;
        dg += ddg;
        og += dog;
        float old = rec ? 0.0f : lp->dec[o], out = old, gi, go;
        if (rec) {
            gi = 1.0f;
            go = 0.0f;
        } else if (lp->xfade) {                /* the closing crossfade */
            go = (float)(LOOPER_XFADE - lp->xfade) * (1.0f / LOOPER_XFADE);
            gi = 1.0f - go;
            out = old * go;                    /* the tail is heard live on this pass */
            lp->xfade--;
        } else {
            gi = dg;
            go = 1.0f - (1.0f - LOOPER_FB) * gi;
        }
        if (gi > 0.0f) lp->touched = 1;
        lp->enc[o] = old * go + x[i] * gi;
        y[i] = out * og * lv;
        uint32_t end = rec ? lp->rec_end : lp->len;
        lp->pos++;
        int wrap = lp->pos >= end;
        if (lp->pos % LC_N == 0u || wrap)
            frame_end(lp, (lp->pos - 1u) / LC_N, (lp->pos - 1u) % LC_N + 1u);
        if (wrap) {
            if (rec) {
                close_rec(lp);
            } else {
                lp->passes++;
                lp->pos = 0;
            }
        }
    }
    lp->dub_g = dg1;
    lp->out_g = og1;
}

/* sum_j kHb[j] * (c[-(2j+1)] + c[2j+1]): the half-band's side taps around
 * c (the decimator); hbi: the same taps at c[-j], c[j + 1] (the
 * interpolator's odd phase). Two chains: FP latency. */
static inline float hb(const float *c)
{
    float a = kHb[0] * (c[-1] + c[1]) + kHb[2] * (c[-5] + c[5]) + kHb[4] * (c[-9] + c[9]) +
              kHb[6] * (c[-13] + c[13]);
    float b = kHb[1] * (c[-3] + c[3]) + kHb[3] * (c[-7] + c[7]) + kHb[5] * (c[-11] + c[11]);
    return a + b;
}

static inline float hbi(const float *c)
{
    float a = kHb[0] * (c[0] + c[1]) + kHb[2] * (c[-2] + c[3]) + kHb[4] * (c[-4] + c[5]) +
              kHb[6] * (c[-6] + c[7]);
    float b = kHb[1] * (c[-1] + c[2]) + kHb[3] * (c[-3] + c[4]) + kHb[5] * (c[-5] + c[6]);
    return a + b;
}

void looper_process(looper_t *lp, float *l, float *r, unsigned n)
{
    if (lp->state <= LOOPER_EMPTY || n != DSP_BLOCK) return;   /* the engine's whole blocks */
    float *w = lp->dw;                   /* [history HB_N | this block] at 44.1 kHz */
    for (unsigned i = 0; i < DSP_BLOCK; i++) w[HB_N + i] = 0.5f * (l[i] + r[i]);
    float x[DSP_BLOCK / 2];
    for (unsigned m = 0; m < DSP_BLOCK / 2; m++) {           /* 2:1 half-band */
        const float *c = w + 2u * m + 1u + HB_N / 2u;
        x[m] = 0.5f * c[0] + hb(c);
    }
    float *v = lp->iw;                   /* [history IH_N | this block] at 22.05 kHz */
    run(lp, x, v + IH_N, DSP_BLOCK / 2);
    for (unsigned m = 0; m < DSP_BLOCK / 2; m++) {           /* 1:2 half-band */
        const float *c = v + m + LOOPER_HB_SIDE - 1u;
        float a = c[0], b = 2.0f * hbi(c);
        l[2u * m] += a;
        r[2u * m] += a;
        l[2u * m + 1u] += b;
        r[2u * m + 1u] += b;
    }
    for (unsigned i = 0; i < IH_N; i++) v[i] = v[DSP_BLOCK / 2 + i];
    for (unsigned i = 0; i < HB_N; i++) w[i] = w[DSP_BLOCK + i];
}

int looper_writing(const looper_t *lp)
{
    const loopio_t *io = lp->io;
    return lp->state == LOOPER_REC || lp->state == LOOPER_DUB || lp->sess || lp->mark ||
           io->w_busy || io->wr_head != io->wr_tail;
}

/* ------------------------------------------------------------ control */

static COLD int writer_busy(const looper_t *lp)
{
    return lp->sess || lp->mark || !ls_writer_idle(lp->ls);
}

COLD void looper_init(looper_t *lp, loopio_t *io, struct loopstore *ls)
{
    memset(lp, 0, sizeof *lp);
    lp->io = io;
    lp->ls = ls;
    lp->level = 1.0f;
    lp->ramp = 1000.0f / ((float)LOOPER_RAMP_MS * LOOPER_FS);
    lp->chunks = ls_chunks(ls);
    lp->state = lp->chunks ? LOOPER_EMPTY : LOOPER_OFF;
}

static COLD void to_empty(looper_t *lp)
{
    ls_clear(lp->ls);
    lp->state = LOOPER_EMPTY;
    lp->sess = lp->mark = 0;
    lp->xfade = 0;
    lp->alt = lp->redo = 0;
    lp->pend = lp->dub_req = 0;
    lp->dub_g = lp->dub_t = 0.0f;
    lp->len = lp->pos = lp->nfr = 0;
    lp->rd_f = 0;
    lp->started = lp->io->alloc;
}

/* a dub from PLAY: the undo point is taken by the flash side in stream
 * order (LIO_SNAP), so the dub starts at once */
static COLD void dub_start(looper_t *lp)
{
    if (!flush_mark(lp) || !lio_wr_mark(lp->io, LIO_SNAP)) {
        lp->dub_req = 1;                   /* no slot: looper_poll tries again */
        return;
    }
    lp->alt = 1;
    lp->redo = 0;
    lp->dub_req = 0;
    lp->dub_t = 1.0f;
    lp->state = LOOPER_DUB;
}

/* fade the loop out; `a` runs in looper_poll once silent and the writer idle */
static COLD void fade_then(looper_t *lp, int a)
{
    lp->dub_t = 0.0f;
    lp->dub_req = 0;
    lp->out_t = 0.0f;
    lp->pend = (uint8_t)a;
    if (lp->state == LOOPER_DUB) lp->state = LOOPER_PLAY;
}

static COLD void play_start(looper_t *lp)
{
    ls_play_from(lp->ls, 0);
    lp->rd_f = 0;
    lp->pos = 0;
    lp->out_g = 0.0f;
    lp->out_t = 1.0f;
    lp->state = LOOPER_PLAY;
}

COLD int looper_cmd(looper_t *lp, int a)
{
    int st = lp->state;
    if (st == LOOPER_OFF) return -2;
    ls_arm(lp->ls);                        /* erase ahead from now on */
    if (a == LOOPER_TAP)
        a = st == LOOPER_EMPTY ? LOOPER_REC_A : st == LOOPER_PLAY ? LOOPER_DUB_A : LOOPER_PLAY_A;
    if (lp->pend && a != LOOPER_CLEAR_A && a != LOOPER_STOP_A) return -1;   /* a fade runs */
    switch (a) {
    case LOOPER_REC_A:
        if (st == LOOPER_EMPTY) {
            if (lp->io->pool < LOOPER_READY_CHUNKS) return -3;
            to_empty(lp);
            ls_rec_begin(lp->ls);
            lp->state = LOOPER_REC;
            lp->rec_end = (lp->chunks - 1u) * CHUNK_SAMPLES;
            lp->out_g = lp->out_t = 1.0f;
            lp->passes = 0;
            return 0;
        }
        if (st != LOOPER_REC) return -1;
        __attribute__((fallthrough));    /* recording: close */
    case LOOPER_PLAY_A:
        if (st == LOOPER_REC) {
            if (lp->pos < lp->rec_end) lp->rec_end = lp->pos > MIN_LEN ? lp->pos : MIN_LEN;
        } else if (st == LOOPER_DUB) {
            lp->dub_t = 0.0f;
            lp->state = LOOPER_PLAY;
        } else if (st == LOOPER_STOP) {
            play_start(lp);
        } else if (st != LOOPER_PLAY) {
            return -1;
        }
        lp->dub_req = 0;
        return 0;
    case LOOPER_DUB_A:
        if (st == LOOPER_REC) {
            if (lp->pos < lp->rec_end) lp->rec_end = lp->pos > MIN_LEN ? lp->pos : MIN_LEN;
            lp->dub_req = 1;                 /* after the closing crossfade */
            return 0;
        }
        if (st == LOOPER_DUB) return 0;
        if (st != LOOPER_PLAY) return -1;
        if (!room(lp, 0u)) return -1;        /* no free flash for a new layer */
        if (lp->sess && !lp->xfade) {        /* still writing a punch-out: the same dub */
            lp->dub_t = 1.0f;
            lp->state = LOOPER_DUB;
        } else if (lp->sess || lp->xfade) {
            lp->dub_req = 1;
        } else {
            dub_start(lp);
        }
        return 0;
    case LOOPER_STOP_A:
        if (st == LOOPER_REC) {
            if (lp->pos < lp->rec_end) lp->rec_end = lp->pos > MIN_LEN ? lp->pos : MIN_LEN;
            lp->out_t = 0.0f;
            lp->pend = LOOPER_STOP_A;
            return 0;
        }
        if (st != LOOPER_PLAY && st != LOOPER_DUB) return st == LOOPER_STOP ? 0 : -1;
        fade_then(lp, LOOPER_STOP_A);
        return 0;
    case LOOPER_UNDO_A:
        if (!lp->alt || (st != LOOPER_PLAY && st != LOOPER_DUB && st != LOOPER_STOP)) return -1;
        if (st == LOOPER_STOP) {
            if (writer_busy(lp)) return -1;
            ls_swap(lp->ls, 0);
            lp->rd_f = 0;
            lp->redo ^= 1u;
            return 0;
        }
        fade_then(lp, LOOPER_UNDO_A);
        return 0;
    case LOOPER_CLEAR_A:
        if (st == LOOPER_EMPTY) return 0;
        if (st == LOOPER_REC || st == LOOPER_STOP) {
            to_empty(lp);
            return 0;
        }
        fade_then(lp, LOOPER_CLEAR_A);
        return 0;
    default:
        return -1;
    }
}

COLD int looper_save(looper_t *lp, unsigned n)
{
    if (lp->state == LOOPER_OFF) return -2;
    if (n >= 2u) return -1;
    if (lp->state == LOOPER_EMPTY || lp->state == LOOPER_REC || lp->len == 0u) return -1;
    if (lp->pend || writer_busy(lp)) return -6;
    return ls_save(lp->ls, n, lp->len, lp->alt, lp->redo);
}

COLD int looper_load(looper_t *lp, unsigned n)
{
    if (lp->state == LOOPER_OFF) return -2;
    if (n >= 2u) return -1;
    if (lp->state == LOOPER_REC || lp->state == LOOPER_DUB) return -1;
    if (lp->pend || writer_busy(lp)) return -6;
    uint32_t len = 0;
    int alt = 0, redo = 0;
    int r = ls_load(lp->ls, n, &len, &alt, &redo);
    if (r) return r;
    lp->len = len;
    lp->nfr = lp->io->nfr;
    lp->state = LOOPER_STOP;
    lp->pos = 0;
    lp->rd_f = 0;
    lp->alt = (uint8_t)(alt != 0);
    lp->redo = (uint8_t)(redo != 0);
    lp->sess = lp->mark = 0;
    lp->xfade = 0;
    lp->pend = lp->dub_req = 0;
    lp->dub_g = lp->dub_t = 0.0f;
    lp->out_g = lp->out_t = 0.0f;
    lp->passes = 0;
    return 0;
}

COLD void looper_poll(looper_t *lp)
{
    if (lp->state == LOOPER_PLAY || lp->state == LOOPER_DUB) {
        if (lp->mark) (void)flush_mark(lp);
        if (lp->pend && lp->out_g == 0.0f && !writer_busy(lp)) {
            int a = lp->pend;
            lp->pend = 0;
            if (a == LOOPER_STOP_A) {
                lp->state = LOOPER_STOP;
                lp->pos = 0;
            } else if (a == LOOPER_CLEAR_A) {
                to_empty(lp);
            } else {                         /* undo / redo: the next frame from the other map */
                ls_swap(lp->ls, lp->rd_f);
                lp->redo ^= 1u;
                lp->out_t = 1.0f;
            }
        }
        if (lp->dub_req && lp->state == LOOPER_PLAY && !lp->sess && !lp->xfade) dub_start(lp);
    }
    /* chunks started but never given a slot (all their frames dropped) */
    if (!lp->sess && !lp->mark && ls_writer_idle(lp->ls)) lp->started = lp->io->alloc;
}

COLD void looper_set_level(looper_t *lp, unsigned pct)
{
    lp->level = (float)(pct > 100u ? 100u : pct) * 0.01f;
}

static COLD unsigned ms(uint64_t samples) { return (unsigned)(samples * 1000u / 22050u); }

COLD void looper_info(const looper_t *lp, looper_info_t *out)
{
    uint32_t rec = lp->state == LOOPER_REC;
    out->state = lp->state;
    out->len_ms = ms(rec ? lp->pos : lp->len);
    out->pos_ms = ms(lp->pos);
    out->max_ms = lp->chunks ? ms((uint64_t)(lp->chunks - 1u) * CHUNK_SAMPLES) : 0u;
    out->undo_max_ms = ms((uint64_t)(lp->chunks / 2u) * CHUNK_SAMPLES);
    out->undo = lp->alt ? (lp->redo ? 2 : 1) : 0;
    out->level = (unsigned)(lp->level * 100.0f + 0.5f);
    out->prep_ms = ms((uint64_t)lp->io->pool * CHUNK_SAMPLES);
    out->flash = lp->chunks != 0u;
    if (lp->state <= LOOPER_EMPTY) out->len_ms = out->pos_ms = 0;
}
