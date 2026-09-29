/* Looper: see looper.h. looper_process and what it calls are on the audio
 * path (ITCM); the control functions are COLD (flash). */
#include <string.h>
#include "cold.h"
#include "looper.h"
#include "dsp.h"

#define FS_FULL 44100.0f
#define SCALE 16384.0f                 /* int16 = x * SCALE: +6 dB headroom */
#define HB_N (4 * LOOPER_HB_SIDE - 2)  /* decimator history: taps - 1 (26) */
#define IH_N (2 * LOOPER_HB_SIDE - 1)  /* interpolator history (13) */

/* Half-band FIR (27 taps, Kaiser beta 6): h[13] = 0.5, h[13 +- (2j+1)] =
 * kHb[j], the even offsets are 0. Sum of kHb = 0.25 (unity at DC). Flat
 * (+-0.03 dB) to 8 kHz, -6 dB at 11.025 kHz, <= -47 dB from 14 kHz and
 * <= -64 dB from 15 kHz. */
static const float kHb[LOOPER_HB_SIDE] = {
    3.132591042e-01f, -9.156855253e-02f, 4.186601721e-02f, -1.937165627e-02f,
    7.920118734e-03f, -2.469291685e-03f, 3.642603099e-04f,
};

/* IMA ADPCM (the standard tables) */
static const int16_t kStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
    8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
    29794, 32767,
};
static const int8_t kIdx[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

/* One IMA step on the packed state s = pred (low 16 bits) | idx << 16:
 * the decoder, and the encoder after quant(), so both track the same
 * state. Packed: it stays in registers across the call (one copy of the
 * code for both: ITCM). */
__attribute__((noinline)) static uint32_t step(uint32_t s, unsigned n)
{
    int st = kStep[s >> 16], d = st >> 3;
    if (n & 4u) d += st;
    if (n & 2u) d += st >> 1;
    if (n & 1u) d += st >> 2;
    int p = (int16_t)s + ((n & 8u) ? -d : d);
    p = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
    int i = (int)(s >> 16) + kIdx[n & 7u];
    i = i < 0 ? 0 : i > 88 ? 88 : i;
    return (uint16_t)p | (uint32_t)i << 16;
}

/* the encoder's nibble for x */
static inline unsigned quant(uint32_t s, int x)
{
    int st = kStep[s >> 16], d = x - (int16_t)s;
    unsigned n = 0;
    if (d < 0) { n = 8; d = -d; }
    if (d >= st) { n |= 4; d -= st; }
    st >>= 1;
    if (d >= st) { n |= 2; d -= st; }
    st >>= 1;
    if (d >= st) n |= 1;
    return n;
}

static inline uint32_t pack(const adpcm_t *a) { return (uint16_t)a->pred | (uint32_t)a->idx << 16; }

static inline void unpack(adpcm_t *a, uint32_t s)
{
    a->pred = (int16_t)s;
    a->idx = (uint8_t)(s >> 16);
}

/* the codec for the tests (flash) */
COLD int looper_adpcm_dec(adpcm_t *a, unsigned nib)
{
    unpack(a, step(pack(a), nib & 15u));
    return a->pred;
}

COLD unsigned looper_adpcm_enc(adpcm_t *a, int x)
{
    unsigned n = quant(pack(a), x);
    unpack(a, step(pack(a), n));
    return n;
}

/* global block g of the memory */
static inline uint8_t *blk(const looper_t *lp, uint32_t g)
{
    return g < lp->nseg0 ? lp->seg[0] + g * LOOPER_BLK_BYTES
                         : lp->seg[1] + (g - lp->nseg0) * LOOPER_BLK_BYTES;
}

static inline uint8_t *bank_blk(const looper_t *lp, unsigned bank, uint32_t b)
{
    return blk(lp, b + (lp->banks == 2 ? bank * lp->half : 0));
}

/* float -> int16, saturated (the M7: VCVT then SSAT) */
static inline int sat16(float v)
{
#if defined(__ARM_FEATURE_SAT)
    return __builtin_arm_ssat((int)v, 16);   /* |v| < 2^17 here: the cast is exact */
#else
    return v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v;
#endif
}

static inline float ramp(float g, float t, float st)
{
    float e = t - g;
    return e > st ? g + st : e < -st ? g - st : t;
}

/* REC closes: the loop is pos samples long; the first block is rewritten
 * with the crossfade (in place, the writer continues the record's
 * encoder: the tail follows the loop end in time). */
__attribute__((noinline)) static void close_rec(looper_t *lp)
{
    lp->len = lp->pos;
    lp->nblk = (lp->len + LOOPER_BLK - 1u) / LOOPER_BLK;
    lp->banks = lp->nblk <= lp->half ? 2 : 1;
    lp->cur = 0;
    lp->alt = lp->redo = 0;
    lp->pos = 0;
    lp->xfade = LOOPER_BLK;
    lp->writing = 1;
    lp->state = LOOPER_PLAY;
}

/* k samples at the loop's rate: x in (read only while writing), y out (the
 * loop, level and fades in). One path for all states that run: REC is a
 * write with no old signal. The state lives in locals here: the nibble
 * stores (uint8_t) would otherwise make the compiler reload it. The fades
 * step linearly across the call (to the value a per-sample ramp reaches). */
static void run(looper_t *lp, const float *x, float *y, unsigned k)
{
    if (lp->state == LOOPER_STOP) {
        for (unsigned i = 0; i < k; i++) y[i] = 0.0f;
        return;
    }
    const float rk = lp->ramp * (float)k, ik = 1.0f / (float)k;
    const float lv = lp->level * (1.0f / SCALE);
    float dg = lp->dub_g, og = lp->out_g;
    const float dg1 = ramp(dg, lp->dub_t, rk), og1 = ramp(og, lp->out_t, rk);
    const float ddg = (dg1 - dg) * ik, dog = (og1 - og) * ik;
    const int dub = lp->dub_t > 0.0f || dg > 0.0f || dg1 > 0.0f;
    int rec = lp->state == LOOPER_REC, wr = lp->writing;
    uint32_t pos = lp->pos, xf = lp->xfade, end = rec ? lp->rec_end : lp->len;
    uint32_t ds = pack(&lp->dec), es = pack(&lp->enc);
    uint8_t *p = bank_blk(lp, lp->cur, pos / LOOPER_BLK);
    for (unsigned i = 0; i < k; i++) {
        uint32_t o = pos % LOOPER_BLK;
        int want = rec || dub || xf;
        dg += ddg;
        og += dog;
        if (o == 0) {
            p = bank_blk(lp, lp->cur, pos / LOOPER_BLK);
            if (!rec) ds = (uint16_t)(p[0] | p[1] << 8) | (uint32_t)(p[2] > 88 ? 88 : p[2]) << 16;
            if (wr && want) {
                p[0] = (uint8_t)es;
                p[1] = (uint8_t)(es >> 8);
                p[2] = (uint8_t)(es >> 16);
                p[3] = 0;
            } else {
                wr = 0;
            }
        }
        if (!wr && want) {                    /* starts here: from the old stream's state */
            wr = 1;
            es = ds;
        }
        uint8_t *b = &p[4 + (o >> 1)];
        float old = 0.0f;
        if (!rec) {
            ds = step(ds, (o & 1u) ? *b >> 4 : *b & 15u);
            old = (float)(int16_t)ds;
        }
        float out = old;
        if (wr) {
            float gi = 1.0f, go = 0.0f;        /* REC */
            if (xf) {                          /* the closing crossfade */
                go = (float)(LOOPER_BLK - xf) * (1.0f / LOOPER_BLK);
                gi = 1.0f - go;
                out = old * go;                /* the tail is heard live on this pass */
                xf--;
            } else if (!rec) {
                gi = dg;
                go = 1.0f - (1.0f - LOOPER_FB) * gi;
            }
            float v = old * go + x[i] * SCALE * gi;
            unsigned nb = quant(es, sat16(v));
            es = step(es, nb);
            *b = (o & 1u) ? (uint8_t)((*b & 0x0Fu) | nb << 4) : (uint8_t)((*b & 0xF0u) | nb);
        }
        y[i] = out * og * lv;
        if (++pos >= end) {
            if (rec) {                         /* the loop closes: reload the state */
                lp->pos = pos;
                unpack(&lp->enc, es);
                close_rec(lp);
                rec = 0;
                wr = 1;
                xf = lp->xfade;
                end = lp->len;
            } else {
                lp->passes++;
            }
            pos = 0;
        }
    }
    lp->pos = pos;
    lp->xfade = xf;
    lp->writing = (uint8_t)wr;
    lp->dub_g = dg1;
    lp->out_g = og1;
    unpack(&lp->dec, ds);
    unpack(&lp->enc, es);
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
    float x[DSP_BLOCK / 2], y[DSP_BLOCK];
    if (lp->hq) {
        run(lp, w + HB_N, y, DSP_BLOCK);
        for (unsigned i = 0; i < DSP_BLOCK; i++) {
            l[i] += y[i];
            r[i] += y[i];
        }
    } else {
        /* 2:1 half-band, only while something is recorded (the history
         * always moves on, so a dub starts from a clean filter) */
        if (lp->state == LOOPER_REC || lp->dub_t > 0.0f || lp->dub_g > 0.0f || lp->xfade)
            for (unsigned m = 0; m < DSP_BLOCK / 2; m++) {
                const float *c = w + 2u * m + 1u + HB_N / 2u;
                x[m] = 0.5f * c[0] + hb(c);
            }
        float *v = lp->iw;               /* [history IH_N | this block] at 22.05 kHz */
        run(lp, x, v + IH_N, DSP_BLOCK / 2);
        for (unsigned m = 0; m < DSP_BLOCK / 2; m++) {   /* 1:2 half-band */
            const float *c = v + m + LOOPER_HB_SIDE - 1u;
            float a = c[0], b = 2.0f * hbi(c);
            l[2u * m] += a;
            r[2u * m] += a;
            l[2u * m + 1u] += b;
            r[2u * m + 1u] += b;
        }
        for (unsigned i = 0; i < IH_N; i++) v[i] = v[DSP_BLOCK / 2 + i];
    }
    for (unsigned i = 0; i < HB_N; i++) w[i] = w[DSP_BLOCK + i];
}

/* ------------------------------------------------------------ control */

/* fade step per sample at the loop's rate */
static COLD void set_rate(looper_t *lp)
{
    lp->ramp = 1000.0f / ((float)LOOPER_RAMP_MS * (lp->hq ? FS_FULL : FS_FULL * 0.5f));
}

COLD void looper_init(looper_t *lp)
{
    memset(lp, 0, sizeof *lp);
    lp->level = 1.0f;
    set_rate(lp);
}

COLD void looper_attach(looper_t *lp, void *m0, size_t bytes0, void *m1, size_t bytes1)
{
    lp->seg[0] = m0;
    lp->seg[1] = m1;
    lp->nseg0 = (uint32_t)(bytes0 / LOOPER_BLK_BYTES);
    lp->total = lp->nseg0 + (m1 ? (uint32_t)(bytes1 / LOOPER_BLK_BYTES) : 0u);
    lp->half = lp->total / 2u;
    lp->state = LOOPER_EMPTY;
    lp->pend = lp->dub_req = 0;
}

COLD void looper_detach(looper_t *lp)
{
    lp->state = LOOPER_OFF;
    lp->seg[0] = lp->seg[1] = NULL;
    lp->total = lp->nseg0 = lp->half = 0;
}

static COLD void to_empty(looper_t *lp)
{
    lp->state = LOOPER_EMPTY;
    lp->writing = lp->xfade = 0;
    lp->pend = lp->dub_req = 0;
    lp->dub_g = lp->dub_t = 0.0f;
    lp->len = lp->pos = 0;
}

/* a dub from PLAY with the writer idle: on a copy of the loop (undo), or in
 * place when the loop does not fit twice */
static COLD void dub_start(looper_t *lp)
{
    if (lp->banks == 2) {
        for (uint32_t b = 0; b < lp->nblk; b++)
            memcpy(bank_blk(lp, lp->cur ^ 1u, b), bank_blk(lp, lp->cur, b), LOOPER_BLK_BYTES);
        lp->cur ^= 1u;                   /* same bytes: playback goes on seamlessly */
        lp->alt = 1;
        lp->redo = 0;
    }
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
    lp->pos = 0;
    lp->out_g = 0.0f;
    lp->out_t = 1.0f;
    lp->state = LOOPER_PLAY;
}

COLD int looper_cmd(looper_t *lp, int a)
{
    int st = lp->state;
    if (st == LOOPER_OFF) return -2;
    if (a == LOOPER_TAP)
        a = st == LOOPER_EMPTY ? LOOPER_REC_A : st == LOOPER_PLAY ? LOOPER_DUB_A : LOOPER_PLAY_A;
    if (lp->pend && a != LOOPER_CLEAR_A && a != LOOPER_STOP_A) return -1;   /* a fade runs */
    uint32_t min = LOOPER_MIN_BLKS * LOOPER_BLK;
    switch (a) {
    case LOOPER_REC_A:
        if (st == LOOPER_EMPTY) {
            to_empty(lp);
            lp->state = LOOPER_REC;
            lp->rec_end = lp->total * LOOPER_BLK;
            lp->banks = 1;
            lp->cur = 0;
            lp->alt = lp->redo = 0;
            lp->dec.pred = lp->enc.pred = 0;
            lp->dec.idx = lp->enc.idx = 0;
            lp->writing = 1;
            lp->out_g = lp->out_t = 1.0f;
            lp->passes = 0;
            return 0;
        }
        if (st != LOOPER_REC) return -1;
        __attribute__((fallthrough));    /* recording: close */
    case LOOPER_PLAY_A:
        if (st == LOOPER_REC) {
            lp->rec_end = lp->pos > min ? lp->pos : min;
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
            lp->rec_end = lp->pos > min ? lp->pos : min;
            lp->dub_req = 1;                 /* after the closing crossfade */
            return 0;
        }
        if (st == LOOPER_DUB) return 0;
        if (st != LOOPER_PLAY) return -1;
        if (lp->writing && !lp->xfade) {     /* still writing a punch-out: the same dub */
            lp->dub_t = 1.0f;
            lp->state = LOOPER_DUB;
        } else if (lp->writing) {
            lp->dub_req = 1;
        } else {
            dub_start(lp);
        }
        return 0;
    case LOOPER_STOP_A:
        if (st == LOOPER_REC) {
            lp->rec_end = lp->pos > min ? lp->pos : min;
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
            lp->cur ^= 1u;
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

COLD void looper_poll(looper_t *lp)
{
    if (lp->state == LOOPER_PLAY || lp->state == LOOPER_DUB) {
        if (lp->pend && lp->out_g == 0.0f && !lp->writing) {
            int a = lp->pend;
            lp->pend = 0;
            if (a == LOOPER_STOP_A) {
                lp->state = LOOPER_STOP;
                lp->pos = 0;
            } else if (a == LOOPER_CLEAR_A) {
                to_empty(lp);
            } else {                         /* undo / redo */
                lp->cur ^= 1u;
                lp->redo ^= 1u;
                lp->out_t = 1.0f;
            }
        }
        if (lp->dub_req && lp->state == LOOPER_PLAY && !lp->writing) {
            lp->dub_req = 0;
            dub_start(lp);
        }
    }
}

COLD int looper_set_hq(looper_t *lp, int on)
{
    if (lp->state > LOOPER_EMPTY) return -1;
    lp->hq = on ? 1 : 0;
    set_rate(lp);
    return 0;
}

COLD void looper_set_level(looper_t *lp, unsigned pct)
{
    lp->level = (float)(pct > 100u ? 100u : pct) * 0.01f;
}

COLD void looper_info(const looper_t *lp, looper_info_t *out)
{
    float fs = lp->hq ? FS_FULL : FS_FULL * 0.5f;
    uint32_t rec = lp->state == LOOPER_REC;
    out->state = lp->state;
    out->len_ms = (unsigned)((float)(rec ? lp->pos : lp->len) * 1000.0f / fs);
    out->pos_ms = (unsigned)((float)lp->pos * 1000.0f / fs);
    out->max_ms = (unsigned)((float)(lp->total * LOOPER_BLK) * 1000.0f / fs);
    out->undo_max_ms = (unsigned)((float)(lp->half * LOOPER_BLK) * 1000.0f / fs);
    out->undo = lp->alt ? (lp->redo ? 2 : 1) : 0;
    out->hq = lp->hq;
    out->level = (unsigned)(lp->level * 100.0f + 0.5f);
    if (lp->state <= LOOPER_EMPTY) out->len_ms = out->pos_ms = 0;
}

