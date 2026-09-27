#include <string.h>
#include "conv.h"

int conv_init(conv_t *c)
{
    memset(c, 0, sizeof *c);
    /* size-specific init: links only the 64-point tables (ITCM is tight) */
    return arm_rfft_fast_init_64_f32(&c->fft) == ARM_MATH_SUCCESS ? 0 : -1;
}

int conv_load(conv_t *c, const float *ir, size_t taps)
{
    if (taps == 0 || taps > CONV_MAX_TAPS) return -1;
    float buf[CONV_N];
    c->parts = (unsigned)((taps + DSP_BLOCK - 1) / DSP_BLOCK);
    for (unsigned p = 0; p < c->parts; p++) {
        memset(buf, 0, sizeof buf);
        size_t n = taps - p * DSP_BLOCK < DSP_BLOCK ? taps - p * DSP_BLOCK : DSP_BLOCK;
        memcpy(buf, ir + p * DSP_BLOCK, n * sizeof(float));
        arm_rfft_fast_f32(&c->fft, buf, c->h[p], 0);
    }
    memset(c->x, 0, sizeof c->x);
    memset(c->prev, 0, sizeof c->prev);
    c->head = 0;
    return 0;
}

/* Packed spectra (CMSIS rfft_fast): [DC, Nyquist, re1, im1, ...]. The
 * complex multiply treats slot 0 as one complex number, so DC and Nyquist
 * (both real) are fixed up separately. */
void conv_process(conv_t *c, const float *in, float *out)
{
    float buf[CONV_N], acc[CONV_N], tmp[CONV_N];
    memcpy(buf, c->prev, sizeof c->prev);
    memcpy(buf + DSP_BLOCK, in, DSP_BLOCK * sizeof(float));
    memcpy(c->prev, in, DSP_BLOCK * sizeof(float));
    arm_rfft_fast_f32(&c->fft, buf, c->x[c->head], 0);

    memset(acc, 0, sizeof acc);
    for (unsigned p = 0; p < c->parts; p++) {
        const float *x = c->x[(c->head + CONV_MAX_PARTS - p) % CONV_MAX_PARTS];
        const float *h = c->h[p];
        arm_cmplx_mult_cmplx_f32(x, h, tmp, CONV_N / 2);
        tmp[0] = x[0] * h[0];
        tmp[1] = x[1] * h[1];
        arm_add_f32(acc, tmp, acc, CONV_N);
    }
    c->head = (c->head + 1) % CONV_MAX_PARTS;

    arm_rfft_fast_f32(&c->fft, acc, buf, 1);
    memcpy(out, buf + DSP_BLOCK, DSP_BLOCK * sizeof(float));   /* overlap-save */
}
