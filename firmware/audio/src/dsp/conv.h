#ifndef FB200_DSP_CONV_H
#define FB200_DSP_CONV_H
/* Uniformly partitioned overlap-save convolution (cab IRs) on CMSIS-DSP's
 * arm_rfft_fast_f32. Partition = DSP_BLOCK, FFT = 2 * DSP_BLOCK, so it adds
 * no latency; callers must pass exactly DSP_BLOCK samples per call. */
#include <stddef.h>
#include "arm_math.h"
#include "dsp.h"

#define CONV_N         (2 * DSP_BLOCK)   /* 64: conv_init uses the 64-point FFT */
#define CONV_MAX_PARTS 64                        /* 2048 taps: 43 ms at 48 kHz */
#define CONV_MAX_TAPS  (CONV_MAX_PARTS * DSP_BLOCK)

typedef struct {
    arm_rfft_fast_instance_f32 fft;
    unsigned parts, head;
    float h[CONV_MAX_PARTS][CONV_N];             /* IR partition spectra (packed) */
    float x[CONV_MAX_PARTS][CONV_N];             /* input spectra ring (FDL) */
    float prev[DSP_BLOCK];                       /* previous input block */
} conv_t;

int conv_init(conv_t *c);                        /* 0 on success */
int conv_load(conv_t *c, const float *ir, size_t taps);   /* taps <= CONV_MAX_TAPS */
void conv_process(conv_t *c, const float *in, float *out); /* DSP_BLOCK samples */
#endif
