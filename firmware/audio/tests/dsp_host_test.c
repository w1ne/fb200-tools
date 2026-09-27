/* Host test for the DSP framework: built and run by tests/test_dsp_host.py. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dsp/dsp.h"
#include "dsp/gain.h"
#include "dsp/math.h"
#include "dsp/testgen.h"

static void fill(dsp_block_t *b, float v)
{
    for (int ch = 0; ch < DSP_CHANNELS; ch++)
        for (int i = 0; i < DSP_BLOCK; i++) b->data[ch][i] = v;
}

int main(void)
{
    /* gain: 0.5x, fully settled */
    dsp_block_t b;
    fill(&b, 1.0f);
    gain_ctx_t g;
    gain_init(&g, 0.5f);
    dsp_chain_t chain = {0};
    dsp_chain_add(&chain, gain_process, &g);
    for (int k = 0; k < 100; k++) {
        fill(&b, 1.0f);                     /* fresh input each block */
        dsp_chain_run(&chain, &b, DSP_BLOCK);
    }
    assert(fabsf(b.data[0][0] - 0.5f) < 1e-3f);
    assert(fabsf(b.data[1][DSP_BLOCK - 1] - 0.5f) < 1e-3f);

    /* gain smoothing: no step larger than 1% of the delta per sample */
    fill(&b, 1.0f);
    gain_set(&g, 1.0f);
    dsp_chain_run(&chain, &b, DSP_BLOCK);
    float prev = 0.5f;
    for (int i = 0; i < DSP_BLOCK; i++) {
        float now = b.data[0][i];
        assert(now - prev < 0.01f + 1e-6f);
        prev = now;
    }

    /* testgen sine: RMS ~ amp/sqrt(2), 1 kHz at 48 kHz */
    testgen_ctx_t tg;
    testgen_init(&tg, 48000.0f);
    testgen_set(&tg, TESTGEN_SINE, 0.5f, 1000.0f);
    double sum = 0;
    int n = 0;
    for (int blk = 0; blk < 50; blk++) {
        dsp_chain_t tchain = {0};
        dsp_chain_add(&tchain, testgen_process, &tg);
        fill(&b, 0.0f);
        dsp_chain_run(&tchain, &b, DSP_BLOCK);
        for (int i = 0; i < DSP_BLOCK; i++) { sum += b.data[0][i] * b.data[0][i]; n++; }
    }
    double rms = sqrt(sum / n);
    assert(fabs(rms - 0.5 / sqrt(2.0)) < 0.01);
    assert(b.data[0][0] == b.data[1][0]);   /* duplicated to both channels */

    /* testgen off is a no-op */
    testgen_set(&tg, TESTGEN_OFF, 0.0f, 0.0f);
    fill(&b, 0.25f);
    dsp_chain_run(&(dsp_chain_t){0}, &b, DSP_BLOCK);
    assert(b.data[0][0] == 0.25f);

    /* math shims vs libm (host only) */
    for (float x = -3.2f; x < 3.2f; x += 0.017f)
        assert(fabsf(dsp_sinf(x) - sinf(x)) < 1e-5f);
    for (float db = -24.0f; db <= 24.0f; db += 0.5f)
        assert(fabsf(dsp_db_to_gain(db) - powf(10.0f, db / 20.0f)) < 0.01f);

    /* stock input gain table (S+0x1a): mute, -55..-5 dB in 5 dB, 0 dB x2, +0.5..+6 dB */
    assert(gain_input_stock(0) == 0.0f);
    for (unsigned i = 1; i <= 25; i++) {
        float db = i <= 11 ? -60.0f + 5.0f * (float)i : i <= 13 ? 0.0f : 0.5f * (float)(i - 13);
        assert(fabsf(gain_input_stock(i) / powf(10.0f, db / 20.0f) - 1.0f) < 1e-6f);
    }
    assert(gain_input_stock(GAIN_INPUT_DEFAULT) == 1.0f && gain_input_stock(12) == 1.0f);
    assert(gain_input_stock(26) == 1.0f && gain_input_stock(255) == 1.0f);   /* -> default */

    printf("dsp host tests OK\n");
    return 0;
}
