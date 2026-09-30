/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_LOOPSTORE_LOOPSTORE_H
#define FB200_LOOPSTORE_LOOPSTORE_H
/* The looper's flash side: the loop lives in the external NOR flash
 * (debug/flash_rmw.h FLASH_LOOP_BASE.., 2.94 MB on the 8 MB chip) and
 * streams through the two small rings of loopio.h.
 *
 * Layout: the area is cut into LS_CHUNK slots (8 kB, 2 sectors). The loop
 * is a sequence of logical chunks of LS_FPC frames (199 x 41 B, 289 ms at
 * 22.05 kHz); map cur[] gives the slot of each. NOR cannot be rewritten in
 * place, so a write session (the first record, the closing crossfade, a
 * dub) writes each chunk it touches as a new version into an erased slot
 * (copy on write: the frames it does not touch are copied from the old
 * version) and installs it in cur[] when the chunk is complete. Undo keeps
 * the map from before the last dub (alt[]): undo and redo swap the maps, no
 * copy. Chunks the dub did not touch are shared by both maps, so a short
 * dub on a long loop needs little space, and a loop up to half the area
 * can be dubbed whole. New versions go to the next erased slot round the
 * area (wear levelling).
 *
 * Erase ahead: slots in neither map are garbage; ls_task erases them in the
 * background (64 kB blocks where 8 free slots line up, else sectors), in
 * slices with Erase Suspend (lsio.h), so the pool of erased slots stays
 * ahead of the writer. The audio side never waits: a record or a dub that
 * would need a slot the pool does not have ends cleanly (loopio.h pool,
 * alloc; dsp/looper.c).
 *
 * `loop save` (ls_save): two records in the sector at FLASH_LOOP_META.
 * Each is the loop's maps, so the audio in those slots survives power-off.
 * ls_init marks those slots and erase-ahead will not touch them. `loop
 * clear` drops the live loop only. A new recording takes erased slots, so
 * a saved loop stays until that record is saved over.
 *
 * Threads: ls_task and the ls_* control calls run in the main loop (COLD,
 * XIP: the flash is never busy while they run, lsio.h). The audio side
 * (dsp/looper.c) runs in engine_task, also inside the lsio_* busy waits. */
#include <stdint.h>
#include "loopstore/loopio.h"
#include "debug/flash_rmw.h"

#define LS_CHUNK      8192u
#define LS_FPC        (LS_CHUNK / LC_BYTES)     /* 199 frames per chunk */
#define LS_MAX_CHUNKS 376u                      /* 2.94 MB: F:0x510000..0x800000 */
#define LS_NONE       0xFFFFu
#define LS_SLICE_MS   3u                        /* erase slice (lsio_erase_run) */
#define LS_PAGES_PER_TASK 3u

typedef struct {
    uint32_t n_used, n_erased, n_garbage;
    uint32_t chunks;
    uint32_t base, end;
    uint32_t drops;       /* frames the writer could not place */
} ls_info_t;

typedef struct loopstore {
    loopio_t *io;
    uint16_t *cur, *alt;          /* logical chunk -> slot, LS_NONE */
    uint32_t base;                /* flash offset of slot 0 */
    uint16_t nch;                 /* slots */
    uint8_t armed;                /* erase ahead allowed */
    uint8_t dirty;                /* garbage may exist (the erase scan looks) */
    uint8_t erased[(LS_MAX_CHUNKS + 7u) / 8u];
    uint8_t used[(LS_MAX_CHUNKS + 7u) / 8u];   /* in cur or alt */
    uint16_t alloc_at, erase_at;  /* round-robin cursors */
    /* the erase in progress */
    uint16_t er_slot;
    uint8_t er_n, er_part, er_on; /* slots it covers (1 or 8), sector 0/1 */
    /* the writer: one chunk version at a time */
    uint8_t w_phase;              /* 0 idle, 1 open, 2 the tail */
    uint8_t w_fresh;
    uint16_t w_lc, w_slot, w_old;
    uint32_t w_pos;               /* the next byte of the new version */
    uint32_t w_end;               /* the tail: copy the old version up to here */
    uint32_t w_head;              /* the head: pages [0, w_head) are copied last */
    /* the head of the chunk before, copied while the next chunk takes its
     * frames (installed when done) */
    uint8_t j_on;
    uint16_t j_lc, j_slot, j_old;
    uint32_t j_pos, j_head;
    uint32_t drops;
    /* the reader: absolute frame rd_base_abs is loop frame rd_base_f */
    uint32_t rd_base_abs, rd_base_f;
    uint32_t page[FLASH_PAGE / 4u];  /* the page being assembled */
    uint32_t pool;                /* erased slots (io->pool) */
} loopstore_t;

/* 0 ok, -1 no area (end - base too small) */
int ls_init(loopstore_t *ls, loopio_t *io, uint16_t *cur, uint16_t *alt, uint32_t base,
            uint32_t end);
void ls_arm(loopstore_t *ls);                /* start erasing ahead */
void ls_task(loopstore_t *ls);               /* main loop, every pass */
/* control (main loop, the audio side between blocks) */
void ls_rec_begin(loopstore_t *ls);          /* forget the loop; read from frame 0 */
void ls_dub_begin(loopstore_t *ls);          /* alt = cur (the undo point) */
void ls_swap(loopstore_t *ls, uint32_t next_f);   /* undo / redo */
void ls_clear(loopstore_t *ls);
/* Store or install record n (0 or 1; the console's slots 1 and 2). The
 * record is the maps, the length and the undo flag. Save refuses a busy
 * writer. Load installs the maps and leaves the slots claimed.
 * 0 ok, -1 bad n / nothing to save / no record, -2 no area, -5 the flash
 * write failed, -6 the writer is busy. */
int ls_save(loopstore_t *ls, unsigned n, uint32_t len, int alt_valid, int redo);
int ls_load(loopstore_t *ls, unsigned n, uint32_t *len, int *alt_valid, int *redo);
void ls_play_from(loopstore_t *ls, uint32_t f);   /* the read stream restarts at frame f */
int ls_writer_idle(const loopstore_t *ls);   /* the write ring is empty, no chunk open */
/* work to do now (writes, erases): the main loop should not sleep. Inline:
 * main.c's loop_work_pending is RAM code. */
static inline int ls_busy(const loopstore_t *ls)
{
    const loopio_t *io = ls->io;
    return ls->nch && (io->wr_head != io->wr_tail || ls->w_phase || ls->j_on ||
                       (ls->armed && (ls->er_on || ls->dirty)));
}
uint32_t ls_chunks(const loopstore_t *ls);   /* slots */
void ls_info(const loopstore_t *ls, ls_info_t *out);
#endif
