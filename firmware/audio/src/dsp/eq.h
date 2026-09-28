#ifndef FB200_DSP_EQ_H
#define FB200_DSP_EQ_H
/* Bass EQ (our addition, the stock has none; docs/PARITY.md M4). Mono, in
 * place, after the cab and before MOD. 7 biquads in one CMSIS-DSP cascade
 * (arm_biquad_cascade_df2T_f32):
 *
 *   HPF 12 dB/oct 20..200 Hz (0 = off) -> 5 peaking bands -> LPF 12 dB/oct
 *   2..20 kHz (0 = off)
 *
 * HPF/LPF: RBJ cookbook, Q = 1/sqrt(2) (Butterworth). Bands: RBJ peaking EQ,
 * 30..10000 Hz, -15..+15 dB, Q 0.3..4; defaults 40, 100, 250, 800, 3000 Hz,
 * 0 dB, Q 1. The designs run in double (the M7 FPU has double) and are
 * rounded to float once.
 *
 * Coefficients: eq_set_* designs the TARGET coefficients (main loop, not the
 * audio path). eq_process moves the current coefficients to the target in a
 * straight line over EQ_RAMP_BLOCKS blocks (11.6 ms at 44.1 kHz), one step per
 * block, and snaps to the target at the end. A biquad is stable when its
 * (a1, a2) is in the stability triangle, and the triangle is convex: every
 * point on the line between two stable designs is stable.
 *
 * A band at 0 dB, a filter at 0 (off) and a switched-off EQ have the
 * identity target {1, 0, 0, 0, 0}. When every stage has settled on the
 * identity, eq_process returns at once: the signal is bit-exact. `eq off`
 * ramps to the identity first, so on/off does not click.
 *
 * RAM: sizeof(eq_t) = 432 B (DTCM). Cost: 7 stages every block while not
 * bypassed (docs/PARITY.md M4 EQ). */
#include <stdint.h>
#include "arm_math.h"
#include "dsp.h"

#define EQ_BANDS 5
#define EQ_STAGES (EQ_BANDS + 2)        /* HPF, bands 1..5, LPF */
#define EQ_RAMP_BLOCKS 16u
#define EQ_HPF_MIN 20.0f
#define EQ_HPF_MAX 200.0f
#define EQ_LPF_MIN 2000.0f
#define EQ_LPF_MAX 20000.0f
#define EQ_BAND_MIN 30.0f
#define EQ_BAND_MAX 10000.0f
#define EQ_GAIN_MAX 15.0f
#define EQ_Q_MIN 0.3f
#define EQ_Q_MAX 4.0f
#define EQ_Q_DEF 1.0f

typedef struct {
    arm_biquad_cascade_df2T_instance_f32 inst;
    float c[5 * EQ_STAGES];             /* current: b0 b1 b2 -a1 -a2 (CMSIS) */
    float t[5 * EQ_STAGES];             /* target */
    float st[2 * EQ_STAGES];            /* df2T state */
    float fs;
    float hpf, lpf;                     /* Hz, 0 = off */
    float f[EQ_BANDS], g[EQ_BANDS], q[EQ_BANDS];
    uint8_t on;
    uint8_t bypass;                     /* settled on the identity: skip */
    uint8_t ramp;                       /* blocks left in the ramp */
} eq_t;

void eq_init(eq_t *e, float fs);        /* off, flat, the default bands */
void eq_reset(eq_t *e);                 /* clear the filter state (keeps the settings) */
void eq_set_on(eq_t *e, int on);
/* Out-of-range values are clamped; hz = 0 switches the filter off. */
void eq_set_hpf(eq_t *e, float hz);
void eq_set_lpf(eq_t *e, float hz);
/* band 0..EQ_BANDS-1; returns -1 for a bad band */
int eq_set_band(eq_t *e, unsigned band, float hz, float gain_db, float q);
/* in place, any n (the ramp steps once per call) */
void eq_process(eq_t *e, float *x, unsigned n);

/* The double-precision RBJ design the firmware uses, for the tests: stage
 * type 0 = HPF, 1 = peak, 2 = LPF. Out: b0 b1 b2 a1 a2 normalised by a0
 * (the RBJ signs, not the CMSIS ones). */
void eq_design(int type, double fs, double f0, double q, double gain_db, double out[5]);
#endif
