#ifndef FB200_DSP_LOOPER_H
#define FB200_DSP_LOOPER_H
/* Looper (our addition, the stock has none; docs/PARITY.md). Mono, after the
 * reverb (it records the processed sound), before the master volume: the
 * loop plays into both channels, the live signal passes at unity.
 *
 * Memory: none of its own. The engine lends it the delay line and the
 * long-IR tail while a loop exists (loop_mem.h); looper_attach gives it the
 * two areas, looper_detach takes them back.
 *
 * Format: 4-bit IMA ADPCM in blocks of LOOPER_BLK samples, each with a
 * header (predictor, step index), so every block decodes on its own. At
 * 22.05 kHz (default: the input decimated by 2 with a 27-tap half-band FIR,
 * flat to 8 kHz, <= -47 dB from 14 kHz, <= -64 dB from 15 kHz; the output interpolated back with the
 * same filter) or at 44.1 kHz ("hq": half the time). Values are int16 at
 * x 16384: +6 dB of headroom over full scale.
 *
 * Overdub: new = old * (1 - (1 - LOOPER_FB) * g) + in * g, g the punch
 * gain (ramps over LOOPER_RAMP_MS). One undo level when the loop fits in
 * half the memory: a dub starts on a copy of the loop (two banks), undo and
 * redo swap the banks. Longer loops dub in place (no undo).
 *
 * Loop points are sample-accurate at the loop's rate (the record length is
 * the sample count, the wrap is at that sample). The first record closes
 * with a crossfade: the first block of the loop fades in while what was
 * played right after the close fades out, so the wrap has no click. Play,
 * stop, undo and clear fade the loop output over LOOPER_RAMP_MS.
 *
 * Threads: looper_process runs in engine_task; everything else in the main
 * loop (between blocks: nothing runs concurrently). The actions that wait
 * for a fade finish in looper_poll. */
#include <stddef.h>
#include <stdint.h>
#include "dsp.h"

#define LOOPER_BLK        256                   /* samples per ADPCM block */
#define LOOPER_BLK_BYTES  (4 + LOOPER_BLK / 2)  /* header + nibbles */
#define LOOPER_FB         0.95f                 /* old layers per dubbed pass */
#define LOOPER_RAMP_MS    5
#define LOOPER_MIN_BLKS   2                     /* shortest loop: 2 blocks (~23 ms) */
#define LOOPER_HB_SIDE    7                     /* half-band side taps (27-tap FIR) */

enum {
    LOOPER_OFF,     /* no memory (the delay and long IRs have it) */
    LOOPER_EMPTY,   /* memory, no loop */
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

typedef struct {
    int16_t pred;
    uint8_t idx;
} adpcm_t;

typedef struct {
    uint8_t *seg[2];              /* memory: two areas of whole blocks */
    uint32_t nseg0, total, half;  /* blocks in seg[0]; in all; in a bank */
    uint8_t state, hq;
    uint8_t cur;                  /* bank that plays (0/1) */
    uint8_t banks;                /* 2: the loop fits twice (undo), else 1 */
    uint8_t alt, redo;            /* the other bank is valid; it is a redo */
    uint8_t writing;              /* the current block is being rewritten */
    uint8_t pend;                 /* action waiting for silence (looper_poll) */
    uint8_t dub_req;              /* a dub waiting for the writer to finish */
    uint32_t pos, len, nblk;      /* samples at the loop's rate */
    uint32_t rec_end;             /* REC closes at this pos */
    uint32_t xfade;               /* samples of the closing crossfade left */
    uint32_t passes;              /* loop wraps (display) */
    float dub_g, dub_t, out_g, out_t, ramp, level;
    adpcm_t dec, enc;
    /* half-band filters: [history | this block] (26 + 32 at 44.1 kHz,
     * 13 + 16 at 22.05 kHz) */
    float dw[4 * LOOPER_HB_SIDE - 2 + DSP_BLOCK];
    float iw[2 * LOOPER_HB_SIDE - 1 + DSP_BLOCK / 2];
} looper_t;

typedef struct {
    int state;
    unsigned len_ms, pos_ms, max_ms, undo_max_ms;
    int undo;                     /* 0 none, 1 undo, 2 redo */
    int hq;
    unsigned level;
} looper_info_t;

void looper_init(looper_t *lp);   /* OFF, 22.05 kHz, level 100 */
/* Lend the memory (EMPTY) / take it back (OFF, from any state: the loop is
 * gone). m1 may be NULL. */
void looper_attach(looper_t *lp, void *m0, size_t bytes0, void *m1, size_t bytes1);
void looper_detach(looper_t *lp);
/* 0 ok, -1 not now (see the action), -2 no memory (OFF: the engine attaches
 * first, loop_mem.h). */
int looper_cmd(looper_t *lp, int action);
void looper_poll(looper_t *lp);                  /* main loop: finish the fades */
int looper_set_hq(looper_t *lp, int on);         /* -1 while a loop exists */
void looper_set_level(looper_t *lp, unsigned pct);   /* 0..100 = 0..unity */
void looper_info(const looper_t *lp, looper_info_t *out);
/* In place on the chain's L/R (post reverb), n <= DSP_BLOCK, n even. */
void looper_process(looper_t *lp, float *l, float *r, unsigned n);

/* The codec, for the tests. */
unsigned looper_adpcm_enc(adpcm_t *s, int x);
int looper_adpcm_dec(adpcm_t *s, unsigned nib);
#endif
