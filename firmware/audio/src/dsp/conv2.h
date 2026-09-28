#ifndef FB200_DSP_CONV2_H
#define FB200_DSP_CONV2_H
/* Two-stage convolver for IRs up to CONV2_MAX_TAPS (M5).
 *
 * Head: taps 0..511 on the conv_t of conv.c (32-sample partitions, 64-point
 * FFT, no latency), the same code and calls as the 512-tap cab.
 *
 * Tail: taps 512..4095 in 256-sample partitions (14) on a 512-point
 * arm_rfft_fast. One frame is 256 samples = 8 audio blocks. The tail result
 * for a frame is computed during the next frame, a slice per block (see
 * conv2.c), and played one frame later again: 2 x 256 = 512 samples of delay,
 * which is exactly the head length. So the sum has no added latency.
 *
 * Tail off (IR <= 512 taps): the tail does no work at all, the cost and the
 * output are those of a lone conv_t (bit-identical).
 *
 * IR changes (conv2_set_ir):
 * - <= 512 taps: instant and exact (a FIR that swaps its coefficients at
 *   this sample); the tail goes off.
 * - > 512 taps: the new tail spectra are computed one partition per
 *   conv2_process call (double buffer), and the head spectra are staged by
 *   conv2_set_ir. Then head and tail swap together. Tail on before: exact
 *   swap at a frame boundary (the new tail starts to accumulate one frame
 *   before), ~15..25 ms after the call. Tail off before: swap at the end of
 *   the call that finishes the load (~14 blocks); the tail starts with an
 *   empty history, so taps 512.. fade in over the tail length.
 *
 * Storage: conv2_t holds the head (8 kB of spectra, 4 kB each for h and x,
 * as cab_t did before) and the state. The tail arrays (~96 kB) are a separate
 * conv2_tail_t so the caller can place them (the engine's: the low DTCM,
 * linker.ld .dtcm_lo). */
#include <stddef.h>
#include <stdint.h>
#include "arm_math.h"
#include "conv.h"
#include "dsp.h"

#define CONV2_HEAD_TAPS  512
#define CONV2_HEAD_PARTS CONV_PARTS(CONV2_HEAD_TAPS)          /* 16 */
#define CONV2_B          256                                  /* tail partition = frame */
#define CONV2_N          (2 * CONV2_B)                        /* 512-point FFT */
#define CONV2_MAX_TAPS   4096
#define CONV2_TAIL_PARTS ((CONV2_MAX_TAPS - CONV2_HEAD_TAPS) / CONV2_B)   /* 14 */
#define CONV2_SLICES     (CONV2_B / DSP_BLOCK)                /* 8 blocks per frame */

typedef struct {
    /* tail IR partition spectra (packed): [gi] plays, [gi ^ 1] loads. A row
     * being loaded holds its 256 taps until its FFT runs. */
    float g[2][CONV2_TAIL_PARTS][CONV2_N];
    float x[CONV2_TAIL_PARTS][CONV2_N];       /* input spectra ring, newest at xhead */
    float hs[CONV2_HEAD_PARTS][CONV_N];       /* head spectra of the IR being loaded */
    float in[3][CONV2_B];                     /* input frames: ring of 3 */
    float acc[CONV2_N];                       /* spectrum being accumulated */
    float work[CONV2_N];                      /* FFT scratch (rfft_fast overwrites its input) */
    float y[CONV2_B];                         /* tail output playing in this frame */
} conv2_tail_t;

typedef struct {
    conv_t head;
    float hh[CONV2_HEAD_PARTS][CONV_N], hx[CONV2_HEAD_PARTS][CONV_N];   /* head storage */
    conv2_tail_t *t;                          /* NULL: head only (<= 512 taps) */
    arm_rfft_fast_instance_f32 fft;           /* 512-point */
    unsigned parts;                           /* active tail partitions, 0 = tail off */
    unsigned pos;                             /* samples into the current frame */
    unsigned slice;                           /* slices done in this frame */
    unsigned mac;                             /* partitions accumulated in this frame */
    unsigned cur;                             /* in[] row being filled */
    unsigned xhead;                           /* x[] row of the newest spectrum */
    unsigned gi;                              /* g[] set that plays */
    unsigned load;                            /* CONV2_LOAD_* */
    unsigned lparts, lnext;                   /* IR being loaded: tail partitions, next FFT */
    uint32_t n;                               /* samples processed (wraps) */
    uint32_t swap_n;                          /* n at which the last IR took effect */
} conv2_t;

enum { CONV2_LOAD_IDLE, CONV2_LOAD_FFT, CONV2_LOAD_READY, CONV2_LOAD_HEAD };

/* tail may be NULL (then taps <= 512). Starts as a unit impulse, clear history. */
int conv2_init(conv2_t *c, conv2_tail_t *tail);
/* New IR, taps 1..CONV2_MAX_TAPS (<= 512 without a tail); ir is copied, the
 * caller may reuse it at once. See the top of this file for when it takes
 * effect. Cost: ~60k instructions (the 16 head FFTs, as the 512-tap cab),
 * the tail FFTs run later in conv2_process. Call it between blocks. 0 on
 * success. */
int conv2_set_ir(conv2_t *c, const float *ir, size_t taps);
/* A long IR load still pending (the IR in use is the previous one). */
static inline int conv2_pending(const conv2_t *c) { return c->load != CONV2_LOAD_IDLE; }
/* Run the rest of a pending load's FFTs now (~27k instructions each). With
 * the tail off the new IR takes effect at once; with the tail on it still
 * waits for its frame boundary. For init and tests, not the audio path. */
void conv2_finish(conv2_t *c);
void conv2_reset(conv2_t *c);                /* clear the input history */
/* Any n, in place allowed. The tail work runs on sample counts, so part
 * blocks keep the schedule. */
void conv2_process(conv2_t *c, const float *in, float *out, size_t n);
#endif
