#ifndef FB200_DSP_TESTGEN_H
#define FB200_DSP_TESTGEN_H
#include <stdint.h>
#include "dsp.h"
typedef enum { TESTGEN_OFF = 0, TESTGEN_SINE, TESTGEN_WHITE, TESTGEN_IMPULSE } testgen_mode_t;
typedef struct {
    testgen_mode_t mode;
    float amp, phase, step, fs;
    uint32_t rng, impulse_countdown;
} testgen_ctx_t;
void testgen_init(testgen_ctx_t *c, float fs);
void testgen_set(testgen_ctx_t *c, testgen_mode_t mode, float amp, float freq);
void testgen_process(void *ctx, dsp_block_t *b, size_t n);
#endif
