/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_DSP_DRUMS_H
#define FB200_DSP_DRUMS_H
/* Drum machine: a port of the stock FB200 sequencer + 11-voice sample player
 * (stock V1.0.1: tick 0x17394, sequencer 0xb3ac, trigger 0x17810, mixer 0x2708,
 * tempo 0xbfd0; see the RE report).
 *
 * Samples: the stock drum bank (the .mr block 1) left in flash at 0x600D0000.
 * Its header is the truth: u32 count, u32 0, count x {u32 page, u32 bytes};
 * entry i is float32 mono 44.1 kHz at bank + page * 512. (The stock code
 * ignores the header and walks the entries back to back from the bank base,
 * which is off by the header and the page padding; we do not copy that.)
 *
 * Rhythms: 90 event lists from the stock image, in the stock data blob
 * (dsp/stock_data.h, drums_data_from_stock). Without them the
 * module stays silent and drums_init() returns -1.
 *
 * Timing is the stock one at any caller block size: the tick counter runs
 * per sample, events and voice starts are resolved on 8-sample sub-blocks
 * (the stock SAI3 interrupt size), 120 ticks per beat, 40..260 BPM. */
#include <stddef.h>
#include <stdint.h>
#include "stock_data.h"

#define DRUMS_VOICES       11
#define DRUMS_MAX_SAMPLES  32
#define DRUMS_RHYTHMS      40
#define DRUMS_SUBBLOCK     8
#define DRUMS_BPM_MIN      40
#define DRUMS_BPM_MAX      260
#define DRUMS_BPM_DEFAULT  110
#define DRUMS_LEVEL_DEFAULT 100
#define DRUMS_BANK_ADDR    0x600D0000u  /* memory-mapped flash, block 1 */

typedef struct {                /* from the stock data blob */
    const uint32_t *events;     /* all event lists back to back */
    uint32_t n_events;
    const uint16_t *lens;       /* words per event list */
    uint32_t n_patterns;        /* 90: 0..79 rhythms, 80..89 count-in clicks */
    const uint8_t *rhythm;      /* user rhythm 0..39 -> event list */
    uint32_t n_rhythms;
    const uint8_t *beats;       /* beats per bar, per event list */
} drums_data_t;

typedef struct {
    uint8_t trig, active, sample;
    uint32_t pos, len;
    float gain;
} drums_voice_t;

typedef struct {
    /* sample bank (parsed header) */
    const float *smp[DRUMS_MAX_SAMPLES];
    uint32_t smp_len[DRUMS_MAX_SAMPLES];
    uint32_t n_samples;
    const drums_data_t *data;
    /* settings (stock F:0x81000 layout: on, -, rhythm, level, bpm) */
    uint8_t on, rhythm, level;
    uint16_t bpm;
    int count_in;               /* >0: play count-in list 79+n once, then the rhythm */
    /* sequencer */
    int pattern, cur_pattern;   /* requested / active event list (-1 = none) */
    uint16_t cur_bpm;
    const uint32_t *start, *ev; /* active list, next event */
    uint32_t until;             /* ticks until the next event */
    uint32_t elapsed;           /* ticks counted since the last sequencer run */
    float frac, spt;            /* sample accumulator, samples per tick */
    int started;
    uint32_t beat_ctr, beat_half, bar_samples;
    volatile uint8_t beat_flag; /* set on each beat half-period (UI blink) */
    /* voices */
    drums_voice_t v[DRUMS_VOICES];
    unsigned rr;
    float sub[DRUMS_SUBBLOCK];
    unsigned sub_pos;           /* samples of `sub` already consumed */
    uint32_t last_tap_ms;
    /* 1 while the flash is busy (erase/program, selfupdate.c): the samples
     * are in flash, so the voices advance without reading them (silence,
     * in time). Set by the engine (engine_pump). */
    uint8_t no_flash;
} drums_t;

/* bank: the block-1 image (0x600D0000 on the pedal); data: the rhythm
 * tables (drums_data_from_stock) or NULL. Returns 0, or -1 if the
 * bank header or the rhythm data is missing (the module then stays silent). */
int drums_init(drums_t *d, const void *bank, const drums_data_t *data);
void drums_start(drums_t *d);
void drums_stop(drums_t *d);
void drums_set_tempo(drums_t *d, unsigned bpm);          /* clamped 40..260 */
void drums_set_rhythm(drums_t *d, unsigned rhythm);      /* 0..39 */
void drums_set_level(drums_t *d, unsigned level);        /* 0..100 */
void drums_count_in(drums_t *d, unsigned beats);         /* 1..9 clicks, then the rhythm */
/* tap tempo like the stock: 60000 / interval, taps > 3 s apart restart */
void drums_tap(drums_t *d, uint32_t now_ms);

/* Raw drum signal (stock 0x2708 output), overwrites out[0..n). */
void drums_render(drums_t *d, float *out, size_t n);
/* Adds drum * level * 0.016 (stock mix, after the chain, before master). */
void drums_process(drums_t *d, float *mix, size_t n);
void drums_process_stereo(drums_t *d, float *l, float *r, size_t n);

/* The rhythm tables inside the stock data blob (dsp/stock_data.h). */
void drums_data_from_stock(drums_data_t *out, const stock_data_t *s);
#endif
