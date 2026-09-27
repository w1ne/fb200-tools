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

void tone_init(tone_t *t);                               /* flat */
/* bass, mid, treble: knob units 0..100; midfreq 0..4 = 200/400/800/1600/3000 Hz */
void tone_set(tone_t *t, int bass, int mid, int midfreq, int treble);
void tone_process(tone_t *t, float *x, unsigned n);      /* in place */
#endif
