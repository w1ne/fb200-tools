/* Host tests for the battery gauge and the idle state machine
 * (src/ui/power_logic.c). Built by tests/test_power_host.py. */
#include <stdio.h>
#include <stdlib.h>
#include "ui/power_logic.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static gauge_event_t feed(gauge_t *g, uint16_t raw, bool chg, unsigned n)
{
    gauge_event_t last = GAUGE_EV_NONE;
    for (unsigned i = 0; i < n; i++) {
        gauge_event_t e = gauge_update(g, raw, chg);
        if (e != GAUGE_EV_NONE) last = e;
    }
    return last;
}

static void test_levels(void)
{
    /* the stock thresholds (ITCM 0x188dc) */
    CHECK(gauge_level_of(0xE74) == 4 && gauge_level_of(0xE73) == 3);
    CHECK(gauge_level_of(0xE10) == 3 && gauge_level_of(0xE0F) == 2);
    CHECK(gauge_level_of(0xD98) == 2 && gauge_level_of(0xD97) == 1);
    CHECK(gauge_level_of(0xC80) == 1 && gauge_level_of(0xC7F) == 0);
    CHECK(gauge_level_of(0) == 0 && gauge_level_of(4095) == 4);
}

static void test_filter_and_hysteresis(void)
{
    gauge_t g;
    gauge_init(&g);
    gauge_update(&g, 3650, false);
    CHECK(gauge_raw(&g) == 3650 && g.level == 3);
    feed(&g, 3650, false, 50);
    CHECK(gauge_raw(&g) == 3650);
    /* a step settles within ~6 time constants, never overshoots */
    gauge_init(&g);
    gauge_update(&g, 3000, false);
    for (int i = 0; i < 60; i++) { gauge_update(&g, 3800, false); CHECK(gauge_raw(&g) <= 3800); }
    CHECK(gauge_raw(&g) >= 3795);
    /* noise around a threshold (audio load): no flapping */
    gauge_init(&g);
    gauge_update(&g, 0xE10 - 2, false);
    CHECK(g.level == 2);
    unsigned changes = 0;
    uint8_t lvl = g.level;
    for (int i = 0; i < 400; i++) {
        gauge_update(&g, (uint16_t)(0xE10 + ((i & 1) ? 8 : -8)), false);
        if (g.level != lvl) { changes++; lvl = g.level; }
    }
    CHECK(changes == 0);
    /* up only GAUGE_HYST above the threshold, down at once */
    feed(&g, 0xE10 + GAUGE_HYST + 2, false, 80);
    CHECK(g.level == 3);
    feed(&g, 0xE10 - 3, false, 80);
    CHECK(g.level == 2);
    /* charging: level 4 as the stock; after unplug the voltage decides */
    feed(&g, 0xE10 - 3, true, 1);
    CHECK(g.level == 4);
    feed(&g, 0xD98 + 40, false, 80);
    CHECK(g.level == 2);
}

static void test_percent(void)
{
    unsigned prev = 0;
    for (uint32_t raw = 2800; raw <= 4000; raw++) {
        unsigned p = gauge_percent((uint16_t)raw);
        CHECK(p <= 100 && p >= prev);
        prev = p;
    }
    CHECK(gauge_percent(0) == 0 && gauge_percent(4095) == 100);
    CHECK(gauge_mv(930) == 1000);
    /* the stock level thresholds on the estimated curve: 4 >= ~75 %, 1 = a few % */
    CHECK(gauge_percent(0xE74) >= 70 && gauge_percent(0xE74) <= 90);
    CHECK(gauge_percent(0xC80) <= 5);
}

static void test_low_and_critical(void)
{
    gauge_t g;
    gauge_init(&g);
    CHECK(feed(&g, 3500, false, 20) == GAUGE_EV_NONE);
    /* level 0: a low warning at once, again after GAUGE_LOW_REPEAT_TICKS */
    gauge_init(&g);
    gauge_update(&g, 3190, false);
    CHECK(g.level == 0);
    unsigned lows = 0;
    for (unsigned i = 0; i < GAUGE_LOW_REPEAT_TICKS * 2 + 5; i++)
        if (gauge_update(&g, 3190, false) == GAUGE_EV_LOW) lows++;
    CHECK(lows == 2);
    /* critical after GAUGE_CRIT_TICKS below GAUGE_CRIT_RAW, once */
    gauge_init(&g);
    gauge_update(&g, GAUGE_CRIT_RAW - 20, false);
    unsigned crit = 0, tick_of_crit = 0;
    for (unsigned i = 1; i <= GAUGE_CRIT_TICKS * 3; i++)
        if (gauge_update(&g, GAUGE_CRIT_RAW - 20, false) == GAUGE_EV_CRITICAL) { crit++; tick_of_crit = i; }
    CHECK(crit == 1 && g.critical && tick_of_crit + 1 == GAUGE_CRIT_TICKS);   /* + the first reading */
    /* a short dip is not critical */
    gauge_t h;
    gauge_init(&h);
    feed(&h, 3400, false, 50);
    feed(&h, GAUGE_CRIT_RAW - 100, false, 20);
    feed(&h, 3400, false, GAUGE_CRIT_TICKS * 2);
    CHECK(!h.critical);
    /* recover: charger in, or the voltage back above the exit threshold */
    CHECK(feed(&g, GAUGE_CRIT_RAW - 20, true, 1) == GAUGE_EV_RECOVERED && !g.critical);
    feed(&g, GAUGE_CRIT_RAW - 20, false, GAUGE_CRIT_TICKS + 60);
    CHECK(g.critical);
    feed(&g, GAUGE_CRIT_RAW + 20, false, 100);
    CHECK(g.critical);                                   /* hysteresis */
    CHECK(feed(&g, GAUGE_CRIT_EXIT_RAW + 30, false, 100) == GAUGE_EV_RECOVERED && !g.critical);
    /* charging never goes critical; no battery (reads ~0) is ignored */
    gauge_init(&g);
    CHECK(feed(&g, GAUGE_CRIT_RAW - 20, true, GAUGE_CRIT_TICKS * 2) == GAUGE_EV_NONE && !g.critical);
    gauge_init(&g);
    CHECK(feed(&g, 5, false, GAUGE_CRIT_TICKS * 2) == GAUGE_EV_NONE && !g.critical);
}

static void test_idle(void)
{
    idle_t s;
    idle_init(&s);
    for (int i = 0; i < 100000; i++) CHECK(!idle_tick(&s, POWER_TICK_MS, 0, false, false));
    CHECK(!s.standby);                                   /* timeout 0 = off */
    idle_init(&s);
    unsigned ticks = 0;
    while (!idle_tick(&s, POWER_TICK_MS, 1, false, false)) ticks++;
    CHECK(s.standby && (ticks + 1) * POWER_TICK_MS == 60000u);
    CHECK(!idle_tick(&s, POWER_TICK_MS, 1, false, false) && s.standby);
    CHECK(idle_activity(&s) && !s.standby);              /* a footswitch wakes it */
    CHECK(!idle_activity(&s));
    /* playing (activity every tick) or charging: never standby */
    for (int i = 0; i < 1000; i++) CHECK(!idle_tick(&s, POWER_TICK_MS, 1, true, false));
    for (int i = 0; i < 1000; i++) CHECK(!idle_tick(&s, POWER_TICK_MS, 1, false, true));
    CHECK(!s.standby);
    /* charging wakes a standby pedal */
    idle_init(&s);
    while (!idle_tick(&s, POWER_TICK_MS, 1, false, false)) {}
    CHECK(idle_tick(&s, POWER_TICK_MS, 1, false, true) && !s.standby);
}

static void test_duty(void)
{
    CHECK(power_duty_ms(100) == 3 && power_duty_ms(66) == 2 && power_duty_ms(33) == 1);
    CHECK(power_duty_ms(0) == 1 && power_duty_ms(200) == 3);
}

int main(void)
{
    test_levels();
    test_filter_and_hysteresis();
    test_percent();
    test_low_and_critical();
    test_idle();
    test_duty();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("power host tests OK\n");
    return 0;
}
