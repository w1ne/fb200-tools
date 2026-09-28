#ifndef FB200_DSP_CONV2_H
#define FB200_DSP_CONV2_H
/* Two-stage convolver for IRs up to CONV2_MAX_TAPS (M5).
 *
 * Head: taps 0..511 on the conv_t of conv.c (32-sample partitions, 64-point
 * FFT, no latency), the same code and calls as the 512-tap cab. With an IR of
 * <= 512 taps the tail is off and the output is bit-identical to a conv_t.
 *
 * Tail: taps 512..4095 in 256-sample partitions (14) on a 512-point
 * arm_rfft_fast. One frame is 256 samples = 8 audio blocks. The tail result
 * for a frame is computed during the next frame, a slice per block (see
 * conv2.c), and played one frame later again: 2 x 256 = 512 samples of delay,
 * which is exactly the head length. So the sum has no added latency.
 *
 * Storage: conv2_t holds the head (8 kB of spectra, 4 kB each for h and x,
 * as cab_t does today) and the state. The tail arrays (66 kB) are a separate
 * conv2_tail_t so the caller can put them in OCRAM. */
#include <stddef.h>
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
    float g[CONV2_TAIL_PARTS][CONV2_N];       /* tail IR partition spectra (packed) */
    /* input spectra ring. One row more than partitions: conv2_set_ir can
     * recompute the frame now playing after the next frame's FFT is in */
    float x[CONV2_TAIL_PARTS + 1][CONV2_N];
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
} conv2_t;

/* tail may be NULL (then taps <= 512). Starts as a unit impulse, clear history. */
int conv2_init(conv2_t *c, conv2_tail_t *tail);
/* New IR, taps 1..CONV2_MAX_TAPS (<= 512 without a tail). Keeps the input
 * history, as a FIR that swaps its coefficients at this sample: exact, no
 * click. Heavy (all tail spectra, and the tail output of this frame and the
 * partial sum of the next one are redone): call it between blocks, not per
 * block (see conv2.c for the cost). 0 on success. */
int conv2_set_ir(conv2_t *c, const float *ir, size_t taps);
void conv2_reset(conv2_t *c);                /* clear the input history */
/* Any n, in place allowed. The tail work runs on sample counts, so part
 * blocks keep the schedule. */
void conv2_process(conv2_t *c, const float *in, float *out, size_t n);
#endif
