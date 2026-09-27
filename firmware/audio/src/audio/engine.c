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
#include "audio/bt_audio.h"
#include "audio/codec.h"
#include "dsp/dsp.h"
#include "dsp/gain.h"
#include "dsp/math.h"
#include "dsp/testgen.h"
#include "dsp/drums.h"
#include "dsp/tuner.h"
#include "dsp/amp.h"
#include "dsp/cab.h"
#include "dsp/gate.h"
#include "dsp/comp.h"
#include "dsp/mod.h"
#include "dsp/reverb.h"
#include "preset/preset.h"
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
static tuner_t s_tuner;
static drums_t s_drums;
static bool s_tuner_on;
static bool s_tuner_mute = true;        /* S+0x2e, stock default 1 */
static dsp_knob_t s_in_gain;            /* S+0x1a, smoothed like the stock */
/* the stock chain (docs/PARITY.md M2): amp (+ tone stack) -> cab, mono */
static amp_t s_amp;
static cab_t s_cab;
static gate_t s_gate;
static comp_t s_comp;
static mod_t s_mod;
static reverb_t s_rev;
static bool s_amp_en, s_cab_en, s_gate_en, s_comp_en, s_mod_en, s_rev_en;
static int s_amp_model = -1, s_cab_type = -1;
static float s_master = 1.0f, s_master_target = 1.0f;
static float s_ir[CAB_TAPS];
static bool s_testgen_in;               /* testgen feeds the chain input */
static uint32_t s_cyc_max, s_cyc_sum, s_cyc_n;   /* DWT cycles per block */
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
    tuner_init(&s_tuner, 440);
    /* stock input gain smoother (callback 0x17ffe): y = t*0.001 + y*0.999 */
    dsp_knob_init(&s_in_gain, 0.001f, 0.999f);
    dsp_knob_set(&s_in_gain, 0.9999702f);
    amp_init(&s_amp, (float)AUDIO_FS);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   /* cycle counter for `stats` */
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    cab_init(&s_cab);
    bt_audio_init();
    gate_init(&s_gate, (float)AUDIO_FS);
    comp_init(&s_comp, (float)AUDIO_FS);
    mod_init(&s_mod, (float)AUDIO_FS);
    reverb_init(&s_rev, (float)AUDIO_FS);
    static drums_data_t rhythms;
    if (g_stock) drums_data_from_stock(&rhythms, g_stock);
    drums_init(&s_drums, (const void *)DRUMS_BANK_ADDR, g_stock ? &rhythms : NULL);  /* NULL: silent */
}

void engine_set_tuner(bool on) { s_tuner_on = on; }

static volatile bool s_reapply;

/* Re-initialise every DSP module; the preset is re-applied from the main
 * loop (engine_needs_reapply). */
void engine_dsp_reset(void)
{
    amp_init(&s_amp, (float)AUDIO_FS);
    cab_init(&s_cab);
    gate_init(&s_gate, (float)AUDIO_FS);
    comp_init(&s_comp, (float)AUDIO_FS);
    mod_init(&s_mod, (float)AUDIO_FS);
    reverb_init(&s_rev, (float)AUDIO_FS);
    s_amp_model = s_cab_type = -1;
    s_reapply = true;
}

bool engine_needs_reapply(void)
{
    bool r = s_reapply;
    s_reapply = false;
    return r;
}

/* User IR slots (cab 11..19): the stock IR store, F:0x88000 used flags,
 * F:0x89000 + slot * 0x2800 data (float32). Returns 0 if the slot is empty.
 * The stock plays an empty slot as silence; that left users with a pedal
 * that "stopped working" (seen 2026-09-27: our first version even read the
 * erased flash as NaN taps and stayed silent for good). An empty slot now
 * bypasses the cab instead. */
static int load_user_ir(unsigned slot)
{
    const volatile uint8_t *used = (const volatile uint8_t *)(0x60000000u + 0x88000u);
    if (used[slot] != 1) return 0;
    const float *ir = (const float *)(0x60000000u + 0x89000u + slot * 0x2800u);
    memcpy(s_ir, ir, sizeof s_ir);
    for (unsigned i = 0; i < CAB_TAPS; i++)
        if (!(s_ir[i] > -1e6f && s_ir[i] < 1e6f)) s_ir[i] = 0.0f;   /* NaN/inf -> 0 */
    cab_set_ir(&s_cab, s_ir, cab_user_ir_gain(s_ir));
    return 1;
}

static bool s_cab_bypass;   /* user IR slot selected but empty */

void engine_apply_preset(const preset_t *p, unsigned master)
{
    s_amp_en = pget(p, P_AMP_EN) != 0;
    s_cab_en = pget(p, P_CAB_EN) != 0;
    int model = pget(p, P_AMP_MODEL);
    if (model != s_amp_model && amp_set_model(&s_amp, model) == 0) s_amp_model = model;
    amp_set_params(&s_amp, pget(p, P_AMP_GAIN), pget(p, P_AMP_BASS), pget(p, P_AMP_MID),
                   pget(p, P_AMP_MIDFREQ), pget(p, P_AMP_TREBLE), pget(p, P_AMP_VOLUME));
    int cab = pget(p, P_CAB_TYPE);
    if (cab != s_cab_type) {
        s_cab_bypass = false;
        if (cab >= 1 && cab <= 10) (void)cab_set_model(&s_cab, cab);
        else if (cab >= 11 && cab <= 19) s_cab_bypass = !load_user_ir((unsigned)(cab - 11));
        s_cab_type = cab;
    }
    s_gate_en = pget(p, P_GATE_EN) != 0;
    gate_set_params(&s_gate, pget(p, P_GATE_THRESH));
    s_comp_en = pget(p, P_COMP_EN) != 0;
    comp_set_params(&s_comp, pget(p, P_COMP_TYPE), pget(p, P_COMP_ATTACK), pget(p, P_COMP_THRESH),
                    pget(p, P_COMP_RATIO), pget(p, P_COMP_LEVEL));
    s_mod_en = pget(p, P_MOD_EN) != 0;
    mod_set_params(&s_mod, pget(p, P_MOD_TYPE), pget(p, P_MOD_P1), pget(p, P_MOD_P2),
                   pget(p, P_MOD_P3), pget(p, P_MOD_P4));
    s_rev_en = pget(p, P_REV_EN) != 0;
    reverb_set_params(&s_rev, pget(p, P_REV_TYPE), pget(p, P_REV_LEVEL), pget(p, P_REV_DECAY),
                      pget(p, P_REV_AE), pget(p, P_REV_A8));
    s_master_target = (float)(master > 100u ? 100u : master) * 0.01f;
}
void engine_apply_settings(const settings_t *s)
{
    /* 0 dB: the float the stock smoother settles on from below (it stalls
     * short of 1.0), for bit parity at the default setting */
    float g = gain_input_stock(s->b[S_IN_GAIN]);
    dsp_knob_set(&s_in_gain, g == 1.0f ? 0.9999702f : g);
    /* stock 0x913c: A4 = 435 + S+0x2c (default 5 = 440 Hz); out of range
     * (e.g. an erased block) -> the default */
    unsigned cal = s->b[S_TUNER_CAL];
    tuner_set_a4(&s_tuner, 435 + (int)(cal <= 15u ? cal : 5u));
    s_tuner_mute = s->b[S_TUNER_MUTE] != 0;
}

bool engine_tuner_poll(tuner_result_t *out) { return tuner_poll(&s_tuner, out) != 0; }
drums_t *engine_drums(void) { return &s_drums; }

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

void engine_testgen_input(bool on) { s_testgen_in = on; }

void engine_cycles(uint32_t *avg, uint32_t *max, uint32_t *budget)
{
    *avg = s_cyc_n ? s_cyc_sum / s_cyc_n : 0;
    *max = s_cyc_max;
    *budget = (uint32_t)((uint64_t)SystemCoreClock * ENGINE_FRAMES / AUDIO_FS);
    s_cyc_sum = s_cyc_n = s_cyc_max = 0;
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
    /* The stock feeds the chain and the tuner with L + R of the instrument
     * ADC; into the chain times the input gain (S+0x1a). At the default
     * 0 dB that is x 0.9999702 (a unity gain whose smoother stalls there -
     * needed for bit parity, tests/test_stock_dsp_parity.py). */
    uint32_t t0 = DWT->CYCCNT;
    float x[ENGINE_FRAMES];
    if (s_testgen_in && s_testgen.mode != TESTGEN_OFF) {   /* test signal into the chain */
        memset(&s_block, 0, sizeof s_block);
        testgen_process(&s_testgen, &s_block, n);
        for (size_t i = 0; i < n; i++) s_block.data[1][i] = 0.0f;   /* mono source on L */
    }
    for (size_t i = 0; i < n; i++) x[i] = s_block.data[0][i] + s_block.data[1][i];
    tuner_feed(&s_tuner, x, n);
    for (size_t i = 0; i < n; i++) x[i] *= dsp_knob_next(&s_in_gain);
    /* stock chain (ITCM 0x7b60): gate -> comp -> amp -> cab -> mod -> reverb */
    if (s_gate_en) gate_process(&s_gate, x, (unsigned)n);
    if (s_comp_en) comp_process(&s_comp, x, (unsigned)n);
    if (s_amp_en) amp_process(&s_amp, x, (unsigned)n);
    if (s_cab_en && !s_cab_bypass) cab_process(&s_cab, x, (unsigned)n);
    if (s_mod_en) mod_process(&s_mod, x, (unsigned)n);
    float xl[ENGINE_FRAMES], xr[ENGINE_FRAMES];
    if (s_rev_en) reverb_process(&s_rev, x, xl, xr, (unsigned)n);   /* mono in, L/R out */
    else { memcpy(xl, x, n * sizeof x[0]); memcpy(xr, x, n * sizeof x[0]); }
    /* Never get stuck silent: one NaN/inf in a filter state keeps a chain
     * outputting NaN (which reaches the DAC as 0). Reset the DSP state and
     * re-apply the preset on the next main-loop pass. */
    for (size_t i = 0; i < n; i++) {
        if (!(xl[i] > -1e6f && xl[i] < 1e6f) || !(xr[i] > -1e6f && xr[i] < 1e6f)) {
            engine_dsp_reset();
            memset(xl, 0, sizeof xl);
            memset(xr, 0, sizeof xr);
            s_stats.dsp_resets++;
            break;
        }
    }
    float btl[ENGINE_FRAMES] = {0}, btr[ENGINE_FRAMES] = {0};
    size_t nbt = bt_audio_pull(btl, btr, n);                 /* Bluetooth audio in */
    for (size_t i = nbt; i < n; i++) btl[i] = btr[i] = 0.0f;
    for (size_t i = 0; i < n; i++) {
        s_master += 0.001f * (s_master_target - s_master);   /* stock: smoothed master */
        float g = s_tuner_on && s_tuner_mute ? 0.0f : s_master;   /* stock: S+0x2e mutes the tuner */
        /* stock mix: out = chain*master + BT*master*1.3 (capture: + BT) */
        s_block.data[0][i] = xl[i] * g + btl[i] * g * 1.3f;
        s_block.data[1][i] = xr[i] * g + btr[i] * g * 1.3f;
    }
    gain_process(&s_gain, &s_block, n);                         /* console `gain` */
    if (s_testgen.mode != TESTGEN_OFF && !s_testgen_in) {
        testgen_process(&s_testgen, &s_block, n);
    }
    if (s_meters) {
        update_meters(&s_block, n);
    }

    /* Drums before the capture tap: the stock leaves them out of the USB
     * recording; a user playing along wants them in (better than stock). */
    if (!s_tuner_on) drums_process_stereo(&s_drums, s_block.data[0], s_block.data[1], n);

    /* USB capture: chain + Bluetooth audio + drums. */
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
        /* stock output clip */
        if (l > 0.95f) l = 0.95f; else if (l < -0.95f) l = -0.95f;
        if (r > 0.95f) r = 0.95f; else if (r < -0.95f) r = -0.95f;
        out[i * 2 + 0] = (int16_t)(l * 32767.0f);
        out[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }
    uint32_t dt = DWT->CYCCNT - t0;
    s_cyc_sum += dt;
    s_cyc_n++;
    if (dt > s_cyc_max) s_cyc_max = dt;
    if (s_drop_tx_blocks != 0) {
        s_drop_tx_blocks--; /* `x` underrun check: skip the refill */
    } else if (sai_tx_fill() > TX_TARGET_FILL) {
        s_stats.latency_skips++;
    } else {
        (void)sai_push(out, n);
    }
}

#endif /* ENGINE_HOST_TEST */
