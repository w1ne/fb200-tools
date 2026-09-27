/* Host test for amp/tone/cab WITHOUT the stock data (the CI build): amp and
 * tone pass audio through, the cab starts flat, cab_set_ir() is a 512-tap FIR
 * scaled by gain * 1.15 and cab_user_ir_gain() follows the stock formula.
 * Built by tests/test_dsp_host.py; stock parity: tests/test_stock_dsp_parity.py. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/amp.h"
#include "dsp/cab.h"
#include "dsp/tone.h"

static float frand(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }

static void test_passthrough(void)
{
    static amp_t amp;
    float x[DSP_BLOCK], y[DSP_BLOCK];
    for (int i = 0; i < DSP_BLOCK; i++) x[i] = y[i] = frand();
    amp_init(&amp, 48000.0f);
    assert(amp_set_model(&amp, 1) != 0);      /* no stock data linked */
    amp_set_params(&amp, 70, 50, 50, 2, 50, 80);
    amp_process(&amp, y, DSP_BLOCK);
    assert(memcmp(x, y, sizeof x) == 0);

    tone_t tone;
    tone_init(&tone);
    tone_set(&tone, 0, 100, 4, 100);
    tone_process(&tone, y, DSP_BLOCK);
    assert(memcmp(x, y, sizeof x) == 0);

    static cab_t cab;
    cab_init(&cab);
    assert(cab_set_model(&cab, 1) != 0);
    cab_process(&cab, y, DSP_BLOCK);
    assert(memcmp(x, y, sizeof x) == 0);
    printf("pass-through without stock data OK\n");
}

static void test_cab_fir(void)
{
    enum { LEN = 3000 };
    static cab_t cab;
    static float ir[CAB_TAPS], x[LEN], y[LEN];
    for (int i = 0; i < CAB_TAPS; i++) ir[i] = frand() * expf(-(float)i / 80.0f);
    for (int i = 0; i < LEN; i++) x[i] = y[i] = frand();
    cab_init(&cab);
    cab_set_ir(&cab, ir, 0.15f);
    /* odd block lengths too: the FIR has no block latency */
    for (size_t i = 0, len = 7; i < LEN; i += len, len = len == 7 ? DSP_BLOCK : 7)
        cab_process(&cab, y + i, (unsigned)(LEN - i < len ? LEN - i : len));
    double maxerr = 0;
    for (int n = 0; n < LEN; n++) {
        double ref = 0;
        for (int k = 0; k < CAB_TAPS && k <= n; k++) ref += (double)ir[k] * x[n - k];
        ref *= 0.15 * 1.15;
        double e = fabs(ref - y[n]);
        if (e > maxerr) maxerr = e;
    }
    printf("cab fir: max err %.2e\n", maxerr);
    assert(maxerr < 1e-5);
}

/* double-precision reference of the stock user-IR gain */
static double ref_gain(const float *ir)
{
    double sum = 0;
    for (int k = 1; k <= 85; k++) {
        double re = 0, im = 0;
        for (int j = 0; j < 512; j++) {
            double w = 0.5 - 0.5 * cos(2 * M_PI * (2 * j + 1) / 1025.0);
            double a = -2 * M_PI * (k - 1) * j / 512.0;
            re += ir[j] * w * cos(a);
            im += ir[j] * w * sin(a);
        }
        sum += sqrt(sqrt(re * re + im * im)) * (30 / k + 1);
    }
    return sum == 0 ? 1.0 : 100.0 / sum;
}

static void test_user_ir_gain(void)
{
    static float ir[CAB_TAPS];
    double worst = 0;
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < CAB_TAPS; i++) ir[i] = frand() * expf(-(float)i / (30.0f + 60.0f * t));
        double g = cab_user_ir_gain(ir), r = ref_gain(ir), e = fabs(g - r) / r;
        if (e > worst) worst = e;
    }
    memset(ir, 0, sizeof ir);
    assert(cab_user_ir_gain(ir) == 1.0f);
    printf("user IR gain: max rel err %.2e\n", worst);
    assert(worst < 1e-4);
}

int main(void)
{
    test_passthrough();
    test_cab_fir();
    test_user_ir_gain();
    printf("amp cab host tests OK\n");
    return 0;
}
