#include <string.h>
#include "ui/ui.h"
#include "ui/controls.h"
#include "ui/display.h"
#include "ui/lightbar.h"
#include "debug/cdc_log.h"
#include "audio/engine.h"
#include "proto/proto.h"
#include "dsp/stock_data.h"
#include "debug/recovery.h"
#include "ui/power.h"

enum { SW_A, SW_B, SW_C, SW_D };
enum { M_GATE, M_COMP, M_AMP, M_CAB, M_MOD, M_REV, M_NONE };

typedef struct {
    const char *name;    /* 3-character display name */
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
    [15] = {"OUt", 14, M_NONE, 0xFF, 0},             /* MASTER (global setting) */
    [14] = {"rLE", 13, M_REV, P_REV_LEVEL, 0},       /* LEVEL (reverb) */
    [12] = {"rEU", 15, M_REV, P_REV_TYPE, 5, 0},     /* REVERB type 0..4 */
    [11] = {"nIH", 9, M_MOD, P_MOD_P2, 0},           /* MIX */
    [8] = {"rAt", 4, M_MOD, P_MOD_P1, 0},            /* RATE */
    [9] = {"nOd", 10, M_MOD, P_MOD_TYPE, 12, 0},     /* MOD type 0..11 */
    [13] = {"CAb", 5, M_CAB, P_CAB_TYPE, 19, 1},     /* CAB 1..19 (11+ = user IR) */
    [10] = {"UOL", 12, M_AMP, P_AMP_VOLUME, 0},      /* VOL */
    [4] = {"bAS", 0, M_AMP, P_AMP_BASS, 0},          /* BASS */
    [6] = {"nid", 1, M_AMP, P_AMP_MID, 0},           /* MID */
    [7] = {"trE", 2, M_AMP, P_AMP_TREBLE, 0},        /* TREBLE */
    [5] = {"GAn", 11, M_AMP, P_AMP_GAIN, 0},         /* GAIN */
    [2] = {"AnP", 3, M_AMP, P_AMP_MODEL, 10, 1},     /* AMP model 1..10 */
    [1] = {"CLE", 6, M_COMP, P_COMP_LEVEL, 0},       /* LEVEL (compressor) */
    [0] = {"tHr", 7, M_COMP, P_COMP_THRESH, 0},      /* THRESH */
    [3] = {"GAt", 8, M_GATE, P_GATE_THRESH, 0},      /* GATE (label confirmed) */
};
static const uint8_t kModuleEnable[M_NONE] = {P_GATE_EN, P_COMP_EN, P_AMP_EN, P_CAB_EN,
                                              P_MOD_EN, P_REV_EN};
/* app protocol module index (fn 0x80 + i): comp, gate, amp, cab, mod, delay, rev */
static const uint8_t kProtoModule[M_NONE] = {1, 0, 2, 3, 4, 6};

static preset_t edit;
static settings_t settings;
static unsigned bank, slot;       /* the loaded preset */
/* Bank browsing (stock 0x1bc0c, 0x19af4): a bank chord only moves the shown
 * bank; the edit buffer stays, so a save can go to another bank. A-D then
 * load from the shown bank, a hold saves to it. In preset mode the display
 * flashes and the browse ends after BROWSE_MS without a chord. */
#define BROWSE_MS 1666u           /* stock: 1666 ticks of its 1 kHz display tick */
#define BROWSE_FLASH_MS 266u      /* stock: shown for 133 ms, blank for 133 ms */
static unsigned view_bank;
static bool browsing;
static uint32_t browse_ms;
static bool stomp, caught[KNOB_COUNT];
static uint8_t down_mask, peak_mask;
static bool long_used;
static uint32_t overlay_until, revision, settings_dirty_ms;
static bool settings_dirty;
static bool ui_log;
static bool tuner_mode, rhythm_mode;
/* Looper mode (ours; hold D, then C long, like the stock's tuner and rhythm
 * chords): A = the looper switch, B = stop/play, hold B = clear. */
static bool looper_mode;
static bool loop_acted;     /* this press of A already acted (rec, close, punch out, play) */
static int loop_shown = -1; /* looper state on the display */
static bool rhythm_dirty, rhythm_notify;   /* unsaved drum settings; send BA when saved */
static uint8_t rhythm_b1;                  /* rhythm block byte 1 (meaning unknown), kept */
static bool modifier_used;   /* A held while a knob turned: A is a modifier */
static uint32_t rhythm_dirty_ms;
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

static void show_loop(void)
{
    looper_info_t in;
    engine_loop_info(&in);
    static const char *const kText[] = {"LP-", "LP-", "rEC", "PLY", "odb", "StP"};
    loop_shown = in.state;
    display_text(kText[in.state <= LOOPER_STOP ? in.state : 0]);
}

static void show_preset(void)
{
    if (rhythm_mode) { show_rhythm(); return; }
    if (looper_mode) { show_loop(); return; }
    char t[4] = {stomp ? 'L' : 'P', (char)('0' + view_bank), "AbCd"[slot], 0};
    /* notifications for front-panel changes are sent by the callers: remote
     * (app) changes are answered by the protocol itself */
    display_text(t);
}

static void load(unsigned b, unsigned s)
{
    bank = view_bank = b % 10u;
    slot = s & 3u;
    browsing = false;
    preset_read(bank * 4u + slot, &edit);
    unsigned fixed = preset_sanitize(&edit);   /* erased/corrupt flash plays safe values */
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
    if (fixed) log_printf("preset %u%c: %s\r\n", bank, "AbCd"[slot],
                          fixed == PRESET_SIZE ? "erased in flash, playing it blank" : "out-of-range fields clamped");
}

/* Stock rhythm settings at F:0x81000: on, -, rhythm, level, bpm (u16 LE).
 * The drum machine is the one copy of this state: the app protocol (BA)
 * reads and writes it through ui_rhythm_block / ui_rhythm_set. */
static void rhythm_settings_load(void)
{
    uint8_t r[RHYTHM_SIZE];
    static const uint8_t def[RHYTHM_SIZE] = {0, 0, 0, 100, 110, 0};
    rhythm_settings_read(r);
    /* stock validation (0x18db2): on<=1, ?<=1, pattern<=39, volume<=100 */
    bool erased = r[0] == 0xFF && r[1] == 0xFF && r[2] == 0xFF;
    if (erased || r[0] > 1 || r[1] > 1 || r[2] >= DRUMS_RHYTHMS || r[3] > 100u) memcpy(r, def, sizeof r);
    drums_t *d = engine_drums();
    drums_stop(d);                               /* the stock always boots with the drums off */
    rhythm_b1 = r[1];
    drums_set_rhythm(d, r[2]);
    drums_set_level(d, r[3] / 10u * 10u);        /* stock: level rounded to tens at boot */
    uint16_t bpm = (uint16_t)(r[4] | (r[5] << 8));
    drums_set_tempo(d, (bpm >= DRUMS_BPM_MIN && bpm <= DRUMS_BPM_MAX) ? bpm : DRUMS_BPM_DEFAULT);
}

void ui_rhythm_block(uint8_t out[RHYTHM_SIZE])
{
    const drums_t *d = engine_drums();
    out[0] = d->on;
    out[1] = rhythm_b1;
    out[2] = d->rhythm;
    out[3] = d->level;
    out[4] = (uint8_t)d->bpm;
    out[5] = (uint8_t)(d->bpm >> 8);
}

void ui_rhythm_set(const uint8_t in[RHYTHM_SIZE])
{
    drums_t *d = engine_drums();
    rhythm_b1 = in[1];
    drums_set_rhythm(d, in[2] < DRUMS_RHYTHMS ? in[2] : 0u);
    drums_set_level(d, in[3] <= 100u ? in[3] : 100u);
    drums_set_tempo(d, (unsigned)(in[4] | (in[5] << 8)));
    if (in[0] && !d->on) drums_start(d);
    else if (!in[0] && d->on) drums_stop(d);
    rhythm_dirty = true;                         /* saved, not echoed */
    rhythm_dirty_ms = s_now;
    if (rhythm_mode && !overlay_until) show_rhythm();
}

void ui_init(void)
{
    rhythm_settings_load();
    settings_read(&settings);
    unsigned fixed = settings_sanitize(&settings);
    if (fixed) log_printf("settings: %s\r\n", fixed == SETTINGS_SIZE ? "erased in flash, stock defaults"
                                                               : "out-of-range fields reset");
    stomp = settings.b[S_STOMP] == 1;
    load(settings.b[S_BANK], settings.b[S_SLOT]);
    settings_dirty = false;
}

void ui_select(unsigned index) { load(index / 4u, index % 4u); }

/* Stock save (0x9f12 and the B/C/D copies, write in 0x67e0): the edit
 * buffer goes to slot s of bank b, which becomes the current preset; the
 * edit buffer is not reloaded. */
static int save_to(unsigned b, unsigned s)
{
    int r = preset_write(b * 4u + s, &edit);
    if (r == 0) {
        bank = view_bank = b;
        slot = s;
        browsing = false;
        settings.b[S_BANK] = (uint8_t)bank;
        settings.b[S_SLOT] = (uint8_t)slot;
        settings.b[S_PRESET] = (uint8_t)(bank * 4u + slot);
        settings_dirty = true;
        revision++;
    }
    if (r == 0) lightbar_save(s_now);     /* the stock blinks for 1 s, then writes */
    display_text(r == 0 ? "SAV" : "ERR");
    overlay_until = 0;
    log_printf("save %u%c: %s\r\n", b, "AbCd"[s], r == 0 ? "ok" : "FAILED");
    return r;
}

int ui_save(void) { return save_to(bank, slot); }

static void toggle_module(int m)
{
    unsigned off = kModuleEnable[m];
    if (m == M_AMP) {   /* stock 0xa4e2: C = amp + cab; either on -> both off, else both on */
        uint16_t on = !pget(&edit, P_AMP_EN) && !pget(&edit, P_CAB_EN);
        pset(&edit, P_AMP_EN, on);
        pset(&edit, P_CAB_EN, on);
    } else {
        pset(&edit, off, pget(&edit, off) ? 0 : 1);
    }
    revision++;
    proto_notify_module(kProtoModule[m]);
    if (m == M_AMP) proto_notify_module(kProtoModule[M_CAB]);
}

/* Rhythm mode buttons as the stock (manual p.14; code 0x9fec A, 0xa2d6 B,
 * 0xa54e C, 0xa714 D): A previous rhythm (1 wraps to 40), B next rhythm,
 * C tap tempo, D play/stop. */
static void rhythm_single(int sw)
{
    drums_t *d = engine_drums();
    if (sw == SW_A) drums_set_rhythm(d, (d->rhythm + DRUMS_RHYTHMS - 1u) % DRUMS_RHYTHMS);
    else if (sw == SW_B) drums_set_rhythm(d, (d->rhythm + 1u) % DRUMS_RHYTHMS);
    else if (sw == SW_C) drums_tap(d, s_now);
    else if (d->on) drums_stop(d);
    else drums_start(d);
    rhythm_dirty = true;
    rhythm_dirty_ms = s_now;
    show_rhythm();
    proto_notify_rhythm();                       /* stock: BA on every button */
}

/* Looper mode (ours). A acts when pressed (the loop points follow the
 * foot): empty = record, recording = close and play, dubbing = back to
 * play, stopped = play from the start. While playing, A released = dub (a
 * press that is held is the undo). Hold A = undo / redo the last dub; tap B
 * = stop / play; hold B = clear (the delay and long IRs come back). */
static void loop_overlay(const char *t)
{
    display_text(t);
    overlay_until = s_now + 1000u;
}

static void loop_press(void)
{
    looper_info_t in;
    engine_loop_info(&in);
    loop_acted = in.state != LOOPER_PLAY;
    if (loop_acted) (void)engine_loop(LOOPER_TAP);
    show_loop();
}

static void loop_single(int sw)
{
    looper_info_t in;
    engine_loop_info(&in);
    if (sw == SW_A) {
        if (!loop_acted && in.state == LOOPER_PLAY) (void)engine_loop(LOOPER_DUB_A);
    } else if (sw == SW_B) {
        if (in.state == LOOPER_PLAY || in.state == LOOPER_DUB) (void)engine_loop(LOOPER_STOP_A);
        else if (in.state == LOOPER_STOP) (void)engine_loop(LOOPER_PLAY_A);
    }
    loop_acted = false;
    if (!overlay_until) show_loop();
}

static void loop_long(int sw)
{
    looper_info_t in;
    engine_loop_info(&in);
    if (sw == SW_A) {
        if (engine_loop(LOOPER_UNDO_A) == 0) loop_overlay(in.undo == 2 ? "rdo" : "Und");
        else loop_overlay("no ");
    } else if (in.state > LOOPER_EMPTY) {
        (void)engine_loop(LOOPER_CLEAR_A);
        loop_overlay("CLr");
    }
    loop_acted = false;
}

static void set_looper(bool on)
{
    looper_mode = on;
    if (on && rhythm_mode) {                    /* one of the two */
        rhythm_mode = false;
        settings.b[S_RHYTHM] = 0;
        proto_notify_rhythm_mode();
    }
    loop_acted = false;
    show_preset();
    log_printf("looper mode %s\r\n", on ? "on" : "off");
}

static void set_tuner(bool on)
{
    tuner_mode = on;
    engine_set_tuner(on);
    settings.b[S_TUNER] = on;
    if (on) display_text(" - ");
    else show_preset();
}

static void action_single(int sw)
{
    if (tuner_mode) {
        set_tuner(false);
        proto_notify_settings();
        return;
    }
    if (rhythm_mode) { rhythm_single(sw); return; }
    if (looper_mode) { loop_single(sw); return; }
    if (!stomp) { load(view_bank, (unsigned)sw); proto_notify_preset(); return; }
    static const int kStomp[4] = {M_REV, M_MOD, M_AMP, M_COMP};   /* A B C D */
    toggle_module(kStomp[sw]);
}

static void action_chord(uint8_t mask)
{
    if (tuner_mode || rhythm_mode || looper_mode) return;   /* stock: chords only in preset/live mode */
    if (mask == ((1u << SW_C) | (1u << SW_D)) || mask == ((1u << SW_A) | (1u << SW_B))) {
        view_bank = (view_bank + (mask & (1u << SW_D) ? 1u : 9u)) % 10u;
        browsing = true;
        browse_ms = s_now;
        show_preset();
    } else if (mask == ((1u << SW_B) | (1u << SW_C))) {
        view_bank = bank;                    /* stock: leaves the browse */
        browsing = false;
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
        power_activity();   /* wakes the panel from standby */
        if (ui_log)
            log_printf("fsw %c %s\r\n", "ABCD"[sw],
                       ev == FSW_PRESS ? "press" : ev == FSW_RELEASE ? "release" : "long");
        if (ev == FSW_PRESS) {
            bool alone = down_mask == 0;
            down_mask |= (uint8_t)(1u << sw);
            peak_mask |= (uint8_t)(1u << sw);
            if (looper_mode && !tuner_mode && alone && sw == SW_A) loop_press();
        } else if (ev == FSW_LONG) {
            if (sw == SW_A && (down_mask & (1u << SW_B))) {          /* stock: B held + A long */
                long_used = true;
                set_tuner(!tuner_mode);
                proto_notify_settings();
            } else if (sw == SW_B && (down_mask & (1u << SW_C))) {   /* C held + B long */
                long_used = true;
                rhythm_mode = !rhythm_mode;
                settings.b[S_RHYTHM] = rhythm_mode;
                if (rhythm_mode) looper_mode = false;   /* one of the two */
                proto_notify_rhythm_mode();
                show_preset();
            } else if (sw == SW_C && (down_mask & (1u << SW_D))) {   /* ours: D held + C long */
                long_used = true;
                set_looper(!looper_mode);
            } else if (looper_mode && !tuner_mode && peak_mask == (1u << sw) &&
                       (sw == SW_A || sw == SW_B)) {
                long_used = true;
                loop_long(sw);
            } else if (peak_mask == (1u << sw) && !rhythm_mode && !tuner_mode && !looper_mode &&
                       !modifier_used) {
                /* stock: hold any switch 1 s = save to that slot of the
                 * shown bank, in preset and live mode */
                long_used = true;
                if (save_to(view_bank, (unsigned)sw) == 0) proto_notify_saved();
                overlay_until = now + 1000u;
            }
        } else if (ev == FSW_RELEASE) {
            down_mask &= (uint8_t)~(1u << sw);
            if (down_mask == 0) {
                if (!long_used && !modifier_used) {
                    if (peak_mask && !(peak_mask & (peak_mask - 1u))) action_single(__builtin_ctz(peak_mask));
                    else action_chord(peak_mask);
                }
                peak_mask = 0;
                long_used = false;
                modifier_used = false;
            }
        }
    }
}

/* Display feedback (better than the stock's bare number): the knob's name
 * for NAME_MS when a different knob is touched, then its value; a dot
 * after the value while the knob has not picked up the stored value yet. */
#define NAME_MS 600u
static int shown_knob = -1;
static uint32_t name_until;

static void show_value(int k, uint16_t v, bool picked)
{
    char t[5];
    t[0] = v >= 100 ? (char)('0' + (v / 100u) % 10u) : ' ';
    t[1] = v >= 10 ? (char)('0' + (v / 10u) % 10u) : ' ';
    t[2] = (char)('0' + v % 10u);
    t[3] = picked ? 0 : '.';
    t[4] = 0;
    (void)k;
    display_text(t);
}

static void knobs(uint32_t now)
{
    for (int k = 0; k < KNOB_COUNT; k++) {
        if (!knob_changed(k)) continue;
        power_activity();
        uint16_t v = knob_units(k), s = stored(k);
        if (ui_log) log_printf("knob k%d = %u (raw %u, stored %u%s)\r\n", k, v,
                               (unsigned)knob_value(k), s, caught[k] ? "" : ", not caught");
        bool a_held = fsw_down(SW_A) && !looper_mode;   /* looper mode: A is the looper's */
        if ((rhythm_mode || a_held) && (k == 14 || k == 8 || (a_held && k == 9))) {
            /* Drum controls (better than stock, which needs the app), acting
             * at once: in rhythm mode, or with footswitch A held as a
             * modifier: LEVEL = drum level 0..100, RATE = tempo 40..260 BPM,
             * and (A held) MOD = rhythm 1..40. */
            drums_t *d = engine_drums();
            uint16_t shown;
            const char *name;
            if (a_held) modifier_used = true;
            if (k == 14) { drums_set_level(d, v); shown = d->level; name = "dLE"; }
            else if (k == 9) {
                drums_set_rhythm(d, knob_value(k) * DRUMS_RHYTHMS / 4096u);
                shown = (uint16_t)(d->rhythm + 1u);
                name = "PAt";
            } else {
                drums_set_tempo(d, DRUMS_BPM_MIN + v * (DRUMS_BPM_MAX - DRUMS_BPM_MIN) / 100u);
                shown = d->bpm;
                name = "bPn";
            }
            rhythm_dirty = rhythm_notify = true;
            rhythm_dirty_ms = now;
            if (k != shown_knob) { shown_knob = k; name_until = now + NAME_MS; display_text(name); }
            else if ((int32_t)(now - name_until) >= 0) show_value(k, shown, true);
            overlay_until = now + 1500u;
            continue;
        }
        if (!caught[k]) {
            /* pickup: act only once the knob reaches the stored value
             * (+-2 % on level knobs; exact on selectors, where a tolerance
             * would jump the loaded model/type to the knob position) */
            int d = (int)v - (int)s, tol = kKnob[k].types ? 0 : 2;
            if (d >= -tol && d <= tol) caught[k] = true;
        }
        if (caught[k] && v != s) {
            if (kKnob[k].field == 0xFF) { settings.b[S_MASTER] = (uint8_t)v; settings_dirty = true; proto_notify_settings(); }
            else { pset(&edit, kKnob[k].field, v); proto_notify_module(kProtoModule[kKnob[k].module]); }
            revision++;
        }
        if (tuner_mode) continue;
        if (k != shown_knob) {
            shown_knob = k;
            name_until = now + NAME_MS;
            display_text(kKnob[k].name);
        } else if ((int32_t)(now - name_until) >= 0) {
            show_value(k, v, caught[k]);
        }
        overlay_until = now + 1500u;
    }
    /* name shown and the knob stopped: switch to its value */
    if (shown_knob >= 0 && name_until && (int32_t)(now - name_until) >= 0) {
        name_until = 0;
        if ((rhythm_mode || (fsw_down(SW_A) && !looper_mode)) && (shown_knob == 14 || shown_knob == 8 || shown_knob == 9)) {
            drums_t *d = engine_drums();
            show_value(shown_knob, shown_knob == 14 ? d->level : shown_knob == 9 ? d->rhythm + 1u : d->bpm, true);
        } else {
            show_value(shown_knob, knob_units(shown_knob), caught[shown_knob]);
        }
    }
}

static void leds(uint32_t now)
{
    bool blink_on = (now / 250u) & 1u;
    for (int k = 0; k < KNOB_COUNT; k++) {
        if (kKnob[k].led < 0) continue;
        if (tuner_mode) { knob_led(kKnob[k].led, false); continue; }   /* stock: off while tuning */
        if (kKnob[k].module == M_NONE) { knob_led(kKnob[k].led, true); continue; }   /* master */
        bool enabled = pget(&edit, kModuleEnable[kKnob[k].module]) != 0;
        knob_led(kKnob[k].led, enabled && (caught[k] || blink_on));
    }
}

/* The footswitch light rings (ui/lightbar.c) follow the mode; live mode
 * lights a ring per module that is on (stock 0x6a94: A reverb, B mod, C amp
 * or cab, D comp). */
static void light_rings(uint32_t now)
{
    lightbar_in_t in = {
        .mode = tuner_mode ? LB_TUNER : rhythm_mode ? LB_RHYTHM : looper_mode ? LB_LOOPER
              : stomp ? LB_LIVE : LB_PRESET,
        .slot = (uint8_t)slot,
        .colour = settings.b[S_LIGHT_COLOUR + slot],
        .level = settings.b[S_LIGHT_LEVEL + slot],
    };
    if (in.mode == LB_LIVE) {
        in.on = (uint8_t)((pget(&edit, P_REV_EN) == 1) << SW_A | (pget(&edit, P_MOD_EN) == 1) << SW_B |
                          (pget(&edit, P_AMP_EN) == 1 || pget(&edit, P_CAB_EN) == 1) << SW_C |
                          (pget(&edit, P_COMP_EN) == 1) << SW_D);
    } else if (in.mode == LB_RHYTHM) {
        const drums_t *d = engine_drums();
        in.on = (uint8_t)(fsw_down(SW_A) << SW_A | fsw_down(SW_B) << SW_B);
        in.playing = d->on;
        in.bpm = d->bpm;
    } else if (in.mode == LB_LOOPER) {
        looper_info_t li;
        engine_loop_info(&li);
        in.loop = (uint8_t)li.state;
        in.loop_top = li.pos_ms < 100u;          /* the loop start: a flash */
    }
    lightbar_task(now, &in);
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
    engine_loop_poll();                          /* looper fades; memory back when cleared */
    if (looper_mode && !tuner_mode && !overlay_until) {
        looper_info_t li;
        engine_loop_info(&li);
        if (li.state != loop_shown) show_loop();   /* closed when full, cleared, stopped */
    }
    knobs(now_ms);
    leds(now_ms);
    light_rings(now_ms);
    if (overlay_until && (int32_t)(now_ms - overlay_until) >= 0) {
        overlay_until = 0;
        shown_knob = -1;
        show_preset();
    }
    if (browsing && !stomp) {
        static bool blank;
        if (now_ms - browse_ms > BROWSE_MS) {
            view_bank = bank;
            browsing = blank = false;
            if (!overlay_until) show_preset();
        } else if (!overlay_until) {
            bool b = (now_ms - browse_ms) % BROWSE_FLASH_MS >= BROWSE_FLASH_MS / 2u;
            if (b != blank) { blank = b; if (b) display_text("   "); else show_preset(); }
        }
    }
    if (rhythm_dirty && now_ms - rhythm_dirty_ms > 3000u) {
        uint8_t r[RHYTHM_SIZE];
        ui_rhythm_block(r);
        (void)rhythm_settings_write(r);
        if (rhythm_notify) proto_notify_rhythm();   /* knob drum changes */
        rhythm_dirty = rhythm_notify = false;
    }
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

/* The stock factory reset (0x18fe0): the 40 presets (20 factory presets, 20
 * "EMPTY"), the default settings and the default rhythm block; preset 1A is
 * loaded. Unlike the stock we keep the master volume (the stock default is
 * 0: silent until the MASTER knob moves) and the BLE name copy at S+0x02.
 * The factory presets come from the stock data blob (version 2). */
int ui_factory_reset(void)
{
    if (!g_stock_factory) {
        log_printf("factory reset refused: the stock data has no factory presets (blob version 1);"
                   " write it again: fb200 update stock <stock .mr>\r\n");
        return -1;
    }
    for (unsigned i = 0; i < PRESET_COUNT; i++) {
        preset_t p;
        memcpy(p.b, g_stock_factory->preset[i < STOCK_FACTORY_NAMED ? i : STOCK_FACTORY_NAMED],
               PRESET_SIZE);
        wdog_feed();                              /* 40 sector writes take a few seconds */
        if (preset_write(i, &p) != 0) return -2;
    }
    settings_t def;
    memcpy(def.b, settings_default, SETTINGS_SIZE);   /* preset_check.c */
    memcpy(def.b + 2, settings.b + 2, 20);
    def.b[S_MASTER] = settings.b[S_MASTER];
    bool bt_off = settings.b[S_BT] == 0;
    settings = def;
    static const uint8_t kRhythm[RHYTHM_SIZE] = {0, 0, 0, 100, 110, 0};
    ui_rhythm_set(kRhythm);
    stomp = rhythm_mode = false;
    tuner_mode = false;
    engine_set_tuner(false);
    load(0, 0);
    wdog_feed();
    if (settings_write(&settings) != 0 || rhythm_settings_write(kRhythm) != 0) return -3;
    settings_dirty = rhythm_dirty = false;
    if (bt_off) proto_hook_bt_enable(true);
    log_printf("factory reset done\r\n");
    return 0;
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
    (void)preset_sanitize(&edit);   /* app 0x97 [0xFF]: a whole record, unchecked by the stock */
    revision++;
}

settings_t *ui_settings(void) { return &settings; }

/* After an app write (B0, B8, C9, ...): the modes follow the settings block
 * at once, as on the stock. */
void ui_settings_changed(void)
{
    (void)settings_sanitize(&settings);   /* app B0/B7/B8/C9 write fields unchecked */
    bool st = settings.b[S_STOMP] == 1, rh = settings.b[S_RHYTHM] == 1;
    bool redraw = st != stomp || rh != rhythm_mode;
    stomp = st;
    rhythm_mode = rh;
    if (rh) looper_mode = false;
    if ((settings.b[S_TUNER] == 1) != tuner_mode) set_tuner(!tuner_mode);
    else if (redraw && !tuner_mode) show_preset();
    settings_dirty = true;
    revision++;
}
