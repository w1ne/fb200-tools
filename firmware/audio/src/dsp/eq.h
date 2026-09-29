#ifndef FB200_DSP_EQ_H
#define FB200_DSP_EQ_H
/* Bass EQ (our addition, the stock has none; docs/PARITY.md M4). Mono, in
 * place, after the cab and before MOD. 7 biquads:
 *
 *   HPF 12 dB/oct 20..200 Hz (0 = off) -> 5 peaking bands -> LPF 12 dB/oct
 *   2..20 kHz (0 = off)
 *
 * HPF/LPF: RBJ cookbook, Q = 1/sqrt(2) (Butterworth). Bands: RBJ peaking EQ,
 * 30..10000 Hz, -15..+15 dB, Q 0.3..4; defaults 40, 100, 250, 800, 3000 Hz,
 * 0 dB, Q 1. The designs run in double (the M7 FPU has double) and are
 * rounded to float once.
 *
 * The stages run in double: coefficients, state and arithmetic (the M7 FPU
 * has double). In float (v0.9.1: arm_biquad_cascade_df1_f32) the low
 * stages' rounding noise was audible: HPF 30 Hz + 40 Hz +6 dB + 100 Hz -4 dB
 * q 2 took the THD+N of a -15 dBFS 1 kHz sine from -80.9 to -71.4 dB (pedal,
 * 2026-09-29; noise peaks at 27..60 Hz). The output of each stage is rounded
 * to float once (-150 dB).
 *
 * Direct form 1, not the transposed DF2 (df2T): the DF1 state is the signal
 * itself (the last inputs and outputs), so a coefficient change leaves no
 * state that belongs to the old filter. In a model of this glide (a 200 Hz
 * sine, LPF 2 -> 20 kHz), df2T put clicks above 8 kHz at -47 dB re the
 * signal, DF1 at -83 dB.
 *
 * Changes glide: each stage moves its parameters to the new setting in a
 * straight line over EQ_RAMP_BLOCKS blocks (11.6 ms at 44.1 kHz) - frequency
 * and Q in octaves, gain in dB - with a new design every block of the glide.
 * Inside the block the coefficients step every sample from the old design to
 * the new one (glide_stage: the same DF1). The design runs in the
 * audio path, but only in the 16 blocks of a glide and only for the stages
 * that move. A straight line between two neighbouring designs is stable:
 * the biquad stability triangle is convex. (A straight line from the old to
 * the new coefficients in one go is stable too, but its middle is not a
 * filter in between: a -15 dB notch going from Q 0.3 to 4 clicked more than
 * a hard switch.)
 *
 * Neutral: a band at 0 dB (b = a); an HPF/LPF with its numerator faded to
 * its denominator (b' = a + m (b - a), m 1 -> 0). `eq off` glides every
 * stage to neutral. A neutral stage whose output equals its input (to 1e-5)
 * is skipped: it does not touch the signal and only keeps its history (the
 * last two samples), so it can start again without a jump. Every stage
 * skipped (off, or flat) = bit-exact. Skipping also keeps the float rounding
 * noise of idle low-frequency stages (~ -75 dB each) out of the signal.
 *
 * RAM: sizeof(eq_t) = 760 B on the target (OCRAM, the engine's s_eq). */
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
    double c[5 * EQ_STAGES];            /* b0 b1 b2 -a1 -a2 (the CMSIS signs) */
    double st[4 * EQ_STAGES];           /* DF1 state: x1 x2 y1 y2 per stage */
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
/* The settings as bytes (the preset, preset.h P_EQ_DATA), EQ_REC bytes:
 *   [0] on, [1] HPF Hz (0 = off), [2..3] LPF Hz u16 LE (0 = off),
 *   then per band [4 + 4 b]: Hz u16 LE, gain s8 in 1/8 dB, Q u8 x 50.
 * eq_load clamps like the setters and glides like them; NULL = off with
 * the default settings (eq_init). eq_save rounds to that grid. */
#define EQ_REC (4 + 4 * EQ_BANDS)
void eq_load(eq_t *e, const uint8_t *rec);
void eq_save(const eq_t *e, uint8_t rec[EQ_REC]);
/* in place, any n (the glide steps once per call) */
void eq_process(eq_t *e, float *x, unsigned n);

/* The double-precision RBJ design the firmware uses, for the tests: stage
 * type 0 = HPF, 1 = peak, 2 = LPF. Out: b0 b1 b2 a1 a2 normalised by a0
 * (the RBJ signs, not the CMSIS ones). */
void eq_design(int type, double fs, double f0, double q, double gain_db, double out[5]);
#endif
