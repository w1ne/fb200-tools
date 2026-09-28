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
 * 14k 29k instructions (the head adds ~17.7k to each).
 * Tail off (<= 512 taps): slice 0 still runs the forward FFT, so an IR
 * change to a long IR plays its tail from the first sample (exact swap). */
static const unsigned char s_mac_end[CONV2_SLICES] = {0, 2, 4, 6, 8, 11, 14, 14};   /* cumulative */

#define XROWS (CONV2_TAIL_PARTS + 1)

static unsigned xrow(const conv2_t *c, unsigned back)   /* back <= XROWS - 1 */
{
    return (c->xhead + XROWS - back) % XROWS;
}

/* acc += sum G_p * X[x0 - p] for p in [p0, p1). Fused complex MAC as conv.c. */
static void mac(conv2_t *c, unsigned x0, unsigned p0, unsigned p1)
{
    conv2_tail_t *t = c->t;
    float *restrict a = t->acc;
    for (unsigned p = p0; p < p1; p++) {
        const float *restrict x = t->x[xrow(c, x0 + p)], *restrict h = t->g[p];
        a[0] += x[0] * h[0];                  /* DC and Nyquist: real */
        a[1] += x[1] * h[1];
        for (unsigned k = 2; k < CONV2_N; k += 2) {
            float xr = x[k], xi = x[k + 1], hr = h[k], hi = h[k + 1];
            a[k]     = a[k] + xr * hr - xi * hi;
            a[k + 1] = a[k + 1] + xr * hi + xi * hr;
        }
    }
}

static void ifft_to_y(conv2_t *c)
{
    conv2_tail_t *t = c->t;
    arm_rfft_fast_f32(&c->fft, t->acc, t->work, 1);        /* overwrites acc */
    arm_copy_f32(t->work + CONV2_B, t->y, CONV2_B);          /* overlap-save */
}

static void run_slice(conv2_t *c, unsigned k)
{
    conv2_tail_t *t = c->t;
    if (k == 0) {
        arm_copy_f32(t->in[(c->cur + 1) % 3], t->work, CONV2_B);
        arm_copy_f32(t->in[(c->cur + 2) % 3], t->work + CONV2_B, CONV2_B);
        c->xhead = (c->xhead + 1) % XROWS;
        arm_rfft_fast_f32(&c->fft, t->work, t->x[c->xhead], 0);
        c->mac = 0;
        if (c->parts) arm_fill_f32(0.0f, t->acc, CONV2_N);
    }
    if (!c->parts) return;                   /* tail off: keep the history only */
    unsigned end = k + 1 == CONV2_SLICES || s_mac_end[k] > c->parts ? c->parts : s_mac_end[k];
    if (end > c->mac) {
        mac(c, 0, c->mac, end);
        c->mac = end;
    }
    if (k + 1 == CONV2_SLICES) ifft_to_y(c);
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
    }
}

int conv2_init(conv2_t *c, conv2_tail_t *tail)
{
    memset(c, 0, sizeof *c);
    c->t = tail;
    if (conv_init(&c->head, c->hh, c->hx, CONV2_HEAD_PARTS) != 0) return -1;
    /* size-specific init: links only the 512-point tables */
    if (tail && arm_rfft_fast_init_512_f32(&c->fft) != ARM_MATH_SUCCESS) return -1;
    conv2_reset(c);
    const float one = 1.0f;
    return conv2_set_ir(c, &one, 1);
}

void conv2_reset(conv2_t *c)
{
    conv_reset(&c->head);
    conv2_tail_t *t = c->t;
    if (t) {
        arm_fill_f32(0.0f, t->x[0], (CONV2_TAIL_PARTS + 1) * CONV2_N);
        arm_fill_f32(0.0f, t->in[0], 3 * CONV2_B);
        arm_fill_f32(0.0f, t->acc, CONV2_N);
        arm_fill_f32(0.0f, t->y, CONV2_B);
    }
    c->pos = c->slice = c->mac = c->cur = c->xhead = 0;
}

/* Cost (4096 taps): 16 64-point FFTs for the head, 14 + 1 512-point FFTs
 * for the tail spectra and the output of this frame, up to 2 x 14 partition
 * MACs: ~510k instructions, more than one block period. Call it from the
 * main loop, not in the audio block; spreading it over blocks (double
 * buffered spectra) is later work. */
int conv2_set_ir(conv2_t *c, const float *ir, size_t taps)
{
    if (taps == 0 || taps > (c->t ? (size_t)CONV2_MAX_TAPS : (size_t)CONV2_HEAD_TAPS)) return -1;
    size_t head = taps < CONV2_HEAD_TAPS ? taps : CONV2_HEAD_TAPS;
    if (conv_set_ir(&c->head, ir, head) != 0) return -1;
    if (!c->t) return 0;

    conv2_tail_t *t = c->t;
    unsigned parts = (unsigned)((taps - head + CONV2_B - 1) / CONV2_B);
    for (unsigned p = 0; p < parts; p++) {
        size_t off = CONV2_HEAD_TAPS + (size_t)p * CONV2_B;
        size_t n = taps - off < CONV2_B ? taps - off : CONV2_B;
        arm_fill_f32(0.0f, t->work + n, CONV2_N - (uint32_t)n);
        arm_copy_f32(ir + off, t->work, (uint32_t)n);
        arm_rfft_fast_f32(&c->fft, t->work, t->g[p], 0);
    }
    c->parts = parts;
    if (!parts) return 0;

    /* As a FIR with swapped coefficients, the new tail applies from this
     * sample on. The rest of this frame plays y = frame f-2: redo it (its
     * spectra are X_{f-2}.., one row further back once slice 0 ran). Then
     * the partial sum for the next frame, if slice 0 started it. */
    unsigned back = c->slice ? 1 : 0;
    arm_fill_f32(0.0f, t->acc, CONV2_N);
    mac(c, back, 0, parts);
    ifft_to_y(c);
    arm_fill_f32(0.0f, t->acc, CONV2_N);
    c->mac = 0;
    if (c->slice) {
        unsigned end = s_mac_end[c->slice - 1] < parts ? s_mac_end[c->slice - 1] : parts;
        mac(c, 0, 0, end);
        c->mac = end;
    }
    return 0;
}

/* Tail off: take the input into the history, then the head on all n
 * samples at once, so the output is bit-identical to a lone conv_t. */
static void capture(conv2_t *c, const float *in, size_t n)
{
    while (n) {
        size_t m = CONV2_B - c->pos < n ? CONV2_B - c->pos : n;
        arm_copy_f32(in, c->t->in[c->cur] + c->pos, (uint32_t)m);
        advance(c, m);
        in += m;
        n -= m;
    }
}

void conv2_process(conv2_t *c, const float *in, float *out, size_t n)
{
    if (!c->t) {
        conv_process(&c->head, in, out, n);
        return;
    }
    if (!c->parts) {
        capture(c, in, n);
        conv_process(&c->head, in, out, n);
        return;
    }
    while (n) {
        size_t m = CONV2_B - c->pos < n ? CONV2_B - c->pos : n;
        const float *y = c->t->y + c->pos;
        arm_copy_f32(in, c->t->in[c->cur] + c->pos, (uint32_t)m);   /* before in place */
        conv_process(&c->head, in, out, m);
        for (size_t i = 0; i < m; i++) out[i] += y[i];
        advance(c, m);                       /* may rewrite y at the frame end */
        in += m;
        out += m;
        n -= m;
    }
}
