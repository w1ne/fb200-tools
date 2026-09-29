#ifndef FB200_POWER_LOGIC_H
#define FB200_POWER_LOGIC_H
/* Battery gauge and idle state machine: pure logic, no SDK, host-tested
 * (tests/power_host_test.c). ui/power.c feeds it the ADC readings, the
 * charger sense and the activity events every POWER_TICK_MS. */
#include <stdbool.h>
#include <stdint.h>

#define POWER_TICK_MS 250u

/* Battery ADC (ADC1 IN9, 12 bit, 3.3 V reference) to battery mV. The
 * divider is not measured yet: 0.75 (raw = mV * 0.9307) puts the stock level
 * thresholds (0xE74, 0xE10, 0xD98, 0xC80) at 3.98, 3.87, 3.74 and 3.44 V,
 * a normal Li-ion scale ("charge now" at 3.44 V).
 * TODO(pedal): read `power` and the battery voltage with a multimeter at
 * the same time; BATT_RAW_PER_V = raw * 1000 / mV. */
#define BATT_RAW_PER_V 930u

/* Stock level thresholds (ITCM 0x188dc): raw >= kLevel[i] -> level 4 - i. */
#define GAUGE_HYST 12u            /* raw (~13 mV) to go up a level again */
#define GAUGE_NO_BATT 1500u       /* below: no battery / not wired, ignore */
#define GAUGE_CRIT_RAW 3118u      /* ~3.35 V: critical (safe state) */
#define GAUGE_CRIT_EXIT_RAW 3257u /* ~3.50 V: leave the safe state */
#define GAUGE_CRIT_TICKS 120u     /* 30 s below GAUGE_CRIT_RAW */
#define GAUGE_LOW_REPEAT_TICKS 1200u   /* low-battery warning again after 5 min */

typedef struct {
    uint32_t filt;         /* raw << 4, IIR (time constant 8 ticks = 2 s) */
    bool started;
    uint8_t level;         /* 0..4 as the stock (4 while charging) */
    bool critical;
    uint16_t crit_ticks;
    uint16_t low_ticks;    /* ticks since the last low warning */
    bool was_low;
} gauge_t;

typedef enum { GAUGE_EV_NONE = 0, GAUGE_EV_LOW = 1, GAUGE_EV_CRITICAL = 2,
               GAUGE_EV_RECOVERED = 3 } gauge_event_t;

void gauge_init(gauge_t *g);
/* One tick: raw battery ADC and the charger sense. Returns an event. */
gauge_event_t gauge_update(gauge_t *g, uint16_t raw, bool charging);
uint16_t gauge_raw(const gauge_t *g);           /* filtered raw */
uint32_t gauge_mv(uint16_t raw);                /* estimated battery mV */
unsigned gauge_percent(uint16_t raw);           /* 0..100, Li-ion curve (estimate) */
uint8_t gauge_level_of(uint16_t raw);           /* stock thresholds, no hysteresis */

/* Idle: after `timeout_min` minutes (0 = never) without activity and not
 * charging, the pedal goes to standby (LEDs dark, audio on); any activity
 * wakes it. */
typedef struct {
    uint32_t idle_ms;
    bool standby;
} idle_t;

void idle_init(idle_t *s);
/* Returns true when the standby state changed. */
bool idle_tick(idle_t *s, uint32_t dt_ms, unsigned timeout_min, bool active, bool charging);
bool idle_activity(idle_t *s);                   /* true: left standby */

/* Knob-LED and display duty in the 3 ms multiplex slot for a brightness
 * level in % (100, 66, 33): lit ms of 3. */
unsigned power_duty_ms(unsigned level_pct);
#endif
