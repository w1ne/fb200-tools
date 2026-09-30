/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_DSP_AMP_H
#define FB200_DSP_AMP_H
/* Stock FB200 amp (ITCM 0x2d78) + tone stack, mono, in place.
 *   u = x * drive_scale2 * drv * drive_scale * pre_gain
 *   drv: gain g = knob/100, 0.05 + 1.9 g (g < 0.5) else 1 + 10 (g - 0.5),
 *        smoothed 0.001/0.999 per sample
 *   pre SOS (10 x DF1) -> 3x oversampling (linear interpolation prev -> u)
 *   -> 256-point waveshaper (odd) -> anti-alias biquad at 3 fs, keep every 3rd
 *   -> post SOS (10 x DF1) -> * out_gain * 2 level * vol (knob/100, smoothed
 *   0.0005/0.9995) -> tone stack.
 * The stock callback feeds the amp (L + R) of the normalised ADC samples
 * (2x for a mono guitar) times 0.9999702f: a unity input gain whose
 * 0.001/0.999 float smoother stalls there. Bit parity needs that exact input:
 * the pre/post chains are ill-conditioned, so a last-bit change anywhere
 * costs up to -42 dB (also why this code must build with -ffp-contract=off).
 * Drive and volume start at 0 and ramp in (time constants 1000 / 2000 samples)
 * and stall a few ulps below target, both like the stock.
 * Coefficients are the stock 44.1 kHz designs; at another fs the filter
 * frequencies scale by fs / 44100. Without stock data (g_stock NULL)
 * or before amp_set_model, amp_process passes audio through. */
#include "arm_math.h"
#include "dsp.h"
#include "stock_data.h"
#include "tone.h"

typedef struct {
    const stock_amp_model_t *m;                  /* NULL: pass-through */
    arm_biquad_casd_df1_inst_f32 pre, post, aa;
    float pre_state[4 * STOCK_AMP_SOS], post_state[4 * STOCK_AMP_SOS], aa_state[4];
    float prev;                                  /* last pre-SOS output */
    float drv, drv_target, vol, vol_target;
    float fs;
    tone_t tone;
} amp_t;

void amp_init(amp_t *a, float fs);
int amp_set_model(amp_t *a, int model);          /* 1..10; 0 on success */
/* stock knob units: gain, bass, mid, treble, volume 0..100; midfreq 0..4 */
void amp_set_params(amp_t *a, int gain, int bass, int mid, int midfreq, int treble, int volume);
void amp_process(amp_t *a, float *x, unsigned n);  /* n <= DSP_BLOCK */
#endif
