/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

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

/* acc[0..3] += the products of n partitions: x rows descend, h rows ascend.
 * The four sums stay in registers across the partitions (no acc load/store
 * per partition) and are four independent FMA chains. Per sum the partition
 * order and the operations are those of the plain loop (a += x0*h0 ...), so
 * the result does not depend on this grouping. Two functions, not a flag:
 * a shared body lets the compiler share x1*h1 between the cases, and that
 * changes the FMA contraction (the last bits). Out of line: one copy each
 * (ITCM is tight). */
#define MAC_LOOP(FIRST)                                                   \
    float a0 = acc[0], a1 = acc[1], a2 = acc[2], a3 = acc[3];             \
    for (; n; n--, x -= CONV_N, h += CONV_N) {                            \
        float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];                 \
        float h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3];                 \
        FIRST                                                             \
        a2 = a2 + x2 * h2 - x3 * h3;          /* two FMAs each */         \
        a3 = a3 + x2 * h3 + x3 * h2;                                      \
    }                                                                     \
    acc[0] = a0;                                                          \
    acc[1] = a1;                                                          \
    acc[2] = a2;                                                          \
    acc[3] = a3;

/* floats 0, 1 are DC and Nyquist: real */
static __attribute__((noinline)) void
mac4_dc(float *restrict acc, const float *restrict x, const float *restrict h, unsigned n)
{
    MAC_LOOP(a0 = a0 + x0 * h0; a1 = a1 + x1 * h1;)
}

static __attribute__((noinline)) void
mac4(float *restrict acc, const float *restrict x, const float *restrict h, unsigned n)
{
    MAC_LOOP(a0 = a0 + x0 * h0 - x1 * h1; a1 = a1 + x0 * h1 + x1 * h0;)
}

/* One step of n <= DSP_BLOCK - fill samples. Packed spectra (CMSIS
 * rfft_fast): [DC, Nyquist, re1, im1, ...]; DC and Nyquist are real, so bin
 * 0 is two real products. A part block zero-pads the rest of the current
 * block: output j needs input up to j only, so the result is exact, and
 * x[head] is written again until the block is full. */
static void step(conv_t *c, const float *in, float *out, size_t n)
{
    float buf[CONV_N], acc[CONV_N];
    arm_copy_f32(in, c->in + DSP_BLOCK + c->fill, (uint32_t)n);
    arm_copy_f32(c->in, buf, CONV_N);         /* rfft_fast overwrites its input */
    arm_rfft_fast_f32(&c->fft, buf, c->x[c->head], 0);

    /* CMSIS has no complex multiply-accumulate: fused here, bin-major.
     * Partition p pairs h[p] with x[(head - p) mod cap]: rows head..0, then
     * cap-1 down (two runs of adjacent rows). */
    unsigned n1 = c->head + 1 < c->parts ? c->head + 1 : c->parts;
    unsigned n2 = c->parts - n1;
    const float *x1 = c->x[c->head], *x2 = c->x[c->cap - 1];
    const float *h1 = c->h[0], *h2 = c->h[n1];
    for (unsigned k = 0; k < CONV_N; k += 4) {
        acc[k] = acc[k + 1] = acc[k + 2] = acc[k + 3] = 0.0f;
        if (k == 0) {
            mac4_dc(acc, x1, h1, n1);
            mac4_dc(acc, x2, h2, n2);
        } else {
            mac4(acc + k, x1 + k, h1 + k, n1);
            mac4(acc + k, x2 + k, h2 + k, n2);
        }
    }

    arm_rfft_fast_f32(&c->fft, acc, buf, 1);
    arm_copy_f32(buf + DSP_BLOCK + c->fill, out, (uint32_t)n);   /* overlap-save */
    c->fill += (unsigned)n;
    c->steps++;
    if (c->fill == DSP_BLOCK) {
        arm_copy_f32(c->in + DSP_BLOCK, c->in, DSP_BLOCK);
        c->head = c->head + 1 == c->cap ? 0 : c->head + 1;
        c->fill = 0;
    }
}

#define REPHASE_MAX_PARTS 16

/* Partition phase: a step runs the whole FFT + MAC however few samples it
 * takes, so a caller whose DSP_BLOCK blocks do not start on a partition edge
 * (one short block, e.g. a SAI hiccup, shifts every later one) pays two steps
 * per block: seen on the pedal, the 512-tap cab at 20.9k cycles instead of
 * ~10.5k. After such a block (fill = k != 0) this moves the partition edge
 * to "now": the input history is taken back from the spectra (inverse FFT
 * of the stored rows; the newest 32 + k samples are exact, in c->in), cut
 * into partitions that end now, and transformed again. One-time cost ~2
 * (parts - 1) 64-point FFTs (~70k cycles for 16 parts); the next parts
 * blocks carry the FFT round trip's rounding (~1e-7 relative), then the
 * output is again that of a convolver that ran in this phase from the start. */
static __attribute__((noinline)) void rephase(conv_t *c)
{
    unsigned k = c->fill, parts = c->parts, cap = c->cap;
    if (k == 0 || parts > REPHASE_MAX_PARTS) return;
    /* s[age]: age 0 = the newest sample; 32 * parts samples */
    float s[REPHASE_MAX_PARTS * DSP_BLOCK], a[CONV_N], b[CONV_N];
    for (unsigned t = 0; t < k; t++) s[t] = c->in[DSP_BLOCK + k - 1 - t];              /* Q0 */
    for (unsigned t = 0; t < DSP_BLOCK && k + t < parts * DSP_BLOCK; t++)
        s[k + t] = c->in[DSP_BLOCK - 1 - t];                                            /* Q1 */
    /* row head - i (i >= 1) = FFT(Q_{i+1} | Q_i): Q_i for i >= 2 from its
     * second half, the oldest partial one from the first half of the last row */
    for (unsigned i = 1; i < parts; i++) {
        unsigned r = (c->head + cap - i) % cap;
        arm_copy_f32(c->x[r], a, CONV_N);
        arm_rfft_fast_f32(&c->fft, a, b, 1);
        for (unsigned half = 0; half < 2; half++) {
            unsigned q = i + 1 - half;                 /* Q_q in b[half * 32 ..] */
            if (q < 2) continue;
            for (unsigned m = 0; m < DSP_BLOCK; m++) {
                unsigned age = k + DSP_BLOCK * (q - 1) + (DSP_BLOCK - 1 - m);
                if (age < parts * DSP_BLOCK) s[age] = b[half * DSP_BLOCK + m];
            }
        }
    }
    /* new partitions P_j end now: P_j[m] = s[32 j + 31 - m]; row head - j =
     * FFT(P_j | P_{j-1}) for j = 1..parts-1, and P_0 into c->in */
    for (unsigned j = 1; j < parts; j++) {
        for (unsigned m = 0; m < DSP_BLOCK; m++) {
            a[m] = s[DSP_BLOCK * j + DSP_BLOCK - 1 - m];
            a[DSP_BLOCK + m] = s[DSP_BLOCK * (j - 1) + DSP_BLOCK - 1 - m];
        }
        arm_rfft_fast_f32(&c->fft, a, c->x[(c->head + cap - j) % cap], 0);
    }
    for (unsigned m = 0; m < DSP_BLOCK; m++) c->in[m] = s[DSP_BLOCK - 1 - m];
    c->fill = 0;
}

void conv_process(conv_t *c, const float *in, float *out, size_t n)
{
    size_t total = n;
    while (n) {
        size_t m = DSP_BLOCK - c->fill < n ? DSP_BLOCK - c->fill : n;
        step(c, in, out, m);
        in += m;
        out += m;
        n -= m;
    }
    /* a whole block that straddled a partition edge: realign (see rephase) */
    if (total == DSP_BLOCK && c->fill) rephase(c);
}
