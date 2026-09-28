/* Host test for the CMSIS-DSP based blocks: built by tests/test_dsp_host.py. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsp/biquad.h"
#include "dsp/conv.h"

static float frand(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }

#define MAX_TAPS 2048                    /* 43 ms at 48 kHz (M5 wants 4096) */
static float s_h[CONV_PARTS(MAX_TAPS)][CONV_N], s_x[CONV_PARTS(MAX_TAPS)][CONV_N];

static void test_conv(size_t taps)
{
    static conv_t c;
    enum { LEN = 4096 };
    static float ir[MAX_TAPS], x[LEN], y[LEN];
    for (size_t i = 0; i < taps; i++) ir[i] = frand() * expf(-(float)i / 300.0f);
    for (size_t i = 0; i < LEN; i++) x[i] = frand();
    assert(conv_init(&c, s_h, s_x, CONV_PARTS(MAX_TAPS)) == 0);
    assert(conv_set_ir(&c, ir, MAX_TAPS + 1) != 0);
    assert(conv_set_ir(&c, ir, taps) == 0);
    for (size_t b = 0; b < LEN; b += DSP_BLOCK) conv_process(&c, x + b, y + b, DSP_BLOCK);
    double maxerr = 0;
    for (size_t n = 0; n < LEN; n++) {
        double ref = 0;
        for (size_t k = 0; k < taps && k <= n; k++) ref += (double)ir[k] * x[n - k];
        double e = fabs(ref - y[n]);
        if (e > maxerr) maxerr = e;
    }
    printf("conv %zu taps: max err %.2e\n", taps, maxerr);
    assert(maxerr < 1e-4);
}

/* Unit impulse after init; part blocks (1..DSP_BLOCK + 5 samples, in place)
 * give the same output as full blocks; conv_reset clears the history. */
static void test_conv_blocks(void)
{
    static conv_t a, b;
    enum { LEN = 2000, TAPS = 300 };
    static float ir[TAPS], x[LEN], ya[LEN], yb[LEN];
    for (size_t i = 0; i < LEN; i++) x[i] = yb[i] = frand();
    assert(conv_init(&a, s_h, s_x, CONV_PARTS(MAX_TAPS)) == 0);
    conv_process(&a, x, ya, 3 * DSP_BLOCK);
    for (size_t i = 0; i < 3 * DSP_BLOCK; i++) assert(fabsf(ya[i] - x[i]) < 1e-6f);

    static float h2[CONV_PARTS(TAPS)][CONV_N], x2[CONV_PARTS(TAPS)][CONV_N];
    for (size_t i = 0; i < TAPS; i++) ir[i] = frand() * expf(-(float)i / 60.0f);
    assert(conv_init(&a, s_h, s_x, CONV_PARTS(MAX_TAPS)) == 0);
    assert(conv_init(&b, h2, x2, CONV_PARTS(TAPS)) == 0);   /* capacity = IR length */
    assert(conv_set_ir(&a, ir, TAPS) == 0 && conv_set_ir(&b, ir, TAPS) == 0);
    conv_process(&a, x, ya, LEN);
    for (size_t i = 0, len = 1; i < LEN; i += len, len = len % (DSP_BLOCK + 5) + 1) {
        if (len > LEN - i) len = LEN - i;
        conv_process(&b, yb + i, yb + i, len);
    }
    double maxerr = 0;
    for (size_t n = 0; n < LEN; n++) maxerr = fmax(maxerr, fabs((double)ya[n] - yb[n]));
    printf("conv part blocks: max err %.2e\n", maxerr);
    assert(maxerr < 1e-5);

    conv_reset(&b);                       /* no history: first block = x * ir only */
    conv_process(&b, x, yb, DSP_BLOCK);
    for (size_t n = 0; n < DSP_BLOCK; n++) {
        double ref = 0;
        for (size_t k = 0; k <= n; k++) ref += (double)ir[k] * x[n - k];
        assert(fabs(ref - yb[n]) < 1e-5);
    }
}

/* |H(e^jw)| of one CMSIS stage {b0,b1,b2,a1,a2} (a negated). */
static double mag(const float *c, double fs, double f)
{
    double w = 2 * M_PI * f / fs;
    double nr = c[0] + c[1] * cos(w) + c[2] * cos(2 * w), ni = -c[1] * sin(w) - c[2] * sin(2 * w);
    double dr = 1 - c[3] * cos(w) - c[4] * cos(2 * w), di = c[3] * sin(w) + c[4] * sin(2 * w);
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

static void test_biquad(void)
{
    biquad_t b;
    biquad_init(&b, 5);
    const float fs = 48000;
    biquad_design(&b, 0, BQ_LOWPASS, fs, 1000, 0.7071f, 0);
    biquad_design(&b, 1, BQ_HIGHPASS, fs, 1000, 0.7071f, 0);
    biquad_design(&b, 2, BQ_PEAK, fs, 1000, 1.0f, 6);
    biquad_design(&b, 3, BQ_LOWSHELF, fs, 200, 0.7071f, -6);
    biquad_design(&b, 4, BQ_HIGHSHELF, fs, 5000, 0.7071f, 3);
    assert(fabs(mag(&b.coeffs[0], fs, 10) - 1.0) < 1e-3);                 /* LP passes DC */
    assert(fabs(20 * log10(mag(&b.coeffs[0], fs, 1000)) + 3.01) < 0.05);  /* -3 dB at fc */
    assert(mag(&b.coeffs[5], fs, 10) < 1e-3);                             /* HP blocks DC */
    assert(fabs(20 * log10(mag(&b.coeffs[10], fs, 1000)) - 6.0) < 0.05);  /* peak +6 dB */
    assert(fabs(20 * log10(mag(&b.coeffs[15], fs, 10)) + 6.0) < 0.1);     /* low shelf */
    assert(fabs(20 * log10(mag(&b.coeffs[20], fs, 20000)) - 3.0) < 0.1);  /* high shelf */
    /* the CMSIS cascade runs: an impulse through a lone peak stage matches the
     * difference equation */
    biquad_t p;
    biquad_init(&p, 1);
    biquad_design(&p, 0, BQ_PEAK, fs, 1000, 1.0f, 6);
    float in[8] = {1}, out[8];
    biquad_process(&p, in, out, 8);
    assert(fabsf(out[0] - p.coeffs[0]) < 1e-6f);
    assert(fabsf(out[1] - (p.coeffs[1] + p.coeffs[3] * out[0])) < 1e-6f);
    printf("biquad OK\n");
}

int main(void)
{
    test_conv(1);
    test_conv(DSP_BLOCK);
    test_conv(1000);
    test_conv(MAX_TAPS);
    test_conv_blocks();
    test_biquad();
    printf("dsp blocks host tests OK\n");
    return 0;
}
