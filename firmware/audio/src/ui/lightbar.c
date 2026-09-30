/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include <string.h>
#include "ui/lightbar.h"
#include "ui/rgb.h"
#include "dsp/looper.h"

/* First LED of each footswitch's ring, A..D: switch s is LEDs 10*s..10*s+9,
 * as the stock addresses them (0x19414; rhythm ring A = switch A at 0x68ca).
 * Checked on the pedal: selecting slot A lights the dome of LEDs 0-9. */
static const uint8_t kRingFirst[4] = {0, 10, 20, 30};

/* Stock palette (DTCM 0x2000766c), 0xRRGGBB: red, orange, yellow, green,
 * cyan, blue, magenta, pink, white. Index 9 reads the word after the table
 * (0x30420000): a dim red. */
static const uint32_t kPalette[10] = {0xFF0000, 0xFF3300, 0xFFFF00, 0x00FF00, 0x00FFFF,
                                      0x0000FF, 0xFF00FF, 0xFF0330, 0xFFFFFF, 0x420000};
/* Live mode, one fixed colour per switch (DTCM 0x20004a2c..0x20004a38):
 * A reverb purple, B mod orange, C amp/cab red, D comp green. */
static const uint32_t kLive[4] = {0x800080, 0xFF6400, 0xFF0000, 0x00FF00};
#define RED 0xFF0000u

#define SHOW_MS 20u
static uint32_t save_ms, beat_ms, shown_ms;
static bool saving;
static uint8_t shown[RGB_COUNT][3];
static unsigned global_pct = 100u;   /* ui/power.c: brightness, 0 = dark */

void lightbar_set_level(unsigned pct) { global_pct = pct > 100u ? 100u : pct; }

uint32_t lightbar_colour(unsigned index, unsigned level)
{
    if (level > 100u) level = 100u;
    if (index > 9u) index = 8u;
    /* stock: scale = (u8)(30 + level * 0.7), byte * (scale / 100.0) in
     * double. The integer form gives the same bytes for every level 0..100
     * and every byte in the palette (checked exhaustively). */
    uint32_t scale = 30u + level * 7u / 10u, c = kPalette[index], out = 0;
    for (int sh = 0; sh <= 16; sh += 8) out |= (((c >> sh) & 0xFFu) * scale / 100u) << sh;
    return out;
}

void lightbar_save(uint32_t now_ms)
{
    save_ms = now_ms;
    saving = true;
}

void lightbar_rings(uint32_t now_ms, const lightbar_in_t *in, uint32_t ring[4])
{
    unsigned slot = in->slot & 3u;
    uint32_t mine = in->mode == LB_LIVE ? kLive[slot] : lightbar_colour(in->colour, in->level);
    memset(ring, 0, 4 * sizeof ring[0]);
    if (saving) {
        /* stock 0x67e0: off 0-200 ms, on 200-400, off, on 600-800, off to 1 s;
         * "on" is the normal ring of the saved slot (0x19414) */
        uint32_t t = now_ms - save_ms;
        if (t < 1000u) {
            if ((t / 200u) & 1u) ring[slot] = mine;
            return;
        }
        saving = false;
    }
    switch (in->mode) {
    case LB_PRESET:
        ring[slot] = mine;
        break;
    case LB_LIVE:
        for (int s = 0; s < 4; s++) if (in->on & (1u << s)) ring[s] = kLive[s];
        break;
    case LB_TUNER:
        break;
    case LB_RHYTHM: {
        /* stock 0x68ca: C off for the first half of each beat (60000 / bpm
         * ms, a free-running 1 kHz counter), red for the second half */
        uint64_t beat = (uint64_t)(now_ms - beat_ms) * (in->bpm ? in->bpm : 1u);
        if (in->on & 1u) ring[0] = RED;
        if (in->on & 2u) ring[1] = RED;
        if (beat >= 60000u) { beat_ms = now_ms; beat = 0; }   /* the next beat */
        if (beat >= 30000u) ring[2] = RED;
        if (in->playing) ring[3] = RED;
        break;
    }
    case LB_LOOPER: {
        static const uint32_t kLoop[] = {[LOOPER_REC] = RED, [LOOPER_PLAY] = 0x00FF00,
                                         [LOOPER_DUB] = 0xFF6400, [LOOPER_STOP] = 0};
        bool playing = in->loop == LOOPER_PLAY || in->loop == LOOPER_DUB;
        if (in->loop <= LOOPER_STOP) ring[0] = playing && in->loop_top ? 0xFFFFFF : kLoop[in->loop];
        if (in->loop >= LOOPER_PLAY) ring[1] = 0x0000FF;
        break;
    }
    }
}

void lightbar_task(uint32_t now_ms, const lightbar_in_t *in)
{
    uint32_t ring[4];
    uint8_t frame[RGB_COUNT][3];
    lightbar_rings(now_ms, in, ring);
    for (int s = 0; s < 4; s++) {
        /* stock 0x17d54 stores each byte >> 2: 25 % of full scale at most */
        uint8_t r = (uint8_t)(ring[s] >> 18), g = (uint8_t)((ring[s] >> 10) & 0x3Fu),
                b = (uint8_t)((ring[s] >> 2) & 0x3Fu);
        if (global_pct < 100u) {
            r = (uint8_t)(r * global_pct / 100u);
            g = (uint8_t)(g * global_pct / 100u);
            b = (uint8_t)(b * global_pct / 100u);
        }
        for (int i = kRingFirst[s]; i < kRingFirst[s] + 10; i++) {
            frame[i][0] = r;
            frame[i][1] = g;
            frame[i][2] = b;
        }
    }
    if (!memcmp(frame, shown, sizeof frame) || now_ms - shown_ms < SHOW_MS) return;
    for (int i = 0; i < RGB_COUNT; i++) rgb_set(i, frame[i][0], frame[i][1], frame[i][2]);
    if (rgb_show()) {
        memcpy(shown, frame, sizeof frame);
        shown_ms = now_ms;
    }
}
