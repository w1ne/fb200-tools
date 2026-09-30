/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_DSP_STOCK_DATA_H
#define FB200_DSP_STOCK_DATA_H
/* Stock FB200 sound data: amp models, cab IRs, tone-stack tables (44.1 kHz
 * designs) and the drum rhythms. It is vendor material, so no firmware image
 * contains it. The host tools (src/fb200/stockdata.py) build this blob from
 * the user's own stock .mr and write it once to its own flash area
 * (`fwstock`); app updates never touch it. At boot stock_load() checks it and
 * copies it to DTCM. Without it the amp, cab and tone pass audio through and
 * the drums stay silent.
 *
 * The layout is shared with stockdata.py: change both, and bump
 * STOCK_VERSION. Version 2 adds the factory presets as a tail after
 * stock_data_t; version 1 blobs (no tail) are still accepted, so pedals
 * keep their sound after an app update. */
#include <stdint.h>

#define STOCK_MAGIC          0x44534246u   /* "FBSD" */
#define STOCK_VERSION        2u
#define STOCK_FLASH          0x60061000u   /* F:0x61000, below the presets */
#define STOCK_FLASH_SIZE     0x00010000u

#define STOCK_AMP_MODELS     10
#define STOCK_AMP_WS         256
#define STOCK_AMP_SOS        10
#define STOCK_CABS           10
#define STOCK_CAB_TAPS       512
#define STOCK_TONE_STEPS     32
#define STOCK_TONE_MID_BANKS 5     /* 200, 400, 800, 1600, 3000 Hz */
#define STOCK_DRUM_EVENTS    4712
#define STOCK_DRUM_PATTERNS  90
#define STOCK_DRUM_RHYTHMS   40
#define STOCK_FACTORY_NAMED  20    /* "Fat Bass" ... "Classic Spring" */
#define STOCK_PRESET_SIZE    0x100

/* SOS rows are CMSIS DF1 {b0, b1, b2, -a1, -a2}: y = b0x+b1x1+b2x2-a1y1-a2y2. */
typedef struct {
    float ws[STOCK_AMP_WS];            /* waveshaper, x = i/254 */
    float pre[STOCK_AMP_SOS][5];
    float post[STOCK_AMP_SOS][5];
    float pre_gain, out_gain, drive_scale, drive_scale2, level;
} stock_amp_model_t;

typedef struct {
    uint32_t magic, version, size;     /* size = sizeof(stock_data_t) */
    uint32_t crc;                      /* CRC-32 of the bytes after this field */
    stock_amp_model_t amp_models[STOCK_AMP_MODELS];
    /* 3x-rate anti-alias low-pass {b0, b1, b2, a1, a2} (ITCM literals 0x318c..):
     * exact bits matter, the amp's filter chains are ill-conditioned */
    float amp_aa[5];
    float cab_taps[STOCK_CABS][STOCK_CAB_TAPS];   /* natural order */
    float cab_gain[STOCK_CABS];
    /* CMSIS {b0, b1, b2, a1, a2} per knob step (idx = int(knob/100 * 31)) */
    float tone_bass[STOCK_TONE_STEPS][5];
    float tone_presence[STOCK_TONE_STEPS][5];
    float tone_treble[STOCK_TONE_STEPS][5];
    float tone_mid[STOCK_TONE_MID_BANKS][STOCK_TONE_STEPS][5];
    /* drum sequencer (dsp/drums.h) */
    uint32_t drum_events[STOCK_DRUM_EVENTS];
    uint16_t drum_lens[STOCK_DRUM_PATTERNS];
    uint8_t drum_rhythm[STOCK_DRUM_RHYTHMS];
    uint8_t drum_beats[STOCK_DRUM_PATTERNS];
} stock_data_t;

/* Version 2 tail: the stock factory presets (DTCM 0x20004E40): the 20 named
 * ones, then one "EMPTY" preset. The stock has 20 EMPTY presets that differ
 * only in the module order field 0xbc (no effect on the sound); the first
 * one stands for all 20. */
typedef struct {
    uint8_t preset[STOCK_FACTORY_NAMED + 1][STOCK_PRESET_SIZE];
} stock_factory_t;

/* The checked stock data, or NULL. Set by stock_load() (or by a host test). */
extern const stock_data_t *g_stock;
/* The factory presets (in flash), or NULL with a version 1 blob. */
extern const stock_factory_t *g_stock_factory;

/* 0 if p holds a valid blob of version 1 or 2, else a negative reason code. */
int stock_check(const void *p, uint32_t len);
const char *stock_error(int code);
/* Check the blob at STOCK_FLASH and copy it to RAM; sets g_stock. Returns
 * the stock_check() code. */
int stock_load(void);
#endif
