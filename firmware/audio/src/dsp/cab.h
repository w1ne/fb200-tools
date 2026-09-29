#ifndef FB200_DSP_CAB_H
#define FB200_DSP_CAB_H
/* Stock FB200 cab: 512-tap FIR, output * gain * 1.15. Runs on the
 * two-stage convolver (conv2.c: no latency; with <= 512 taps the lone
 * partitioned FFT convolver of conv.c, bit-identical and the same cost as
 * before); the stock runs it direct form. Cabs 1..10 are the stock IRs;
 * user IRs (stock slots 11..19) get their gain from cab_user_ir_gain().
 * Starts flat (pass-through); without stock data cab_set_model() fails and
 * it stays flat. Long IRs (M5, up to CAB_MAX_TAPS) need the tail storage
 * (cab_init_long); stock and user slots are 512 taps. */
#include "arm_math.h"
#include "conv2.h"
#include "dsp.h"

#define CAB_TAPS     512
#define CAB_MAX_TAPS CONV2_MAX_TAPS

typedef struct {
    conv2_t conv;                            /* head spectra in here (DTCM) */
    float scale;                             /* gain * 1.15 */
    float next_scale;                        /* for a long IR still loading */
    int scale_due;
    int active;
} cab_t;

void cab_init(cab_t *c);                     /* <= 512 taps; links no long-IR code */
/* With the tail storage (~96 kB, OCRAM): IRs up to CAB_MAX_TAPS. */
void cab_init_long(cab_t *c, conv2_tail_t *tail);
/* The tail storage lent / given back (conv2_detach_tail / _attach_tail):
 * a long IR plays its first 512 taps meanwhile. */
void cab_detach_tail(cab_t *c);
void cab_attach_tail(cab_t *c, conv2_tail_t *tail);
int cab_set_model(cab_t *c, int cab);        /* 1..10; 0 on success */
/* Load an IR (CAB_TAPS samples, natural order) with the stock output gain. */
void cab_set_ir(cab_t *c, const float *ir, float gain);
/* Any length 1..CAB_MAX_TAPS (<= CAB_TAPS without the tail). Up to 512
 * taps it takes effect at once; a longer IR loads over ~15-25 ms of audio
 * and the gain changes with it (conv2.h). 0 on success. */
int cab_set_ir_len(cab_t *c, const float *ir, unsigned taps, float gain);
/* Stock gain for a user IR (ITCM 0x5918): x = ir[0..511] * Hann (1026-point,
 * odd samples), 512-point CFFT, gain = 100 / sum_{k=1..85} sqrt|X[k-1]| *
 * (30/k + 1); 1 when the sum is 0. */
float cab_user_ir_gain(const float *ir);
/* In place, n <= DSP_BLOCK. n < DSP_BLOCK is exact but costs a full block. */
void cab_process(cab_t *c, float *x, unsigned n);
#endif
