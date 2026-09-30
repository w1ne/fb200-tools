/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include "gain.h"

/* 10^(dB/20) as float; the stock table (DTCM 0x20007770, read in the
 * emulation) has the same dB steps and agrees within 1 ulp. */
static const float kInputGain[26] = {
    0.0f, 0.00177827943f, 0.00316227763f, 0.00562341325f, 0.00999999978f, 0.0177827943f,
    0.0316227749f, 0.0562341325f, 0.100000001f, 0.177827939f, 0.316227764f, 0.562341332f,
    1.0f, 1.0f, 1.05925369f, 1.12201846f, 1.18850219f, 1.25892544f, 1.33352149f, 1.41253757f,
    1.49623561f, 1.58489323f, 1.67880404f, 1.77827942f, 1.88364911f, 1.99526227f,
};

float gain_input_stock(unsigned index)
{
    return kInputGain[index < sizeof kInputGain / sizeof kInputGain[0] ? index : GAIN_INPUT_DEFAULT];
}
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
