/* Host test: the SAI rings (audio/sai_ring.h, the code sai.c runs) give the
 * engine whole DSP_BLOCK blocks only, whatever the input chunks, overruns
 * and main-loop stalls, and the DAC stays in step with the ADC: every DAC
 * block is one contiguous engine block (or silence), no frame repeats or
 * goes back, and after a stall the latency comes back to its normal value.
 * The engine side runs the real cab (conv_t steps: 1 per block).
 *
 * The frames carry their ADC index (1-based; L = bits 0..14, R = bits
 * 15..29), so the DAC side reads back which input frame it plays. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio/sai.h"
#include "audio/sai_ring.h"
#include "dsp/cab.h"
#include "dsp/dsp.h"

#define B SAI_BLOCK_FRAMES
_Static_assert(B == DSP_BLOCK, "one SAI block = one DSP block");

static sai_ring_t s_rx, s_tx;
static cab_t s_cab;
static uint32_t s_rand = 12345u;
static uint32_t rnd(uint32_t n)
{
    s_rand = s_rand * 1664525u + 1013904223u;
    return (s_rand >> 8) % n;
}

static void enc(int16_t *f, uint32_t idx)
{
    f[0] = (int16_t)(idx & 0x7fffu);
    f[1] = (int16_t)((idx >> 15) & 0x7fffu);
}
static uint32_t dec(const int16_t *f)
{
    return (uint32_t)(uint16_t)f[0] | (uint32_t)(uint16_t)f[1] << 15;
}

/* ---- ring unit checks ---- */
static void test_ring_units(void)
{
    static sai_ring_t r;
    int16_t blk[B * 2], got[B * 2];
    for (unsigned i = 0; i < B; i++) enc(&blk[i * 2], i + 1);
    /* the counters wrap in the middle of the run */
    r.head = r.tail = 0xFFFFFF00u;
    unsigned puts = 0;
    while (sai_ring_put(&r, blk, B)) puts++;
    assert(puts == SAI_RING_FRAMES / B);             /* the full size is usable */
    assert(sai_ring_fill(&r) == SAI_RING_FRAMES);
    assert(!sai_ring_put(&r, blk, 1));               /* full: nothing written */
    assert(sai_ring_fill(&r) == SAI_RING_FRAMES);
    for (unsigned k = 0; k < puts; k++) {
        assert(sai_ring_get(&r, got, B));
        assert(memcmp(got, blk, sizeof blk) == 0);
    }
    assert(r.head == 0xFFFFFF00u + SAI_RING_FRAMES);  /* wrapped past 0 */
    assert(!sai_ring_get(&r, got, B));
    /* 31 queued: no block, nothing taken */
    assert(sai_ring_put(&r, blk, B - 1));
    assert(!sai_ring_get(&r, got, B));
    assert(sai_ring_fill(&r) == B - 1);
    /* 500 queued: a 32-frame chunk does not fit and is dropped whole (the
     * first ring wrote 12 of it and left the 32-frame grid) */
    static sai_ring_t o;
    int16_t big[500 * 2] = {0};
    assert(sai_ring_put(&o, big, 500));
    assert(!sai_ring_put(&o, blk, B));
    assert(sai_ring_fill(&o) == 500);
    /* the bounded push: skipped above max_fill, never partial */
    static sai_ring_t t;
    unsigned pushed = 0;
    for (unsigned k = 0; k < 10; k++) pushed += (unsigned)sai_ring_put_bounded(&t, blk, B, SAI_TX_TARGET_FILL);
    assert(pushed == SAI_TX_TARGET_FILL / B + 1);
    assert(sai_ring_fill(&t) == SAI_TX_TARGET_FILL + B);
    printf("ring units OK\n");
}

/* ---- the engine loop against the eDMA ---- */
typedef struct {
    uint64_t blocks, rx_drops, rx_dropped_frames, skips, silent_blocks;
    uint32_t last_idx;          /* last ADC index the DAC played */
    uint32_t lat_max;           /* DAC time - ADC index, over the window */
} sim_t;

/* engine_task: one whole block from RX, through the cab, into the DAC ring */
static int engine_step(sim_t *s)
{
    int16_t in[B * 2];
    if (!sai_ring_get(&s_rx, in, B)) return 0;
    float x[B];
    for (unsigned i = 0; i < B; i++) x[i] = (float)in[i * 2] * (1.0f / 32768.0f);
    uint32_t steps = s_cab.conv.head.steps;
    cab_process(&s_cab, x, B);
    assert(s_cab.conv.head.steps - steps == 1u);      /* no re-phase, ever */
    for (unsigned i = 0; i < B; i++) assert(x[i] > -2.0f && x[i] < 2.0f);
    /* identity to the DAC: the index goes through untouched */
    if (!sai_ring_put_bounded(&s_tx, in, B, SAI_TX_TARGET_FILL)) s->skips++;
    return 1;
}

/* One DAC block (the TX eDMA callback): whole engine block or silence. */
static void dac_block(sim_t *s, uint64_t t, int aligned)
{
    int16_t out[B * 2];
    if (!sai_ring_get(&s_tx, out, B)) {
        s->silent_blocks++;
        return;
    }
    uint32_t first = dec(&out[0]);
    assert(first > s->last_idx);                      /* never repeats or goes back */
    if (aligned) assert(first % B == 1u);             /* on the ADC block grid */
    for (unsigned i = 1; i < B; i++) assert(dec(&out[i * 2]) == first + i);   /* contiguous */
    s->last_idx = first + B - 1;
    uint32_t lat = (uint32_t)(t - first);
    if (lat > s->lat_max) s->lat_max = lat;
}

/* ticks of B frames. chunk 0: the ADC delivers B-frame blocks (the eDMA);
 * else chunks of 1..2*chunk-1 frames. stall_per: a stall starts with
 * probability 1/stall_per per tick and lasts 1..max_stall ticks. */
static uint64_t s_t, s_produced;
static unsigned s_stall;

static void run(sim_t *s, uint64_t ticks, unsigned chunk, unsigned stall_per, unsigned max_stall)
{
    for (uint64_t k = 0; k < ticks; k++) {
        s_t += B;
        /* ADC: everything up to now, in chunks */
        while (s_produced + (chunk ? 1u : B) <= s_t) {
            unsigned n = chunk ? 1u + rnd(2u * chunk - 1u) : B;
            if (s_produced + n > s_t && chunk) n = (unsigned)(s_t - s_produced);
            int16_t buf[128 * 2];
            for (unsigned i = 0; i < n; i++) enc(&buf[i * 2], (uint32_t)(s_produced + i + 1));
            if (!sai_ring_put(&s_rx, buf, n)) {
                s->rx_drops++;
                s->rx_dropped_frames += n;
            }
            s_produced += n;
        }
        dac_block(s, s_t, chunk == 0);
        /* main loop: stalled, or 1..3 passes (engine_task takes one block) */
        if (s_stall) {
            s_stall--;
        } else {
            if (stall_per && rnd(stall_per) == 0) s_stall = 1u + rnd(max_stall);
            unsigned passes = 1u + rnd(3);
            for (unsigned p = 0; p < passes; p++) {
                if (engine_step(s)) s->blocks++;
            }
        }
        if (chunk == 0) assert(sai_ring_fill(&s_rx) % B == 0u);   /* on the grid */
    }
}

static void scenario(const char *name, unsigned chunk)
{
    memset(&s_rx, 0, sizeof s_rx);
    memset(&s_tx, 0, sizeof s_tx);
    cab_init(&s_cab);
    static float ir[CAB_TAPS];               /* a cab (flat = no convolver) */
    for (unsigned i = 0; i < CAB_TAPS; i++) ir[i] = (float)(int)(rnd(2001) - 1000u) * 1e-5f;
    ir[0] = 0.5f;
    cab_set_ir(&s_cab, ir, 1.0f);
    assert(s_cab.active);
    s_t = s_produced = 0;
    s_stall = 0;
    sim_t s = {0};
    /* start-up: the engine starts 20 ms late (the boot overrun) */
    s_stall = 28;
    run(&s, 2000, chunk, 0, 0);
    uint32_t base = 0;
    s.lat_max = 0;
    run(&s, 4000, chunk, 0, 0);
    base = s.lat_max;
    uint64_t drops0 = s.rx_drops;
    /* stalls up to 40 blocks (29 ms: a long console command, a flash write) */
    run(&s, 200000, chunk, 150, 40);
    uint64_t drops = s.rx_drops - drops0;
    assert(drops > 0 && s.skips > 0 && s.silent_blocks > 0);   /* the paths ran */
    /* calm again: the latency comes back to the normal value */
    run(&s, 2000, chunk, 0, 0);
    s.lat_max = 0;
    run(&s, 4000, chunk, 0, 0);
    printf("%s: %llu blocks, rx drops %llu (%llu frames), tx skips %llu, silent %llu, "
           "latency normal %u, after stalls %u frames\n", name,
           (unsigned long long)s.blocks, (unsigned long long)drops,
           (unsigned long long)s.rx_dropped_frames, (unsigned long long)s.skips,
           (unsigned long long)s.silent_blocks, (unsigned)base, (unsigned)s.lat_max);
    /* a stall must not leave extra latency behind (odd chunks: the part
     * block waiting in RX can differ, < B) */
    assert(s.lat_max <= base + (chunk ? B - 1u : 0u));
    assert(base <= SAI_TX_TARGET_FILL + 3u * B);
}

int main(void)
{
    test_ring_units();
    scenario("edma blocks", 0);
    scenario("odd chunks", 24);
    scenario("tiny chunks", 3);
    printf("blocks host tests OK\n");
    return 0;
}
