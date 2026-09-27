#ifndef FB200_LED_H
#define FB200_LED_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t gpio, pin;
    uint32_t mux_reg, mux_val, cfg_reg;
} led_pin_t;

void led_init(void);
void led_task(void); /* main loop: heartbeat / streaming indicator / scan */

void led_set(bool on);
void led_set_streaming(bool streaming);
bool led_get(void);
void led_select(int gpio, int pin); /* 0,0 = none */

/* Candidate scan: the stock firmware's output pads, driven one at a time so
 * the user can see which one lights the LED. */
void led_scan_start(void);
void led_scan_stop(void);
bool led_scan_active(void);
int led_candidate_count(void);
const char *led_candidate_name(int idx);

/* Pure: heartbeat/streaming pattern (host-tested). */
#define LED_HEARTBEAT_MS 500u
static inline bool led_pattern(bool streaming, uint32_t ms)
{
    if (streaming) {
        return true;
    }
    return ((ms / LED_HEARTBEAT_MS) & 1u) == 0u; /* 1 Hz, 50% duty */
}

#endif
