/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_DSP_COMP_H
#define FB200_DSP_COMP_H
/* Stock compressor "CS Comp" (FB200 ITCM 0x2300). Preset fields: 0x14 enable,
 * 0x16 type (the stock has one type and ignores it), 0x18 attack,
 * 0x1a threshold, 0x1c ratio, 0x1e level, all 0..100.
 *   env    = |x| peak over 501-sample windows, held for the next window;
 *            falls 0.25 % of the held peak per sample towards it (so after
 *            silence it stays up: a stock quirk, kept)
 *   T      = threshold table, -60..0 dB in 100 uneven ~0.6 dB steps
 *   slope  = 1 / (1 + 9 sqrt(ratio / 100))      (1:1 .. 1:10)
 *   gain   = (env < T ? env : T + (env - T) slope) / env, then through a
 *            129-slot ring read (1 - attack) * 128 samples late: attack is a
 *            look-behind delay of 0..2.9 ms, then a 0.2/0.8 one-pole
 *   audio  = HP (poles 2.7 and 151 Hz, unity at Nyquist), then a biquad
 *            b = [.9314 1.5595 .6970], a = [1 1.5595 .6284] (-4 dB near 20 kHz)
 *   out    = audio * gain * level * 6
 * The knobs are smoothed like the stock (dsp_knob_t). Other sample rates keep
 * the times (window, release, attack delay); the two filters keep their
 * 44.1 kHz coefficients (they shift up 9 % at 48 kHz). */
#include "dsp.h"

#define COMP_RING_MAX 160   /* 129 slots at 44.1 kHz, 141 at 48 kHz */

typedef struct {
    dsp_knob_t attack, thr, ratio, level;
    int thr_index;                       /* cached T for this table index */
    float T;
    float win, held, env, count, hold, release;
    float ring[COMP_RING_MAX];
    unsigned ring_len, w;
    float g;                             /* smoothed gain */
    float x1, x2, y1, y2, v1, v2;        /* HP and shelf filter state */
} comp_t;

void comp_init(comp_t *c, float fs);
void comp_set_params(comp_t *c, unsigned type, unsigned attack, unsigned threshold,
                     unsigned ratio, unsigned level);
void comp_process(comp_t *c, float *x, unsigned n);   /* in place, mono */
#endif
