/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_DSP_TUNER_H
#define FB200_DSP_TUNER_H
/* Tuner: a port of the stock FB200 tuner (V1.0.1; feed 0x3aa4 + 0x159e0 in
 * the audio callback, YIN 0x3728 / octave 0x11438 / note 0xbbc0 in the main
 * loop; see the RE report).
 *
 * Audio side (cheap, call from the audio path with the input block):
 *   one-pole low-pass (a = 0.05) -> keep every 4th sample (11025 Hz) into two
 *   1024-sample buffers, plus the stock signal-present detector.
 * Main loop: tuner_poll() runs YIN (window 500, lags 8..499, threshold 0.1,
 *   parabolic refinement, reject if the best CMNDF value > 0.2) on each full
 *   buffer (one analysis per 1024 decimated samples = 92.9 ms) and maps the
 *   pitch to a note with the stock 13-entry A..A# table and A4 = 430..450 Hz.
 *
 * Stock display: note 1..12 = A#, B, C, C#, D, D#, E, F, F#, G, G#, A;
 * deviation 1..100, 50 = in tune (dead band ~ +-2 cents), > 50 sharp. The
 * deviation is linear in Hz between the note and the half-way point to the
 * neighbour; `cents` below is the true log value for our own UI. */
#include <stddef.h>
#include <stdint.h>

#define TUNER_BUF       1024
#define TUNER_WIN       500
#define TUNER_FS        11025.0f
#define TUNER_CAL_MIN   430
#define TUNER_CAL_MAX   450

typedef struct {
    int valid;          /* 1 = a pitch was found in the last analysis */
    int silent;         /* stock signal-present detector says "no signal" */
    int note;           /* 1..12 (A#..A), 0 = none */
    int octave;         /* stock octave index 0..7 (C-based), -1 = none */
    int deviation;      /* stock 1..100, 50 = in tune */
    float cents;        /* -50..+50 from the nearest note (log) */
    float freq;         /* Hz, 0 when not valid */
    float confidence;   /* 1 - CMNDF minimum (0..1) */
} tuner_result_t;

typedef struct {
    /* audio-side state (written by tuner_feed) */
    float lp, a;
    int dec;
    float buf[2][TUNER_BUF];
    int16_t idx[2];
    volatile uint8_t full[2];
    uint8_t busy;             /* stock flag [0x20]: A done, B still filling */
    int16_t below_thr;        /* samples since the last one above threshold */
    volatile uint8_t silent;
    /* main-loop state */
    float cal;                /* A4 / 440 */
    tuner_result_t res;
    float acf[TUNER_WIN], d[TUNER_WIN], dn[TUNER_WIN + 1];
} tuner_t;

void tuner_init(tuner_t *t, int a4_hz);                  /* 430..450, 440 default */
void tuner_set_a4(tuner_t *t, int a4_hz);
/* x = the tuner input (the stock feeds L + R of the instrument ADC). */
void tuner_feed(tuner_t *t, const float *x, size_t n);
/* Runs at most one analysis; returns 1 when `out` was updated. */
int tuner_poll(tuner_t *t, tuner_result_t *out);
/* The analysis on its own (buffer of >= 1000 samples at fs): Hz or 0. */
float tuner_yin(tuner_t *t, const float *x, float fs, float *confidence);
#endif
