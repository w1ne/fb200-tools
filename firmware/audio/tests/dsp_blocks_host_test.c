/* Host test for the CMSIS-DSP based blocks: built by tests/test_dsp_host.py. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsp/biquad.h"
#include "dsp/conv.h"
#include "dsp/gate.h"

static float frand(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }

static void test_conv(size_t taps)
{
    static conv_t c;
    enum { LEN = 4096 };
    static float ir[CONV_MAX_TAPS], x[LEN], y[LEN];
    for (size_t i = 0; i < taps; i++) ir[i] = frand() * expf(-(float)i / 300.0f);
    for (size_t i = 0; i < LEN; i++) x[i] = frand();
    assert(conv_init(&c) == 0);
    assert(conv_load(&c, ir, taps) == 0);
    for (size_t b = 0; b < LEN; b += DSP_BLOCK) conv_process(&c, x + b, y + b);
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

static void test_gate(void)
{
    gate_t g;
    gate_init(&g, 48000);
    gate_set(&g, -50, 6, 1, 20, 50);
    float buf[DSP_BLOCK];
    for (int k = 0; k < 400; k++) {                 /* -70 dB noise: closes */
        for (int i = 0; i < DSP_BLOCK; i++) buf[i] = 0.000316f * frand();
        gate_process(&g, buf, DSP_BLOCK);
    }
    assert(g.gain < 0.01f);
    for (int k = 0; k < 50; k++) {                  /* -20 dB signal: opens */
        for (int i = 0; i < DSP_BLOCK; i++) buf[i] = 0.1f * frand();
        gate_process(&g, buf, DSP_BLOCK);
    }
    assert(g.gain > 0.99f);
    printf("gate OK\n");
}

int main(void)
{
    test_conv(1);
    test_conv(DSP_BLOCK);
    test_conv(1000);
    test_conv(CONV_MAX_TAPS);
    test_biquad();
    test_gate();
    printf("dsp blocks host tests OK\n");
    return 0;
}
