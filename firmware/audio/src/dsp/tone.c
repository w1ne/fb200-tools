#include <string.h>
#include "tone.h"
#include "stock_dsp_data.h"

#define PRESENCE_STEP 15   /* stock: presence is fixed at knob 50 */

void tone_init(tone_t *t)
{
    memset(t, 0, sizeof *t);
    for (unsigned s = 0; s < TONE_STAGES; s++) t->coeffs[5 * s] = 1.0f;
    arm_biquad_cascade_df1_init_f32(&t->inst, TONE_STAGES, t->coeffs, t->state);
}

#ifdef FB200_STOCK_DSP
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* stock: (int)(knob fraction * 31), in float */
static int step(int knob) { return (int)(knob_fraction(knob) * 31.0f); }

void tone_set(tone_t *t, int bass, int mid, int midfreq, int treble)
{
    const float *rows[TONE_STAGES] = {
        stock_tone_bass[step(bass)],
        stock_tone_mid[clampi(midfreq, 0, STOCK_TONE_MID_BANKS - 1)][step(mid)],
        stock_tone_presence[PRESENCE_STEP],
        stock_tone_treble[step(treble)],
    };
    for (unsigned s = 0; s < TONE_STAGES; s++) memcpy(&t->coeffs[5 * s], rows[s], 5 * sizeof(float));
    t->active = 1;
}
#else
void tone_set(tone_t *t, int bass, int mid, int midfreq, int treble)
{
    (void)t; (void)bass; (void)mid; (void)midfreq; (void)treble;
}
#endif

void tone_process(tone_t *t, float *x, unsigned n)
{
    if (t->active) arm_biquad_cascade_df1_f32(&t->inst, x, x, n);
}
