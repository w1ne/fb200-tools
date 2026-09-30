/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_DRIFT_H
#define FB200_DRIFT_H

#include <stddef.h>
#include <stdint.h>

/* Elastic FIFO helpers shared by the USB rings and the engine. Pure so the
 * host test (tests/test_dsp_host.py) can exercise the drift behaviour. */

/* Adaptive resampler for the host playback (replaces drift_fill/drift_trim
 * of v0.9.1). The host clock and the codec clock differ by tens of ppm:
 * drift_fill repeated a frame when the ring ran dry and drift_trim dropped
 * frames when it overfilled - a step in the waveform each time, every ~2.5 s
 * on the pedal (2026-09-29: a -10 dBFS 1 kHz sine, 8 ticks in 18.5 s, the
 * worst 10 ms window -23.5 dB re the signal).
 *
 * drift_rs_pull reads the ring at a fractional position that advances by
 * `ratio` frames per output frame (4-point Catmull-Rom interpolation), and
 * steers `ratio` so the ring fill (low-passed) sits at DRIFT_RS_TARGET: a
 * slow PI loop (drift.c), +-DRIFT_RS_MAX_PPM. No frame is repeated or dropped while the
 * host streams at a rate within that range. Start: silence until the fill
 * reaches the target (latency ~DRIFT_RS_TARGET frames, 2.9 ms at 44.1 kHz).
 * Fallbacks, counted: ring empty -> the last frame repeats (*inserts); fill
 * above max_fill -> restart at the target (*drops). */
#define DRIFT_RS_TARGET 128u
#define DRIFT_RS_MAX_PPM 2000.0f

typedef struct {
    float frac;          /* position between win[1] and win[2], 0..1 */
    float ratio;         /* ring frames per output frame */
    float fill_lp, fill_lp2;   /* the ring fill through two low-passes, frames */
    float integ;         /* PI integral, frame-seconds */
    float fs;
    int running;         /* 0: waiting for the fill to reach the target */
    int16_t win[4][2];   /* the 4 frames around the read position */
} drift_rs_t;

void drift_rs_init(drift_rs_t *rs, float fs);   /* also: the host stopped */
/* `frames` stereo frames into out[]; consumes about frames * ratio frames
 * from ring[cap * 2] at *tail. Returns frames. */
size_t drift_rs_pull(drift_rs_t *rs, const int16_t *ring, uint32_t cap, uint32_t *tail,
                     uint32_t head, int16_t *out, size_t frames, uint32_t max_fill,
                     uint32_t *inserts, uint32_t *drops);

/* Host playback into the chain input (reamping, console `usb in|mix`). The
 * chain input is L + R of the ADC block; the stereo int16 playback goes in as
 * its mono mean (L + R) / 2, so a file played on both channels enters at its
 * own level. replace != 0: l = mean, r = 0 (the instrument is dropped);
 * replace == 0: l += mean (instrument + playback). */
void drift_play_to_input(float *l, float *r, const int16_t *play, size_t n,
                         int replace);

#endif
