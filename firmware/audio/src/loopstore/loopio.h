/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_LOOPSTORE_LOOPIO_H
#define FB200_LOOPSTORE_LOOPIO_H
/* The two frame streams between the looper's audio side (dsp/looper.c,
 * engine_task) and its flash side (loopstore.c, the main loop), in DTCM.
 * Frames are LC_BYTES (dsp/loopcodec.h). Single producer, single consumer;
 * the audio side also runs inside the flash side's busy waits (flash_pump),
 * so every index is written by one side only. Counters are absolute frame
 * numbers that run free (uint32 wrap is fine: LIO_RING is a power of two).
 *
 *   read (flash -> audio): the frames of the loop in play order. The flash
 *     side fills rd_head.. (never more than LIO_RING ahead of rd_tail); the
 *     audio side takes frame rd_tail at each frame start, or counts an
 *     underrun and plays silence for that frame (rd_tail moves on either
 *     way: the loop keeps time). The flash side skips to rd_tail when it
 *     is behind.
 *   write (audio -> flash): new frames with their loop frame index and
 *     flags; LIO_END closes a write session (no data). A full ring drops
 *     the frame (wr_over): the flash side fills the gap from the old frame
 *     (or silence for a first record). LIO_SNAP (no data) marks a dub's
 *     start: the flash side takes the undo point there, after every frame
 *     before it is in place (ls_dub_begin). */
#include <stdint.h>
#include "dsp/loopcodec.h"

#define LIO_RING   16u
#define LIO_FRESH  0x40000000u   /* first record: the chunk has no old version */
#define LIO_END    0x80000000u   /* end of a write session */
#define LIO_SNAP   0x20000000u   /* a dub starts: the map so far is the undo point */
#define LIO_FRAME  0x00FFFFFFu

typedef struct {
    uint8_t rd[LIO_RING][LC_BYTES];
    uint8_t wr[LIO_RING][LC_BYTES];
    uint32_t wr_meta[LIO_RING];
    volatile uint32_t rd_head;     /* flash side */
    volatile uint32_t rd_tail;     /* audio side */
    volatile uint32_t wr_head;     /* audio side */
    volatile uint32_t wr_tail;     /* flash side */
    /* flash side: erased chunks not yet given to a write session, and the
     * chunks given so far (monotonic): the audio side may start a new
     * chunk only while (its chunks started - alloc) < pool */
    volatile uint32_t pool;
    volatile uint32_t alloc;
    volatile uint32_t w_busy;      /* flash side: a chunk is still being written */
    volatile uint32_t nfr;         /* audio side: loop length in frames (0: first record) */
    uint32_t rd_under, wr_over;    /* audio side: counters */
} loopio_t;

#define LIO_BARRIER() __asm volatile("" ::: "memory")

/* audio side: the next frame of the read stream, or NULL (underrun) */
static inline const uint8_t *lio_rd_take(loopio_t *io)
{
    uint32_t t = io->rd_tail;
    const uint8_t *f = (int32_t)(io->rd_head - t) > 0 ? io->rd[t % LIO_RING] : 0;
    if (!f) io->rd_under++;
    return f;
}

static inline void lio_rd_done(loopio_t *io)   /* after lio_rd_take (data read) */
{
    LIO_BARRIER();
    io->rd_tail = io->rd_tail + 1u;
}

/* audio side: a slot for a new frame, or NULL (full: counted, dropped) */
static inline uint8_t *lio_wr_slot(loopio_t *io)
{
    if (io->wr_head - io->wr_tail >= LIO_RING) {
        io->wr_over++;
        return 0;
    }
    return io->wr[io->wr_head % LIO_RING];
}

static inline void lio_wr_push(loopio_t *io, uint32_t meta)   /* after lio_wr_slot */
{
    io->wr_meta[io->wr_head % LIO_RING] = meta;
    LIO_BARRIER();
    io->wr_head = io->wr_head + 1u;
}

/* audio side: a marker (LIO_END, LIO_SNAP) needs a slot too; 0 when full */
static inline int lio_wr_mark(loopio_t *io, uint32_t mark)
{
    if (io->wr_head - io->wr_tail >= LIO_RING) return 0;
    lio_wr_push(io, mark);
    return 1;
}
#endif
