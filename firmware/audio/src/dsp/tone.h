/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_DSP_TONE_H
#define FB200_DSP_TONE_H
/* Stock tone stack: bass (peak 100 Hz), mid (peak, 5 selectable centres),
 * presence (high shelf, fixed step 15), treble (peak 4.5 kHz). Four DF1
 * biquads (CMSIS-DSP), coefficients taken from the stock 32-step tables,
 * step = int(knob/100 * 31). Without the stock data it passes audio through. */
#include "arm_math.h"

#define TONE_STAGES 4

/* stock knob scaling: knob * 0.01f (not knob / 100.0f; they differ in the last bit) */
static inline float knob_fraction(int knob)
{
    return (float)(knob < 0 ? 0 : knob > 100 ? 100 : knob) * 0.01f;
}

typedef struct {
    arm_biquad_casd_df1_inst_f32 inst;
    float coeffs[5 * TONE_STAGES];
    float state[4 * TONE_STAGES];
    int active;
} tone_t;

/* In-place DF1 cascade with the arithmetic of CMSIS arm_biquad_cascade_df1_f32
 * (per stage and sample: b0 x0 + b1 x1 + b2 x2 + a1 y1 + a2 y2, left to
 * right, the same state layout), so bit-identical to it; stages run in pairs
 * over the block, so two recurrences share the FPU pipeline. Coefficients
 * {b0, b1, b2, a1, a2} per stage, state {x1, x2, y1, y2} per stage. */
/* one DF1 stage and sample, CMSIS order; the including file must be built
 * with -ffp-contract=off (no FMA), as the stock objects are (Makefile) */
#define TONE_DF1(c, x0, x1, x2, y1, y2) \
    ((c)[0] * (x0) + (c)[1] * (x1) + (c)[2] * (x2) + (c)[3] * (y1) + (c)[4] * (y2))
void tone_df1(const float *coeffs, float *state, unsigned stages, float *x, unsigned n);

void tone_init(tone_t *t);                               /* flat */
/* bass, mid, treble: knob units 0..100; midfreq 0..4 = 200/400/800/1600/3000 Hz */
void tone_set(tone_t *t, int bass, int mid, int midfreq, int treble);
void tone_process(tone_t *t, float *x, unsigned n);      /* in place */
#endif
