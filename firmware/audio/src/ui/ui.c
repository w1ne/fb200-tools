#include <string.h>
#include "ui/ui.h"
#include "ui/controls.h"
#include "ui/display.h"
#include "debug/cdc_log.h"
#include "audio/engine.h"
#include "proto/proto.h"

enum { SW_A, SW_B, SW_C, SW_D };
enum { M_GATE, M_COMP, M_AMP, M_CAB, M_MOD, M_REV, M_NONE };

typedef struct {
    int8_t led;          /* knob LED index, -1 none */
    uint8_t module;
    uint8_t field;       /* preset offset; 0xFF = master volume (settings) */
    uint8_t types;       /* 0 = 0..100 knob, N = selector with N positions */
    uint8_t base;        /* first selector value: amp/cab are 1-based, mod/rev 0-based */
} knob_map_t;

/* Knob table, indexed by mux channel k0..k15. Measured on the pedal
 * (2026-09-27: the user turned every knob left to right and read the
 * labels): MASTER k15, LEVEL k14, REVERB k12, MIX k11, RATE k8, MOD k9,
 * CAB k13, VOL k10, BASS k4, MID k6, TREBLE k7, GAIN k5, AMP k2, LEVEL k1,
 * THRESH k0, GATE k3. The channel assignment recovered from the stock code
 * (ITCM 0x5a60) was wrong; its knob-LED pairing was right. */
static const knob_map_t kKnob[KNOB_COUNT] = {
    [15] = {14, M_NONE, 0xFF, 0},             /* MASTER (global setting) */
    [14] = {13, M_REV, P_REV_LEVEL, 0},       /* LEVEL (reverb) */
    [12] = {15, M_REV, P_REV_TYPE, 5, 0},     /* REVERB type 0..4 */
    [11] = {9, M_MOD, P_MOD_P2, 0},           /* MIX */
    [8] = {4, M_MOD, P_MOD_P1, 0},            /* RATE */
    [9] = {10, M_MOD, P_MOD_TYPE, 12, 0},     /* MOD type 0..11 */
    [13] = {5, M_CAB, P_CAB_TYPE, 19, 1},     /* CAB 1..19 (11+ = user IR) */
    [10] = {12, M_AMP, P_AMP_VOLUME, 0},      /* VOL */
    [4] = {0, M_AMP, P_AMP_BASS, 0},          /* BASS */
    [6] = {1, M_AMP, P_AMP_MID, 0},           /* MID */
    [7] = {2, M_AMP, P_AMP_TREBLE, 0},        /* TREBLE */
    [5] = {11, M_AMP, P_AMP_GAIN, 0},         /* GAIN */
    [2] = {3, M_AMP, P_AMP_MODEL, 10, 1},     /* AMP model 1..10 */
    [1] = {6, M_COMP, P_COMP_LEVEL, 0},       /* LEVEL (compressor) */
    [0] = {7, M_COMP, P_COMP_THRESH, 0},      /* THRESH */
    [3] = {8, M_GATE, P_GATE_THRESH, 0},      /* GATE */
};
static const uint8_t kModuleEnable[M_NONE] = {P_GATE_EN, P_COMP_EN, P_AMP_EN, P_CAB_EN,
                                              P_MOD_EN, P_REV_EN};
/* app protocol module index (fn 0x80 + i): comp, gate, amp, cab, mod, delay, rev */
static const uint8_t kProtoModule[M_NONE] = {1, 0, 2, 3, 4, 6};

static preset_t edit;
static settings_t settings;
static unsigned bank, slot;
static bool stomp, caught[KNOB_COUNT];
static uint8_t down_mask, peak_mask;
static bool long_used;
static uint32_t overlay_until, revision, settings_dirty_ms;
static bool settings_dirty;
static bool ui_log;
static bool tuner_mode, rhythm_mode;
static uint32_t s_now;

void ui_set_log(bool on) { ui_log = on; }

static uint16_t knob_units(int k)
{
    uint32_t v = knob_value(k);
    if (kKnob[k].types) return (uint16_t)(kKnob[k].base + v * kKnob[k].types / 4096u);
    return (uint16_t)((v * 100u + 2047u) / 4095u);
}

static uint16_t stored(int k)
{
    return kKnob[k].field == 0xFF ? settings.b[S_MASTER] : pget(&edit, kKnob[k].field);
}

static void show_rhythm(void)
{
    drums_t *d = engine_drums();
    unsigned r = d->rhythm + 1u;                     /* 1..40 */
    char t[4] = {'d', (char)('0' + r / 10u), (char)('0' + r % 10u), 0};
    display_text(t);
}

static void show_preset(void)
{
    if (rhythm_mode) { show_rhythm(); return; }
    char t[4] = {stomp ? 'L' : 'P', (char)('0' + bank), "AbCd"[slot], 0};
    /* notifications for front-panel changes are sent by the callers: remote
     * (app) changes are answered by the protocol itself */
    display_text(t);
}

static void load(unsigned b, unsigned s)
{
    bank = b % 10u;
    slot = s & 3u;
    preset_read(bank * 4u + slot, &edit);
    for (int k = 0; k < KNOB_COUNT; k++) caught[k] = (kKnob[k].field == 0xFF);   /* master is global */
    settings.b[S_BANK] = (uint8_t)bank;
    settings.b[S_SLOT] = (uint8_t)slot;
    settings.b[S_PRESET] = (uint8_t)(bank * 4u + slot);
    settings_dirty = true;
    revision++;
    show_preset();
    char name[21];
    memcpy(name, edit.b, 20);
    name[20] = 0;
    log_printf("preset %u%c \"%s\"\r\n", bank, "AbCd"[slot], name);
}

/* Stock rhythm settings at F:0x81000: on, -, rhythm, level, bpm (u16 LE). */
static void rhythm_settings_load(void)
{
    uint8_t r[RHYTHM_SIZE];
    rhythm_settings_read(r);
    drums_t *d = engine_drums();
    if (r[2] < DRUMS_RHYTHMS) drums_set_rhythm(d, r[2]);
    if (r[3] <= 100u) drums_set_level(d, r[3]);
    uint16_t bpm = (uint16_t)(r[4] | (r[5] << 8));
    drums_set_tempo(d, (bpm >= DRUMS_BPM_MIN && bpm <= DRUMS_BPM_MAX) ? bpm : DRUMS_BPM_DEFAULT);
}

void ui_init(void)
{
    rhythm_settings_load();
    settings_read(&settings);
    stomp = settings.b[S_STOMP] == 1;
    load(settings.b[S_BANK], settings.b[S_SLOT]);
    settings_dirty = false;
}

void ui_select(unsigned index) { load(index / 4u, index % 4u); }

int ui_save(void)
{
    int r = preset_write(bank * 4u + slot, &edit);
    display_text(r == 0 ? "SAV" : "ERR");
    overlay_until = 0;
    log_printf("save %u%c: %s\r\n", bank, "AbCd"[slot], r == 0 ? "ok" : "FAILED");
    return r;
}

static void toggle_module(int m)
{
    unsigned off = kModuleEnable[m];
    pset(&edit, off, pget(&edit, off) ? 0 : 1);
    if (m == M_AMP) pset(&edit, P_CAB_EN, pget(&edit, P_AMP_EN));   /* stock: C = amp + cab */
    revision++;
    proto_notify_module(kProtoModule[m]);
    if (m == M_AMP) proto_notify_module(kProtoModule[M_CAB]);
}

/* Rhythm mode buttons (our mapping; the stock in-mode roles are not known):
 * A play/stop, B previous rhythm, C next rhythm, D tap tempo. */
static void rhythm_single(int sw)
{
    drums_t *d = engine_drums();
    if (sw == SW_A) { if (d->on) drums_stop(d); else drums_start(d); }
    else if (sw == SW_B) drums_set_rhythm(d, (d->rhythm + DRUMS_RHYTHMS - 1u) % DRUMS_RHYTHMS);
    else if (sw == SW_C) drums_set_rhythm(d, (d->rhythm + 1u) % DRUMS_RHYTHMS);
    else drums_tap(d, s_now);
    show_rhythm();
}

static void action_single(int sw)
{
    if (tuner_mode) {
        tuner_mode = false;
        engine_set_tuner(false);
        settings.b[S_TUNER] = 0;
        proto_notify_settings();
        show_preset();
        return;
    }
    if (rhythm_mode) { rhythm_single(sw); return; }
    if (!stomp) { load(bank, (unsigned)sw); proto_notify_preset(); return; }
    static const int kStomp[4] = {M_REV, M_MOD, M_AMP, M_COMP};   /* A B C D */
    toggle_module(kStomp[sw]);
}

static void action_chord(uint8_t mask)
{
    if (mask == ((1u << SW_C) | (1u << SW_D))) { load(bank + 1u, slot); proto_notify_preset(); }
    else if (mask == ((1u << SW_A) | (1u << SW_B))) { load(bank + 9u, slot); proto_notify_preset(); }
    else if (mask == ((1u << SW_B) | (1u << SW_C))) {
        stomp = !stomp;
        settings.b[S_STOMP] = stomp;
        settings_dirty = true;
        show_preset();
    }
}

static void footswitches(uint32_t now)
{
    int sw;
    fsw_event_t ev;
    while ((ev = fsw_event(&sw)) != FSW_NONE) {
        if (ui_log)
            log_printf("fsw %c %s\r\n", "ABCD"[sw],
                       ev == FSW_PRESS ? "press" : ev == FSW_RELEASE ? "release" : "long");
        if (ev == FSW_PRESS) {
            down_mask |= (uint8_t)(1u << sw);
            peak_mask |= (uint8_t)(1u << sw);
        } else if (ev == FSW_LONG) {
            if (sw == SW_A && (down_mask & (1u << SW_B))) {          /* stock: B held + A long */
                long_used = true;
                tuner_mode = !tuner_mode;
                engine_set_tuner(tuner_mode);
                settings.b[S_TUNER] = tuner_mode;
                proto_notify_settings();
                if (tuner_mode) display_text(" - ");
                else show_preset();
            } else if (sw == SW_B && (down_mask & (1u << SW_C))) {   /* C held + B long */
                long_used = true;
                rhythm_mode = !rhythm_mode;
                settings.b[S_RHYTHM] = rhythm_mode;
                proto_notify_rhythm_mode();
                show_preset();
            } else if (peak_mask == (1u << SW_A) && !stomp && !rhythm_mode && !tuner_mode) {
                long_used = true;
                if (ui_save() == 0) proto_notify_saved();
                overlay_until = now + 1000u;
            }
        } else if (ev == FSW_RELEASE) {
            down_mask &= (uint8_t)~(1u << sw);
            if (down_mask == 0) {
                if (!long_used) {
                    if (peak_mask && !(peak_mask & (peak_mask - 1u))) action_single(__builtin_ctz(peak_mask));
                    else action_chord(peak_mask);
                }
                peak_mask = 0;
                long_used = false;
            }
        }
    }
}

static void knobs(uint32_t now)
{
    for (int k = 0; k < KNOB_COUNT; k++) {
        if (!knob_changed(k)) continue;
        uint16_t v = knob_units(k), s = stored(k);
        if (ui_log) log_printf("knob k%d = %u (raw %u, stored %u%s)\r\n", k, v,
                               (unsigned)knob_value(k), s, caught[k] ? "" : ", not caught");
        if (!caught[k]) {
            /* pickup: act only once the knob reaches the stored value
             * (+-2 % on level knobs; exact on selectors, where a tolerance
             * would jump the loaded model/type to the knob position) */
            int d = (int)v - (int)s, tol = kKnob[k].types ? 0 : 2;
            if (d < -tol || d > tol) continue;
            caught[k] = true;
        }
        if (v == s) continue;
        if (kKnob[k].field == 0xFF) { settings.b[S_MASTER] = (uint8_t)v; settings_dirty = true; proto_notify_settings(); }
        else { pset(&edit, kKnob[k].field, v); proto_notify_module(kProtoModule[kKnob[k].module]); }
        revision++;
        char t[5];
        t[0] = v >= 100 ? '1' : ' ';
        t[1] = v >= 10 ? (char)('0' + (v / 10u) % 10u) : ' ';
        t[2] = (char)('0' + v % 10u);
        t[3] = 0;
        display_text(t);
        overlay_until = now + 1000u;
    }
}

static void leds(uint32_t now)
{
    bool blink_on = (now / 250u) & 1u;
    for (int k = 0; k < KNOB_COUNT; k++) {
        if (kKnob[k].led < 0) continue;
        if (kKnob[k].module == M_NONE) { knob_led(kKnob[k].led, true); continue; }   /* master */
        bool enabled = pget(&edit, kModuleEnable[kKnob[k].module]) != 0;
        knob_led(kKnob[k].led, enabled && (caught[k] || blink_on));
    }
}

/* note index 1..12 = A#, B, C, C#, D, D#, E, F, F#, G, G#, A (stock) */
static void show_tuner(void)
{
    tuner_result_t r;
    if (!engine_tuner_poll(&r)) return;
    if (!r.valid || r.silent || r.note < 1 || r.note > 12) { display_text(" - "); return; }
    static const char letter[] = " AbCCddEFFGGA";
    static const uint8_t sharp[13] = {0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0};
    char t[6], *p = t;
    *p++ = r.cents < -3.0f ? '-' : ' ';            /* flat: left arrow */
    *p++ = letter[r.note];
    if (sharp[r.note]) *p++ = '.';
    *p++ = r.cents > 3.0f ? '-' : ' ';             /* sharp: right arrow */
    *p = 0;
    display_text(t);
}

void ui_task(uint32_t now_ms)
{
    s_now = now_ms;
    if (tuner_mode) show_tuner();
    footswitches(now_ms);
    knobs(now_ms);
    leds(now_ms);
    if (overlay_until && (int32_t)(now_ms - overlay_until) >= 0) { overlay_until = 0; show_preset(); }
    /* settings are few and rarely change: persist 3 s after the first
     * unsaved change (the stock saves them on power fail) */
    if (settings_dirty) {
        if (!settings_dirty_ms) settings_dirty_ms = now_ms;
        if (now_ms - settings_dirty_ms > 3000u) {
            settings_write(&settings);
            settings_dirty = false;
            settings_dirty_ms = 0;
        }
    }
}

void ui_flush_settings(void)
{
    if (settings_dirty) {
        settings_write(&settings);
        settings_dirty = false;
        settings_dirty_ms = 0;
    }
}

const preset_t *ui_edit_preset(void) { return &edit; }
unsigned ui_preset_index(void) { return bank * 4u + slot; }
uint8_t ui_master(void) { return settings.b[S_MASTER]; }
uint32_t ui_revision(void) { return revision; }

void ui_edit_write(unsigned off, const void *src, unsigned n)
{
    if (off >= PRESET_SIZE || n > PRESET_SIZE - off) return;
    memcpy(edit.b + off, src, n);
    revision++;
}

void ui_edit_load(const preset_t *p)
{
    edit = *p;
    revision++;
}

settings_t *ui_settings(void) { return &settings; }

void ui_settings_changed(void)
{
    stomp = settings.b[S_STOMP] == 1;
    settings_dirty = true;
    revision++;
}
