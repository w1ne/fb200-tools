#ifndef FB200_DSP_REVERB_H
#define FB200_DSP_REVERB_H
/* Stock FB200 reverb (types Room, Hall, Plate, Spring, Mod; stock functions
 * 0xdbd8 0xc0d8 0xd2f8 0xe350 0xc9c0, init 0x6630). Freeverb-like, mono in,
 * stereo out:
 *   6 damped combs (0-2 fed from L, 3-5 from R), summed crosswise
 *   A = (c0+c2+c4)/3, B = (c1+c3+c5)/3, each through a feed-forward echo
 *   y = x + 0.2 x[n-D] (the "allpasses"), a 600-sample stereo delay whose two
 *   taps are LFO-modulated (not Spring), two high-pass biquads, a one-pole
 *   tone low-pass, then out = dry + gain * level * wet.
 *   Spring adds a "drip": an input-level driven, LFO-swept cascade of three
 *   allpass biquads mixed into the tone filter.
 * Field map (preset u16): 0xa4 enable, 0xa6 type, 0xaa level (smoothed),
 * 0xac decay (comb feedback), 0xae tone (low-pass coefficient); 0xa8 is not
 * read by the stock audio path (the 4th argument P+0x20 stays 0.5 and only
 * feeds a slew in Room that nothing reads).
 * At fs = 44100 it follows the stock bit for bit (host, -ffp-contract=off)
 * except for the two LFO wavetables, rebuilt from fitted formulas (~1e-6).
 * Other rates scale delay lengths and LFO rates. Buffers are sized for
 * fs <= 48000. Put reverb_t in .bss (46.9 kB) + 2 kB of static tables. */
#include "dsp.h"

enum { REVERB_ROOM, REVERB_HALL, REVERB_PLATE, REVERB_SPRING, REVERB_MOD, REVERB_TYPES };

#define REVERB_COMB_MAX 1488      /* longest comb 1367 @ 44.1 kHz -> 48 kHz */
#define REVERB_ECHO_MAX 387       /* 355 */
#define REVERB_MDL_MAX  654       /* 600-sample modulated delay */

typedef struct {
    float x1, x2, y1, y2;
} reverb_bq_state_t;

typedef struct {
    float fs, scale;              /* scale = fs / 44100 */
    unsigned type;
    int fresh;
    dsp_knob_t level;             /* P+0x2c: smoothed 0xaa / 100 */
    float tone, fb;               /* per type from 0xae, 0xac */
    unsigned comb_len[6], echo_len, mdl_len;
    float center, depth, rate;    /* modulated delay, samples / per-sample phase */
    /* state (stock RAM 0x20010cb4 block) */
    float comb[6][REVERB_COMB_MAX];
    unsigned comb_idx[6];
    float comb_out[6], comb_lp[6];
    float echo[2][REVERB_ECHO_MAX];
    unsigned echo_idx[2];
    float mdl[2][REVERB_MDL_MAX];
    unsigned mdl_idx;
    float phase;                  /* LFO phase 0..1 */
    reverb_bq_state_t hp1[2];     /* float biquad */
    reverb_bq_state_t hp2[2];     /* double-precision biquad, float state */
    float lp[2];                  /* tone low-pass, L/R */
    /* Spring drip (stock 0x20017c58 block + 0x20010d24..) */
    float drip_peak, drip_hold, drip_env1, drip_env2, drip_phase;
    unsigned drip_count;
    float drip_fb, drip_x1, drip_x2;
    float drip_y[3][2];           /* [stage][y1, y2] */
    float drip_coef[128][5];
} reverb_t;

void reverb_init(reverb_t *r, float fs);
/* stock knob units 0..100; type 0..4. A type change re-inits the state. */
void reverb_set_params(reverb_t *r, unsigned type, unsigned level, unsigned decay,
                       unsigned tone, unsigned p_a8);
/* mono in, stereo out; n <= DSP_BLOCK */
void reverb_process(reverb_t *r, const float *in, float *out_l, float *out_r, unsigned n);
#endif
