/* Audio engine: codec ADC -> DSP chain -> codec DAC, with the USB path as
 * capture (processed signal) and monitor (host playback mixed into the DAC).
 *
 * Drift: the host playback ring is elastic - when it starves the last frame
 * is repeated, when it stays overfull the tail is advanced, so long-term
 * latency stays bounded. Both are counted in engine_stats_t.
 *
 * The DSP chain is currently a gain node plus an optional test generator;
 * the real chain (EQ, amp sim, ...) lands in later milestones. */
#include <string.h>
#include "audio/engine.h"
#include "audio/audio_config.h"


#ifndef ENGINE_HOST_TEST
#include "audio/sai.h"
#include "audio/usb_audio.h"
#include "audio/codec.h"
#include "dsp/dsp.h"
#include "dsp/gain.h"
#include "dsp/math.h"
#include "dsp/testgen.h"
#include "fsl_sai.h"
#include "tusb.h"
#endif

/* One engine block = one DSP block: s_block holds DSP_BLOCK frames per
 * channel. (It was 64 against a 32-frame dsp_block_t: after the start-up
 * backlog the engine wrote past s_block.) */
#include "dsp/dsp.h"
#define ENGINE_FRAMES DSP_BLOCK
#define RING_FRAMES 512
#define MAX_RING_FILL (RING_FRAMES - ENGINE_FRAMES)
#define FAULT_MUTE_MS 100u
/* DAC latency bound. At start-up the RX ring overflows before the engine
 * runs; draining that backlog filled the TX ring (511 frames = 10.6 ms) and,
 * with equal rates, it stayed full. Above the target, skip a block. */
#define TX_TARGET_FILL (2u * SAI_BLOCK_FRAMES + ENGINE_FRAMES)

/* ---- pure drift helpers (host-tested) ---- */

size_t engine_drift_fill(const int16_t *ring, uint32_t cap, uint32_t *tail,
                         uint32_t head, int16_t *out, size_t frames,
                         int16_t last[2], uint32_t *inserts)
{
    size_t n = 0;
    while (n < frames) {
        if (*tail == head) {
            out[n * 2 + 0] = last[0];
            out[n * 2 + 1] = last[1];
            (*inserts)++;
        } else {
            last[0] = ring[*tail * 2 + 0];
            last[1] = ring[*tail * 2 + 1];
            out[n * 2 + 0] = last[0];
            out[n * 2 + 1] = last[1];
            *tail = (*tail + 1u) % cap;
        }
        n++;
    }
    return n;
}

uint32_t engine_drift_trim(uint32_t cap, uint32_t head, uint32_t *tail,
                           uint32_t max_fill, uint32_t *drops)
{
    uint32_t fill = (head + cap - *tail) % cap;
    uint32_t dropped = 0;
    while (fill > max_fill) {
        *tail = (*tail + 1u) % cap;
        dropped++;
        fill--;
    }
    *drops += dropped;
    return dropped;
}

volatile float g_meter_peak[2];

#ifndef ENGINE_HOST_TEST

static gain_ctx_t s_gain;
static testgen_ctx_t s_testgen;
static dsp_block_t s_block;
static engine_stats_t s_stats;
static float s_gain_db;
static bool s_mute;          /* user mute (console / later: UI) */
static bool s_fault_mute;    /* SAI FIFO fault: short mute, then release */
static uint32_t s_fault_ms;
static bool s_meters;
static uint32_t s_drop_tx_blocks;
static uint32_t s_meter_last_ms;

void engine_init(void)
{
    usb_audio_init();
    sai_audio_init();
    gain_init(&s_gain, 1.0f);
    testgen_init(&s_testgen, (float)AUDIO_FS);
    memset(&s_stats, 0, sizeof(s_stats));
    g_meter_peak[0] = g_meter_peak[1] = 0.0f;
    s_gain_db = 0.0f;
    s_mute = false;
    s_fault_mute = false;
    s_meters = false;
}

void engine_get_stats(engine_stats_t *out)
{
    *out = s_stats;
}

void engine_set_gain_db(float db)
{
    s_gain_db = db;
    gain_set(&s_gain, dsp_db_to_gain(db));
}

float engine_get_gain_db(void)
{
    return s_gain_db;
}

void engine_set_testgen(int mode, float amp, float freq)
{
    testgen_set(&s_testgen, (testgen_mode_t)mode, amp, freq);
}

void engine_set_mute(bool mute)
{
    s_mute = mute;
    codec_mute(s_mute || s_fault_mute);
}

bool engine_get_mute(void)
{
    return s_mute || s_fault_mute;
}

void engine_set_meters(bool on)
{
    s_meters = on;
}

void engine_drop_tx(uint32_t blocks)
{
    s_drop_tx_blocks = blocks;
}

static void check_sai_faults(void)
{
    uint32_t rx = SAI_RxGetStatusFlag(SAI1);
    uint32_t tx = SAI_TxGetStatusFlag(SAI1);
    if ((rx & kSAI_FIFOErrorFlag) != 0u || (tx & kSAI_FIFOErrorFlag) != 0u) {
        s_stats.dma_errors++;
        SAI_RxClearStatusFlags(SAI1, kSAI_FIFOErrorFlag);
        SAI_TxClearStatusFlags(SAI1, kSAI_FIFOErrorFlag);
        s_fault_ms = tusb_time_millis_api();
        if (!s_fault_mute) {
            s_fault_mute = true;
            codec_mute(true);
        }
    } else if (s_fault_mute && tusb_time_millis_api() - s_fault_ms >= FAULT_MUTE_MS) {
        /* A fault used to mute for good; start-up underruns left the pedal
         * silent. Release after FAULT_MUTE_MS without a new fault. */
        s_fault_mute = false;
        codec_mute(s_mute);
    }
}

static void update_meters(const dsp_block_t *b, size_t n)
{
    float peak0 = 0.0f, peak1 = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float a = b->data[0][i], c = b->data[1][i];
        if (a < 0) a = -a;
        if (c < 0) c = -c;
        if (a > peak0) peak0 = a;
        if (c > peak1) peak1 = c;
    }
    uint32_t now = tusb_time_millis_api();
    if (now - s_meter_last_ms >= 1000u) {
        s_meter_last_ms = now;
        g_meter_peak[0] = peak0;
        g_meter_peak[1] = peak1;
    }
}

/* Everything is paced by the codec ADC (SAI RX): each block of n input
 * frames yields exactly n output frames = processed input + host playback.
 * (The first version also pushed a 64-frame monitor block on every main-loop
 * pass, paced by the CPU: the DAC ring sat full, the processed signal was
 * dropped, and the insert counter ran away. Seen on the pedal 2026-09-27.) */
void engine_task(void)
{
    static int16_t last[2];
    int16_t in[ENGINE_FRAMES * 2];
    int16_t play[ENGINE_FRAMES * 2];
    int16_t out[ENGINE_FRAMES * 2];

    check_sai_faults();

    size_t n = sai_pull(in, ENGINE_FRAMES);
    if (n == 0) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        s_block.data[0][i] = (float)in[i * 2 + 0] * (1.0f / 32768.0f);
        s_block.data[1][i] = (float)in[i * 2 + 1] * (1.0f / 32768.0f);
    }
    gain_process(&s_gain, &s_block, n);
    if (s_testgen.mode != TESTGEN_OFF) {
        testgen_process(&s_testgen, &s_block, n);
    }
    if (s_meters) {
        update_meters(&s_block, n);
    }

    /* USB capture carries the processed signal only. */
    {
        float fb[ENGINE_FRAMES * 2];
        for (size_t i = 0; i < n; i++) {
            fb[i * 2 + 0] = s_block.data[0][i];
            fb[i * 2 + 1] = s_block.data[1][i];
        }
        usb_audio_push(fb, n);
    }

    /* Host playback, drift-compensated, only while the host streams. */
    if (usb_audio_playing()) {
        (void)usb_audio_trim(MAX_RING_FILL, &s_stats.fifo_drops);
        (void)usb_audio_pull16(play, n, last, &s_stats.fifo_inserts);
    } else {
        memset(play, 0, n * 2 * sizeof play[0]);
        last[0] = last[1] = 0;
    }

    for (size_t i = 0; i < n; i++) {
        float l = s_block.data[0][i] + (float)play[i * 2 + 0] * (1.0f / 32768.0f);
        float r = s_block.data[1][i] + (float)play[i * 2 + 1] * (1.0f / 32768.0f);
        if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
        out[i * 2 + 0] = (int16_t)(l * 32767.0f);
        out[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }
    if (s_drop_tx_blocks != 0) {
        s_drop_tx_blocks--; /* `x` underrun check: skip the refill */
    } else if (sai_tx_fill() > TX_TARGET_FILL) {
        s_stats.latency_skips++;
    } else {
        (void)sai_push(out, n);
    }
}

#endif /* ENGINE_HOST_TEST */
