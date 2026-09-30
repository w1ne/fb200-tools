/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* MOD block: port of the stock FB200 modulation types (see mod.h).
 *
 * Each type is decompiled from the stock image (addresses below) and keeps its
 * arithmetic: float vs double steps, operation order and rounding, so the
 * output matches the stock sample for sample at 44.1 kHz. Build with
 * -ffp-contract=off for that (a fused multiply-add changes the last bit; the
 * pragma below does it for GCC).
 *
 * Stock conventions: p = knob * 0.01f (p2 smoothed); LFO phases are floats in
 * cycles (0..1, wavetable 0xb4e4) or radians (sin/cos from libm, double);
 * the tables are a sine (sin 2 pi n / 256) and a triangle with rounded peaks;
 * the delay lines are rings of 500/550/600/700 floats read with linear
 * interpolation; most types mix (1 - p2) x + k p2 wet. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("fp-contract=off")
#endif
#include <string.h>
#include "cold.h"
#include "arm_math.h"
#include "mod.h"

/* ---- shared tables: LFO wavetables and the two static allpass banks */
static float tab_sin[256], tab_tri[256];
static float rot_b1[256], rot_b0;         /* rotary: b1 per index, b0 = -a2 fixed */
static float step_b0[128], step_b1[128];  /* step phaser */
static float tables_fs;

/* sin/cos in double (the stock calls libm): quadrant reduction + Taylor */
static double ksin(double r)
{
    double r2 = r * r, t = r, s = r;
    for (int i = 1; i < 10; i++) { t *= -r2 / (double)((2 * i) * (2 * i + 1)); s += t; }
    return s;
}

static double kcos(double r)
{
    double r2 = r * r, t = 1.0, s = 1.0;
    for (int i = 1; i < 10; i++) { t *= -r2 / (double)((2 * i - 1) * (2 * i)); s += t; }
    return s;
}

static double dsincos(double x, int cosine)
{
    const double hpi = 1.57079632679489661923;
    double q = x / hpi;
    long k = (long)(q + (q >= 0 ? 0.5 : -0.5));
    double r = x - (double)k * hpi;
    switch ((k + cosine) & 3) {
    case 0: return ksin(r);
    case 1: return kcos(r);
    case 2: return -ksin(r);
    default: return -kcos(r);
    }
}

static double dsin(double x) { return dsincos(x, 0); }
static double dcos(double x) { return dsincos(x, 1); }
static double dtan(double x) { return dsin(x) / dcos(x); }

/* first-order-pole allpass section as the stock builds it (phaser 0xb890):
 * c = (t - 1) / (t + 1), t = tan(pi B / fs); b = {-c, -(1 - c) cos w, 1, (1 - c) cos w, c} */
static double ap_c(double bw, double fs)
{
    double t = dtan(3.14159265358979323846 * bw / fs);
    return (t - 1.0) / (t + 1.0);
}

static float q(double v, double one) { v *= one; return (float)((v < 0 ? (long)(v - 0.5) : (long)(v + 0.5)) / one); }

static void tables_init(float fs)
{
    const double pi2 = 6.28318530717958647692, a = 1.0 / 58.75;
    for (int n = 0; n < 256; n++) {
        /* triangle, slope 1/58.75, parabolic caps for |n - 64| < 10.5
         * (the stock table 0x20017048 within 5e-6) */
        int k = n & 127;
        double s = n < 128 ? 1.0 : -1.0;
        if (k > 64) k = 128 - k;
        double v = k <= 53 ? a * k : 1.0 - (a / 21.0) * (64 - k) * (64 - k);
        tab_tri[n] = (float)(s * v);
        tab_sin[n] = (float)dsin(pi2 * n / 256.0);
    }
    /* rotary bank (stock Q15 table 0x2000ebdc): B = 573.3 Hz, F = 601 + 1.035 n + 0.035 n^2 */
    double c = ap_c(573.3, fs);
    rot_b0 = q(-c, 32768.0);
    for (int n = 0; n < 256; n++)
        rot_b1[n] = q(-(1 - c) * dcos(pi2 * (601.0 + 1.035 * n + 0.035 * n * n) / fs), 32768.0);
    /* step phaser bank (stock Q24 table 0x20010068): B = 88.2 (n + 11), F = 200 + 5.15 n + 0.15 n^2 */
    for (int n = 0; n < 128; n++) {
        c = ap_c(88.2 * (n + 11), fs);
        step_b0[n] = q(-c, 16777216.0);
        step_b1[n] = q(-(1 - c) * dcos(pi2 * (200.0 + 5.15 * n + 0.15 * n * n) / fs), 16777216.0);
    }
    tables_fs = fs;
}

/* stock 0xb4e4: linear-interpolated wavetable read, phase 0..1 */
static float lfo(const float *tab, float ph)
{
    if (ph > 1.0f) ph = 1.0f;
    if (ph < 0.0f) ph = 0.0f;
    float p = ph * 255.0f;
    int i = (int)p;
    float v0 = tab[i], v1 = i >= 255 ? tab[0] : tab[i + 1];
    float fr = p - (float)i;
    return v0 * (1.0f - fr) + v1 * fr;
}

/* radian phase, stock style: ph += 2 rate * 3.1415926 / fs (double), wrap at 2 pi */
static float phase_rad(float ph, double inc2, float fs)
{
    ph = (float)(inc2 * 3.1415926 / (double)fs + (double)ph);
    if (ph > 6.28318548f)                           /* 0x40c90fda */
        ph = (float)((double)ph - 6.2831852);
    return ph;
}

/* ring read: tap at w + int(d) + off (w already advanced), linear
 * interpolation towards the next slot */
static float ring_tap(const float *buf, int n, int w, float d, int off)
{
    int di = (int)d;
    float fr = d - (float)di, ifr = 1.0f - fr;
    int r = w + di + off;
    if (r >= n) r -= n;
    float a = buf[r], b = buf[r + 1 < n ? r + 1 : r + 1 - n];
    return a * ifr + b * fr;
}

static void ring_put(float *buf, short *w, int n, float x)
{
    buf[*w] = x;
    if (++*w >= n) *w -= n;
}

/* direct form I biquad, y = b0 x + b1 x1 + b2 x2 + a1 y1 + a2 y2 (stock sign) */
static float sos(mod_sos_t *s, float x, float b0, float b1, float b2, float a1, float a2)
{
    float y = x * b0 + s->x1 * b1 + s->x2 * b2 + s->y1 * a1 + s->y2 * a2;
    s->x2 = s->x1; s->x1 = x; s->y2 = s->y1; s->y1 = y;
    return y;
}

/* chorus output low-pass (double, both choruses) */
static float chorus_lp(mod_sos_t *s, float u)
{
    float y = (float)((double)u * 0.695168 - (double)s->x1 * 1.301535 + (double)s->x2 * 0.61597
                      + (double)s->y1 * 1.859336 - (double)s->y2 * 0.873054);
    s->x2 = s->x1; s->x1 = u; s->y2 = s->y1; s->y1 = y;
    return y;
}

/* chorus depth: steps 0.0002 per sample towards p4 */
static float depth_ramp(float d, float p4)
{
    if (d > p4) d = (float)((double)d - 0.0002);
    if (d < p4) d = (float)((double)d + 0.0002);
    return d;
}

static int scale(const mod_t *m, int n) { return (int)((float)n * m->r + 0.5f); }

void mod_init(mod_t *m, float fs)
{
    memset(m, 0, sizeof *m);
    if (fs > MOD_FS_MAX) fs = MOD_FS_MAX;
    if (tables_fs != fs) tables_init(fs);
    m->fs = fs;
    m->r = fs / 44100.0f;
    m->dt = 2.267573696145125e-05 * (44100.0 / (double)fs);
    m->n[0] = scale(m, MOD_FL_N);
    m->n[1] = scale(m, MOD_VIB_N);
    m->n[2] = scale(m, MOD_MC2_N);
    m->n[3] = scale(m, MOD_MC3_N);
    float t = (float)dtan(8000.0 * 3.1415926 * m->dt);        /* phaser pole, 0xb890 */
    m->ph_c = (t - 1.0f) / (t + 1.0f);
    dsp_knob_init(&m->mix, 0.01f, 0.99f);
    m->type = MOD_TYPES;
}

COLD void mod_set_params(mod_t *m, unsigned type, unsigned p1, unsigned p2, unsigned p3,
                    unsigned p4)
{
    if (type >= MOD_TYPES) type = 0;
    if (type != m->type) {                 /* the stock commit zeroes every MOD state */
        memset(&m->s, 0, sizeof m->s);
        m->type = type;
    }
    m->p1 = (float)p1 * 0.01f;
    m->p3 = (float)p3 * 0.01f;
    m->p4 = (float)p4 * 0.01f;
    dsp_knob_set(&m->mix, (float)p2 * 0.01f);
    /* filter band-pass pole: bandwidth 10 + 190 p3 Hz (0xbdb0) */
    float bw = 10.0f + m->p3 * 190.0f;
    float t = (float)dtan((double)bw * 3.1415926 * m->dt);
    m->filt_c = (t - 1.0f) / (t + 1.0f);
}

/* 0xb890 Phaser: 3 allpass sections, pole c from 8 kHz, centre
 * fc = 1201 + 3000 p3 (tri + 1); tri LFO 0.1164 + 13.332 (0.7 p1)^2 Hz;
 * feedback 0.6 sqrt(p2); out (1 - sqrt p2) x + 1.3 sqrt(p2) ap */
static float phaser(mod_t *m, float x, float p2)
{
    mod_ph_t *s = &m->s.ph;
    float sq;
    arm_sqrt_f32(p2, &sq);
    float a = (float)((double)m->p1 * 0.7);
    float rate = (float)((double)(a * a * 6.666f * 2.0f) + 0.1164);
    s->ph = (float)((double)s->ph + (double)rate * m->dt);
    if (s->ph > 1.0f) s->ph = s->ph - 1.0f;
    float fc = 1200.0f + (lfo(tab_tri, s->ph) + 1.0f) * 3000.0f * m->p3 * 1.0f;
    fc = fc + 1.0f;
    float w = (float)((double)(fc * 2.0f) * 3.1415926 * m->dt);
    float cs = (float)dcos((double)w);
    float c = m->ph_c, b1 = (1.0f - c) * -cs;
    float in = (float)((double)x + (double)s->fb * ((double)sq * 0.6));
    float y = sos(&s->s[0], in, -c, b1, 1.0f, -b1, c);
    y = sos(&s->s[1], y, -c, b1, 1.0f, -b1, c);
    y = sos(&s->s[2], y, -c, b1, 1.0f, -b1, c);
    s->fb = y;
    return (float)((double)((1.0f - sq) * x) + (double)y * 1.3 * (double)sq);
}

/* 0x104f0 Step phaser: tri LFO (0.681 + 13.7781 p1) Hz; each rising pass
 * through 63/127 samples a new step from the input bits (7 bits * p3); the
 * allpass bank index slews one step per 31 samples; band-pass on the output;
 * feedback (0.81 + 0.0008 (128 - c)) g, g = 0.3 (0.8 p2)^2 + 0.7 (0.8 p2) */
static float step_phaser(mod_t *m, float x, float p2)
{
    mod_step_t *s = &m->s.step;
    float rate = (float)(0.681 + (double)m->p1 * 13.7781);
    float g0 = (float)((double)p2 * 0.8);
    float g = (float)((double)(g0 * g0) * 0.3 + (double)g0 * 0.7);
    int k = (short)(int)(m->p3 * 63.0f);
    s->ph = rate / m->fs + s->ph;
    if (s->ph >= 1.0f) s->ph = s->ph - 1.0f;
    float l = lfo(tab_tri, s->ph) * 127.0f;
    if (l >= 63.0f && !(s->prev > 63.0f)) {
        unsigned bits = ((unsigned)(int)(x * 8388607.0f) >> 2) & 127u;
        s->tgt = (short)(int)((float)(int)bits * m->p3);
    }
    s->prev = l;
    if (++s->cnt > (short)(30.0f * m->r + 0.5f)) {
        s->cnt = 0;
        if (s->cur > s->tgt) s->cur--;
        if (s->cur < s->tgt) s->cur++;
    }
    float cpos = (float)((double)(63 - k) + (double)(l * (float)k) * 0.015873015873016);
    double fbk = (0.81 + (128.0 - (double)cpos) * 0.0008) * (double)g;
    float in = (float)((double)x + fbk * (double)s->fb);
    int i = s->cur < 0 ? 0 : s->cur > 127 ? 127 : s->cur;
    float b0 = step_b0[i], b1 = step_b1[i];
    float y = sos(&s->s[0], in, b0, b1, 1.0f, -b1, -b0);
    y = sos(&s->s[1], y, b0, b1, 1.0f, -b1, -b0);
    y = sos(&s->s[2], y, b0, b1, 1.0f, -b1, -b0);
    s->fb = y;
    mod_sos_t *bp = &s->bp;
    float v = (float)((double)y * 0.346745 - (double)bp->x2 * 0.346745 + (double)bp->y1 * 1.291392
                      - (double)bp->y2 * 0.30651);
    bp->x2 = bp->x1; bp->x1 = y; bp->y2 = bp->y1; bp->y1 = v;
    return (float)((double)((1.0f - g) * x) + (double)(v * g) * 1.3);
}

/* 0x9b38 Flanger: 500-slot ring, delay 135 + 32.5 sin (lag 32-97 samples),
 * LFO 6 (0.1002 + 1.9714 * 0.7 p1) rad/s, negative feedback 0.85 + 0.1091 p3;
 * wet 10 p2 v on an input scaled by 0.05. Stock dry is x (1 - x). */
static float flanger(mod_t *m, float x, float p2)
{
    mod_fl_t *s = &m->s.fl;
    float a = (float)((double)m->p1 * 0.7);
    float rate;
    if (a > 0.7f)                                   /* never taken: p1 <= 1 */
        rate = (float)((double)(a * a) * 77.5 - (double)a * 66.426 + 10.0);
    else
        rate = (float)(0.1002 + (double)a * 1.9714);
    float fb = (float)(0.85 + (double)m->p3 * 0.1091);
    s->ph = phase_rad(s->ph, (double)(rate * 6.0f), m->fs);
    float l = lfo(tab_sin, (float)((double)s->ph * 0.159154943091895));
    float d = (float)(135.0 + (double)l * 25.0 * 1.3) * m->r;
    ring_put(s->buf, &s->w, m->n[0],
             (float)((double)(-s->fb) + (double)x * 0.25 * 0.20000000298023224));
    float v = ring_tap(s->buf, m->n[0], s->w, d, scale(m, 300));
    s->fb = v * fb;
    float dry = m->fix_flanger_dry ? 1.0f - p2 : 1.0f - x;
    return v * p2 * 10.0f + x * dry;
}

/* 0x91d0 Jet flanger: as the flanger with delay 135 + 48 sin (lag 16-112),
 * LFO 2 (0.1 + 4 p1^2) rad/s, positive feedback 0.7022 + 0.2789 p3,
 * wet 28.8 p2^2 v */
static float jet(mod_t *m, float x, float p2)
{
    mod_fl_t *s = &m->s.fl;
    float rate = (float)((double)(m->p1 * m->p1 * 4.0f) + 0.1);
    float fb = (float)(0.7022 + (double)m->p3 * 0.2789);
    float p2sq = p2 * p2;
    s->ph = phase_rad(s->ph, (double)(rate * 2.0f), m->fs);
    float l = lfo(tab_sin, (float)((double)s->ph * 0.159154943091895));
    float d = (float)(135.0 + (double)l * 32.0 * 1.5) * m->r;
    ring_put(s->buf, &s->w, m->n[0], (float)((double)s->fb + (double)x * 0.25 * 0.2));
    float v = ring_tap(s->buf, m->n[0], s->w, d, scale(m, 300));
    s->fb = v * fb;
    float y = (float)((double)v * 1.2);
    return (float)((double)((1.0f - p2sq) * x) + (double)y * 2.4 * (double)p2sq * 10.0);
}

/* 0x10cd0 Tremolo: triangle LFO (0.2 + 17.143 p1) / 65536 cycles/sample
 * (0.13-11.7 Hz); wet = x low-passed with 0.1 + 0.9 p3^2, gain 2.925 * (tri+1)/2 */
static float tremolo(mod_t *m, float x, float p2)
{
    mod_trem_t *s = &m->s.trem;
    float p3sq = m->p3 * m->p3;
    float rate = (float)(0.2 + (double)m->p1 * 17.143);
    float d = (float)(0.1 + (double)p3sq * 0.9);
    float p2sq = p2 * p2;
    s->sm = x * d + s->sm * (1.0f - d);
    s->ph = (float)((double)s->ph + (double)rate * 0.5 * 3.0517578125e-05 * (44100.0 / m->fs));
    if (s->ph >= 1.0f) s->ph = s->ph - 1.0f;
    float w = (float)((double)s->sm * 1.3);
    float l = (lfo(tab_tri, s->ph) + 1.0f) * 0.5f;
    float g = (float)((double)w * 1.5 * (double)l);
    return (float)((double)((1.0f - p2sq) * x) + (double)g * 1.5 * (double)p2sq);
}

/* 0x10928 Stutter tremolo: sine (double) updated every 8 samples at
 * (4.25 + 127.1 p1) / 8 Hz; > 0.5 gates on (1/3 duty), gain slews 1/256 per
 * sample; wet low-passed with 0.03 + 0.87 p3^2 */
static float stutter(mod_t *m, float x, float p2)
{
    mod_stut_t *s = &m->s.stut;
    float rate = (float)(4.25 + (double)m->p1 * 127.1);
    float p3sq = m->p3 * m->p3;
    float c = (float)(0.03 + (double)p3sq * 0.87);
    s->sm = x * c + s->sm * (1.0f - c);
    if (++s->cnt > 7) {
        s->ph = phase_rad(s->ph, (double)(rate * 2.0f), m->fs);
        s->cnt = 0;
    }
    float t = (float)(dsin((double)s->ph) * 32768.0) > 16384.0f ? 256.0f : 0.0f;
    if ((float)s->cur < t) s->cur++;
    if ((float)s->cur > t) s->cur--;
    float g = (float)s->cur * 0.00390625f;
    float wet = (float)((double)s->sm * 1.9 * (double)g) * p2;
    return (float)((double)((1.0f - p2) * x) + (double)wet * 1.5);
}

/* 0x156d0 Vibrato: 700-slot ring, delay (80 + 305 (sin + 1)) p2^2, sine LFO
 * (7.7223 a^4 + 13.7215 a^3 - 16.2925 a^2 + 7.8802 a + 0.1) rad/s * 2,
 * a = 0.7 p1; wet only, low-passed with 0.1 + 0.9 p3 */
static float vibrato(mod_t *m, float x, float p2)
{
    mod_vib_t *s = &m->s.vib;
    ring_put(s->buf, &s->w, m->n[1], x);
    float a = (float)((double)m->p1 * 0.7);
    float a2 = a * a;
    double da2 = a2, da = a;
    float c = (float)(0.1 + (double)m->p3 * 0.9);
    float p2sq = p2 * p2;
    float rate = (float)(da2 * 7.7223 * da2 + da2 * 13.7215 * da + da2 * -16.2925 + da * 7.8802 + 0.1);
    s->ph = phase_rad(s->ph, (double)(rate * 2.0f), m->fs);
    float d = (float)(80.0 + (dsin((double)s->ph) + 1.0) * 305.0) * p2sq * m->r;
    float v = ring_tap(s->buf, m->n[1], s->w, d, 0);
    s->sm = v * c + s->sm * (1.0f - c);
    return s->sm;
}

/* 0xee48 Rotary: 3 allpasses from a 256-entry bank (603-3138 Hz) indexed by a
 * sine LFO (0.681 + 38.5 p1) / 4 Hz, summed with the dry signal (notches);
 * low-pass p3 + 0.1 / 0.9 - p3; out (1 - p2) x + 1.2 p2 wet */
static float rotary(mod_t *m, float x, float p2)
{
    mod_rot_t *s = &m->s.rot;
    float rate = (float)((0.681 + (double)m->p1 * 38.5) * 0.25);
    s->ph = rate / m->fs + s->ph;
    if (s->ph > 1.0f) s->ph = s->ph - 1.0f;
    float l = (lfo(tab_sin, s->ph) + 1.0f) * 0.5f;
    int i = (short)(int)(l * 255.0f);
    float b0 = rot_b0, b1 = rot_b1[i];
    float y = sos(&s->s[0], x, b0, b1, 1.0f, -b1, -b0);
    y = sos(&s->s[1], y, b0, b1, 1.0f, -b1, -b0);
    y = sos(&s->s[2], y, b0, b1, 1.0f, -b1, -b0);
    float u = y + x;
    s->sm = (float)(((double)m->p3 + 0.1) * (double)u + (double)s->sm * (0.9 - (double)m->p3));
    return (float)((double)((1.0f - p2) * x) + (double)s->sm * 1.2 * (double)p2);
}

/* 0x6208 Analog chorus: 700-slot ring, delay (450 + 100 (tri + 1)) depth
 * (lag 8.5-10.8 ms at p4 = 50), tri LFO (0.6 + 4 p1^2) rad/s * 2; depth ramps
 * to p4; wet = x 1.2 + v, * 0.65, low-pass (double biquad), * 1.7, one-pole
 * 0.1 + 0.9 p3; out (1 - p2) x + 1.5 p2 wet */
static float achorus(mod_t *m, float x, float p2)
{
    mod_ach_t *s = &m->s.ach;
    s->depth = depth_ramp(s->depth, m->p4);
    float c = (float)(0.1 + (double)m->p3 * 0.9);
    ring_put(s->buf, &s->w, m->n[1], x);
    float rate = (float)((double)(m->p1 * m->p1 * 4.0f) + 0.6);
    s->ph = phase_rad(s->ph, (double)(rate * 2.0f), m->fs);
    float l = lfo(tab_tri, (float)((double)s->ph * 0.159154943091895));
    float d = (float)(450.0 + (double)(l + 1.0f) * 100.0) * s->depth * m->r;
    float v = ring_tap(s->buf, m->n[1], s->w, d, 0);
    float u = (float)((double)v + (double)x * 1.2);
    u = (float)((double)u * 0.65);
    float y = chorus_lp(&s->lp, u);
    s->sm = (float)((double)y * 1.7) * c + s->sm * (1.0f - c);
    return (float)((double)((1.0f - p2) * x) + (double)(s->sm * p2) * 1.5);
}

/* 0x10e20 Multi chorus: 3 voices (rings 700/600/550, delays 440 + 120 (tri + 1),
 * 370 + 96 (..), 410 + 65 (..), * depth), LFO rates f, 0.95 f, 1.06 f with
 * f = 0.6 + 5 p1 rad/s * 2; sum (v1 + 0.75 v2 + 0.8 v3 + 1.3 x) 0.7 is mixed
 * with p2, one-pole 0.1 + 0.9 p3, low-pass (double biquad), * 1.15 */
static float mchorus(mod_t *m, float x, float p2)
{
    mod_mch_t *s = &m->s.mch;
    static const double base[3] = {440.0, 370.0, 410.0}, span[3] = {120.0, 96.0, 65.0};
    float *buf[3] = {s->b1, s->b2, s->b3};
    s->depth = depth_ramp(s->depth, m->p4);
    float c = (float)(0.1 + (double)m->p3 * 0.9);
    float rate = (float)((double)(m->p1 * 5.0f) + 0.6);
    float v[3];
    for (int k = 0; k < 3; k++) {
        int n = m->n[1 + k];
        ring_put(buf[k], &s->w[k], n, x);
        double inc2 = k == 0 ? (double)(rate * 2.0f)
                    : k == 1 ? (double)rate * 0.95 * 2.0 : (double)rate * 1.06 * 2.0;
        s->ph[k] = phase_rad(s->ph[k], inc2, m->fs);
        float l = lfo(tab_tri, (float)((double)s->ph[k] * 0.159154943091895));
        float d = (float)(base[k] + (double)(l + 1.0f) * span[k]) * s->depth * m->r;
        v[k] = ring_tap(buf[k], n, s->w[k], d, 0);
    }
    float u = (float)(((double)v[0] + (double)v[1] * 0.75 + (double)v[2] * 0.8 + (double)x * 1.3) * 0.7);
    u = u * p2 + x * (1.0f - p2);
    s->sm = u * c + s->sm * (1.0f - c);
    float y = chorus_lp(&s->lp, s->sm);
    return (float)((double)y * 1.15);
}

/* 0xed38 Ring modulator: sine carrier 0.000742 + 0.02333 p1 cycles/sample
 * (33-1062 Hz), wet low-passed with 0.1 + 0.9 p3 */
static float ringmod(mod_t *m, float x, float p2)
{
    mod_trem_t *s = &m->s.trem;
    float inc = (float)((double)m->p1 * 0.02333);
    s->ph = (float)(((double)inc + 0.0007423687423687) * (44100.0 / m->fs) + (double)s->ph);
    if (s->ph > 1.0f) s->ph = s->ph - 1.0f;
    float w = x * lfo(tab_sin, s->ph) * 1.5f;
    double a = 0.1 + (double)m->p3 * 0.9, b = 0.9 - (double)m->p3 * 0.9;
    s->sm = (float)(a * (double)w + (double)s->sm * b);
    return s->sm * p2 + x * (1.0f - p2);
}

/* 0xbdb0 Filter (auto-wah): band-pass (pole from bandwidth 10 + 190 p3 Hz)
 * swept 400-2000 Hz by a tri LFO (9.3065 p1^3 - 1.3809 p1^2 + 1.4835 p1 + 0.1164 Hz);
 * input * 2.7; out 1.15 ((1 - p2) x + 1.5 p2 y) */
static float filter(mod_t *m, float x, float p2)
{
    mod_filt_t *s = &m->s.filt;
    float p1sq = m->p1 * m->p1, p1cu = p1sq * m->p1;
    float rate = (float)((double)p1cu * 9.3065 - (double)p1sq * 1.3809 + (double)m->p1 * 1.4835 + 0.1164);
    s->ph = (float)((double)s->ph + (double)rate * m->dt);
    if (s->ph > 1.0f) s->ph = s->ph - 1.0f;
    float fc = 400.0f + (lfo(tab_tri, s->ph) + 1.0f) * 800.0f;
    float c = m->filt_c;
    float w = (float)((double)(fc * 2.0f) * 3.1415926 * m->dt);
    float cs = (float)dcos((double)w);
    float in = (float)((double)x * 2.7);
    float b0 = (c + 1.0f) * 0.5f, a1 = -(cs * (1.0f - c)), a2 = -c;
    float y = in * b0 + s->s.x1 * 0.0f + s->s.x2 * -b0 - s->s.y1 * a1 - s->s.y2 * a2;
    s->s.x2 = s->s.x1; s->s.x1 = in; s->s.y2 = s->s.y1; s->s.y1 = y;
    float o = (float)((double)((1.0f - p2) * x) + (double)y * 1.5 * (double)p2);
    return (float)((double)o * 1.15);
}

void mod_process(mod_t *m, float *x, unsigned n)
{
    static float (*const fx[MOD_TYPES])(mod_t *, float, float) = {
        phaser, step_phaser, flanger, jet, tremolo, stutter,
        vibrato, rotary, achorus, mchorus, ringmod, filter,
    };
    if (m->type >= MOD_TYPES) return;
    for (unsigned i = 0; i < n; i++) x[i] = fx[m->type](m, x[i], dsp_knob_next(&m->mix));
}
