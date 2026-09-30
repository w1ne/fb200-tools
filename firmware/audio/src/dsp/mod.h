/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_DSP_MOD_H
#define FB200_DSP_MOD_H
/* MOD block: port of the 12 stock FB200 modulation types (function table
 * 0x20010bfc of the stock image), sample-exact at 44.1 kHz.
 * Knobs are stock units 0..100 (preset fields 0x76 type, 0x78..0x7e p1..p4);
 * p2 (mix) runs through the stock knob smoother. Mono, in place.
 * mod_t holds the delay lines: put it in .bss. Other sample rates (up to
 * MOD_FS_MAX) scale the delay lengths and LFO rates. */
#include "dsp.h"

enum {
    MOD_PHASER, MOD_STEP_PHASER, MOD_FLANGER, MOD_JET_FLANGER, MOD_TREMOLO,
    MOD_STUTTER, MOD_VIBRATO, MOD_ROTARY, MOD_ANALOG_CHORUS, MOD_MULTI_CHORUS,
    MOD_RING, MOD_FILTER, MOD_TYPES
};

#define MOD_FS_MAX 48000.0f
/* stock ring lengths at 44.1 kHz, and their size at MOD_FS_MAX */
#define MOD_FL_N 500      /* flanger, jet */
#define MOD_FL_MAX 545
#define MOD_VIB_N 700     /* vibrato, analog chorus, multi chorus voice 1 */
#define MOD_VIB_MAX 763
#define MOD_MC2_N 600     /* multi chorus voice 2 */
#define MOD_MC2_MAX 654
#define MOD_MC3_N 550     /* multi chorus voice 3 */
#define MOD_MC3_MAX 599

typedef struct { float x1, x2, y1, y2; } mod_sos_t;

typedef struct { float ph, sm; } mod_trem_t;                     /* tremolo, ring */
typedef struct { short cur, cnt; float sm, ph; } mod_stut_t;
typedef struct { short w; float ph, fb; float buf[MOD_FL_MAX]; } mod_fl_t;
typedef struct { short w; float ph, sm; float buf[MOD_VIB_MAX]; } mod_vib_t;
typedef struct { float ph, sm; mod_sos_t s[3]; } mod_rot_t;
typedef struct { float ph, fb; mod_sos_t s[3]; } mod_ph_t;
typedef struct {
    short cnt, cur, tgt;
    float ph, prev, fb;
    mod_sos_t s[3], bp;
} mod_step_t;
typedef struct { float ph; mod_sos_t s; } mod_filt_t;
typedef struct {
    short w; float ph, depth, sm; mod_sos_t lp;
    float buf[MOD_VIB_MAX];
} mod_ach_t;
typedef struct {
    short w[3]; float ph[3], depth, sm; mod_sos_t lp;
    float b1[MOD_VIB_MAX], b2[MOD_MC2_MAX], b3[MOD_MC3_MAX];
} mod_mch_t;

typedef struct {
    float fs, r;              /* r = fs / 44100: scales the stock sample counts */
    double dt;                /* 1 / fs as the stock's 2.267573696145125e-05 */
    unsigned type;
    float p1, p3, p4;         /* knob * 0.01 */
    dsp_knob_t mix;           /* p2, smoothed like the stock callback does */
    int fix_flanger_dry;      /* 0: stock flanger dry = x * (1 - x) (a stock bug,
                                 kept for parity); 1: dry = x * (1 - p2) */
    float ph_c, filt_c;       /* allpass pole (phaser), band-pass pole (filter) */
    int n[4];                 /* scaled ring lengths: FL, VIB, MC2, MC3 */
    union {
        mod_trem_t trem;
        mod_stut_t stut;
        mod_fl_t fl;
        mod_vib_t vib;
        mod_rot_t rot;
        mod_ph_t ph;
        mod_step_t step;
        mod_filt_t filt;
        mod_ach_t ach;
        mod_mch_t mch;
    } s;
} mod_t;

void mod_init(mod_t *m, float fs);
/* type 0..11 (enum above), p1 rate, p2 mix, p3, p4: 0..100. A type change
 * clears the state like the stock commit (0x174a4) does. */
void mod_set_params(mod_t *m, unsigned type, unsigned p1, unsigned p2, unsigned p3,
                    unsigned p4);
void mod_process(mod_t *m, float *x, unsigned n);   /* in place, n <= DSP_BLOCK */
#endif
