#include <string.h>
#include "ui/ui.h"
#include "ui/controls.h"
#include "ui/display.h"
#include "debug/cdc_log.h"

enum { SW_A, SW_B, SW_C, SW_D };
enum { M_GATE, M_COMP, M_AMP, M_CAB, M_MOD, M_REV, M_NONE };

typedef struct {
    int8_t led;          /* knob LED index, -1 none */
    uint8_t module;
    uint8_t field;       /* preset offset; 0xFF = master volume (settings) */
    uint8_t types;       /* 0 = 0..100 knob, N = selector with N positions */
    uint8_t base;        /* first selector value: amp/cab are 1-based, mod/rev 0-based */
} knob_map_t;

/* stock knob table (ITCM 0x5a60), indexed by mux channel k0..k15 */
static const knob_map_t kKnob[KNOB_COUNT] = {
    [0] = {6, M_COMP, P_COMP_LEVEL, 0},   [1] = {3, M_AMP, P_AMP_MODEL, 10, 1},
    [2] = {8, M_GATE, P_GATE_THRESH, 0},  [3] = {0, M_AMP, P_AMP_BASS, 0},
    [4] = {11, M_AMP, P_AMP_GAIN, 0},     [5] = {1, M_AMP, P_AMP_MID, 0},
    [6] = {2, M_AMP, P_AMP_TREBLE, 0},    [7] = {7, M_COMP, P_COMP_THRESH, 0},
    [8] = {12, M_AMP, P_AMP_VOLUME, 0},   [9] = {9, M_MOD, P_MOD_P2, 0},
    [10] = {15, M_REV, P_REV_TYPE, 5, 0},    [11] = {5, M_CAB, P_CAB_TYPE, 19, 1},
    [12] = {13, M_REV, P_REV_LEVEL, 0},   [13] = {-1, M_NONE, 0xFF, 0},
    [14] = {4, M_MOD, P_MOD_P1, 0},       [15] = {10, M_MOD, P_MOD_TYPE, 12, 0},
};
static const uint8_t kModuleEnable[M_NONE] = {P_GATE_EN, P_COMP_EN, P_AMP_EN, P_CAB_EN,
                                              P_MOD_EN, P_REV_EN};

static preset_t edit;
static settings_t settings;
static unsigned bank, slot;
static bool stomp, caught[KNOB_COUNT];
static uint8_t down_mask, peak_mask;
static bool long_used;
static uint32_t overlay_until, revision, settings_dirty_ms;
static bool settings_dirty;
static bool ui_log;

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

static void show_preset(void)
{
    char t[4] = {stomp ? 'L' : 'P', (char)('0' + bank), "AbCd"[slot], 0};
    display_text(t);
}

static void load(unsigned b, unsigned s)
{
    bank = b % 10u;
    slot = s & 3u;
    preset_read(bank * 4u + slot, &edit);
    for (int k = 0; k < KNOB_COUNT; k++) caught[k] = (k == 13);   /* master is global */
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

void ui_init(void)
{
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
}

static void action_single(int sw)
{
    if (!stomp) { load(bank, (unsigned)sw); return; }
    static const int kStomp[4] = {M_REV, M_MOD, M_AMP, M_COMP};   /* A B C D */
    toggle_module(kStomp[sw]);
}

static void action_chord(uint8_t mask)
{
    if (mask == ((1u << SW_C) | (1u << SW_D))) load(bank + 1u, slot);
    else if (mask == ((1u << SW_A) | (1u << SW_B))) load(bank + 9u, slot);
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
            if (peak_mask == (1u << SW_A) && !stomp) { long_used = true; ui_save(); overlay_until = now + 1000u; }
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
            int d = (int)v - (int)s;
            if (d < -2 || d > 2) continue;    /* pickup: wait until the knob passes the value */
            caught[k] = true;
        }
        if (v == s) continue;
        if (kKnob[k].field == 0xFF) { settings.b[S_MASTER] = (uint8_t)v; settings_dirty = true; }
        else pset(&edit, kKnob[k].field, v);
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
        bool enabled = pget(&edit, kModuleEnable[kKnob[k].module]) != 0;
        knob_led(kKnob[k].led, enabled && (caught[k] || blink_on));
    }
}

void ui_task(uint32_t now_ms)
{
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

const preset_t *ui_edit_preset(void) { return &edit; }
unsigned ui_preset_index(void) { return bank * 4u + slot; }
uint8_t ui_master(void) { return settings.b[S_MASTER]; }
uint32_t ui_revision(void) { return revision; }
