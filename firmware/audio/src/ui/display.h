#ifndef FB200_DISPLAY_H
#define FB200_DISPLAY_H
#include <stdbool.h>
#include <stdint.h>
/* 3-digit 14-segment LED display, multiplexed from the main loop
 * (docs/UI_AND_STORAGE.md §1). Characters: 0-9, A b C d E F G I L O P R -,
 * space; a '.' after a character lights its decimal point. */
void display_init(void);
void display_text(const char *s);
void display_task(uint32_t now_ms);   /* next digit every 3 ms */
/* Knob LEDs share GPIO4 (IO0..15, active low). */
void knob_led(int led, bool on);
#endif
