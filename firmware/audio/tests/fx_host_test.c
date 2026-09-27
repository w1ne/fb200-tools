/* Host sanity tests for the stock-effect ports (gate, comp, mod, reverb).
 * Parity with the stock DSP is tests/test_fx_parity.py; this checks behaviour
 * that must hold at our own 48 kHz rate too. Built by tests/test_dsp_host.py. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/comp.h"
#include "dsp/gate.h"
#include "dsp/mod.h"
#include "dsp/reverb.h"

#define N 16384
static float frand(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }

static double rms(const float *x, unsigned n)
{
    double s = 0;
    for (unsigned i = 0; i < n; i++) s += (double)x[i] * x[i];
    return sqrt(s / n);
}

static void sine(float *x, unsigned n, float amp, float hz, float fs)
{
    for (unsigned i = 0; i < n; i++) x[i] = amp * sinf(6.2831853f * hz * (float)i / fs);
}

/* process x in blocks of `blk` (<= DSP_BLOCK) through fn */
#define RUN(fn, ctx, x, n, blk)                                                  \
    for (unsigned _o = 0; _o < (n); _o += (blk))                                 \
        fn((ctx), (x) + _o, ((n) - _o < (blk)) ? (n) - _o : (blk))

static void test_gate(float fs)
{
    static gate_t g;
    static float x[N];
    gate_init(&g, fs);
    gate_set_params(&g, 100);
    for (unsigned i = 0; i < N; i++) x[i] = 0.000316f * frand();        /* -70 dB noise */
    float in = (float)rms(x + N / 2, N / 2);
    RUN(gate_process, &g, x, N, DSP_BLOCK);
    assert(rms(x + N / 2, N / 2) < 0.01 * in);                          /* closed */
    for (unsigned i = 0; i < N; i++) x[i] = 0.1f * frand();             /* -20 dB */
    in = (float)rms(x + N / 2, N / 2);
    RUN(gate_process, &g, x, N, DSP_BLOCK);
    assert(fabs(rms(x + N / 2, N / 2) / in - 1.0) < 1e-3);              /* open */

    gate_init(&g, fs);                                                  /* knob 0: dry */
    gate_set_params(&g, 0);
    for (unsigned i = 0; i < N; i++) x[i] = 0.001f * frand();
    static float y[N];
    memcpy(y, x, sizeof x);
    RUN(gate_process, &g, x, N, DSP_BLOCK);
    assert(memcmp(x, y, sizeof x) == 0);
    printf("gate %.0f Hz OK\n", fs);
}

static double comp_gain_db(float fs, unsigned ratio, float amp)
{
    static comp_t c;
    static float x[N];
    comp_init(&c, fs);
    comp_set_params(&c, 0, 50, 20, ratio, 50);
    sine(x, N, amp, 1000.0f, fs);
    RUN(comp_process, &c, x, N, DSP_BLOCK);
    return 20 * log10(rms(x + N / 2, N / 2) / (amp / sqrt(2)));
}

static void test_comp(float fs)
{
    /* ratio 0 is linear: gain 6 * level = 3 (+9.5 dB) through flat filters */
    assert(fabs(comp_gain_db(fs, 0, 0.3f) - 9.54) < 0.5);
    assert(fabs(comp_gain_db(fs, 0, 0.03f) - 9.54) < 0.5);
    /* ratio 100 above the threshold (-48 dB): L = T + (e - T) / 10 */
    double g = comp_gain_db(fs, 100, 0.3f);
    assert(g < 9.54 - 15 && g > 9.54 - 25);
    printf("comp %.0f Hz OK (ratio 100 at -10 dBFS: %+.1f dB)\n", fs, g);
}

static int finite_below(const float *x, unsigned n, float lim)
{
    for (unsigned i = 0; i < n; i++)
        if (!(x[i] > -lim && x[i] < lim)) return 0;
    return 1;
}

/* every type: finite, bounded, and the same result whatever the block size */
static void test_mod(float fs)
{
    static mod_t a, b;
    static float x[N], y[N];
    for (unsigned t = 0; t < MOD_TYPES; t++) {
        for (unsigned k = 0; k < 3; k++) {
            unsigned p = k * 50;
            mod_init(&a, fs);
            mod_init(&b, fs);
            mod_set_params(&a, t, p, 100 - p / 2, p, 100 - p);
            mod_set_params(&b, t, p, 100 - p / 2, p, 100 - p);
            sine(x, N, 0.3f, 110.0f, fs);
            for (unsigned i = 0; i < N; i++) x[i] += 0.05f * frand();
            memcpy(y, x, sizeof x);
            RUN(mod_process, &a, x, N, DSP_BLOCK);
            RUN(mod_process, &b, y, N, 7);
            assert(finite_below(x, N, 4.0f));
            assert(memcmp(x, y, sizeof x) == 0);
        }
    }
    printf("mod %.0f Hz OK (%d types)\n", fs, MOD_TYPES);
}

/* impulse: a dry click, a wet tail on both sides that decays */
static void test_reverb(float fs)
{
    static reverb_t r;
    static float x[4 * N], l[4 * N], rr[4 * N];
    unsigned n = 4 * N;
    for (unsigned t = 0; t < 5; t++) {
        reverb_init(&r, fs);
        reverb_set_params(&r, t, 70, 50, 50, 50);
        memset(x, 0, sizeof x);
        x[0] = 0.5f;
        for (unsigned o = 0; o < n; o += DSP_BLOCK) reverb_process(&r, x + o, l + o, rr + o, DSP_BLOCK);
        assert(finite_below(l, n, 2.0f) && finite_below(rr, n, 2.0f));
        double early_l = rms(l + 2000, 8000), early_r = rms(rr + 2000, 8000);
        double late = rms(l + n - 8000, 8000) + rms(rr + n - 8000, 8000);
        assert(early_l > 1e-4 && early_r > 1e-4);            /* a tail on both sides */
        assert(late < 0.1 * (early_l + early_r));            /* that decays */
        assert(memcmp(l + 1000, rr + 1000, 4000 * sizeof(float)) != 0);   /* stereo */
    }
    printf("reverb %.0f Hz OK\n", fs);
}

int main(void)
{
    test_gate(44100.0f);
    test_gate(48000.0f);
    test_comp(44100.0f);
    test_comp(48000.0f);
    test_mod(44100.0f);
    test_mod(48000.0f);
    test_reverb(44100.0f);
    test_reverb(48000.0f);
    printf("fx host tests OK\n");
    return 0;
}
