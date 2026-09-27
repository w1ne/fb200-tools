#include <string.h>
#include "biquad.h"
#include "math.h"

void biquad_init(biquad_t *b, unsigned stages)
{
    if (stages > BIQUAD_MAX_STAGES) stages = BIQUAD_MAX_STAGES;
    memset(b, 0, sizeof *b);
    b->stages = stages;
    for (unsigned s = 0; s < stages; s++) b->coeffs[5 * s] = 1.0f;   /* b0 = 1 */
    arm_biquad_cascade_df2T_init_f32(&b->inst, (uint8_t)stages, b->coeffs, b->state);
}

void biquad_design(biquad_t *b, unsigned stage, bq_type_t type, float fs, float f0, float q,
                   float gain_db)
{
    if (stage >= b->stages) return;
    float w0 = 6.28318530718f * f0 / fs;
    float cw = arm_cos_f32(w0), sw = arm_sin_f32(w0);
    float alpha = sw / (2.0f * q);
    float A = dsp_db_to_gain(gain_db * 0.5f);                    /* 10^(dB/40) */
    float sqA2a = 0.0f;
    arm_sqrt_f32(A, &sqA2a);
    sqA2a *= 2.0f * alpha;
    float b0, b1, b2, a0, a1, a2;
    switch (type) {
    case BQ_LOWPASS:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case BQ_HIGHPASS:
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case BQ_PEAK:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
    case BQ_LOWSHELF:
        b0 = A * ((A + 1) - (A - 1) * cw + sqA2a);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sqA2a);
        a0 = (A + 1) + (A - 1) * cw + sqA2a;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sqA2a;
        break;
    case BQ_HIGHSHELF:
    default:
        b0 = A * ((A + 1) + (A - 1) * cw + sqA2a);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sqA2a);
        a0 = (A + 1) - (A - 1) * cw + sqA2a;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sqA2a;
        break;
    }
    float *c = &b->coeffs[5 * stage];
    /* CMSIS convention: y = b0 x + b1 x1 + b2 x2 + a1 y1 + a2 y2 (a negated). */
    c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = -a1 / a0; c[4] = -a2 / a0;
}

void biquad_process(biquad_t *b, const float *in, float *out, unsigned n)
{
    arm_biquad_cascade_df2T_f32(&b->inst, in, out, n);
}
