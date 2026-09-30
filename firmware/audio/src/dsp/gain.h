/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_DSP_GAIN_H
#define FB200_DSP_GAIN_H
#include "dsp.h"
typedef struct { dsp_smooth_t gain; } gain_ctx_t;
void gain_init(gain_ctx_t *c, float value);
void gain_set(gain_ctx_t *c, float value);
void gain_process(void *ctx, dsp_block_t *b, size_t n);
/* The stock input gain (global setting S+0x1a, set by the app): index 0 =
 * mute, 1..11 = -55..-5 dB in 5 dB steps, 12 and 13 (default) = 0 dB,
 * 14..25 = +0.5..+6 dB. Out-of-range index -> the default. */
#define GAIN_INPUT_DEFAULT 13u
float gain_input_stock(unsigned index);
#endif
