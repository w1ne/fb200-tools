/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_DSP_OUTQ_H
#define FB200_DSP_OUTQ_H
/* Float -> int16 for the codec DAC and the USB capture (our output stage;
 * the stock sends 32-bit words to the DAC, so it has no 16-bit step).
 *
 * v0.9.1 cast `(int16_t)(v * 32767)`: truncation toward zero. That is a
 * dead zone of +-1 LSB around 0 (crossover distortion): a -60 dBFS sine came
 * back with THD -31 dB (pedal, USB reamp, 2026-09-29), -35.8 dB rounded
 * (the rest is the 16-bit quantisation by the host and at the capture: a
 * model of both matches the pedal to 0.6 dB). A float sine at -60 dBFS
 * rounded once: THD -54.6 dB vs -41.5 dB truncated (outq_host_test). No
 * DC shift of -0.5 LSB.
 *
 * Optional TPDF dither (+-1 LSB triangular, console `dither on`, off by
 * default): the quantisation error becomes a signal-independent noise
 * (-96 dBFS in 20 Hz..20 kHz), no harmonics at any level. Off by default:
 * with the instrument connected the ADC noise (~-84 dBFS at the input)
 * already dithers the signal, and the dither would add ~1 dB of noise. An
 * exact 0.0 stays 0 (no dither): digital silence (gate closed, tuner mute)
 * stays silent.
 *
 * The input is clamped to +-1.0 (full scale 32767). */
#include <stdint.h>

typedef struct { uint32_t rng; int dither; } outq_t;

static inline void outq_init(outq_t *q, uint32_t seed)
{
    q->rng = seed ? seed : 0x9E3779B9u;
    q->dither = 0;
}

static inline uint32_t outq_rand(outq_t *q)      /* xorshift32 */
{
    uint32_t r = q->rng;
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    q->rng = r;
    return r;
}

/* round half away from zero after clamping to +-1.0 */
static inline int16_t outq_round(float v)
{
    if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
    float s = v * 32767.0f;
    return (int16_t)(s >= 0.0f ? s + 0.5f : s - 0.5f);
}

static inline int16_t outq_sample(outq_t *q, float v)
{
    if (!q->dither || v == 0.0f) return outq_round(v);
    if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
    /* two uniforms in [0, 1) LSB -> triangular in (-1, 1) LSB */
    uint32_t r = outq_rand(q);
    float d = (float)(r & 0xFFFFu) * (1.0f / 65536.0f) - (float)(r >> 16) * (1.0f / 65536.0f);
    float s = v * 32767.0f + d;
    s = s >= 0.0f ? s + 0.5f : s - 0.5f;
    if (s > 32767.0f) s = 32767.0f; else if (s < -32767.0f) s = -32767.0f;
    return (int16_t)s;
}
#endif
