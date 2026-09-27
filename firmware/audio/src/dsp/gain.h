#ifndef FB200_DSP_GAIN_H
#define FB200_DSP_GAIN_H
#include "dsp.h"
typedef struct { dsp_smooth_t gain; } gain_ctx_t;
void gain_init(gain_ctx_t *c, float value);
void gain_set(gain_ctx_t *c, float value);
void gain_process(void *ctx, dsp_block_t *b, size_t n);
#endif
