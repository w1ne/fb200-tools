/* Bass EQ: see eq.h. */
#include <string.h>
#include "eq.h"

#define PI_D 3.14159265358979323846
#define TINY 1e-20f                     /* filter states below this are flushed */

/* No libm on the target: double sine and 2^x for the designs (main loop). */
static double sin_d(double x)            /* 0 <= x <= pi */
{
    if (x > 0.5 * PI_D) x = PI_D - x;
    double x2 = x * x, t = x, s = x;
    for (int k = 1; k <= 10; k++) {      /* Taylor to x^21: < 1e-15 on [0, pi/2] */
        t *= -x2 / (double)((2 * k) * (2 * k + 1));
        s += t;
    }
    return s;
}

static double exp2_d(double x)
{
    int e = (int)x;
    double f = x - (double)e;
    if (f < 0.0) { f += 1.0; e -= 1; }
    double y = f * 0.69314718055994531, t = 1.0, s = 1.0;
    for (int k = 1; k <= 14; k++) { t *= y / (double)k; s += t; }
    for (; e > 0; e--) s *= 2.0;
    for (; e < 0; e++) s *= 0.5;
    return s;
}

/* RBJ Audio-EQ-Cookbook. cos w = 1 - 2 sin^2(w/2): exact at low w. */
void eq_design(int type, double fs, double f0, double q, double gain_db, double out[5])
{
    double w = 2.0 * PI_D * f0 / fs;
    double sh = sin_d(0.5 * w);
    double cw = 1.0 - 2.0 * sh * sh;
    double alpha = sin_d(w) / (2.0 * q);
    double b0, b1, b2, a0, a1 = -2.0 * cw, a2;
    if (type == 0) {                     /* high-pass */
        b0 = 0.5 * (1.0 + cw); b1 = -(1.0 + cw); b2 = b0;
        a0 = 1.0 + alpha; a2 = 1.0 - alpha;
    } else if (type == 2) {              /* low-pass */
        b0 = 0.5 * (1.0 - cw); b1 = 1.0 - cw; b2 = b0;
        a0 = 1.0 + alpha; a2 = 1.0 - alpha;
    } else {                             /* peaking, A = 10^(dB/40) */
        double A = exp2_d(gain_db / 40.0 * 3.32192809488736235);
        b0 = 1.0 + alpha * A; b1 = a1; b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A; a2 = 1.0 - alpha / A;
    }
    out[0] = b0 / a0; out[1] = b1 / a0; out[2] = b2 / a0;
    out[3] = a1 / a0; out[4] = a2 / a0;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void set_identity(float *c)
{
    c[0] = 1.0f; c[1] = c[2] = c[3] = c[4] = 0.0f;
}

static void target(float *t, int type, float fs, float f0, float q, float gain_db)
{
    double d[5];
    eq_design(type, fs, f0, q, gain_db, d);
    /* CMSIS: y = b0 x + b1 x1 + b2 x2 + a1 y1 + a2 y2 (a negated) */
    t[0] = (float)d[0]; t[1] = (float)d[1]; t[2] = (float)d[2];
    t[3] = (float)-d[3]; t[4] = (float)-d[4];
}

/* All targets from the settings; start a ramp if any changed. */
static void update(eq_t *e)
{
    float t[5 * EQ_STAGES];
    for (unsigned s = 0; s < EQ_STAGES; s++) set_identity(&t[5 * s]);
    if (e->on) {
        if (e->hpf > 0.0f) target(&t[0], 0, e->fs, e->hpf, 0.70710678f, 0.0f);
        for (unsigned b = 0; b < EQ_BANDS; b++)
            if (e->g[b] != 0.0f) target(&t[5 * (b + 1)], 1, e->fs, e->f[b], e->q[b], e->g[b]);
        if (e->lpf > 0.0f) target(&t[5 * (EQ_STAGES - 1)], 2, e->fs, e->lpf, 0.70710678f, 0.0f);
    }
    if (memcmp(t, e->t, sizeof t) != 0) {
        memcpy(e->t, t, sizeof t);
        e->ramp = EQ_RAMP_BLOCKS;
        e->bypass = 0;
    }
}

void eq_reset(eq_t *e)
{
    memset(e->st, 0, sizeof e->st);
}

void eq_init(eq_t *e, float fs)
{
    static const float def_f[EQ_BANDS] = {40.0f, 100.0f, 250.0f, 800.0f, 3000.0f};
    memset(e, 0, sizeof *e);
    e->fs = fs;
    for (unsigned b = 0; b < EQ_BANDS; b++) { e->f[b] = def_f[b]; e->q[b] = EQ_Q_DEF; }
    for (unsigned s = 0; s < EQ_STAGES; s++) { set_identity(&e->c[5 * s]); set_identity(&e->t[5 * s]); }
    e->bypass = 1;
    arm_biquad_cascade_df2T_init_f32(&e->inst, EQ_STAGES, e->c, e->st);
}

void eq_set_on(eq_t *e, int on)
{
    e->on = on != 0;
    update(e);
}

void eq_set_hpf(eq_t *e, float hz)
{
    e->hpf = hz > 0.0f ? clampf(hz, EQ_HPF_MIN, EQ_HPF_MAX) : 0.0f;
    update(e);
}

void eq_set_lpf(eq_t *e, float hz)
{
    e->lpf = hz > 0.0f ? clampf(hz, EQ_LPF_MIN, EQ_LPF_MAX) : 0.0f;
    update(e);
}

int eq_set_band(eq_t *e, unsigned band, float hz, float gain_db, float q)
{
    if (band >= EQ_BANDS) return -1;
    e->f[band] = clampf(hz, EQ_BAND_MIN, EQ_BAND_MAX);
    e->g[band] = clampf(gain_db, -EQ_GAIN_MAX, EQ_GAIN_MAX);
    e->q[band] = clampf(q, EQ_Q_MIN, EQ_Q_MAX);
    update(e);
    return 0;
}

void eq_process(eq_t *e, float *x, unsigned n)
{
    if (e->bypass) return;                      /* flat and settled: bit-exact */
    int settled = 0;
    if (e->ramp) {
        /* one straight-line step: after `ramp` more steps c == t */
        if (--e->ramp == 0) {
            memcpy(e->c, e->t, sizeof e->c);
            settled = 1;
        } else {
            const float k = 1.0f / (float)(e->ramp + 1u);
            for (unsigned i = 0; i < 5 * EQ_STAGES; i++) e->c[i] += k * (e->t[i] - e->c[i]);
        }
    }
    arm_biquad_cascade_df2T_f32(&e->inst, x, x, n);
    for (unsigned i = 0; i < 2 * EQ_STAGES; i++)   /* decaying states go subnormal on silence */
        if (e->st[i] < TINY && e->st[i] > -TINY) e->st[i] = 0.0f;
    if (settled) {
        /* bypass from the next block when every stage is the identity */
        int id = 1;
        for (unsigned s = 0; s < EQ_STAGES && id; s++) {
            const float *c = &e->c[5 * s];
            id = c[0] == 1.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f && c[4] == 0.0f;
        }
        if (id) { e->bypass = 1; eq_reset(e); }
    }
}
