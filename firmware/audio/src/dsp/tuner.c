/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include "tuner.h"
#include "cold.h"
#include <string.h>

#define LP_A        0.05f
#define DECIM       4
#define LEVEL_THR   400          /* (x * 8388606) >> 4, i.e. ~ -62 dBFS */
#define SILENT_N    750          /* samples below the threshold -> silent */
#define MIN_LAG     8
#define YIN_THR     0.1f
#define YIN_REJECT  0.2f

/* Octave bounds (stock 0x20016fd4) and the A2..A#3 note table (0x20016ff8). */
static const float oct_tab[9] = {27.5f, 32.703f, 65.406f, 130.813f, 261.626f,
                                 523.251f, 1046.505f, 2093.004f, 4186.009f};
static const float note_tab[14] = {110.0f, 116.5409f, 123.4708f, 130.8128f, 138.5913f,
                                   146.8324f, 155.5635f, 164.8138f, 174.6141f,
                                   184.9972f, 195.9977f, 207.6524f, 220.0f, 233.0819f};

static float log2_approx(float x)          /* |err| < 1e-4 for x > 0 */
{
    union { float f; uint32_t u; } v = {x};
    float e = (float)((int)((v.u >> 23) & 0xff) - 127);
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;             /* m in [1, 2) */
    float m = v.f, t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
    /* log2(m) = 2/ln2 * atanh(t) */
    return e + 2.8853900818f * t * (1.0f + t2 * (0.33333333f + t2 * (0.2f + t2 * 0.14285714f)));
}

void tuner_set_a4(tuner_t *t, int a4_hz)
{
    if (a4_hz < TUNER_CAL_MIN) a4_hz = TUNER_CAL_MIN;
    if (a4_hz > TUNER_CAL_MAX) a4_hz = TUNER_CAL_MAX;
    t->cal = (float)a4_hz / 440.0f;                        /* stock table 0x1ca4c */
}

void tuner_init(tuner_t *t, int a4_hz)
{
    memset(t, 0, sizeof *t);
    t->a = LP_A;
    t->idx[1] = TUNER_BUF / 2;   /* stock initial value (DTCM data): the first B
                                  * buffer starts half full of zeros */
    t->res.octave = -1;
    tuner_set_a4(t, a4_hz ? a4_hz : 440);
}

/* stock 0x3aa4 + 0x159e0, once per input sample */
void tuner_feed(tuner_t *t, const float *x, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float s = x[i];
        float q = s * 8388606.0f;
        int32_t xi = q >= 2147483520.0f ? 0x7fffffff : q <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)q;
        if ((xi >> 4) >= LEVEL_THR) {
            t->below_thr = 0;
            t->silent = 0;
        } else {
            if (++t->below_thr > 800) t->below_thr = SILENT_N;
            if (t->below_thr >= SILENT_N) t->silent = 1;
        }
        float y = t->a * s;
        y += (1.0f - t->a) * t->lp;
        t->lp = y;
        if (++t->dec < DECIM) continue;
        t->dec = 0;
        if (!(t->full[0] | t->busy)) {                     /* buffer A */
            if (t->idx[0] < TUNER_BUF) t->buf[0][t->idx[0]] = y;
            if (++t->idx[0] == TUNER_BUF) {
                t->idx[0] = 0;
                if (t->full[1] != 1) {
                    t->full[0] = 1;
                    t->busy = 1;
                } else {
                    t->full[0] = 0;
                    t->busy = 0;
                    continue;
                }
            }
        }
        /* buffer B fills only while A waits for the main loop (busy); it
         * starts with the sample that completed A */
        if (t->full[1] || t->busy != 1) continue;
        if (t->idx[1] < TUNER_BUF) t->buf[1][t->idx[1]] = y;
        if (++t->idx[1] == TUNER_BUF) {
            t->idx[1] = 0;
            if (t->full[0] != 1) {
                t->full[1] = 1;
                t->busy = 0;
            } else {
                t->full[1] = 0;
            }
        }
    }
}

/* stock 0x3728: YIN over 500 samples, lags MIN_LAG..499 */
COLD float tuner_yin(tuner_t *t, const float *x, float fs, float *confidence)
{
    float *acf = t->acf, *d = t->d, *dn = t->dn;
    float e0 = 0.0f;
    for (int j = 0; j < TUNER_WIN; j++) e0 += x[j] * x[j];
    for (int tau = 0; tau < TUNER_WIN - 2; tau++) {
        float s = 0.0f;
        for (int m = tau; m < tau + TUNER_WIN; m++) s += x[m] * x[m - tau];
        acf[tau] = s;
    }
    acf[TUNER_WIN - 2] = acf[TUNER_WIN - 1] = 0.0f;        /* never written by the stock */
    float et = e0;
    for (int tau = 0; tau < TUNER_WIN; tau++) {
        d[tau] = (e0 + et) - acf[tau] * 2.0f;
        et -= x[tau] * x[tau];
        /* stock off-by-one: adds x[tau + 501], so the lagged energy window
         * is 501 samples long after the first step (kept for parity) */
        et += x[tau + TUNER_WIN + 1] * x[tau + TUNER_WIN + 1];
    }
    dn[0] = 1.0f;
    double sum = 0.0;
    for (int tau = 1; tau < TUNER_WIN; tau++) {
        sum += (double)d[tau];
        dn[tau] = sum == 0.0 ? 100.0f : (float)((double)(d[tau] * (float)tau) / sum);
    }
    dn[TUNER_WIN] = x[TUNER_WIN - 1];                      /* stock reads one past */
    dn[MIN_LAG - 3] = dn[MIN_LAG - 2] = dn[MIN_LAG - 1] = 100.0f;

    float gmin = 100.0f, cnt = 0.0f, prev = 0.0f, prevprev = 0.0f;
    int gidx = 0, n_all = 0, first_below = -1;
    for (int tau = MIN_LAG; tau < TUNER_WIN; tau++) {
        float cur = dn[tau - 1];
        if (gmin > cur) {
            gmin = cur;
            gidx = tau - 1;
        }
        cnt += 1.0f;
        if (cnt > 2.0f) cnt = 3.0f;
        if (cur > prev && prevprev > prev && cnt > 2.0f) {  /* local minimum at tau-2 */
            cnt = 0.0f;
            n_all++;
            if (prev < YIN_THR && first_below < 0) first_below = tau - 2;
        }
        prevprev = prev;
        prev = cur;
    }
    float tp = 0.0f;
    if (n_all >= 1) tp = (float)(first_below >= 0 ? first_below : gidx);
    if (tp >= 1.0f && tp < (float)TUNER_WIN) {
        int i = (int)tp;
        float a = dn[i - 1], b = dn[i], c = dn[i + 1];
        float den = (a - b * 2.0f) + c;
        if (den != 0.0f) tp = ((a - c) * 0.5f) / den + tp;
    }
    float f = tp != 0.0f ? fs / tp : 0.0f;
    if (gmin > YIN_REJECT) f = 0.0f;
    if (confidence) *confidence = gmin < 1.0f ? 1.0f - gmin : 0.0f;
    return f;
}

/* stock 0xbbc0: note 1..12 (A#..A) and deviation 1..100 */
static void note_map(tuner_t *t, float f)
{
    float cal = t->cal;
    while (note_tab[0] > f) f *= 2.0f;
    while (note_tab[12] <= f) f *= 0.5f;
    int i = 0;
    float lo = 0.0f, hi = 0.0f;
    for (int k = 0; k < 13; k++) {
        lo = note_tab[k] * cal;
        hi = note_tab[k + 1] * cal;
        if (lo <= f && f < hi) {
            i = k;
            break;
        }
    }
    lo = note_tab[i] * cal;
    float mid = (hi + lo) * 0.5f;
    int dev;
    if (f < mid) {
        float r = ((f - lo) * 1000.0f) / (mid - lo);
        if (r > 40.0f) {
            dev = (int)(((r - 40.0f) * 5.0f) / 96.0f + 50.0f);
            if (dev > 100) dev = 100;
        } else {
            dev = 50;
        }
    } else {
        i++;
        float r = ((hi - f) * 1000.0f) / (hi - mid);
        if (r > 40.0f) {
            dev = (int)(50.0f - ((r - 40.0f) * 5.0f) / 96.0f);
            if (dev <= 0) dev = 1;
        } else {
            dev = 50;
        }
    }
    if (i >= 12) i -= 12;
    if (i <= 0) i += 12;
    t->res.note = i;
    t->res.deviation = dev;
}

/* stock 0x11438 */
static void post(tuner_t *t, float f, float conf)
{
    tuner_result_t *r = &t->res;
    r->valid = 0;
    r->freq = 0.0f;
    r->confidence = conf;
    if (f > 24.0f && f < 4100.0f) {
        int o = -1;
        for (int k = 0; k < 8; k++)
            if (oct_tab[k] <= f && f < oct_tab[k + 1]) o = k;
        r->octave = o;
        note_map(t, f);
        float semis = 12.0f * log2_approx(f / (440.0f * t->cal));
        float nearest = (float)(int)(semis + (semis >= 0.0f ? 0.5f : -0.5f));
        r->cents = (semis - nearest) * 100.0f;
        r->freq = f;
        r->valid = 1;
    }
    r->silent = t->silent;
    if (t->silent) {                                       /* stock "no signal" */
        r->valid = 0;
        r->note = 0;
        r->deviation = 0;
        r->octave = -1;
        r->freq = 0.0f;
        r->cents = 0.0f;
    }
}

COLD int tuner_poll(tuner_t *t, tuner_result_t *out)
{
    int b = t->full[0] == 1 ? 0 : t->full[1] == 1 ? 1 : -1;
    if (b < 0) return 0;
    float conf;
    float f = tuner_yin(t, t->buf[b], TUNER_FS, &conf);
    post(t, f, conf);
    t->full[b] = 0;
    if (out) *out = t->res;
    return 1;
}
