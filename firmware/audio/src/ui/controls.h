#ifndef FB200_CONTROLS_H
#define FB200_CONTROLS_H
#include <stdbool.h>
#include <stdint.h>
/* Footswitches A-D (active low) and 16 knobs (two 74HC4051 into ADC1),
 * docs/UI_AND_STORAGE.md §2. Values are 0..4095, 0 = fully counter-clockwise
 * (the stock inverts the raw reading the same way). */
#define FSW_COUNT 4
#define KNOB_COUNT 16
typedef enum { FSW_NONE, FSW_PRESS, FSW_RELEASE, FSW_LONG } fsw_event_t;

void controls_init(void);
void controls_task(uint32_t now_ms);
bool fsw_down(int sw);                      /* 0..3 = A..D */
fsw_event_t fsw_event(int *sw);             /* next queued event, FSW_NONE if none */
uint16_t knob_value(int k);
bool knob_changed(int k);                   /* moved since the last call */
uint16_t adc1_read(uint32_t ch);            /* raw 12-bit ADC1 conversion */
#endif
