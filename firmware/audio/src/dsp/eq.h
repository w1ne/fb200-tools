#ifndef FB200_DSP_EQ_H
#define FB200_DSP_EQ_H
/* Bass EQ (our addition, the stock has none; docs/PARITY.md M4). Mono, in
 * place, after the cab and before MOD. 7 biquads in one CMSIS-DSP cascade:
 *
 *   HPF 12 dB/oct 20..200 Hz (0 = off) -> 5 peaking bands -> LPF 12 dB/oct
 *   2..20 kHz (0 = off)
 *
 * HPF/LPF: RBJ cookbook, Q = 1/sqrt(2) (Butterworth). Bands: RBJ peaking EQ,
 * 30..10000 Hz, -15..+15 dB, Q 0.3..4; defaults 40, 100, 250, 800, 3000 Hz,
 * 0 dB, Q 1. The designs run in double (the M7 FPU has double) and are
 * rounded to float once.
 *
 * Settled: arm_biquad_cascade_df1_f32. Direct form 1, not the transposed
 * DF2 (df2T): the DF1 state is the signal itself (the last inputs and
 * outputs), so a coefficient change does not leave a state that belongs to
 * the old filter. Measured (tests/eq_host_test.c): an LPF glide from 20 to
 * 2 kHz puts -20 dB of clicks above 8 kHz with df2T, < -60 dB with DF1.
 *
 * Changes glide: each stage moves its parameters to the new setting in a
 * straight line over EQ_RAMP_BLOCKS blocks (11.6 ms at 44.1 kHz) - frequency
 * and Q in octaves, gain in dB - with a new design every block of the glide
 * (only while it glides; the design runs in the audio path then, ~0.5k
 * cycles a stage). Inside the block the coefficients step every sample from
 * the old design to the new one (plain C, same DF1). A straight line between
 * two neighbouring designs is stable: the biquad stability triangle is
 * convex. (A straight line between the old and new coefficient sets is
 * stable too, but its middle is not a filter in between: a -15 dB notch
 * going from Q 0.3 to 4 made more clicks than a hard switch.)
 *
 * Off states: a band at 0 dB is the identity (b = a). An HPF/LPF fades its
 * numerator to its denominator (b' = a + m (b - a), m 1 -> 0). `eq off`
 * glides every stage to that. When all stages are neutral and each stage's
 * output equals its input (to 1e-5), eq_process only notes the last two
 * input samples: the signal is bit-exact. Leaving the bypass seeds every
 * stage's history with them: no jump.
 *
 * RAM: sizeof(eq_t) = 588 B on the target (OCRAM, the engine's s_eq). */
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

/* one stage's parameters: log2 Hz, log2 Q, v = gain dB (bands) or the mix
 * m 0..1 (HPF/LPF); v = 0 is neutral (the identity) */
typedef struct { float lf, lq, v; } eq_par_t;

typedef struct eq_s {
    float c[5 * EQ_STAGES];             /* b0 b1 b2 -a1 -a2 (CMSIS) */
    float st[4 * EQ_STAGES];            /* DF1 state: x1 x2 y1 y2 per stage */
    eq_par_t cur[EQ_STAGES], tgt[EQ_STAGES];
    float fs;
    /* the settings as set (Hz, dB, Q), for the console */
    float hpf, lpf;                     /* 0 = off */
    float f[EQ_BANDS], g[EQ_BANDS], q[EQ_BANDS];
    uint8_t ramp[EQ_STAGES];            /* blocks left in each stage's glide */
    uint8_t active[EQ_STAGES];          /* 0: neutral and settled, skipped */
    uint8_t on;
    uint8_t bypass;                     /* every stage skipped */
} eq_t;

void eq_init(eq_t *e, float fs);        /* off, flat, the default bands */
void eq_reset(eq_t *e);                 /* clear the filter state (keeps the settings) */
void eq_set_on(eq_t *e, int on);
/* Out-of-range values are clamped; hz = 0 switches the filter off. */
void eq_set_hpf(eq_t *e, float hz);
void eq_set_lpf(eq_t *e, float hz);
/* band 0..EQ_BANDS-1; returns -1 for a bad band */
int eq_set_band(eq_t *e, unsigned band, float hz, float gain_db, float q);
/* in place, any n (the glide steps once per call) */
void eq_process(eq_t *e, float *x, unsigned n);

/* The double-precision RBJ design the firmware uses, for the tests: stage
 * type 0 = HPF, 1 = peak, 2 = LPF. Out: b0 b1 b2 a1 a2 normalised by a0
 * (the RBJ signs, not the CMSIS ones). */
void eq_design(int type, double fs, double f0, double q, double gain_db, double out[5]);
#endif
