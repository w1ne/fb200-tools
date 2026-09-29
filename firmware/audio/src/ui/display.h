#ifndef FB200_DISPLAY_H
#define FB200_DISPLAY_H
#include <stdbool.h>
#include <stdint.h>
/* 3-digit 14-segment LED display, multiplexed from the main loop
 * (docs/UI_AND_STORAGE.md §1). Characters: 0-9, A b C d E F G H I L n o O P
 * r R S t U u Y -,
 * space; a '.' after a character lights its decimal point. */
void display_init(void);
void display_text(const char *s);
void display_task(uint32_t now_ms);   /* every ms; next digit every 3 ms */
/* Knob LEDs share GPIO4 (IO0..15, active low); display_task drives them. */
void knob_led(int led, bool on);
/* Brightness of the display and the knob LEDs: 100, 66 or 33 % (lit 3, 2
 * or 1 ms of each 3 ms digit slot). */
void display_set_level(unsigned pct);
/* Show `text` instead of the normal text, lit on_ms of every period_ms,
 * knob LEDs off (standby, low battery); NULL = back to normal. */
void display_override(const char *text, uint16_t on_ms, uint16_t period_ms);
#endif
