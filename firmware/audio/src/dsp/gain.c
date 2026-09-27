#include "gain.h"
void gain_init(gain_ctx_t *c, float value) { dsp_smooth_init(&c->gain, value, 0.01f); }
void gain_set(gain_ctx_t *c, float value) { dsp_smooth_set(&c->gain, value); }
void gain_process(void *ctx, dsp_block_t *b, size_t n)
{
    gain_ctx_t *c = ctx;
    for (size_t i = 0; i < n; i++) {
        float g = dsp_smooth_next(&c->gain);
        for (size_t ch = 0; ch < DSP_CHANNELS; ch++) b->data[ch][i] *= g;
    }
}
