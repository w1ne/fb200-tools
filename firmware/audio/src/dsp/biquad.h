#ifndef FB200_DSP_BIQUAD_H
#define FB200_DSP_BIQUAD_H
/* EQ: RBJ Audio-EQ-Cookbook designs run through CMSIS-DSP's
 * arm_biquad_cascade_df2T_f32 (one channel per instance). */
#include "arm_math.h"

#define BIQUAD_MAX_STAGES 8

typedef enum { BQ_LOWPASS, BQ_HIGHPASS, BQ_PEAK, BQ_LOWSHELF, BQ_HIGHSHELF } bq_type_t;

typedef struct {
    arm_biquad_cascade_df2T_instance_f32 inst;
    float coeffs[5 * BIQUAD_MAX_STAGES];
    float state[2 * BIQUAD_MAX_STAGES];
    unsigned stages;
} biquad_t;

void biquad_init(biquad_t *b, unsigned stages);          /* all stages pass-through */
/* Design one stage: f0 in Hz, q, gain_db (peak/shelves). */
void biquad_design(biquad_t *b, unsigned stage, bq_type_t type, float fs, float f0, float q,
                   float gain_db);
void biquad_process(biquad_t *b, const float *in, float *out, unsigned n);
#endif
