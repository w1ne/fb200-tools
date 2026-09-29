/* Host tests for the bass EQ (src/dsp/eq.c). Built by tests/test_dsp_host.py
 * (test_eq_suite); every check is an assert.
 *
 * 1. Response: the measured response of each band, the HPF and the LPF (the
 *    DFT of the impulse response through eq_process) vs the RBJ cookbook
 *    target in double (written here again with libm, not eq_design), and vs
 *    the analog prototype where the bilinear transform keeps it (the centre
 *    gain, the -3 dB point).
 * 2. Flat = bit-exact: on + flat, off, and back to flat after a boost.
 * 3. No clicks: a sine through a parameter change (definition at max_step).
 * 4. Stable at the extremes, random changes mid-ramp, silence ends in zeros. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/eq.h"

#define IR_LEN 131072                   /* 3 s: the slowest pole (30 Hz, Q 4) is < -150 dB */

static eq_t e;
static float ir[IR_LEN];

static void run(float *x, unsigned n)
{
    for (unsigned o = 0; o < n; o += DSP_BLOCK)
        eq_process(&e, x + o, n - o < DSP_BLOCK ? n - o : DSP_BLOCK);
}

static void settle(void)
{
    float z[DSP_BLOCK] = {0};
    for (unsigned b = 0; b < EQ_RAMP_BLOCKS + 1; b++) eq_process(&e, z, DSP_BLOCK);
    for (unsigned s = 0; s < EQ_STAGES; s++) assert(e.ramp[s] == 0);
    eq_reset(&e);
}

/* RBJ target, double, the textbook formulas. type 0 HP, 1 peak, 2 LP. */
static double rbj_db(int type, double fs, double f0, double q, double g, double f)
{
    double w = 2 * M_PI * f0 / fs, c = cos(w), al = sin(w) / (2 * q), A = pow(10, g / 40);
    double b[3], a[3];
    if (type == 0) { b[0] = (1 + c) / 2; b[1] = -(1 + c); b[2] = b[0]; a[0] = 1 + al; a[1] = -2 * c; a[2] = 1 - al; }
    else if (type == 2) { b[0] = (1 - c) / 2; b[1] = 1 - c; b[2] = b[0]; a[0] = 1 + al; a[1] = -2 * c; a[2] = 1 - al; }
    else { b[0] = 1 + al * A; b[1] = -2 * c; b[2] = 1 - al * A; a[0] = 1 + al / A; a[1] = -2 * c; a[2] = 1 - al / A; }
    double wf = 2 * M_PI * f / fs;
    double nr = b[0] + b[1] * cos(wf) + b[2] * cos(2 * wf), ni = -b[1] * sin(wf) - b[2] * sin(2 * wf);
    double dr = a[0] + a[1] * cos(wf) + a[2] * cos(2 * wf), di = -a[1] * sin(wf) - a[2] * sin(2 * wf);
    return 10 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

static unsigned ir_n;                  /* the impulse response up to its last |h| > 1e-10 */

/* Measured response in dB at f: DFT of the impulse response (a rotating
 * phasor, renormalised every 4096 samples). */
static double meas_db(double fs, double f)
{
    double re = 0, im = 0, w = 2 * M_PI * f / fs, cr = cos(w), ci = -sin(w), pr = 1, pi = 0;
    for (unsigned n = 0; n < ir_n; n++) {
        re += ir[n] * pr; im += ir[n] * pi;
        double t = pr * cr - pi * ci; pi = pr * ci + pi * cr; pr = t;
        if ((n & 4095u) == 4095u) { pr = cos(w * (n + 1)); pi = -sin(w * (n + 1)); }
    }
    return 10 * log10(re * re + im * im);
}

static void impulse(void)
{
    memset(ir, 0, sizeof ir);
    ir[0] = 1.0f;
    run(ir, IR_LEN);
    ir_n = IR_LEN;
    while (ir_n > 1 && fabsf(ir[ir_n - 1]) < 1e-10f) ir_n--;
    assert(ir_n < IR_LEN - 4096);                    /* it did decay */
}

/* One stage alone. Returns the worst |measured - RBJ| in dB over 20 Hz..
 * 0.45 fs where the target is above -24 dB (below that a dB error means
 * little and the float noise floor shows). */
static double check_stage(float fs, int type, float f0, float q, float g, int verbose)
{
    eq_init(&e, fs);
    eq_set_on(&e, 1);
    if (type == 0) eq_set_hpf(&e, f0);
    else if (type == 2) eq_set_lpf(&e, f0);
    else eq_set_band(&e, 2, f0, g, q);
    settle();
    impulse();
    double worst = 0;
    for (double f = 20; f < 0.45 * fs; f *= 1.03) {
        double t = rbj_db(type, fs, f0, type == 1 ? q : M_SQRT1_2, g, f);
        if (t < -24) continue;
        double d = fabs(meas_db(fs, f) - t);
        if (d > worst) worst = d;
    }
    /* analog prototype: the bilinear transform with the RBJ prewarp keeps
     * the peak gain at f0 and the -3.01 dB point of the Butterworth HP/LP */
    double at = meas_db(fs, f0), want = type == 1 ? g : -3.0103;
    if (verbose)
        printf("  %s %.0f Hz q %.2f %+.0f dB @%.0f: worst %.4f dB vs RBJ, at f0 %.4f dB (analog %.4f)\n",
               type == 0 ? "hpf" : type == 2 ? "lpf" : "band", f0, q, g, fs, worst, at, want);
    assert(fabs(at - want) < 0.1);
    return worst;
}

static void test_response(float fs)
{
    printf("response @%.0f Hz:\n", fs);
    /* the DF1 recursion runs in double: < 0.0001 dB (in float, v0.9.1, the
     * rounding grew as f0/fs fell: 0.05 dB at 44.1 kHz, 0.15 dB at 48 kHz) */
    const double tol = 0.01;
    /* the defaults and the ends of every range */
    const float bands[] = {40, 100, 250, 800, 3000};
    for (unsigned i = 0; i < 5; i++) {
        assert(check_stage(fs, 1, bands[i], 1.0f, 15.0f, 1) < tol);
        assert(check_stage(fs, 1, bands[i], 1.0f, -15.0f, 1) < tol);
    }
    assert(check_stage(fs, 1, 10000, 4.0f, 15.0f, 1) < tol);
    assert(check_stage(fs, 1, 40, 4.0f, -15.0f, 1) < tol);
    assert(check_stage(fs, 1, 30, 0.3f, 15.0f, 1) < tol);
    for (float f = EQ_HPF_MIN; f <= EQ_HPF_MAX; f *= 1.5f) assert(check_stage(fs, 0, f, 0, 0, f == EQ_HPF_MIN) < tol);
    assert(check_stage(fs, 0, EQ_HPF_MAX, 0, 0, 1) < tol);
    for (float f = EQ_LPF_MIN; f <= EQ_LPF_MAX; f *= 1.5f) assert(check_stage(fs, 2, f, 0, 0, f == EQ_LPF_MIN) < tol);
    assert(check_stage(fs, 2, EQ_LPF_MAX, 0, 0, 1) < tol);

    /* grid over every band range: 30..10000 Hz, Q 0.3..4, +-15 dB */
    double worst_grid = 0;
    for (float f = EQ_BAND_MIN; f <= EQ_BAND_MAX; f *= 1.4f)
        for (float q = EQ_Q_MIN; q <= EQ_Q_MAX * 1.001f; q *= 1.93f)
            for (float g = -15; g <= 15; g += 7.5f) {
                if (g == 0) continue;
                double w = check_stage(fs, 1, f, q, g, 0);
                if (w > worst_grid) worst_grid = w;
            }
    printf("  grid (30..10000 Hz, Q 0.3..4, +-15 dB): worst %.4f dB vs RBJ\n", worst_grid);
    assert(worst_grid < 0.1);

    /* all seven at once = the sum of the seven in dB */
    eq_init(&e, fs);
    eq_set_on(&e, 1);
    eq_set_hpf(&e, 35);
    eq_set_lpf(&e, 8000);
    const float g5[5] = {4, -3, -6, 2.5f, 5}, q5[5] = {0.7f, 1.4f, 2, 1, 0.5f};
    for (unsigned b = 0; b < 5; b++) eq_set_band(&e, b, bands[b], g5[b], q5[b]);
    settle();
    impulse();
    double worst = 0;
    for (double f = 20; f < 0.45 * fs; f *= 1.05) {
        double t = rbj_db(0, fs, 35, M_SQRT1_2, 0, f) + rbj_db(2, fs, 8000, M_SQRT1_2, 0, f);
        for (unsigned b = 0; b < 5; b++) t += rbj_db(1, fs, bands[b], q5[b], g5[b], f);
        if (t < -24) continue;
        double d = fabs(meas_db(fs, f) - t);
        if (d > worst) worst = d;
    }
    printf("  all 7 stages: worst %.4f dB vs RBJ\n", worst);
    assert(worst < 0.1);
}

static float frand(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }

/* Run x until the EQ bypasses (back to neutral and the state died out);
 * returns the first sample that ran bypassed. */
static unsigned run_to_bypass(float *x, unsigned n)
{
    unsigned o = 0;
    for (; o < n && !e.bypass; o += DSP_BLOCK) eq_process(&e, x + o, DSP_BLOCK);
    assert(e.bypass);
    run(x + o, n - o);
    return o;
}

/* Flat = bit-exact: the output is the input, bit for bit. */
static void test_flat(void)
{
    enum { N = 44100 };
    static float x[N], y[N];
    for (unsigned i = 0; i < N; i++) x[i] = 0.5f * frand();
    eq_init(&e, 44100);                              /* off (the default) */
    memcpy(y, x, sizeof x);
    run(y, N);
    assert(memcmp(x, y, sizeof x) == 0);
    eq_set_on(&e, 1);                                /* on, flat */
    assert(e.bypass);
    memcpy(y, x, sizeof x);
    run(y, N);
    assert(memcmp(x, y, sizeof x) == 0);

    eq_set_band(&e, 0, 40, 6, 1);                    /* a boost, then back to 0 dB */
    eq_set_hpf(&e, 50);
    eq_set_lpf(&e, 5000);
    memcpy(y, x, sizeof x);
    run(y, N / 2);
    assert(memcmp(x, y, sizeof x / 2) != 0);
    eq_set_band(&e, 0, 40, 0, 1);
    eq_set_hpf(&e, 0);
    eq_set_lpf(&e, 0);
    unsigned from = N / 2 + run_to_bypass(y + N / 2, N / 2);
    assert(memcmp(x + from, y + from, (N - from) * sizeof x[0]) == 0);
    unsigned t1 = from - N / 2;

    eq_set_band(&e, 1, 100, -9, 2);                  /* a cut, then `eq off` */
    run(y, N / 2);
    eq_set_on(&e, 0);
    memcpy(y, x, sizeof x);
    from = run_to_bypass(y, N);
    assert(memcmp(x + from, y + from, (N - from) * sizeof x[0]) == 0);
    printf("flat: off, on + flat: bit-exact; back to flat after a boost: bypassed after %.1f ms, "
           "`eq off` after a cut: after %.1f ms, then bit-exact\n", t1 / 44.1, from / 44.1);
    assert(t1 < 44100 / 2 && from < 44100 / 2);
}

/* No clicks. Definition (X): a sine at f <= 400 Hz goes through a parameter
 * change; the output's content above 8 kHz (a 4th-order Butterworth
 * high-pass, double) must stay below X = -70 dB re the output peak. Every
 * frequency in these cases (the sine, the bands, the filter corners that
 * matter) is far below 8 kHz: a filter change alone puts almost nothing
 * there (the steady sine leaks < -80 dB). A click or zipper is broadband and
 * shows up there. Negative controls, or X would test nothing: `hard`
 * switches the coefficients at once and must fail X in some cases; a step
 * of 2.5e-3 (1 % of the input sine) added at the change must fail X in all. */
typedef struct { const char *name; float f; int what; float a[3], b[3]; } change_t;

static void apply(int what, const float *p)
{
    if (what == 0) eq_set_hpf(&e, p[0]);
    else if (what == 2) eq_set_lpf(&e, p[0]);
    else eq_set_band(&e, 1, p[0], p[1], p[2]);
}

/* The negative control: every stage jumps to its setting now (the design
 * of eq.c design_stage, written here with eq_design). */
static void hard_switch(void)
{
    for (unsigned s = 0; s < EQ_STAGES; s++) {
        const eq_par_t *p = &e.tgt[s];
        int type = s == 0 ? 0 : s == EQ_STAGES - 1 ? 2 : 1;
        double d[5];
        eq_design(type, e.fs, exp2(p->lf), exp2(p->lq), type == 1 ? p->v : 0.0, d);
        if (type != 1) {
            d[0] = 1.0 + p->v * (d[0] - 1.0);
            d[1] = d[3] + p->v * (d[1] - d[3]);
            d[2] = d[4] + p->v * (d[2] - d[4]);
        }
        double *c = &e.c[5 * s];
        c[0] = d[0]; c[1] = d[1]; c[2] = d[2];
        c[3] = -d[3]; c[4] = -d[4];
        e.cur[s] = *p;
        e.ramp[s] = 0;
    }
}

/* worst |HF| after the change / output peak, in dB */
static double click_db(const change_t *c, int hard, float step)
{
    const float fs = 44100;
    enum { N = 44100 };
    static float x[N];
    /* the change at a peak of the sine (a zero crossing hides a hard switch) */
    const float ph = 0.5f * (float)M_PI - 2 * (float)M_PI * c->f * (float)(N / 2) / fs;
    for (unsigned i = 0; i < N; i++) x[i] = 0.25f * sinf(2 * (float)M_PI * c->f * (float)i / fs + ph);
    eq_init(&e, fs);
    eq_set_on(&e, 1);
    apply(c->what, c->a);
    settle();
    run(x, N / 2);                                   /* steady before */
    apply(c->what, c->b);
    if (hard) hard_switch();
    run(x + N / 2, N / 2);
    for (unsigned i = N / 2; i < N; i++) x[i] += step;   /* the metric's own check */
    /* 4th-order Butterworth high-pass at 8 kHz: two RBJ sections */
    const double qs[2] = {0.54119610, 1.30656296};
    double z[2][2] = {{0}}, peak = 0, worst = 0;
    for (unsigned n = 0; n < N; n++) {
        double v = x[n];
        if (fabs(v) > peak) peak = fabs(v);
        for (int k = 0; k < 2; k++) {
            double w = 2 * M_PI * 8000 / fs, cw = cos(w), al = sin(w) / (2 * qs[k]), a0 = 1 + al;
            double b0 = (1 + cw) / 2 / a0, b1 = -(1 + cw) / a0, a1 = -2 * cw / a0, a2 = (1 - al) / a0;
            double y = b0 * v + z[k][0];                 /* df2T, double */
            z[k][0] = b1 * v - a1 * y + z[k][1];
            z[k][1] = b0 * v - a2 * y;
            v = y;
        }
        if (n >= N / 2 - 4410 && n < N / 2 + 4410 && fabs(v) > worst) worst = fabs(v);
    }
    return 20 * log10(worst / peak + 1e-12);
}

static void test_no_clicks(void)
{
    const change_t cases[] = {
        {"band 100 Hz 0 -> +15 dB", 100, 1, {100, 0, 1}, {100, 15, 1}},
        {"band 100 Hz +15 -> -15 dB", 100, 1, {100, 15, 1}, {100, -15, 1}},
        {"band +15 dB 60 -> 400 Hz", 100, 1, {60, 15, 2}, {400, 15, 2}},
        {"band -15 dB q 0.3 -> 4", 100, 1, {100, -15, 0.3f}, {100, -15, 4}},
        {"band 800 Hz +15 -> -15 dB", 400, 1, {800, 15, 1}, {800, -15, 1}},
        {"hpf off -> 200 Hz", 150, 0, {0}, {200}},
        {"hpf 20 -> 200 Hz", 150, 0, {20}, {200}},
        {"hpf 200 -> 20 Hz", 60, 0, {200}, {20}},
        {"lpf 20 -> 2 kHz", 200, 2, {20000}, {2000}},
        {"lpf 2 -> 20 kHz", 200, 2, {2000}, {20000}},
    };
    int caught = 0;
    const unsigned nc = sizeof cases / sizeof cases[0];
    for (unsigned i = 0; i < nc; i++) {
        double r = click_db(&cases[i], 0, 0), h = click_db(&cases[i], 1, 0);
        double st = click_db(&cases[i], 0, 2.5e-3f);
        printf("click: %-26s glide %6.1f dB  hard switch %6.1f dB  + a 2.5e-3 step %6.1f dB\n",
               cases[i].name, r, h, st);
        assert(r < -70);
        assert(st > -70);                            /* a step of 1 % of the input sine is seen */
        if (h >= -70) caught++;
    }
    assert(caught >= 2);                             /* and so is a hard switch */
    printf("no clicks: every glide < -70 dB above 8 kHz (re the output peak); a hard switch "
           "fails %d of %u\n", caught, nc);
}

/* Extremes: every stage at its limits, and random settings every 1-40
 * blocks (so often mid-glide) for 20 s of noise. Finite, bounded, and a
 * silence afterwards decays to exact zeros (no subnormals). */
static void test_stable(void)
{
    enum { N = 44100 * 20 };
    float *x = malloc(N * sizeof *x);
    const float ext[2] = {EQ_BAND_MIN, EQ_BAND_MAX};
    for (int k = 0; k < 8; k++) {
        eq_init(&e, 48000);
        eq_set_on(&e, 1);
        eq_set_hpf(&e, (k & 1) ? EQ_HPF_MAX : EQ_HPF_MIN);
        eq_set_lpf(&e, (k & 2) ? EQ_LPF_MIN : EQ_LPF_MAX);
        for (unsigned b = 0; b < EQ_BANDS; b++)
            eq_set_band(&e, b, ext[(k >> 2) & 1], (b & 1) ? -EQ_GAIN_MAX : EQ_GAIN_MAX,
                        (b & 2) ? EQ_Q_MIN : EQ_Q_MAX);
        for (unsigned i = 0; i < 48000; i++) x[i] = 0.5f * frand();
        run(x, 48000);
        for (unsigned i = 0; i < 48000; i++) assert(isfinite(x[i]) && fabsf(x[i]) < 100.0f);
    }
    eq_init(&e, 44100);
    eq_set_on(&e, 1);
    srand(7);
    float peak = 0;
    for (unsigned o = 0; o < N;) {
        unsigned blocks = 1 + (unsigned)rand() % 40;
        int what = rand() % 4;
        if (what == 0) eq_set_hpf(&e, (rand() % 3) ? 20.0f + 180.0f * (frand() + 1) / 2 : 0.0f);
        else if (what == 1) eq_set_lpf(&e, (rand() % 3) ? 2000.0f + 18000.0f * (frand() + 1) / 2 : 0.0f);
        else eq_set_band(&e, (unsigned)rand() % EQ_BANDS, 30.0f * powf(333.3f, (frand() + 1) / 2),
                         15.0f * frand(), 0.3f * powf(13.3f, (frand() + 1) / 2));
        if (rand() % 20 == 0) eq_set_on(&e, rand() % 2);
        for (unsigned b = 0; b < blocks && o < N; b++, o += DSP_BLOCK) {
            unsigned n = N - o < DSP_BLOCK ? N - o : DSP_BLOCK;
            for (unsigned i = 0; i < n; i++) x[o + i] = 0.5f * frand();
            eq_process(&e, x + o, n);
            for (unsigned i = 0; i < n; i++) {
                assert(isfinite(x[o + i]));
                if (fabsf(x[o + i]) > peak) peak = fabsf(x[o + i]);
            }
        }
    }
    assert(peak < 60.0f);                           /* 5 x +15 dB on 0.5 noise: < 0.5 * 5.6^2 * ... */
    /* silence: exact zeros within 4 s, no subnormal on the way */
    eq_set_on(&e, 1);
    eq_set_band(&e, 0, 30, 15, 4);
    memset(x, 0, 176400 * sizeof *x);
    x[0] = 1.0f;
    run(x, 176400);
    for (unsigned i = 0; i < 176400; i++) assert(x[i] == 0.0f || fabsf(x[i]) >= 1e-30f);
    for (unsigned i = 0; i < 4 * EQ_STAGES; i++) assert(e.st[i] == 0.0f);
    free(x);
    printf("stable: 8 extreme settings at 48 kHz, 20 s of random changes (peak %.1f), silence -> 0\n",
           peak);
}

/* Rounding noise of settled low stages (pedal 2026-09-29: in float, HPF
 * 30 Hz + 40 Hz +6 dB + 100 Hz -4 dB q 2 took a -15 dBFS 1 kHz sine from
 * THD+N -80.9 to -71.4 dB). The error vs a double cascade of the same
 * designs; the float DF1 of v0.9.1 (CMSIS arithmetic) as the control. */
static void test_noise(void)
{
    enum { N = 88200 };
    static float x[N];
    static double ref[N], flt[N];
    const double fs = 44100;
    eq_init(&e, 44100);
    eq_set_hpf(&e, 30);
    eq_set_band(&e, 0, 40, 6, 1);
    eq_set_band(&e, 1, 100, -4, 2);
    eq_set_on(&e, 1);
    settle();
    double c[3][5];
    eq_design(0, fs, 30, M_SQRT1_2, 0, c[0]);
    eq_design(1, fs, 40, 1, 6, c[1]);
    eq_design(1, fs, 100, 2, -4, c[2]);
    for (unsigned i = 0; i < N; i++) {
        x[i] = (float)(0.178 * sin(2 * M_PI * 1000.0 * i / fs) + 0.05 * sin(2 * M_PI * 41.0 * i / fs));
        ref[i] = flt[i] = x[i];
    }
    for (unsigned k = 0; k < 3; k++) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        float fx1 = 0, fx2 = 0, fy1 = 0, fy2 = 0;
        const float b0 = (float)c[k][0], b1 = (float)c[k][1], b2 = (float)c[k][2],
                    a1 = (float)-c[k][3], a2 = (float)-c[k][4];
        for (unsigned i = 0; i < N; i++) {
            double in = ref[i], y = c[k][0] * in + c[k][1] * x1 + c[k][2] * x2 - c[k][3] * y1 - c[k][4] * y2;
            x2 = x1; x1 = in; y2 = y1; y1 = y; ref[i] = y;
            float fi = (float)flt[i], fy = (b0 * fi) + (b1 * fx1) + (b2 * fx2) + (a1 * fy1) + (a2 * fy2);
            fx2 = fx1; fx1 = fi; fy2 = fy1; fy1 = fy; flt[i] = fy;
        }
    }
    run(x, N);
    double sig = 0, err = 0, ferr = 0;
    for (unsigned i = N / 2; i < N; i++) {
        sig += ref[i] * ref[i];
        err += (x[i] - ref[i]) * (x[i] - ref[i]);
        ferr += (flt[i] - ref[i]) * (flt[i] - ref[i]);
    }
    double db = 10 * log10(err / sig + 1e-300), fdb = 10 * log10(ferr / sig + 1e-300);
    printf("noise: HPF 30 + 40 Hz +6 + 100 Hz -4 q 2: error re the signal %.1f dB (float DF1: %.1f dB)\n",
           db, fdb);
    assert(db < -120.0);
    assert(fdb > db + 20.0);                         /* the control sees the float noise */
}

static void test_clamps(void)
{
    eq_init(&e, 44100);
    assert(e.bypass && !e.on && e.f[0] == 40.0f && e.f[4] == 3000.0f && e.q[2] == EQ_Q_DEF);
    eq_set_hpf(&e, 5);   assert(e.hpf == EQ_HPF_MIN);
    eq_set_hpf(&e, 900); assert(e.hpf == EQ_HPF_MAX);
    eq_set_hpf(&e, 0);   assert(e.hpf == 0.0f);
    eq_set_lpf(&e, 100); assert(e.lpf == EQ_LPF_MIN);
    eq_set_lpf(&e, 3e4f); assert(e.lpf == EQ_LPF_MAX);
    assert(eq_set_band(&e, 5, 100, 0, 1) == -1);
    assert(eq_set_band(&e, 4, 1, 99, 99) == 0);
    assert(e.f[4] == EQ_BAND_MIN && e.g[4] == EQ_GAIN_MAX && e.q[4] == EQ_Q_MAX);
    assert(e.bypass);                                /* off: no ramp for settings */
    printf("clamps OK, sizeof(eq_t) = %u B\n", (unsigned)sizeof(eq_t));
}

int main(void)
{
    test_clamps();
    test_flat();
    test_no_clicks();
    test_stable();
    test_noise();
    test_response(44100);
    test_response(48000);
    printf("eq host tests OK\n");
    return 0;
}
