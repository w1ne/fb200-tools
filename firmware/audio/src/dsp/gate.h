#ifndef FB200_DSP_GATE_H
#define FB200_DSP_GATE_H
/* Stock noise gate (FB200 ITCM 0xb5a0), threshold knob = preset field 0x60.
 *   t    = knob / 100 (smoothed), T = 0.01 t^2 + 1e-5, w = min(1, 10 t)
 *   d    = clamp(env - 0.4 T, 0, 3 T)        env: detector.h
 *   ctl  -> d: +1.5e-4 per sample up, -5e-5 down, capped at 3 T
 *   g    = max(ctl, one-pole 0.1/0.9 of ctl)  (smooths only the fall)
 *   out  = x * (w g / 3T + 1 - w)
 * So the gate is a soft downward expander over 0.4 T .. 3.4 T, fully open
 * above; a knob below 10 blends in the dry signal (w < 1).
 * Stock quirk kept for parity: the detector steps twice per sample (the chain
 * steps it for every sample, the gate once more), so the filter sees each
 * sample twice and the 800-step hold lasts 400 samples. */
#include "detector.h"
#include "dsp.h"

typedef struct {
    detector_t det;
    dsp_knob_t thr;                     /* knob / 100, smoothed like the stock */
    float ctl, smooth;                  /* gain ramp and its one-pole follower */
    double up, down, a, b;              /* ramp steps, follower coefficients */
} gate_t;

void gate_init(gate_t *g, float fs);
void gate_set_params(gate_t *g, unsigned threshold);   /* 0..100 */
void gate_process(gate_t *g, float *x, unsigned n);    /* in place, mono */
#endif
