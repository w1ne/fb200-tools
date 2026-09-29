#include <string.h>
#include "conv2.h"

/* Tail timing (frame f = the 256 samples being filled now, in[cur]):
 *   slice 0            X_{f-1} = FFT(in[f-2] | in[f-1]) into the newest x[] row
 *   slices 1..6        acc += G_p * X_{f-1-p}, p = 0..parts-1 (s_mac_end)
 *   slice 7 (frame end) y = second half of IFFT(acc): taps 512.. for frame
 *                      f-1, played during frame f+1 (2 frames = 512 samples
 *                      late: the head covers taps 0..511)
 * Slice k runs once DSP_BLOCK * (k + 1) samples of the frame are in, so a
 * caller with part blocks (or blocks over a frame edge) gets the same work.
 * Split, by instruction counts on the M7 (tools/conv2_cycles.py): a 512-point
 * rfft_fast is ~27k (forward) / ~29k (inverse), a partition MAC ~4.6k. The
 * FFTs cannot be split through the CMSIS API, so each gets a block of its
 * own and the 14 MACs go to blocks 1..6 (2 2 2 2 3 3): the MAC blocks stay
 * under the FFT blocks. Tail work per block, 4096 taps: 27k 9k 9k 9k 9k 14k
 * 14k 29k instructions (the head adds ~17.5k to each).
 *
 * Tail off (<= 512 taps): no tail work and no input history at all, so the
 * cab costs what the lone conv_t costs.
 *
 * Long IR load (why this way): conv2_set_ir at 4096 taps did all 14 tail
 * FFTs plus a redo of the playing frame at once, ~510k instructions: more
 * than a block period (435k cycles at 600 MHz / 44.1 kHz), an audio dropout
 * when called between blocks. Now conv2_set_ir only stages: the head spectra
 * (16 small FFTs, the cost of a 512-tap IR change today) and the tail taps,
 * into the idle half of g[]. conv2_process then runs one tail FFT per call
 * (~28k), never in a block that has slice 0 or 7 (tail on), and the swap
 * needs no extra work: a new tail that starts to accumulate at slice 0 of
 * frame f is the whole of y for frame f+1, so switching G at slice 0 of f
 * and the head at the start of f+1 is a FIR swap at the start of f+1,
 * exact. The work stays in the audio code path (deterministic, host
 * testable, no main loop hook); a crossfade would need two convolvers. */
static const unsigned char s_mac_end[CONV2_SLICES] = {0, 2, 4, 6, 8, 11, 14, 14};   /* cumulative */

#define XROWS CONV2_TAIL_PARTS

static unsigned xrow(const conv2_t *c, unsigned back)   /* back < XROWS */
{
    return (c->xhead + XROWS - back) % XROWS;
}

/* acc += sum G_p * X_{newest - p} for p in [p0, p1). Fused complex MAC as conv.c. */
static void mac(conv2_t *c, unsigned p0, unsigned p1)
{
    conv2_tail_t *t = c->t;
    float *restrict a = t->acc;
    for (unsigned p = p0; p < p1; p++) {
        const float *restrict x = t->x[xrow(c, p)], *restrict h = t->g[c->gi][p];
        a[0] += x[0] * h[0];                  /* DC and Nyquist: real */
        a[1] += x[1] * h[1];
        for (unsigned k = 2; k < CONV2_N; k += 2) {
            float xr = x[k], xi = x[k + 1], hr = h[k], hi = h[k + 1];
            a[k]     = a[k] + xr * hr - xi * hi;
            a[k + 1] = a[k + 1] + xr * hi + xi * hr;
        }
    }
}

/* The staged head goes live (as conv_set_ir: the head keeps its history). */
__attribute__((noinline)) static void head_swap(conv2_t *c)   /* cold: out of the block path */
{
    arm_copy_f32(c->t->hs[0], c->hh[0], CONV2_HEAD_PARTS * CONV_N);
    c->head.parts = CONV2_HEAD_PARTS;
    c->load = CONV2_LOAD_IDLE;
    c->swap_n = c->n;
}

/* One tail partition of the IR being loaded: its taps -> its spectrum. */
static void load_step(conv2_t *c)
{
    conv2_tail_t *t = c->t;
    float *row = t->g[c->gi ^ 1][c->lnext];
    arm_copy_f32(row, t->work, CONV2_B);
    arm_fill_f32(0.0f, t->work + CONV2_B, CONV2_B);
    arm_rfft_fast_f32(&c->fft, t->work, row, 0);
    if (++c->lnext < c->lparts) return;
    if (c->parts) {
        c->load = CONV2_LOAD_READY;           /* switch at the next slice 0 */
    } else {                                  /* tail off: on now, empty history */
        c->gi ^= 1;
        c->parts = c->lparts;
        head_swap(c);
    }
}

static void run_slice(conv2_t *c, unsigned k)
{
    conv2_tail_t *t = c->t;
    if (k == 0) {
        if (c->load == CONV2_LOAD_READY) {   /* new tail: all of the next y */
            c->gi ^= 1;
            c->parts = c->lparts;
            c->load = CONV2_LOAD_HEAD;
        }
        arm_copy_f32(t->in[(c->cur + 1) % 3], t->work, CONV2_B);
        arm_copy_f32(t->in[(c->cur + 2) % 3], t->work + CONV2_B, CONV2_B);
        c->xhead = (c->xhead + 1) % XROWS;
        arm_rfft_fast_f32(&c->fft, t->work, t->x[c->xhead], 0);
        c->mac = 0;
        arm_fill_f32(0.0f, t->acc, CONV2_N);
    }
    unsigned end = k + 1 == CONV2_SLICES || s_mac_end[k] > c->parts ? c->parts : s_mac_end[k];
    if (end > c->mac) {
        mac(c, c->mac, end);
        c->mac = end;
    }
    if (k + 1 == CONV2_SLICES) {
        arm_rfft_fast_f32(&c->fft, t->acc, t->work, 1);       /* overwrites acc */
        arm_copy_f32(t->work + CONV2_B, t->y, CONV2_B);         /* overlap-save */
    }
}

/* n samples went into in[cur] (n <= CONV2_B - pos): run the slices due. */
static void advance(conv2_t *c, size_t n)
{
    c->pos += (unsigned)n;
    while (c->slice < CONV2_SLICES && c->pos >= DSP_BLOCK * (c->slice + 1))
        run_slice(c, c->slice++);
    if (c->pos == CONV2_B) {
        c->cur = (c->cur + 1) % 3;
        c->pos = c->slice = 0;
        if (c->load == CONV2_LOAD_HEAD) head_swap(c);   /* new IR from here */
    }
}

static void tail_clear(conv2_t *c)
{
    conv2_tail_t *t = c->t;
    if (t) {
        arm_fill_f32(0.0f, t->x[0], XROWS * CONV2_N);
        arm_fill_f32(0.0f, t->in[0], 3 * CONV2_B);
        arm_fill_f32(0.0f, t->acc, CONV2_N);
        arm_fill_f32(0.0f, t->y, CONV2_B);
    }
    c->pos = c->slice = c->mac = c->cur = c->xhead = 0;
}

static int init_start(conv2_t *c)
{
    conv2_reset(c);
    const float one = 1.0f;
    return conv2_set_ir(c, &one, 1);
}

int conv2_init_head(conv2_t *c)
{
    memset(c, 0, sizeof *c);
    if (conv_init(&c->head, c->hh, c->hx, CONV2_HEAD_PARTS) != 0) return -1;
    return init_start(c);
}

int conv2_init(conv2_t *c, conv2_tail_t *tail)
{
    memset(c, 0, sizeof *c);
    c->t = tail;
    if (conv_init(&c->head, c->hh, c->hx, CONV2_HEAD_PARTS) != 0) return -1;
    /* size-specific init: links only the 512-point tables */
    if (tail && arm_rfft_fast_init_512_f32(&c->fft) != ARM_MATH_SUCCESS) return -1;
    return init_start(c);
}

void conv2_reset(conv2_t *c)
{
    conv_reset(&c->head);
    tail_clear(c);                            /* restarts the frame clock */
    if (c->load == CONV2_LOAD_HEAD) head_swap(c);   /* its tail is live, y is 0 */
}

int conv2_set_ir(conv2_t *c, const float *ir, size_t taps)
{
    if (taps == 0 || taps > (c->t ? (size_t)CONV2_MAX_TAPS : (size_t)CONV2_HEAD_TAPS)) return -1;
    if (taps <= CONV2_HEAD_TAPS) {            /* instant: head only, tail off */
        if (conv_set_ir(&c->head, ir, taps) != 0) return -1;
        c->parts = 0;
        c->load = CONV2_LOAD_IDLE;
        c->swap_n = c->n;
        return 0;
    }
    conv2_tail_t *t = c->t;
    /* the tail already switched to the previous IR: its head goes live now
     * (early by less than a frame), hs is needed for this one */
    if (c->load == CONV2_LOAD_HEAD) head_swap(c);
    (void)conv_spectra(&c->head, ir, CONV2_HEAD_TAPS, t->hs);
    unsigned parts = (unsigned)((taps - CONV2_HEAD_TAPS + CONV2_B - 1) / CONV2_B);
    for (unsigned p = 0; p < parts; p++) {   /* taps into the idle g[] rows */
        size_t off = CONV2_HEAD_TAPS + (size_t)p * CONV2_B;
        size_t n = taps - off < CONV2_B ? taps - off : CONV2_B;
        float *row = t->g[c->gi ^ 1][p];
        arm_copy_f32(ir + off, row, (uint32_t)n);
        if (n < CONV2_B) arm_fill_f32(0.0f, row + n, CONV2_B - (uint32_t)n);
    }
    c->lparts = parts;
    c->lnext = 0;
    c->load = CONV2_LOAD_FFT;
    if (!c->parts) tail_clear(c);            /* tail off: it starts empty */
    return 0;
}

void conv2_finish(conv2_t *c)
{
    while (c->load == CONV2_LOAD_FFT) load_step(c);
}

void conv2_process(conv2_t *c, const float *in, float *out, size_t n)
{
    if (!c->parts) {                          /* tail off: the lone conv_t */
        conv_process(&c->head, in, out, n);
        c->n += (uint32_t)n;
        if (c->load == CONV2_LOAD_FFT && n) load_step(c);   /* may turn the tail on */
        return;
    }
    if (c->load == CONV2_LOAD_FFT && n && c->slice != 0 && c->slice != CONV2_SLICES - 1)
        load_step(c);
    while (n) {
        size_t m = CONV2_B - c->pos < n ? CONV2_B - c->pos : n;
        const float *y = c->t->y + c->pos;
        arm_copy_f32(in, c->t->in[c->cur] + c->pos, (uint32_t)m);   /* before in place */
        conv_process(&c->head, in, out, m);
        for (size_t i = 0; i < m; i++) out[i] += y[i];
        c->n += (uint32_t)m;
        advance(c, m);                       /* may rewrite y at the frame end */
        in += m;
        out += m;
        n -= m;
    }
}
