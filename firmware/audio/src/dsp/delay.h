#ifndef FB200_DSP_DELAY_H
#define FB200_DSP_DELAY_H
/* Bass delay (our addition, the stock has none; docs/PARITY.md M4). Mono, in
 * place, after MOD and before the reverb:
 *
 *   y = line[n - d]                       (linear interpolation, d glides)
 *   line[n] = LP(HP(x + fb * y))          (the repeats lose low end each pass)
 *   out = x + mix * y                     (the dry signal stays at unity)
 *
 * Knobs 0..100 like the stock modules: fb (x 0.95 max), mix (x 1.0 max).
 * time in ms, 20..1000. lowcut 0 = off, else a 12 dB/oct high-pass at
 * 20 * 25^(k/100) Hz (20..500 Hz; 63 = 150 Hz). tone 100 = off, else a
 * 6 dB/oct low-pass at 1000 * 10^(k/100) Hz (1..10 kHz).
 *
 * The line is int16 (x 16384: +-2.0 full scale, truncated toward zero, so a
 * tail always dies out to exact zeros): 1 s at 48 kHz = 96 kB. That does not
 * fit in DTCM: the caller gives the line (the engine's is in OCRAM, linker.ld
 * .ocram); delay_t itself (~110 B) stays in DTCM. */
#include <stdint.h>
#include "dsp.h"

#define DELAY_FS_MAX 48000
#define DELAY_MS_MIN 20u
#define DELAY_MS_MAX 1000u
#define DELAY_LEN (DELAY_FS_MAX * DELAY_MS_MAX / 1000u + 4u)   /* samples */
#define DELAY_SCALE 16384.0f
/* settings for a preset that never had our delay (console `delay on`) */
#define DELAY_DEF_MS 350u
#define DELAY_DEF_FB 30u
#define DELAY_DEF_MIX 35u
#define DELAY_DEF_LOWCUT 63u      /* 150 Hz */
#define DELAY_DEF_TONE 70u        /* 5 kHz */

typedef struct {
    float fs;
    int fresh;                   /* next set_params snaps instead of gliding */
    unsigned time_ms, fb_k, mix_k, cut_k, tone_k;
    float d, d_t, d_c;           /* delay in samples: current, target, glide */
    float fb, fb_t, mix, mix_t;  /* smoothed gains */
    int hp_on, lp_on;
    float hb0, hb1, hb2, ha1, ha2;   /* high-pass biquad (DF1) */
    float hx1, hx2, hy1, hy2;
    float la, lz;                /* one-pole low-pass: z += a * (v - z) */
    unsigned w;                  /* write index */
    int16_t *line;               /* DELAY_LEN samples */
} delay_t;

/* fs <= DELAY_FS_MAX; line = int16_t[DELAY_LEN], cleared here */
void delay_init(delay_t *dl, float fs, int16_t *line);
void delay_clear(delay_t *dl);            /* silence the line and filters, snap the knobs */
/* time in ms (clamped to 20..1000), knobs 0..100 (see above) */
void delay_set_params(delay_t *dl, unsigned time_ms, unsigned fb, unsigned mix, unsigned lowcut,
                      unsigned tone);
/* in place; n <= DSP_BLOCK (the filter states are flushed once per call) */
void delay_process(delay_t *dl, float *x, unsigned n);
/* High-pass cut-off (Hz) for a lowcut knob, 0 when off: for the tests. */
float delay_lowcut_hz(unsigned lowcut);
#endif
