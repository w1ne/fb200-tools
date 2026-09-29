/* Looper: see looper.h. looper_process and what it calls are on the audio
 * path (ITCM); the control functions are COLD (flash). */
#include <string.h>
#include "cold.h"
#include "looper.h"
#include "dsp.h"

#define FS_FULL 44100.0f
#define SCALE 16384.0f                 /* int16 = x * SCALE: +6 dB headroom */
#define HB_N (4 * LOOPER_HB_SIDE - 2)  /* decimator history: 34 = taps - 1 */
#define IH_N (2 * LOOPER_HB_SIDE - 1)  /* interpolator history: 17 */

/* Half-band FIR (35 taps, Kaiser beta 7): h[17] = 0.5, h[17 +- (2j+1)] =
 * kHb[j], the even offsets are 0. Sum of kHb = 0.25 (unity at DC). Flat
 * (+-0.002 dB) to 8 kHz, -6 dB at 11.025 kHz, <= -70 dB from 14 kHz. */
static const float kHb[LOOPER_HB_SIDE] = {
    3.147821785e-01f, -9.585885661e-02f, 4.783006966e-02f, -2.565559095e-02f,
    1.333691788e-02f, -6.326767135e-03f, 2.562961912e-03f, -7.819818642e-04f,
    1.110685615e-04f,
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

/* the decoder step, shared by the encoder so both track the same state
 * (not inlined: ITCM is scarce, docs/FIRMWARE_BRINGUP.md) */
__attribute__((noinline)) static int adpcm_step(adpcm_t *s, unsigned n)
{
    int step = kStep[s->idx];
    int d = step >> 3;
    if (n & 4u) d += step;
    if (n & 2u) d += step >> 1;
    if (n & 1u) d += step >> 2;
    int p = s->pred + ((n & 8u) ? -d : d);
    if (p > 32767) p = 32767;
    else if (p < -32768) p = -32768;
    s->pred = (int16_t)p;
    int i = s->idx + kIdx[n & 7u];
    s->idx = (uint8_t)(i < 0 ? 0 : i > 88 ? 88 : i);
    return p;
}

int looper_adpcm_dec(adpcm_t *s, unsigned nib) { return adpcm_step(s, nib & 15u); }

unsigned looper_adpcm_enc(adpcm_t *s, int x)
{
    int step = kStep[s->idx], d = x - s->pred;
    unsigned n = 0;
    if (d < 0) { n = 8; d = -d; }
    if (d >= step) { n |= 4; d -= step; }
    step >>= 1;
    if (d >= step) { n |= 2; d -= step; }
    step >>= 1;
    if (d >= step) n |= 1;
    (void)adpcm_step(s, n);
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

static inline void hdr_get(const uint8_t *p, adpcm_t *s)
{
    s->pred = (int16_t)(p[0] | p[1] << 8);
    s->idx = p[2] > 88 ? 88 : p[2];
}

static inline void hdr_put(uint8_t *p, const adpcm_t *s)
{
    p[0] = (uint8_t)s->pred;
    p[1] = (uint8_t)((uint16_t)s->pred >> 8);
    p[2] = s->idx;
    p[3] = 0;
}

static inline unsigned nib_get(const uint8_t *p, uint32_t o)
{
    unsigned b = p[4 + (o >> 1)];
    return (o & 1u) ? b >> 4 : b & 15u;
}

static inline void nib_put(uint8_t *p, uint32_t o, unsigned n)
{
    uint8_t *b = &p[4 + (o >> 1)];
    *b = (o & 1u) ? (uint8_t)((*b & 0x0Fu) | n << 4) : (uint8_t)((*b & 0xF0u) | n);
}

static inline float ramp(float g, float t, float step)
{
    float e = t - g;
    return e > step ? g + step : e < -step ? g - step : t;
}

/* REC closes: the loop is pos samples long; the first block is rewritten
 * with the crossfade (in place, the writer continues the record's
 * encoder: the tail follows the loop end in time). */
static void close_rec(looper_t *lp)
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

/* k samples at the loop's rate: x in, y out (the loop, level and fades in).
 * One path for all states that run: REC is a write with no old signal. */
static void run(looper_t *lp, const float *x, float *y, unsigned k)
{
    const float rs = lp->ramp, lv = lp->level * (1.0f / SCALE);
    for (unsigned i = 0; i < k; i++) {
        y[i] = 0.0f;
        if (lp->state == LOOPER_STOP) continue;
        const int rec = lp->state == LOOPER_REC;
        uint32_t o = lp->pos % LOOPER_BLK;
        uint8_t *p = bank_blk(lp, lp->cur, lp->pos / LOOPER_BLK);
        int want = rec || lp->dub_t > 0.0f || lp->dub_g > 0.0f || lp->xfade;
        if (o == 0) {
            if (!rec) hdr_get(p, &lp->dec);
            if (lp->writing && want) hdr_put(p, &lp->enc);
            else lp->writing = 0;
        }
        if (!lp->writing && want) {           /* starts here: from the old stream's state */
            lp->writing = 1;
            lp->enc = lp->dec;
        }
        float old = rec ? 0.0f : (float)looper_adpcm_dec(&lp->dec, nib_get(p, o));
        float out = old;
        if (lp->writing) {
            float gi = 1.0f, go = 0.0f;        /* REC */
            if (lp->xfade) {                   /* the closing crossfade */
                go = (float)(LOOPER_BLK - lp->xfade) * (1.0f / LOOPER_BLK);
                gi = 1.0f - go;
                out = old * go;                /* the tail is heard live on this pass */
                lp->xfade--;
            } else if (!rec) {
                gi = lp->dub_g;
                go = 1.0f - (1.0f - LOOPER_FB) * gi;
            }
            float v = old * go + x[i] * SCALE * gi;
            int q = v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v;
            nib_put(p, o, looper_adpcm_enc(&lp->enc, q));
        }
        lp->dub_g = ramp(lp->dub_g, lp->dub_t, rs);
        lp->out_g = ramp(lp->out_g, lp->out_t, rs);
        y[i] = out * lp->out_g * lv;
        if (++lp->pos >= (rec ? lp->rec_end : lp->len)) {
            if (rec) {
                close_rec(lp);
            } else {
                lp->pos = 0;
                lp->passes++;
            }
        }
    }
}

/* 2:1 half-band decimation in place: x[0..n) -> x[0..n/2) */
static void decimate(looper_t *lp, float *x, unsigned n)
{
    float w[HB_N + DSP_BLOCK];
    memcpy(w, lp->dh, sizeof lp->dh);
    memcpy(w + HB_N, x, n * sizeof x[0]);
    for (unsigned m = 0; m < n / 2u; m++) {
        const float *c = w + 2u * m + 1u + HB_N / 2u;   /* centre */
        float a = 0.5f * c[0];
        for (unsigned j = 0; j < LOOPER_HB_SIDE; j++)
            a += kHb[j] * (c[-(int)(2u * j + 1u)] + c[2u * j + 1u]);
        x[m] = a;
    }
    memcpy(lp->dh, w + n, sizeof lp->dh);
}

/* 1:2 half-band interpolation: y[0..k) -> out[0..2k) */
static void interpolate(looper_t *lp, const float *y, float *out, unsigned k)
{
    float v[IH_N + DSP_BLOCK / 2];
    memcpy(v, lp->ih, sizeof lp->ih);
    memcpy(v + IH_N, y, k * sizeof y[0]);
    for (unsigned m = 0; m < k; m++) {
        const float *c = v + m + LOOPER_HB_SIDE - 1u;
        float a = 0.0f;
        for (unsigned j = 0; j < LOOPER_HB_SIDE; j++) a += kHb[j] * (c[-(int)j] + c[j + 1u]);
        out[2u * m] = c[0];
        out[2u * m + 1u] = 2.0f * a;
    }
    memcpy(lp->ih, v + k, sizeof lp->ih);
}

void looper_process(looper_t *lp, float *l, float *r, unsigned n)
{
    if (lp->state <= LOOPER_EMPTY) return;
    float x[DSP_BLOCK], y[DSP_BLOCK / 2], o[DSP_BLOCK];
    if (n != DSP_BLOCK) return;          /* the engine's whole blocks only */
    for (unsigned i = 0; i < n; i++) x[i] = 0.5f * (l[i] + r[i]);
    if (lp->hq) {
        run(lp, x, o, n);
    } else {
        decimate(lp, x, n);
        run(lp, x, y, n / 2u);
        interpolate(lp, y, o, n / 2u);
    }
    for (unsigned i = 0; i < n; i++) {
        l[i] += o[i];
        r[i] += o[i];
    }
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

