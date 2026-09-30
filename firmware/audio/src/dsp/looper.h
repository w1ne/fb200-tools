/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_DSP_LOOPER_H
#define FB200_DSP_LOOPER_H
/* Looper (our addition, the stock has none; docs/PARITY.md M8). Mono, after
 * the reverb (it records the processed sound), before the master volume:
 * the loop plays into both channels, the live signal passes at unity.
 *
 * Storage: the external NOR flash (loopstore/loopstore.h), up to 108 s.
 * This file is the audio side: it decodes the loop from the read stream
 * and sends the new frames down the write stream (loopstore/loopio.h); the
 * flash side (loopstore.c, the main loop) moves them to and from the flash.
 * The delay and the long-IR cab keep their RAM.
 *
 * Format: 22.05 kHz (the input decimated by 2 with a 27-tap half-band FIR,
 * flat to 8 kHz, <= -47 dB from 14 kHz, <= -64 dB from 15 kHz; the output
 * interpolated back with the same filter), frames of LC_N samples in
 * 10-bit block floating point (dsp/loopcodec.h, 60 dB SNR).
 *
 * Overdub: new = old * (1 - (1 - LOOPER_FB) * g) + in * g, g the punch
 * gain (ramps over LOOPER_RAMP_MS). Undo: the loop before the last dub (the
 * whole dub, however many passes); again = redo. A dub over the whole loop
 * needs as much free flash as the loop: loops up to undo_max (54 s) dub
 * whole, a longer loop dubs until the free flash runs out (the dub fades
 * out at a chunk end).
 *
 * Loop points are sample-accurate at 22.05 kHz. The first record closes
 * with a crossfade: the first LOOPER_XFADE samples of the loop fade in
 * while what was played right after the close fades out (written as a new
 * version of the first frames), so the wrap has no click. Play, stop, undo
 * and clear fade the loop output over LOOPER_RAMP_MS. A record closes by
 * itself when the flash is full or the erase falls behind (at a chunk end,
 * with the same crossfade).
 *
 * Threads: looper_process runs in engine_task (also inside the flash
 * side's busy waits); everything else in the main loop. The actions that
 * wait for a fade or for the flash side finish in looper_poll. */
#include <stddef.h>
#include <stdint.h>
#include "dsp.h"
#include "dsp/loopcodec.h"
#include "loopstore/loopio.h"

#define LOOPER_FS         22050.0f
#define LOOPER_FB         0.95f                 /* old layers per dubbed pass */
#define LOOPER_RAMP_MS    5
#define LOOPER_MIN_MS     500                   /* shortest loop */
#define LOOPER_XFADE      (8 * LC_N)            /* closing crossfade: 256 samples, 11.6 ms */
#define LOOPER_HB_SIDE    7                     /* half-band side taps (27-tap FIR) */
#define LOOPER_READY_CHUNKS 3u                  /* erased chunks a record needs to start */

enum {
    LOOPER_OFF,     /* no flash area (the chip is too small or unknown) */
    LOOPER_EMPTY,   /* no loop */
    LOOPER_REC,     /* first record */
    LOOPER_PLAY,
    LOOPER_DUB,
    LOOPER_STOP,    /* loop kept, silent */
};

enum {
    LOOPER_TAP,     /* one-switch cycle: rec -> play -> dub -> play ...; stop -> play */
    LOOPER_REC_A,   /* empty: record; recording: close (play) */
    LOOPER_PLAY_A,  /* close a record, end a dub, restart from stop */
    LOOPER_DUB_A,   /* from play (a record closes first) */
    LOOPER_STOP_A,
    LOOPER_UNDO_A,  /* undo the last dub; again = redo */
    LOOPER_CLEAR_A,
};

struct loopstore;

typedef struct {
    loopio_t *io;
    struct loopstore *ls;
    uint8_t state;
    uint8_t alt, redo;            /* the undo map is valid; it is a redo */
    uint8_t pend;                 /* action waiting for silence (looper_poll) */
    uint8_t dub_req;              /* a dub waiting for the writer */
    uint8_t sess, fresh;          /* a write session is open (audio side); first record */
    uint8_t touched;              /* the current frame is new (write it) */
    uint32_t mark;                /* a marker (LIO_END/LIO_SNAP) waiting for a ring slot */
    uint32_t pos, len, nfr;       /* samples / frames at 22.05 kHz */
    uint32_t rec_end;             /* REC closes at this pos */
    uint32_t xfade;               /* samples of the closing crossfade left */
    uint32_t passes;              /* loop wraps */
    uint32_t rd_f;                /* loop frame of the next read-stream frame */
    uint32_t started, w_lc;       /* chunks started by write sessions; the last one */
    uint32_t chunks;              /* the flash area, in chunks (0: none) */
    uint32_t cut;                 /* records and dubs ended early by the free flash */
    float dub_g, dub_t, out_g, out_t, ramp, level;
    float dec[LC_N], enc[LC_N];   /* the frame playing (old); the frame being written */
    /* half-band filters: [history | this block] (26 + 32 at 44.1 kHz,
     * 13 + 16 at 22.05 kHz) */
    float dw[4 * LOOPER_HB_SIDE - 2 + DSP_BLOCK];
    float iw[2 * LOOPER_HB_SIDE - 1 + DSP_BLOCK / 2];
} looper_t;

typedef struct {
    int state;
    unsigned len_ms, pos_ms, max_ms, undo_max_ms;
    int undo;                     /* 0 none, 1 undo, 2 redo */
    unsigned level;
    unsigned prep_ms;             /* erased flash ready for recording */
    int flash;                    /* the flash area exists */
} looper_info_t;

/* OFF when the store has no area, else EMPTY; level 100 */
void looper_init(looper_t *lp, loopio_t *io, struct loopstore *ls);
/* 0 ok, -1 not now, -2 no flash area (OFF), -3 not ready (the flash is
 * still being erased: try again) */
int looper_cmd(looper_t *lp, int action);
/* Save or load record n (0 or 1). Save keeps the current loop (not while
 * recording, empty, or while the writer is busy). Load installs it and
 * stops at frame 0. 0 ok, -1 nothing to save or no record, -2 no flash
 * area, -5 flash write failed, -6 writer busy. */
int looper_save(looper_t *lp, unsigned n);
int looper_load(looper_t *lp, unsigned n);
void looper_poll(looper_t *lp);                  /* main loop: finish the fades */
void looper_set_level(looper_t *lp, unsigned pct);   /* 0..100 = 0..unity */
void looper_info(const looper_t *lp, looper_info_t *out);
/* A write session runs or waits in the flash side: another flash writer
 * (flash_store) must wait. RAM code (flash-write path). */
int looper_writing(const looper_t *lp);
/* In place on the chain's L/R (post reverb), n == DSP_BLOCK. */
void looper_process(looper_t *lp, float *l, float *r, unsigned n);
#endif
