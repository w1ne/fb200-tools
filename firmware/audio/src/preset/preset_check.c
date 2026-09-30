/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Presets and settings read from flash (or written whole by the app, 0x97)
 * are untrusted: a power loss during a sector write leaves the sector erased
 * (8 presets, or the settings), a bad write leaves any bytes. The stock
 * clamps what the app writes field by field (proto.c set_module) but plays a
 * stored preset as it is. We play the stored bytes only after these checks,
 * so an erased or corrupt record never plays out-of-range values (a reverb
 * decay of 655 % feeds back without bound). Flash is never rewritten here:
 * the stock app and firmware keep reading the same bytes. */
#include <string.h>
#include "preset/preset.h"
#include "dsp/stock_data.h"

/* Stock default settings (0x20004E00, written by its factory reset). */
const uint8_t settings_default[SETTINGS_SIZE] = {
    'B', '1', [S_BT] = 1, [0x19] = 1, [S_IN_GAIN] = 13, 13, 13, 13, 13,
    [0x28] = 100, 100, 100, 100, [S_TUNER_CAL] = 5, [S_TUNER_MUTE] = 1,
};

#define TYPE 0x8000u   /* a type field: out of range -> 1 (the stock's rule for app writes) */
#define MOD_LAST 0x4000u /* mod p5: over 240 -> 100 (the stock's rule) */

/* The stock limits of every module block (app fn 0x80..0x86, ITCM 0x46c4).
 * Only upper bounds: nothing below 0 exists, and the DSP clamps its own
 * lower limits (delay time). */
static const struct { uint8_t off, words; uint16_t max[8]; } kLimits[7] = {
    {P_COMP_EN, 6, {1, TYPE | 21, 100, 100, 100, 100}},
    {P_GATE_EN, 3, {1, TYPE | 4, 100}},
    {P_AMP_EN, 8, {1, TYPE | 120, 100, 100, 100, 100, 100, 100}},
    {P_CAB_EN, 6, {1, TYPE | 120, 4, 100, 100, 9}},
    {P_MOD_EN, 7, {1, TYPE | 21, 100, 100, 100, 100, MOD_LAST | 240}},
    {P_DLY_EN, 5, {1, TYPE | 6, 100, 100, 2500}},
    {P_REV_EN, 6, {1, TYPE | 5, 200, 100, 100, 100}},
};

bool preset_erased(const preset_t *p)
{
    for (unsigned i = 0; i < PRESET_SIZE; i++)
        if (p->b[i] != 0xFF) return false;
    return true;
}

unsigned preset_sanitize(preset_t *p)
{
    if (preset_erased(p)) {
        /* the stock's own blank preset (factory "EMPTY"), else all modules off */
        if (g_stock_factory) {
            memcpy(p->b, g_stock_factory->preset[STOCK_FACTORY_NAMED], PRESET_SIZE);
        } else {
            memset(p->b, 0, PRESET_SIZE);
            memcpy(p->b + P_NAME, "EMPTY", 5);
        }
        return PRESET_SIZE;
    }
    unsigned fixed = 0;
    for (unsigned m = 0; m < 7u; m++) {
        for (unsigned i = 0; i < kLimits[m].words; i++) {
            unsigned off = kLimits[m].off + 2u * i, lim = kLimits[m].max[i];
            uint16_t v = pget(p, off), w = v;
            if (lim & TYPE) { if (v > (lim & 0xFFFu)) w = 1; }
            else if (lim & MOD_LAST) { if (v > (lim & 0xFFFu)) w = 100; }
            else if (v > lim) w = (uint16_t)lim;
            if (w != v) { pset(p, off, w); fixed++; }
        }
    }
    return fixed;
}

/* Settings fields with a range; anything else is kept as it is (the light
 * rings render any colour/level the stock way, lightbar.c). Out of range ->
 * the stock default of that field. */
static const struct { uint8_t off, max; } kSettingsLimits[] = {
    {S_PRESET, PRESET_COUNT - 1}, {S_BT, 1}, {S_MASTER, 100}, {0x19, 1},
    {0x1b, 72}, {0x1c, 72}, {0x1d, 72}, {0x1e, 72},
    {S_STOMP, 1}, {S_RHYTHM, 1}, {S_SLOT, 3}, {S_BANK, 9},
    {S_TUNER_CAL, 15}, {S_TUNER, 1}, {S_TUNER_MUTE, 1},
};

unsigned settings_sanitize(settings_t *s)
{
    bool erased = true;
    for (unsigned i = 0; i < SETTINGS_SIZE && erased; i++) erased = s->b[i] == 0xFF;
    if (erased) {
        memcpy(s->b, settings_default, SETTINGS_SIZE);
        return SETTINGS_SIZE;
    }
    unsigned fixed = 0;
    for (unsigned i = 0; i < sizeof kSettingsLimits / sizeof kSettingsLimits[0]; i++) {
        unsigned off = kSettingsLimits[i].off;
        uint8_t def = (off >= 0x1b && off <= 0x1e) ? 9u : settings_default[off];   /* 0xB7: > 72 -> 9 */
        if (s->b[off] > kSettingsLimits[i].max) { s->b[off] = def; fixed++; }
    }
    return fixed;
}
