#ifndef FB200_SAI_RING_H
#define FB200_SAI_RING_H
/* The SAI frame rings (stereo interleaved int16) between the eDMA callbacks
 * and the engine. Single producer, single consumer; one side runs in an
 * interrupt. Header only, so the host test (tests/blocks_host_test.c) runs
 * this code.
 *
 * Whole chunks only: a put that does not fit drops the whole chunk (the
 * caller counts it), a get takes exactly n frames or nothing. The first
 * ring wrote as much of an overrun block as fitted: the fill left the
 * 32-frame grid and the engine then got short blocks (n < DSP_BLOCK) until
 * the next underrun; the partitioned FFT cab re-phased (~100k cycles) and
 * the per-block state (tuner, drums, glides, `prof`) counted a short block
 * as a whole one. With 32-frame DMA blocks the fill now stays a multiple of
 * 32, and the engine only ever runs whole blocks.
 *
 * head and tail count frames and run free (uint32 wrap is fine: the size is
 * a power of two); the full size is usable. */
#include <stddef.h>
#include <stdint.h>

#define SAI_RING_FRAMES 512u
_Static_assert((SAI_RING_FRAMES & (SAI_RING_FRAMES - 1u)) == 0u, "power of two");

typedef struct {
    int16_t buf[SAI_RING_FRAMES * 2];
    volatile uint32_t head;      /* frames written (producer) */
    volatile uint32_t tail;      /* frames read (consumer) */
} sai_ring_t;

#define SAI_RING_BARRIER() __asm volatile("" ::: "memory")

static inline uint32_t sai_ring_fill(const sai_ring_t *r)
{
    return r->head - r->tail;
}

/* All n frames, or none (returns 0: the ring had no room). */
static inline int sai_ring_put(sai_ring_t *r, const int16_t *src, size_t n)
{
    uint32_t h = r->head;
    if (n > SAI_RING_FRAMES - (h - r->tail)) return 0;
    for (size_t i = 0; i < n; i++, h++) {
        uint32_t k = h & (SAI_RING_FRAMES - 1u);
        r->buf[k * 2 + 0] = src[i * 2 + 0];
        r->buf[k * 2 + 1] = src[i * 2 + 1];
    }
    SAI_RING_BARRIER();          /* the frames before the new head */
    r->head = h;
    return 1;
}

/* The engine's DAC push: none while more than max_fill frames are queued
 * (the latency bound, sai.h SAI_TX_TARGET_FILL), else sai_ring_put. */
static inline int sai_ring_put_bounded(sai_ring_t *r, const int16_t *src, size_t n,
                                       uint32_t max_fill)
{
    if (sai_ring_fill(r) > max_fill) return 0;
    return sai_ring_put(r, src, n);
}

/* All n frames, or none (returns 0: fewer than n queued). */
static inline int sai_ring_get(sai_ring_t *r, int16_t *dst, size_t n)
{
    uint32_t t = r->tail;
    if (r->head - t < n) return 0;
    SAI_RING_BARRIER();          /* the frames after the head check */
    for (size_t i = 0; i < n; i++, t++) {
        uint32_t k = t & (SAI_RING_FRAMES - 1u);
        dst[i * 2 + 0] = r->buf[k * 2 + 0];
        dst[i * 2 + 1] = r->buf[k * 2 + 1];
    }
    SAI_RING_BARRIER();
    r->tail = t;
    return 1;
}
#endif
