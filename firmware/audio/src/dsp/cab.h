#ifndef FB200_DSP_CAB_H
#define FB200_DSP_CAB_H
/* Stock FB200 cab: 512-tap FIR, output * gain * 1.15. Runs on the
 * partitioned FFT convolver (conv.c: no latency, about a quarter of the
 * direct FIR's CPU); the stock runs it direct form. Cabs 1..10 are the stock IRs; user IRs
 * (stock slots 11..19) get their gain from cab_user_ir_gain(). Starts flat
 * (pass-through); without stock data cab_set_model() fails and it stays flat. */
#include "arm_math.h"
#include "conv.h"
#include "dsp.h"

#define CAB_TAPS 512
/* conv capacity: the spectra (2 x 256 B per partition) sit in DTCM.
 * Longer IRs (M5) raise this. */
#define CAB_PARTS CONV_PARTS(CAB_TAPS)

typedef struct {
    conv_t conv;
    float h[CAB_PARTS][CONV_N], x[CAB_PARTS][CONV_N];   /* conv storage */
    float scale;                             /* gain * 1.15 */
    int active;
} cab_t;

void cab_init(cab_t *c);
int cab_set_model(cab_t *c, int cab);        /* 1..10; 0 on success */
/* Load an IR (CAB_TAPS samples, natural order) with the stock output gain. */
void cab_set_ir(cab_t *c, const float *ir, float gain);
/* Stock gain for a user IR (ITCM 0x5918): x = ir[0..511] * Hann (1026-point,
 * odd samples), 512-point CFFT, gain = 100 / sum_{k=1..85} sqrt|X[k-1]| *
 * (30/k + 1); 1 when the sum is 0. */
float cab_user_ir_gain(const float *ir);
/* In place, n <= DSP_BLOCK. n < DSP_BLOCK is exact but costs a full block. */
void cab_process(cab_t *c, float *x, unsigned n);
#endif
