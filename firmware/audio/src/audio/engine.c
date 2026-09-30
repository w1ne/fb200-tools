/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Audio engine: codec ADC -> DSP chain -> codec DAC, with the USB path as
 * capture (processed signal) and monitor (host playback mixed into the DAC).
 *
 * Drift: the host playback is resampled to the codec clock (audio/drift.h
 * drift_rs, a PI loop on the ring fill). Only when that fails (ring dry or
 * far overfull) a frame repeats or frames drop; both are counted in
 * engine_stats_t.
 *
 * The DSP chain is currently a gain node plus an optional test generator;
 * the real chain (EQ, amp sim, ...) lands in later milestones. */
#include <string.h>
#include "cold.h"
#include "audio/engine.h"
#include "debug/cdc_log.h"
#include "audio/audio_config.h"


#ifndef ENGINE_HOST_TEST
#include "audio/sai.h"
#include "audio/usb_audio.h"
#include "audio/drift.h"
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
#include "dsp/delay.h"
#include "dsp/eq.h"
#include "dsp/looper.h"
#include "loopstore/loopstore.h"
#include "loopstore/lsio.h"
#include "debug/flash_rmw.h"
#include "debug/selfupdate.h"
#include "dsp/outq.h"
#include "preset/preset.h"
#include "irstore/irstore.h"
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
static cab_t s_cab __attribute__((section(".ocram")));
/* RAM (memory map: docs/FIRMWARE_BRINGUP.md): CPU-only state moved out of
 * DTCM to make room for the delay line: the cab's head spectra and s_ir to
 * OCRAM (cached), the reverb to the ITCM above the code, the long-IR tail to
 * the low DTCM. Sizes: ENGINE_IR_TAPS (engine.h) and DELAY_MS_MAX
 * (dsp/delay.h); the link fails if they do not fit. */
#define ENGINE_LONG_IR (ENGINE_IR_TAPS > CAB_TAPS)
_Static_assert(ENGINE_IR_TAPS >= CAB_TAPS && ENGINE_IR_TAPS <= CAB_MAX_TAPS, "ENGINE_IR_TAPS");
_Static_assert(AUDIO_FS <= DELAY_FS_MAX, "the delay line is sized for DELAY_FS_MAX");
_Static_assert(SAI_BLOCK_FRAMES == ENGINE_FRAMES, "sai_pull_block moves one engine block");
#if ENGINE_LONG_IR
/* long IRs (M5): 96 kB, in the low DTCM (linker.ld .dtcm_lo) */
static conv2_tail_t s_cab_tail __attribute__((section(".dtcm_lo")));
#define CAB_INIT(c) cab_init_long((c), &s_cab_tail)
#else
#define CAB_INIT(c) cab_init(c)
#endif
static gate_t s_gate;
static comp_t s_comp;
static mod_t s_mod;
static reverb_t s_rev __attribute__((section(".itcm_bss")));
/* Our bass delay (docs/PARITY.md M4). Its line (DELAY_LEN int16, 88 kB for
 * 1 s): the DTCM between .bss and the stack (linker.ld .dtcm_hi). */
static delay_t s_dly;
static int16_t s_dly_line[DELAY_LEN] __attribute__((section(".dtcm_hi")));
static bool s_dly_en;
/* Our bass EQ (docs/PARITY.md M4), after the cab. In the preset with our
 * marker (preset.h P_EQ_DATA); without it (every stock preset) off. Set from the
 * console (`eq`, into the edit buffer). Off (or flat) = bypassed, bit-exact.
 * OCRAM (cached): in .bss its 508 B push the 2 kB-aligned USB buffer
 * (_dcd_data) up by 2 kB and DTCM overflows. The EQ touches ~70 words of it
 * per block. */
static eq_t s_eq __attribute__((section(".ocram")));
/* The looper (dsp/looper.h): the loop is in the flash
 * (loopstore/loopstore.h). The audio side's state and the two frame rings
 * (2.1 kB) in the low DTCM's spare, the flash side's state and chunk maps
 * (2.2 kB, main loop only) in OCRAM. */
static looper_t s_loop __attribute__((section(".dtcm_lo")));
static loopio_t s_lio __attribute__((section(".dtcm_lo")));
static loopstore_t s_ls __attribute__((section(".ocram")));
static uint16_t s_lmap[2][LS_MAX_CHUNKS] __attribute__((section(".ocram")));
static bool s_amp_en, s_cab_en, s_gate_en, s_comp_en, s_mod_en, s_rev_en;
static int s_amp_model = -1, s_cab_type = -1;
static float s_master = 1.0f, s_master_target = 1.0f;
/* user IR staging, read only when a slot loads: OCRAM. Also `cab long`, and
 * the long IR upload's buffer (engine_ir_borrow; no cab change meanwhile). */
static float s_ir[ENGINE_IR_TAPS] __attribute__((section(".ocram")));
static bool s_ir_busy;
static bool s_testgen_in;               /* testgen feeds the chain input */
static int s_usb_route;                 /* ENGINE_USB_OUT / _IN / _MIX */
static uint32_t s_cyc_max, s_cyc_sum, s_cyc_n;   /* DWT cycles per block */
/* `prof`: DWT cycles per chain stage, summed over blocks (engine_profile) */
enum { P_IN, P_TUNER, P_GATE, P_COMP, P_AMP, P_CAB, P_EQ, P_MOD, P_DELAY, P_REVERB, P_LOOP,
       P_MIX, P_DRUMS, P_USB_IN, P_OUT, P_COUNT };
static uint32_t s_prof[P_COUNT], s_prof_n, s_prof_t;
#define PROF(stage) do { uint32_t now_ = DWT->CYCCNT; s_prof[stage] += now_ - s_prof_t; \
                         s_prof_t = now_; } while (0)
static uint32_t s_drop_tx_blocks;
static uint32_t s_meter_last_ms;
static bool s_ready;          /* engine_init done: engine_pump may run */
static drift_rs_t s_rs;       /* host playback resampler (audio/drift.h) */
static outq_t s_outq;         /* DAC float -> int16: rounding, optional dither (dsp/outq.h) */
static float s_in_peak;       /* chain input |L + R| peak, for the idle timer (ui/power.c) */

COLD void engine_init(void)
{
    usb_audio_init();
    sai_audio_init();
    gain_init(&s_gain, 1.0f);
    outq_init(&s_outq, 0x2545F491u);
    drift_rs_init(&s_rs, (float)AUDIO_FS);
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
    CAB_INIT(&s_cab);
    bt_audio_init();
    gate_init(&s_gate, (float)AUDIO_FS);
    comp_init(&s_comp, (float)AUDIO_FS);
    mod_init(&s_mod, (float)AUDIO_FS);
    reverb_init(&s_rev, (float)AUDIO_FS);
    delay_init(&s_dly, (float)AUDIO_FS, s_dly_line);
    eq_init(&s_eq, (float)AUDIO_FS);
    /* the looper's area: the JEDEC ID sizes it (8 MB: F:0x510000..0x800000);
     * nothing in it is read before the looper writes it */
    (void)flash_probe();
    lsio_init(flash_suspend_ok());
    (void)ls_init(&s_ls, &s_lio, s_lmap[0], s_lmap[1], FLASH_LOOP_BASE, flash_loop_end());
    looper_init(&s_loop, &s_lio, &s_ls);
    static drums_data_t rhythms;
    if (g_stock) drums_data_from_stock(&rhythms, g_stock);
    drums_init(&s_drums, (const void *)DRUMS_BANK_ADDR, g_stock ? &rhythms : NULL);  /* NULL: silent */
    s_ready = true;
}

void engine_pump(void)
{
    if (!s_ready) return;
    s_drums.no_flash = 1;
    usb_audio_task();
    engine_task();
    s_drums.no_flash = 0;
}

void engine_set_tuner(bool on) { s_tuner_on = on; }

static volatile bool s_reapply;

/* Re-initialise every DSP module; the preset is re-applied from the main
 * loop (engine_needs_reapply). */
void engine_dsp_reset(void)
{
    amp_init(&s_amp, (float)AUDIO_FS);
    CAB_INIT(&s_cab);
    gate_init(&s_gate, (float)AUDIO_FS);
    comp_init(&s_comp, (float)AUDIO_FS);
    mod_init(&s_mod, (float)AUDIO_FS);
    reverb_init(&s_rev, (float)AUDIO_FS);
    delay_init(&s_dly, (float)AUDIO_FS, s_dly_line);
    eq_reset(&s_eq);            /* the settings come back with the preset */
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
    memcpy(s_ir, ir, CAB_TAPS * sizeof s_ir[0]);
    for (unsigned i = 0; i < CAB_TAPS; i++)
        if (!(s_ir[i] > -1e6f && s_ir[i] < 1e6f)) s_ir[i] = 0.0f;   /* NaN/inf -> 0 */
    cab_set_ir(&s_cab, s_ir, cab_user_ir_gain(s_ir));
    return 1;
}

static bool s_cab_bypass;   /* user IR slot selected but empty */

/* Long IR slots (cab 20..83, irstore.h): the data CRC is checked before the
 * IR loads; an empty or bad slot bypasses the cab, as an empty user slot.
 * The gain is the stored one (the stock rule, computed at upload). A build
 * with less IR RAM (ENGINE_IR_TAPS) plays the first taps. */
static int load_long_ir(unsigned slot)
{
    irstore_entry_t e;
    int taps = irstore_load(slot, s_ir, ENGINE_IR_TAPS, &e);
    return taps > 0 && cab_set_ir_len(&s_cab, s_ir, (unsigned)taps, e.gain) == 0;
}

COLD float *engine_ir_borrow(void)
{
    /* the upload needs a whole slot (and the table) in it: long IRs on only */
    if (s_ir_busy || ENGINE_IR_TAPS < IRSTORE_TAPS) return NULL;
    s_ir_busy = true;
    return s_ir;
}

COLD void engine_ir_release(void)
{
    s_ir_busy = false;
    s_cab_type = -1;   /* reload the preset's cab: its slot may be new */
    s_reapply = true;
}

/* `cab long <taps>` (console, for `prof` on the pedal): the first taps of
 * a synthetic IR (noise, -60 dB at 4096) in the cab until the next cab
 * change; 0 goes back to the preset's cab. More than ENGINE_IR_TAPS: -2
 * (long IRs off). */
COLD int engine_cab_long(unsigned taps)
{
    if (taps == 0) {
        s_cab_type = -1;
        s_reapply = true;
        return 0;
    }
    if (taps > ENGINE_IR_TAPS) return -2;
    if (s_ir_busy) return -4;
    uint32_t seed = 1;
    float env = 1.0f;
    for (unsigned i = 0; i < ENGINE_IR_TAPS; i++, env *= 0.99832f) {
        seed = seed * 1664525u + 1013904223u;
        s_ir[i] = (float)(int32_t)seed * (1.0f / 2147483648.0f) * env;
    }
    s_cab_bypass = false;
    return cab_set_ir_len(&s_cab, s_ir, taps, cab_user_ir_gain(s_ir));
}

COLD void engine_apply_preset(const preset_t *p, unsigned master)
{
    s_amp_en = pget(p, P_AMP_EN) != 0;
    s_cab_en = pget(p, P_CAB_EN) != 0;
    int model = pget(p, P_AMP_MODEL);
    if (model != s_amp_model && amp_set_model(&s_amp, model) == 0) s_amp_model = model;
    amp_set_params(&s_amp, pget(p, P_AMP_GAIN), pget(p, P_AMP_BASS), pget(p, P_AMP_MID),
                   pget(p, P_AMP_MIDFREQ), pget(p, P_AMP_TREBLE), pget(p, P_AMP_VOLUME));
    int cab = pget(p, P_CAB_TYPE);
    if (cab != s_cab_type && !s_ir_busy) {   /* busy: engine_ir_release reapplies */
        s_cab_bypass = false;
        if (cab >= 1 && cab <= 10) (void)cab_set_model(&s_cab, cab);
        else if (cab >= 11 && cab <= 19) s_cab_bypass = !load_user_ir((unsigned)(cab - 11));
        else if (irstore_slot_of((unsigned)cab) >= 0)
            s_cab_bypass = !load_long_ir((unsigned)irstore_slot_of((unsigned)cab));
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
    /* Stock presets have the delay "on" but the stock never plays it: only
     * presets with our marker play (preset.h preset_delay_on). Switching it
     * on starts from a silent line, not from old audio. */
    bool dly = preset_delay_on(p);
    if (dly && !s_dly_en) delay_clear(&s_dly);
    delay_set_params(&s_dly, pget(p, P_DLY_TIME), pget(p, P_DLY_FB), pget(p, P_DLY_MIX),
                     pget(p, P_DLY_LOWCUT), pget(p, P_DLY_TONE));
    s_dly_en = dly;
    eq_load(&s_eq, preset_eq(p));   /* glides; unchanged stages keep going */
    s_master_target = (float)(master > 100u ? 100u : master) * 0.01f;
}
COLD void engine_apply_settings(const settings_t *s)
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

COLD int engine_loop(int action) { return looper_cmd(&s_loop, action); }
COLD int engine_loop_save(unsigned n) { return looper_save(&s_loop, n); }
COLD int engine_loop_load(unsigned n) { return looper_load(&s_loop, n); }
COLD void engine_loop_poll(void) { looper_poll(&s_loop); }
COLD void engine_loop_info(looper_info_t *out) { looper_info(&s_loop, out); }
COLD void engine_loop_level(unsigned pct) { looper_set_level(&s_loop, pct); }
COLD void engine_loop_arm(void) { ls_arm(&s_ls); }
COLD void engine_loop_task(void) { ls_task(&s_ls); }
int engine_loop_busy(void) { return ls_busy(&s_ls); }
int engine_loop_writing(void) { return looper_writing(&s_loop); }
COLD void engine_loop_stats(engine_loop_stats_t *out)
{
    ls_info(&s_ls, &out->store);
    out->io = &s_lio;
    out->cut = s_loop.cut;
}

float engine_input_peak(void)
{
    float p = s_in_peak;
    s_in_peak = 0.0f;
    return p;
}

bool engine_tuner_poll(tuner_result_t *out) { return tuner_poll(&s_tuner, out) != 0; }
drums_t *engine_drums(void) { return &s_drums; }
struct eq_s *engine_eq(void) { return &s_eq; }

COLD void engine_get_stats(engine_stats_t *out)
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
void engine_set_dither(bool on)
{
    s_outq.dither = on;
    usb_audio_set_dither(on);
}
bool engine_get_dither(void) { return s_outq.dither != 0; }
void engine_set_usb_route(int route) { s_usb_route = route; }
int engine_get_usb_route(void) { return s_usb_route; }

COLD void engine_cycles(uint32_t *avg, uint32_t *max, uint32_t *budget)
{
    *avg = s_cyc_n ? s_cyc_sum / s_cyc_n : 0;
    *max = s_cyc_max;
    *budget = (uint32_t)((uint64_t)SystemCoreClock * ENGINE_FRAMES / AUDIO_FS);
    s_cyc_sum = s_cyc_n = s_cyc_max = 0;
}

COLD void engine_profile(void)
{
    static const char *const names[P_COUNT] = {
        "in", "tuner", "gate", "comp", "amp", "cab", "eq", "mod", "delay", "reverb", "loop",
        "mix", "drums", "usb-in", "out",
    };
    uint32_t n = s_prof_n ? s_prof_n : 1u;
    for (int i = 0; i < P_COUNT; i++) {
        log_printf("prof %s %lu cycles/block\r\n", names[i], (unsigned long)(s_prof[i] / n));
        s_prof[i] = 0;
    }
    log_printf("prof over %lu blocks (budget %lu)\r\n", (unsigned long)s_prof_n,
               (unsigned long)((uint64_t)SystemCoreClock * ENGINE_FRAMES / AUDIO_FS));
    s_prof_n = 0;
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
    int16_t in[ENGINE_FRAMES * 2];
    int16_t play[ENGINE_FRAMES * 2];
    int16_t out[ENGINE_FRAMES * 2];

    check_sai_faults();

    /* Whole blocks only (audio/sai_ring.h): the chain always runs n =
     * DSP_BLOCK. No block yet: the main loop comes back. */
    if (!sai_pull_block(in)) return;
    const size_t n = ENGINE_FRAMES;
    for (size_t i = 0; i < n; i++) {
        s_block.data[0][i] = (float)in[i * 2 + 0] * (1.0f / 32768.0f);
        s_block.data[1][i] = (float)in[i * 2 + 1] * (1.0f / 32768.0f);
    }
    /* The stock feeds the chain and the tuner with L + R of the instrument
     * ADC; into the chain times the input gain (S+0x1a). At the default
     * 0 dB that is x 0.9999702 (a unity gain whose smoother stalls there -
     * needed for bit parity, tests/test_stock_dsp_parity.py). */
    uint32_t t0 = DWT->CYCCNT;
    s_prof_t = t0;
    /* Host playback, resampled to the codec clock (audio/drift.h), only
     * while the host streams. Pulled here, in the same block as the capture
     * push: `usb in|mix` adds no block latency (docs/AUDIO_PATH.md). */
    if (usb_audio_playing()) {
        (void)usb_audio_pull_rs(&s_rs, play, n, MAX_RING_FILL, &s_stats.fifo_inserts,
                                &s_stats.fifo_drops);
    } else {
        memset(play, 0, n * 2 * sizeof play[0]);
        drift_rs_init(&s_rs, (float)AUDIO_FS);   /* the next stream starts fresh */
    }
    if (s_usb_route != ENGINE_USB_OUT) {   /* reamping: playback into the chain */
        drift_play_to_input(s_block.data[0], s_block.data[1], play, n,
                            s_usb_route == ENGINE_USB_IN);
        memset(play, 0, n * 2 * sizeof play[0]);   /* not also dry into the DAC */
    }
    float x[ENGINE_FRAMES];
    if (s_testgen_in && s_testgen.mode != TESTGEN_OFF) {   /* test signal into the chain */
        memset(&s_block, 0, sizeof s_block);
        testgen_process(&s_testgen, &s_block, n);
        for (size_t i = 0; i < n; i++) s_block.data[1][i] = 0.0f;   /* mono source on L */
    }
    float pk = s_in_peak;
    for (size_t i = 0; i < n; i++) {
        x[i] = s_block.data[0][i] + s_block.data[1][i];
        float a = x[i] < 0.0f ? -x[i] : x[i];
        if (a > pk) pk = a;
    }
    s_in_peak = pk;
    PROF(P_IN);
    tuner_feed(&s_tuner, x, n);
    PROF(P_TUNER);
    for (size_t i = 0; i < n; i++) x[i] *= dsp_knob_next(&s_in_gain);
    /* stock chain (ITCM 0x7b60): gate -> comp -> amp -> cab -> mod -> reverb;
     * our EQ goes between cab and mod, our delay between mod and reverb
     * (off: the stock chain exactly) */
    if (s_gate_en) gate_process(&s_gate, x, (unsigned)n);
    PROF(P_GATE);
    if (s_comp_en) comp_process(&s_comp, x, (unsigned)n);
    PROF(P_COMP);
    if (s_amp_en) amp_process(&s_amp, x, (unsigned)n);
    PROF(P_AMP);
    if (s_cab_en && !s_cab_bypass) cab_process(&s_cab, x, (unsigned)n);
    PROF(P_CAB);
    eq_process(&s_eq, x, (unsigned)n);   /* returns at once when off or flat */
    PROF(P_EQ);
    if (s_mod_en) mod_process(&s_mod, x, (unsigned)n);
    PROF(P_MOD);
    if (s_dly_en) delay_process(&s_dly, x, (unsigned)n);
    PROF(P_DELAY);
    float xl[ENGINE_FRAMES], xr[ENGINE_FRAMES];
    if (s_rev_en) reverb_process(&s_rev, x, xl, xr, (unsigned)n);   /* mono in, L/R out */
    else { memcpy(xl, x, n * sizeof x[0]); memcpy(xr, x, n * sizeof x[0]); }
    PROF(P_REVERB);
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
    /* the looper: records the chain (mono), plays into both sides */
    looper_process(&s_loop, xl, xr, (unsigned)n);
    PROF(P_LOOP);
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
    PROF(P_MIX);

    /* Drums before the capture tap: the stock leaves them out of the USB
     * recording; a user playing along wants them in (better than stock). */
    if (!s_tuner_on) drums_process_stereo(&s_drums, s_block.data[0], s_block.data[1], n);
    PROF(P_DRUMS);

    /* USB capture: chain + Bluetooth audio + drums. */
    {
        float fb[ENGINE_FRAMES * 2];
        for (size_t i = 0; i < n; i++) {
            fb[i * 2 + 0] = s_block.data[0][i];
            fb[i * 2 + 1] = s_block.data[1][i];
        }
        usb_audio_push(fb, n);
    }
    PROF(P_USB_IN);

    for (size_t i = 0; i < n; i++) {
        float l = s_block.data[0][i] + (float)play[i * 2 + 0] * (1.0f / 32768.0f);
        float r = s_block.data[1][i] + (float)play[i * 2 + 1] * (1.0f / 32768.0f);
        /* stock output clip */
        if (l > 0.95f) l = 0.95f; else if (l < -0.95f) l = -0.95f;
        if (r > 0.95f) r = 0.95f; else if (r < -0.95f) r = -0.95f;
        out[i * 2 + 0] = outq_sample(&s_outq, l);   /* rounded (v0.9.1: truncated) */
        out[i * 2 + 1] = outq_sample(&s_outq, r);
    }
    PROF(P_OUT);
    s_prof_n++;
    uint32_t dt = DWT->CYCCNT - t0;
    s_cyc_sum += dt;
    s_cyc_n++;
    if (dt > s_cyc_max) s_cyc_max = dt;
    if (s_drop_tx_blocks != 0) {
        s_drop_tx_blocks--; /* `x` underrun check: skip the refill */
    } else if (!sai_push_block(out, SAI_TX_TARGET_FILL)) {
        s_stats.latency_skips++;
    }
}

#endif /* ENGINE_HOST_TEST */
