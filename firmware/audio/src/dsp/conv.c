#include <string.h>
#include "conv.h"

int conv_init(conv_t *c, float (*h)[CONV_N], float (*x)[CONV_N], unsigned cap)
{
    memset(c, 0, sizeof *c);
    c->h = h;
    c->x = x;
    c->cap = cap;
    /* size-specific init: links only the 64-point tables (ITCM is tight) */
    if (cap == 0 || arm_rfft_fast_init_64_f32(&c->fft) != ARM_MATH_SUCCESS) return -1;
    conv_reset(c);
    const float one = 1.0f;
    return conv_set_ir(c, &one, 1);
}

void conv_reset(conv_t *c)
{
    memset(c->x, 0, c->cap * sizeof c->x[0]);
    memset(c->in, 0, sizeof c->in);
    c->head = c->fill = 0;
}

unsigned conv_spectra(const conv_t *c, const float *ir, size_t taps, float (*h)[CONV_N])
{
    float buf[CONV_N];
    unsigned parts = CONV_PARTS(taps);
    for (unsigned p = 0; p < parts; p++) {
        memset(buf, 0, sizeof buf);
        size_t n = taps - p * DSP_BLOCK < DSP_BLOCK ? taps - p * DSP_BLOCK : DSP_BLOCK;
        memcpy(buf, ir + p * DSP_BLOCK, n * sizeof(float));
        arm_rfft_fast_f32(&c->fft, buf, h[p], 0);
    }
    return parts;
}

int conv_set_ir(conv_t *c, const float *ir, size_t taps)
{
    if (taps == 0 || taps > (size_t)c->cap * DSP_BLOCK) return -1;
    /* the x ring is left alone: its spectra do not depend on the IR */
    c->parts = conv_spectra(c, ir, taps, c->h);
    return 0;
}

/* One step of n <= DSP_BLOCK - fill samples. Packed spectra (CMSIS
 * rfft_fast): [DC, Nyquist, re1, im1, ...]; DC and Nyquist are real, so bin
 * 0 is two real products. A part block zero-pads the rest of the current
 * block: output j needs input up to j only, so the result is exact, and
 * x[head] is written again until the block is full. */
static void step(conv_t *c, const float *in, float *out, size_t n)
{
    float buf[CONV_N], acc[CONV_N];
    memcpy(c->in + DSP_BLOCK + c->fill, in, n * sizeof(float));
    memcpy(buf, c->in, sizeof buf);           /* rfft_fast overwrites its input */
    arm_rfft_fast_f32(&c->fft, buf, c->x[c->head], 0);

    /* CMSIS has no complex multiply-accumulate: fused here, it saves a tmp
     * pass (mult to tmp, then add) over every partition */
    memset(acc, 0, sizeof acc);
    float *restrict a = acc;
    unsigned i = c->head;
    for (unsigned p = 0; p < c->parts; p++) {
        const float *restrict x = c->x[i], *restrict h = c->h[p];
        a[0] += x[0] * h[0];
        a[1] += x[1] * h[1];
        for (unsigned k = 2; k < CONV_N; k += 2) {
            float xr = x[k], xi = x[k + 1], hr = h[k], hi = h[k + 1];
            a[k]     = a[k] + xr * hr - xi * hi;          /* two FMAs each */
            a[k + 1] = a[k + 1] + xr * hi + xi * hr;
        }
        i = i ? i - 1 : c->cap - 1;
    }

    arm_rfft_fast_f32(&c->fft, acc, buf, 1);
    memcpy(out, buf + DSP_BLOCK + c->fill, n * sizeof(float));   /* overlap-save */
    c->fill += (unsigned)n;
    if (c->fill == DSP_BLOCK) {
        memcpy(c->in, c->in + DSP_BLOCK, DSP_BLOCK * sizeof(float));
        c->head = c->head + 1 == c->cap ? 0 : c->head + 1;
        c->fill = 0;
    }
}

void conv_process(conv_t *c, const float *in, float *out, size_t n)
{
    while (n) {
        size_t m = DSP_BLOCK - c->fill < n ? DSP_BLOCK - c->fill : n;
        step(c, in, out, m);
        in += m;
        out += m;
        n -= m;
    }
}
