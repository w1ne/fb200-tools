/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* LED / status output. The pin is not yet known: the stock firmware
 * configures ~45 pads as GPIO outputs (display, relays, ...), so the board
 * tells us which is the LED via `ledscan`, which drives each candidate in
 * turn. Once identified, `ledpin <gpio> <pin>` selects it and led_task()
 * shows streaming (solid) or idle (1 Hz heartbeat).
 *
 * Direct register access (the pads are muxed to ALT5 = GPIO here). */
#include <string.h>
#include "led.h"
#include "tusb.h"
#include "debug/cdc_log.h"

#define GPIO_BASE(g) ((volatile uint32_t *)((g) == 1   ? 0x401B8000u \
                                            : (g) == 2 ? 0x401BC000u \
                                            : (g) == 3 ? 0x401C0000u \
                                                       : 0x401C4000u))
#define GPIO_GDIR 1u        /* +0x04 */
#define GPIO_DR_SET 0x21u   /* +0x84 */
#define GPIO_DR_CLEAR 0x22u /* +0x88 */

#define PAD_CFG_DEFAULT 0x10B0u /* same pad settings the stock uses */
#define SCAN_HOLD_MS 1500u

#include "led_candidates.inc"

static led_pin_t s_pin; /* gpio 0 = none */
static bool s_on;
static bool s_scan;
static int s_scan_idx;
static uint32_t s_scan_deadline;
static bool s_streaming;

void led_init(void)
{
    memset(&s_pin, 0, sizeof(s_pin));
    s_scan = false;
    s_scan_idx = -1;
    s_on = false;
    s_streaming = false;
}

bool led_get(void)
{
    return s_on;
}

int led_candidate_count(void)
{
    return (int)(sizeof(kCandidates) / sizeof(kCandidates[0]));
}

const char *led_candidate_name(int idx)
{
    static char name[24];
    if (idx < 0 || idx >= led_candidate_count()) {
        return "?";
    }
    const led_pin_t *p = &kCandidates[idx];
    unsigned n = 0;
    name[n++] = 'G';
    name[n++] = 'P';
    name[n++] = 'I';
    name[n++] = 'O';
    name[n++] = (char)('0' + p->gpio);
    name[n++] = '_';
    name[n++] = 'I';
    name[n++] = 'O';
    if (p->pin >= 10) {
        name[n++] = (char)('0' + p->pin / 10);
    }
    name[n++] = (char)('0' + p->pin % 10);
    name[n] = '\0';
    return name;
}

static void drive(bool on)
{
    if (s_pin.gpio == 0) {
        s_on = false;
        return;
    }
    volatile uint32_t *base = GPIO_BASE(s_pin.gpio);
    base[on ? GPIO_DR_SET : GPIO_DR_CLEAR] = 1u << s_pin.pin;
    s_on = on;
}

void led_set(bool on)
{
    drive(on);
}

void led_select(int gpio, int pin)
{
    if (gpio < 1 || gpio > 4 || pin < 0 || pin > 31) {
        s_pin.gpio = 0;
        return;
    }
    s_pin = (led_pin_t){(uint8_t)gpio, (uint8_t)pin, 0, 0, 0};
    GPIO_BASE(gpio)[GPIO_GDIR] |= 1u << pin;
    drive(false);
}

static void configure_candidate(const led_pin_t *p)
{
    *(volatile uint32_t *)p->mux_reg = p->mux_val; /* ALT5 = GPIO */
    *(volatile uint32_t *)p->cfg_reg = PAD_CFG_DEFAULT;
    s_pin = (led_pin_t){p->gpio, p->pin, 0, 0, 0};
    GPIO_BASE(p->gpio)[GPIO_GDIR] |= 1u << p->pin;
    drive(false);
}

void led_scan_start(void)
{
    s_scan = true;
    s_scan_idx = -1;
    s_scan_deadline = 0;
}

void led_scan_stop(void)
{
    s_scan = false;
    drive(false);
}

bool led_scan_active(void)
{
    return s_scan;
}

void led_set_streaming(bool streaming)
{
    s_streaming = streaming;
}

void led_task(void)
{
    uint32_t now = tusb_time_millis_api();

    if (s_scan) {
        if (s_scan_deadline == 0 || now >= s_scan_deadline) {
            if (s_scan_deadline != 0) {
                drive(false); /* end of the previous candidate */
            }
            s_scan_idx++;
            if (s_scan_idx >= led_candidate_count()) {
                log_printf("ledscan done (pin not selected)\r\n");
                led_scan_stop();
                return;
            }
            configure_candidate(&kCandidates[s_scan_idx]);
            log_printf("ledscan %d/%d: %s high\r\n", s_scan_idx + 1,
                       led_candidate_count(), led_candidate_name(s_scan_idx));
            drive(true);
            s_scan_deadline = now + SCAN_HOLD_MS;
        }
        return;
    }

    if (s_pin.gpio == 0) {
        return;
    }
    bool want = led_pattern(s_streaming, now);
    if (want != s_on) {
        drive(want);
    }
}
