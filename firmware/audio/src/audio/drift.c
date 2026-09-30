/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include "audio/drift.h"

void drift_play_to_input(float *l, float *r, const int16_t *play, size_t n,
                         int replace)
{
    for (size_t i = 0; i < n; i++) {
        float m = ((float)play[i * 2 + 0] + (float)play[i * 2 + 1]) * (0.5f / 32768.0f);
        if (replace) {
            l[i] = m;
            r[i] = 0.0f;
        } else {
            l[i] += m;
        }
    }
}

/* ---- adaptive resampler (drift.h) ---- */

/* The loop must be slow. The raw fill is a sawtooth of +-20 frames (1 ms USB
 * packets against 32-frame blocks) whose sampled pattern has components down
 * to a few Hz; any of it in `ratio` is a phase modulation of the playback.
 * v1 (0.2 s low-pass, 30 ppm per frame) had it: on the pedal a reamped 1 kHz
 * sine read THD+N -37 dB in the first 1.5 s after the stream start, and the
 * phase wandered +-0.16 rad after that. Now: the fill through two one-pole
 * low-passes (1 s each: the sawtooth at >= 3 Hz is down > 50 dB), a PI loop
 * with 10 ppm per frame (crossover 0.44 rad/s) and the integral zero at
 * 0.11 rad/s. A 100 ppm clock error moves the fill by < 10 frames; up to
 * +-200 ppm nothing repeats even at the start (host model). At the start the
 * filters take the fill as it is: no initial correction. */
#define RS_KP 1.0e-5f
#define RS_KI 1.1e-6f
#define RS_LP_S 1.0f

void drift_rs_init(drift_rs_t *rs, float fs)
{
    *rs = (drift_rs_t){.ratio = 1.0f, .fs = fs, .fill_lp = (float)DRIFT_RS_TARGET,
                       .fill_lp2 = (float)DRIFT_RS_TARGET};
}

static float clamp_ppm(float r)
{
    const float d = DRIFT_RS_MAX_PPM * 1e-6f;
    return r > 1.0f + d ? 1.0f + d : r < 1.0f - d ? 1.0f - d : r;
}

static int16_t sat16(float v)
{
    v = v >= 0.0f ? v + 0.5f : v - 0.5f;
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)v;
}

size_t drift_rs_pull(drift_rs_t *rs, const int16_t *ring, uint32_t cap, uint32_t *tail,
                     uint32_t head, int16_t *out, size_t frames, uint32_t max_fill,
                     uint32_t *inserts, uint32_t *drops)
{
    uint32_t fill = (head + cap - *tail) % cap;
    if (fill > max_fill) {                        /* way off: restart at the target */
        uint32_t skip = fill - DRIFT_RS_TARGET;
        *tail = (*tail + skip) % cap;
        *drops += skip;
        fill = DRIFT_RS_TARGET;
        rs->fill_lp = rs->fill_lp2 = (float)fill;
    }
    if (!rs->running) {
        if (fill < DRIFT_RS_TARGET) {             /* prefill: silence */
            for (size_t i = 0; i < frames * 2; i++) out[i] = 0;
            return frames;
        }
        float fs = rs->fs;
        drift_rs_init(rs, fs);
        rs->running = 1;
        rs->frac = 1.0f;                          /* the first frame comes in at once */
        rs->fill_lp = rs->fill_lp2 = (float)fill; /* start from the fill as it is */
    }
    /* steer the ratio: more fill than the target -> read faster */
    const float dt = (float)frames / rs->fs;
    rs->fill_lp += (dt / RS_LP_S) * ((float)fill - rs->fill_lp);
    rs->fill_lp2 += (dt / RS_LP_S) * (rs->fill_lp - rs->fill_lp2);
    float e = rs->fill_lp2 - (float)DRIFT_RS_TARGET;
    rs->integ += e * dt;
    const float ilim = DRIFT_RS_MAX_PPM * 1e-6f / RS_KI;
    if (rs->integ > ilim) rs->integ = ilim; else if (rs->integ < -ilim) rs->integ = -ilim;
    rs->ratio = clamp_ppm(1.0f + RS_KP * e + RS_KI * rs->integ);

    float frac = rs->frac;
    for (size_t n = 0; n < frames; n++) {
        while (frac >= 1.0f) {                    /* next frame into the window */
            for (int k = 0; k < 3; k++) {
                rs->win[k][0] = rs->win[k + 1][0];
                rs->win[k][1] = rs->win[k + 1][1];
            }
            if (*tail != head) {
                rs->win[3][0] = ring[*tail * 2 + 0];
                rs->win[3][1] = ring[*tail * 2 + 1];
                *tail = (*tail + 1u) % cap;
            } else {
                (*inserts)++;                     /* dry: win[3] repeats */
            }
            frac -= 1.0f;
        }
        const float t = frac, t2 = t * t, t3 = t2 * t;
        /* Catmull-Rom weights for p0..p3 at t between p1 and p2 */
        const float w0 = 0.5f * (-t3 + 2.0f * t2 - t);
        const float w1 = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
        const float w2 = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
        const float w3 = 0.5f * (t3 - t2);
        for (int c = 0; c < 2; c++) {
            float v = w0 * (float)rs->win[0][c] + w1 * (float)rs->win[1][c] +
                      w2 * (float)rs->win[2][c] + w3 * (float)rs->win[3][c];
            out[n * 2 + c] = sat16(v);
        }
        frac += rs->ratio;
    }
    rs->frac = frac;
    return frames;
}
