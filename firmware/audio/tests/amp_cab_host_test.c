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

/* An IR change mid-stream keeps the input history, as the old direct FIR
 * (arm_fir_f32) that swaps its coefficients at the same block: no reset. */
static void test_cab_swap(void)
{
    enum { LEN = 4000, SWAP = 40 * DSP_BLOCK };
    static cab_t cab;
    static float ir[2][CAB_TAPS], rev[CAB_TAPS], st[CAB_TAPS + DSP_BLOCK - 1];
    static float x[LEN], y[LEN], ref[LEN];
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < CAB_TAPS; i++) ir[t][i] = frand() * expf(-(float)i / (60.0f + 100 * t));
    for (int i = 0; i < LEN; i++) x[i] = y[i] = frand();
    arm_fir_instance_f32 fir;
    for (int i = 0; i < CAB_TAPS; i++) rev[i] = ir[0][CAB_TAPS - 1 - i];
    arm_fir_init_f32(&fir, CAB_TAPS, rev, st, DSP_BLOCK);
    cab_init(&cab);
    cab_set_ir(&cab, ir[0], 0.2f);
    for (int i = 0; i < LEN; i += DSP_BLOCK) {
        if (i == SWAP) {
            cab_set_ir(&cab, ir[1], 0.2f);
            for (int k = 0; k < CAB_TAPS; k++) rev[k] = ir[1][CAB_TAPS - 1 - k];
        }
        unsigned len = LEN - i < DSP_BLOCK ? (unsigned)(LEN - i) : DSP_BLOCK;
        cab_process(&cab, y + i, len);
        arm_fir_f32(&fir, x + i, ref + i, len);
        arm_scale_f32(ref + i, (float)(0.2 * 1.15), ref + i, len);
    }
    double maxerr = 0, peak = 0;
    for (int n = 0; n < LEN; n++) {
        maxerr = fmax(maxerr, fabs((double)ref[n] - y[n]));
        peak = fmax(peak, fabs(ref[n]));
    }
    printf("cab IR swap vs direct FIR: max err %.2e (peak %.2f)\n", maxerr, peak);
    assert(peak > 0.1 && maxerr < 1e-5);
}

/* Long IRs (cab_init_long, cab_set_ir_len): the IR and its gain change
 * together when the load is done. From the flat start (tail off) the new
 * tail fades in (sees input from the swap on); long -> long is an exact
 * FIR swap. */
static void test_cab_long(void)
{
    enum { LEN = 282 * DSP_BLOCK, REQ_A = 10 * DSP_BLOCK, REQ_B = 120 * DSP_BLOCK };
    static conv2_tail_t tail;
    static cab_t cab;
    static float ir[2][CAB_MAX_TAPS], x[LEN], y[LEN];
    static const unsigned taps[2] = {CAB_MAX_TAPS, 2000};
    static const float gain[2] = {0.3f, 0.7f};
    for (int t = 0; t < 2; t++)
        for (unsigned i = 0; i < taps[t]; i++) ir[t][i] = frand() * expf(-(float)i / (700.0f + 300 * t));
    for (int i = 0; i < LEN; i++) x[i] = y[i] = frand();
    cab_init(&cab);                          /* no tail (the firmware with long IRs off) */
    assert(cab_set_ir_len(&cab, ir[0], CAB_TAPS, 1.0f) == 0);
    assert(cab_set_ir_len(&cab, ir[0], CAB_TAPS + 1, 1.0f) != 0 && cab.conv.t == NULL);
    cab_init_long(&cab, &tail);
    assert(cab_set_ir_len(&cab, ir[0], CAB_MAX_TAPS + 1, 1.0f) != 0);
    const float one = 1.0f;
    assert(cab_set_ir_len(&cab, &one, 1, 1.0f) == 0);   /* active from sample 0 */
    uint32_t at[2] = {0, 0};
    for (int i = 0; i < LEN; i += DSP_BLOCK) {
        if (i == REQ_A) assert(cab_set_ir_len(&cab, ir[0], taps[0], gain[0]) == 0);
        if (i == REQ_B) assert(cab_set_ir_len(&cab, ir[1], taps[1], gain[1]) == 0);
        int loading = conv2_pending(&cab.conv);
        cab_process(&cab, y + i, DSP_BLOCK);
        /* swaps at a block end (conv2 counts only the samples it saw) */
        if (loading && !conv2_pending(&cab.conv)) at[i >= REQ_B] = (uint32_t)i + DSP_BLOCK;
    }
    assert(at[0] > REQ_A && at[1] > REQ_B);
    double maxerr = 0, peak = 0;
    for (int n = 0; n < LEN; n++) {
        double ref = x[n] * (float)1.15;    /* unit IR before the first long one */
        int t = n >= (int)at[1] ? 1 : n >= (int)at[0] ? 0 : -1;
        if (t >= 0) {
            double acc = 0;
            for (int k = 0; k < (int)taps[t] && k <= n; k++) {
                if (t == 0 && k >= CONV2_HEAD_TAPS && n - k < (int)at[0]) break;   /* fade in */
                acc += (double)ir[t][k] * x[n - k];
            }
            ref = acc * (float)((double)gain[t] * 1.15);
        }
        maxerr = fmax(maxerr, fabs(ref - y[n]));
        peak = fmax(peak, fabs(ref));
    }
    printf("cab long IR: swaps at +%u, +%u samples, max err %.2e (peak %.2f)\n",
           (unsigned)(at[0] - REQ_A), (unsigned)(at[1] - REQ_B), maxerr, peak);
    assert(peak > 0.1 && maxerr < 1e-5 * peak);
}

/* tone_df1 (the amp's and tone stack's DF1 cascades) is bit-identical to
 * CMSIS arm_biquad_cascade_df1_f32: stage counts odd and even, every block
 * length mod 4, over several calls; and a pair whose state is not the
 * cascade's own (B's x history != A's y history) takes the plain path. */
static void test_tone_df1(void)
{
    enum { MAXS = 10, MAXN = 97 };
    static const unsigned lens[] = {1, 2, 3, 4, 5, 6, 7, 8, 31, 32, 33, 96, 97};
    float coef[5 * MAXS], sa[4 * MAXS], sb[4 * MAXS], xa[MAXN], xb[MAXN];
    unsigned checked = 0;
    for (unsigned stages = 1; stages <= MAXS; stages++) {
        for (unsigned s = 0; s < stages; s++) {   /* stable: poles inside the unit circle */
            float r = 0.5f + 0.45f * (frand() + 1.0f) / 2.0f, th = 3.0f * (frand() + 1.0f) / 2.0f;
            float *c = &coef[5 * s];
            c[0] = frand(); c[1] = frand(); c[2] = frand();
            c[3] = 2.0f * r * cosf(th); c[4] = -r * r;
        }
        for (int mismatch = 0; mismatch < 2; mismatch++) {
            arm_biquad_casd_df1_inst_f32 ref;
            arm_biquad_cascade_df1_init_f32(&ref, (uint8_t)stages, coef, sa);
            memset(sb, 0, sizeof sb);
            if (mismatch && stages >= 2) {        /* a state set some other way */
                for (unsigned i = 0; i < 4 * stages; i++) sa[i] = sb[i] = frand();
            }
            for (unsigned call = 0; call < 3 * sizeof lens / sizeof lens[0]; call++) {
                unsigned n = lens[call % (sizeof lens / sizeof lens[0])];
                for (unsigned i = 0; i < n; i++) xa[i] = xb[i] = frand();
                arm_biquad_cascade_df1_f32(&ref, xa, xa, n);
                tone_df1(coef, sb, stages, xb, n);
                assert(memcmp(xa, xb, n * sizeof xa[0]) == 0);
                assert(memcmp(sa, sb, 4 * stages * sizeof sa[0]) == 0);
                checked += n;
            }
        }
    }
    printf("tone_df1 bit-identical to CMSIS df1: %u samples\n", checked);
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
    test_cab_swap();
    test_cab_long();
    test_user_ir_gain();
    test_tone_df1();
    printf("amp cab host tests OK\n");
    return 0;
}
