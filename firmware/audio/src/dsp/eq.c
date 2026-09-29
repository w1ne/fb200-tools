/* Bass EQ: see eq.h. */
#include <string.h>
#include "cold.h"
#include "eq.h"

#define PI_D 3.14159265358979323846
#define LOG2_Q_BW (-0.5f)                /* log2(1/sqrt(2)): the HPF/LPF Q */
#define TINY 1e-20f                     /* filter states below this are flushed */
#define QUIET 1e-5f                     /* neutral + |out - in| below this: bypass */

/* No libm on the target: double sine, 2^x, log2 for the designs. Horner with
 * constant coefficients: no divisions (they are slow in double). */
static double sin_d(double x)            /* 0 <= x <= pi */
{
    if (x > 0.5 * PI_D) x = PI_D - x;
    static const double k[10] = {         /* (-1)^n / (2n+1)!, n = 1..10 */
        -1.0 / 6, 1.0 / 120, -1.0 / 5040, 1.0 / 362880, -1.0 / 39916800,
        1.0 / 6227020800.0, -1.0 / 1307674368000.0, 1.0 / 355687428096000.0,
        -1.0 / 121645100408832000.0, 1.0 / 51090942171709440000.0};
    double x2 = x * x, s = k[9];
    for (int i = 8; i >= 0; i--) s = k[i] + x2 * s;
    return x * (1.0 + x2 * s);           /* < 1e-15 on [0, pi/2] */
}

static double exp2_d(double x)
{
    int e = (int)x;
    double f = x - (double)e;
    if (f < 0.0) { f += 1.0; e -= 1; }
    static const double k[13] = {         /* ln2^n / n!, n = 1..13 */
        6.9314718055994531e-01, 2.4022650695910071e-01, 5.5504108664821580e-02,
        9.6181291076284772e-03, 1.3333558146428443e-03, 1.5403530393381609e-04,
        1.5252733804059840e-05, 1.3215486790144307e-06, 1.0178086009239699e-07,
        7.0549116208011233e-09, 4.4455382718708114e-10, 2.5678435993488202e-11,
        1.3691488853904124e-12};
    double s = k[12];
    for (int i = 11; i >= 0; i--) s = k[i] + f * s;
    s = 1.0 + f * s;
    for (; e > 0; e--) s *= 2.0;
    for (; e < 0; e++) s *= 0.5;
    return s;
}

static double log2_d(double x)           /* x > 0; main loop only */
{
    int e = 0;
    while (x >= 2.0) { x *= 0.5; e++; }
    while (x < 1.0) { x *= 2.0; e--; }
    double z = (x - 1.0) / (x + 1.0), z2 = z * z, t = z, s = 0.0;   /* z <= 1/3 */
    for (int n = 0; n < 16; n++) { s += t / (double)(2 * n + 1); t *= z2; }
    return (double)e + 2.0 * s * 1.4426950408889634;
}

/* RBJ Audio-EQ-Cookbook. cos w = 1 - 2 sin^2(w/2): exact at low w. */
void eq_design(int type, double fs, double f0, double q, double gain_db, double out[5])
{
    double w = 2.0 * PI_D * f0 / fs;
    if (w > 0.999 * PI_D) w = 0.999 * PI_D;
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
        double A = exp2_d(gain_db * (3.3219280948873623 / 40.0));
        double aA = alpha * A, a_A = alpha / A;
        b0 = 1.0 + aA; b1 = a1; b2 = 1.0 - aA;
        a0 = 1.0 + a_A; a2 = 1.0 - a_A;
    }
    double r = 1.0 / a0;
    out[0] = b0 * r; out[1] = b1 * r; out[2] = b2 * r;
    out[3] = a1 * r; out[4] = a2 * r;
    if (type == 1 && gain_db == 0.0) {   /* 0 dB: exactly b = a */
        out[0] = 1.0; out[1] = out[3]; out[2] = out[4];
    }
}

static int stage_type(unsigned s) { return s == 0 ? 0 : s == EQ_STAGES - 1 ? 2 : 1; }

/* The stage's current parameters -> its coefficients (double, CMSIS signs). */
static void design_stage(eq_t *e, unsigned s)
{
    const eq_par_t *p = &e->cur[s];
    int type = stage_type(s);
    double d[5];
    eq_design(type, e->fs, exp2_d(p->lf), exp2_d(p->lq), type == 1 ? p->v : 0.0, d);
    if (type != 1) {                     /* b' = a + m (b - a): m = 0 is b' = a exactly */
        double m = p->v;
        d[0] = 1.0 + m * (d[0] - 1.0);
        d[1] = d[3] + m * (d[1] - d[3]);
        d[2] = d[4] + m * (d[2] - d[4]);
    }
    /* CMSIS: y = b0 x + b1 x1 + b2 x2 + a1 y1 + a2 y2 (a negated) */
    double *c = &e->c[5 * s];
    c[0] = d[0]; c[1] = d[1]; c[2] = d[2];
    c[3] = -d[3]; c[4] = -d[4];
}

static int neutral(const eq_par_t *p)
{
    return p->v == 0.0f;                 /* 0 dB or mix 0 */
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The settings -> each stage's target; start the glide of what changed. */
static void update(eq_t *e)
{
    for (unsigned s = 0; s < EQ_STAGES; s++) {
        eq_par_t t = e->tgt[s];
        if (s == 0 || s == EQ_STAGES - 1) {
            float hz = s == 0 ? e->hpf : e->lpf;
            if (hz > 0.0f) t.lf = (float)log2_d(hz);     /* off: keeps its frequency */
            t.v = e->on && hz > 0.0f ? 1.0f : 0.0f;
        } else {
            unsigned b = s - 1;
            t.lf = (float)log2_d(e->f[b]);
            t.lq = (float)log2_d(e->q[b]);
            t.v = e->on ? e->g[b] : 0.0f;
        }
        if (memcmp(&t, &e->tgt[s], sizeof t) == 0) continue;
        e->tgt[s] = t;
        if (neutral(&e->cur[s])) {    /* the shape does not matter at neutral: jump */
            e->cur[s].lf = t.lf;
            e->cur[s].lq = t.lq;
            if (neutral(&t)) {        /* neutral to neutral: nothing to glide */
                e->cur[s] = t;
                e->ramp[s] = 0;
                design_stage(e, s);
                continue;
            }
        }
        e->ramp[s] = EQ_RAMP_BLOCKS;
        e->active[s] = 1;
        e->bypass = 0;
    }
}

void eq_reset(eq_t *e)
{
    memset(e->st, 0, sizeof e->st);
}

static const float def_f[EQ_BANDS] = {40.0f, 100.0f, 250.0f, 800.0f, 3000.0f};

void eq_init(eq_t *e, float fs)
{
    memset(e, 0, sizeof *e);
    e->fs = fs;
    for (unsigned b = 0; b < EQ_BANDS; b++) { e->f[b] = def_f[b]; e->q[b] = EQ_Q_DEF; }
    for (unsigned s = 0; s < EQ_STAGES; s++) {
        eq_par_t p = {0.0f, LOG2_Q_BW, 0.0f};
        if (s == 0) p.lf = (float)log2_d(EQ_HPF_MIN);
        else if (s == EQ_STAGES - 1) p.lf = (float)log2_d(EQ_LPF_MAX);
        else { p.lf = (float)log2_d(def_f[s - 1]); p.lq = (float)log2_d(EQ_Q_DEF); }
        e->cur[s] = e->tgt[s] = p;
        design_stage(e, s);
    }
    e->bypass = 1;
}

static float cut(float hz, float lo, float hi) { return hz > 0.0f ? clampf(hz, lo, hi) : 0.0f; }

static void set_band(eq_t *e, unsigned band, float hz, float gain_db, float q)
{
    e->f[band] = clampf(hz, EQ_BAND_MIN, EQ_BAND_MAX);
    e->g[band] = clampf(gain_db, -EQ_GAIN_MAX, EQ_GAIN_MAX);
    e->q[band] = clampf(q, EQ_Q_MIN, EQ_Q_MAX);
}

void eq_set_on(eq_t *e, int on)
{
    e->on = on != 0;
    update(e);
}

void eq_set_hpf(eq_t *e, float hz)
{
    e->hpf = cut(hz, EQ_HPF_MIN, EQ_HPF_MAX);
    update(e);
}

void eq_set_lpf(eq_t *e, float hz)
{
    e->lpf = cut(hz, EQ_LPF_MIN, EQ_LPF_MAX);
    update(e);
}

int eq_set_band(eq_t *e, unsigned band, float hz, float gain_db, float q)
{
    if (band >= EQ_BANDS) return -1;
    set_band(e, band, hz, gain_db, q);
    update(e);
    return 0;
}

COLD void eq_load(eq_t *e, const uint8_t *r)
{
    static const uint8_t def[EQ_REC] = {0, 0, 0, 0, 40, 0, 0, 50, 100, 0, 0, 50,
                                        250, 0, 0, 50, 0x20, 3, 0, 50, 0xb8, 0x0b, 0, 50};
    if (!r) r = def;                    /* off, flat, the default bands (eq_init) */
    e->on = r[0] != 0;
    e->hpf = cut(r[1], EQ_HPF_MIN, EQ_HPF_MAX);
    e->lpf = cut((float)(r[2] | r[3] << 8), EQ_LPF_MIN, EQ_LPF_MAX);
    for (unsigned b = 0; b < EQ_BANDS; b++) {
        const uint8_t *s = &r[4 + 4 * b];
        set_band(e, b, (float)(s[0] | s[1] << 8), (float)(int8_t)s[2] / 8.0f, (float)s[3] / 50.0f);
    }
    update(e);
}

static unsigned rnd(float x) { return (unsigned)(int)(x + (x < 0.0f ? -0.5f : 0.5f)); }

void eq_save(const eq_t *e, uint8_t r[EQ_REC])
{
    unsigned lpf = rnd(e->lpf);
    r[0] = e->on;
    r[1] = (uint8_t)rnd(e->hpf);
    r[2] = (uint8_t)lpf; r[3] = (uint8_t)(lpf >> 8);
    for (unsigned b = 0; b < EQ_BANDS; b++) {
        uint8_t *s = r + 4 + 4 * b;
        unsigned f = rnd(e->f[b]);
        s[0] = (uint8_t)f; s[1] = (uint8_t)(f >> 8);
        s[2] = (uint8_t)rnd(e->g[b] * 8.0f);
        s[3] = (uint8_t)rnd(e->q[b] * 50.0f);
    }
}

/* One stage over a block, DF1 (state x1 x2 y1 y2), in double, with the
 * coefficients moving from c0 to c1 in n equal steps: no step between
 * blocks (the glide). c0 == NULL: settled, the coefficients c1. */
static void run_stage(double *st, const double *c0, const double *c1, float *x, unsigned n)
{
    double b0 = c1[0], b1 = c1[1], b2 = c1[2], a1 = c1[3], a2 = c1[4];
    double d0 = 0.0, d1 = 0.0, d2 = 0.0, d3 = 0.0, d4 = 0.0;
    if (c0) {
        const double r = 1.0 / (double)n;
        d0 = (c1[0] - c0[0]) * r; d1 = (c1[1] - c0[1]) * r; d2 = (c1[2] - c0[2]) * r;
        d3 = (c1[3] - c0[3]) * r; d4 = (c1[4] - c0[4]) * r;
        b0 = c0[0]; b1 = c0[1]; b2 = c0[2]; a1 = c0[3]; a2 = c0[4];
    }
    double x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
    for (unsigned i = 0; i < n; i++) {
        if (c0) { b0 += d0; b1 += d1; b2 += d2; a1 += d3; a2 += d4; }
        double in = (double)x[i];
        double y = (b0 * in) + (b1 * x1) + (b2 * x2) + (a1 * y1) + (a2 * y2);
        x2 = x1; x1 = in; y2 = y1; y1 = y;
        x[i] = (float)y;
    }
    st[0] = x1; st[1] = x2; st[2] = y1; st[3] = y2;
}

void eq_process(eq_t *e, float *x, unsigned n)
{
    if (n == 0) return;
    int any = 0;
    for (unsigned s = 0; s < EQ_STAGES; s++) {
        double *st = &e->st[4 * s], *c = &e->c[5 * s];
        if (e->ramp[s]) {                       /* gliding */
            double c0[5];
            memcpy(c0, c, sizeof c0);
            eq_par_t *p = &e->cur[s];
            const eq_par_t *t = &e->tgt[s];
            if (--e->ramp[s] == 0) {
                *p = *t;                        /* exactly the setting */
            } else {                            /* a straight line: `ramp` more steps */
                const float k = 1.0f / (float)(e->ramp[s] + 1u);
                p->lf += k * (t->lf - p->lf);
                p->lq += k * (t->lq - p->lq);
                p->v += k * (t->v - p->v);
            }
            design_stage(e, s);
            run_stage(st, c0, c, x, n);
        } else if (e->active[s]) {
            run_stage(st, NULL, c, x, n);
        } else {                                /* skipped: x untouched; keep the history */
            st[1] = st[3] = n >= 2 ? x[n - 2] : st[0];
            st[0] = st[2] = x[n - 1];
            continue;
        }
        for (unsigned i = 0; i < 4; i++)         /* decaying signals go subnormal */
            if (st[i] < TINY && st[i] > -TINY) st[i] = 0.0f;
        /* skip from the next block: neutral, settled, out = in (to QUIET) */
        double d1 = st[0] - st[2], d2 = st[1] - st[3];
        if (!e->ramp[s] && neutral(&e->cur[s]) && d1 < QUIET && d1 > -QUIET && d2 < QUIET && d2 > -QUIET)
            e->active[s] = 0;
        any |= e->active[s];
    }
    e->bypass = !any;
}
