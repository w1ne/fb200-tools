/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_DSP_CONV_H
#define FB200_DSP_CONV_H
/* Uniformly partitioned overlap-save convolution (cab IRs) on CMSIS-DSP's
 * arm_rfft_fast_f32. Partition = DSP_BLOCK, FFT = 2 * DSP_BLOCK: no latency.
 * The caller owns the spectra storage (CONV_PARTS(taps) rows each), so every
 * instance reserves only the IR length it needs. */
#include <stddef.h>
#include <stdint.h>
#include "arm_math.h"
#include "dsp.h"

#define CONV_N            (2 * DSP_BLOCK)   /* 64: conv_init uses the 64-point FFT */
#define CONV_PARTS(taps)  (((taps) + DSP_BLOCK - 1) / DSP_BLOCK)

typedef struct {
    arm_rfft_fast_instance_f32 fft;
    float (*h)[CONV_N];          /* cap IR partition spectra (packed) */
    float (*x)[CONV_N];          /* cap input spectra: ring (FDL), newest at head */
    unsigned cap, parts, head, fill;
    uint32_t steps;              /* FFT + MAC steps run (wraps): tests, profiling */
    float in[CONV_N];            /* previous block | current block (fill samples) */
} conv_t;

/* h, x: cap rows each. Starts as a unit impulse with a clear history. */
int conv_init(conv_t *c, float (*h)[CONV_N], float (*x)[CONV_N], unsigned cap);
/* New IR (taps <= cap * DSP_BLOCK). Keeps the input history, as a FIR that
 * swaps its coefficients: no reset, no click. 0 on success. */
int conv_set_ir(conv_t *c, const float *ir, size_t taps);
/* The IR partition spectra into h (CONV_PARTS(taps) rows, taps <= cap *
 * DSP_BLOCK), as conv_set_ir stores them; returns the row count. For a
 * caller that stages an IR and swaps it in later (conv2.c). */
unsigned conv_spectra(const conv_t *c, const float *ir, size_t taps, float (*h)[CONV_N]);
void conv_reset(conv_t *c);      /* clear the input history */
/* Any n, in place allowed. Full blocks (fill 0, n = DSP_BLOCK) are the fast
 * path; a part block costs a full block and stays exact (see conv.c). */
void conv_process(conv_t *c, const float *in, float *out, size_t n);
#endif
