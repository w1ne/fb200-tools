/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include "ui/power_logic.h"

static const uint16_t kLevel[4] = {0xE74u, 0xE10u, 0xD98u, 0xC80u};   /* >= -> 4,3,2,1 */

/* Li-ion under a light load (~0.15 C), mV -> %. An estimate: the pedal's
 * cell and load are not measured (TODO(pedal): a drain log with `power log`). */
static const struct { uint16_t mv; uint8_t pct; } kCurve[] = {
    {3400, 0}, {3550, 5}, {3650, 10}, {3700, 20}, {3740, 30}, {3770, 40},
    {3800, 50}, {3840, 60}, {3900, 70}, {3970, 80}, {4050, 90}, {4150, 100},
};
#define CURVE_N (sizeof kCurve / sizeof kCurve[0])

uint8_t gauge_level_of(uint16_t raw)
{
    uint8_t i = 0;
    while (i < 4 && raw < kLevel[i]) i++;
    return (uint8_t)(4u - i);
}

uint32_t gauge_mv(uint16_t raw) { return (uint32_t)raw * 1000u / BATT_RAW_PER_V; }

unsigned gauge_percent(uint16_t raw)
{
    uint32_t mv = gauge_mv(raw);
    if (mv <= kCurve[0].mv) return 0;
    if (mv >= kCurve[CURVE_N - 1].mv) return 100;
    unsigned i = 1;
    while (mv > kCurve[i].mv) i++;
    uint32_t m0 = kCurve[i - 1].mv, m1 = kCurve[i].mv, p0 = kCurve[i - 1].pct, p1 = kCurve[i].pct;
    return (unsigned)(p0 + (p1 - p0) * (mv - m0) / (m1 - m0));
}

void gauge_init(gauge_t *g) { *g = (gauge_t){0}; }

uint16_t gauge_raw(const gauge_t *g) { return (uint16_t)((g->filt + 8u) >> 4); }

gauge_event_t gauge_update(gauge_t *g, uint16_t raw, bool charging)
{
    if (!g->started) {
        g->filt = (uint32_t)raw << 4;
        g->started = true;
        g->level = charging ? 4u : gauge_level_of(raw);
    } else {
        g->filt = g->filt - (g->filt >> 3) + ((uint32_t)raw << 1);   /* y += (x - y) / 8 */
    }
    uint16_t f = gauge_raw(g);
    bool batt = f >= GAUGE_NO_BATT;
    /* Level: stock thresholds on the filtered value; down at once, up only
     * GAUGE_HYST above the threshold (no flapping under audio load). */
    if (charging) {
        g->level = 4u;                                  /* stock 0x1897c */
    } else {
        uint8_t t = gauge_level_of(f);
        if (t < g->level) g->level = t;
        else if (t > g->level && f >= kLevel[4u - t] + GAUGE_HYST) g->level = t;
    }

    gauge_event_t ev = GAUGE_EV_NONE;
    if (g->critical) {
        if (charging || f >= GAUGE_CRIT_EXIT_RAW) {
            g->critical = false;
            g->crit_ticks = 0;
            ev = GAUGE_EV_RECOVERED;
        }
    } else if (!charging && batt && f < GAUGE_CRIT_RAW) {
        if (++g->crit_ticks >= GAUGE_CRIT_TICKS) {
            g->critical = true;
            ev = GAUGE_EV_CRITICAL;
        }
    } else {
        g->crit_ticks = 0;
    }

    bool low = !charging && batt && g->level == 0u;
    if (low) {
        if (!g->was_low || ++g->low_ticks >= GAUGE_LOW_REPEAT_TICKS) {
            g->low_ticks = 0;
            if (ev == GAUGE_EV_NONE) ev = GAUGE_EV_LOW;
        }
    } else {
        g->low_ticks = 0;
    }
    g->was_low = low;
    return ev;
}

void idle_init(idle_t *s) { *s = (idle_t){0}; }

bool idle_activity(idle_t *s)
{
    s->idle_ms = 0;
    if (!s->standby) return false;
    s->standby = false;
    return true;
}

bool idle_tick(idle_t *s, uint32_t dt_ms, unsigned timeout_min, bool active, bool charging)
{
    if (active || charging || timeout_min == 0u) return idle_activity(s);
    if (s->standby) return false;
    s->idle_ms += dt_ms;
    if (s->idle_ms < timeout_min * 60000u) return false;
    s->standby = true;
    return true;
}

unsigned power_duty_ms(unsigned level_pct)
{
    if (level_pct >= 100u) return 3u;
    if (level_pct >= 66u) return 2u;
    return 1u;
}
