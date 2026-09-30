/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_RGB_H
#define FB200_RGB_H
#include <stdbool.h>
#include <stdint.h>
/* 40 WS2812-type RGB LEDs on GPIO_B0_02 = FLEXIO2_D02, driven like the stock:
 * SDK FlexIO UART at ~6.67 Mbaud fed by eDMA, one UART character per
 * WS2812 bit (docs/UI_AND_STORAGE.md §3). */
#define RGB_COUNT 40
void rgb_init(void);
void rgb_set(int i, uint8_t r, uint8_t g, uint8_t b);
void rgb_fill(uint8_t r, uint8_t g, uint8_t b);
bool rgb_show(void);                        /* start a DMA frame; false while one is busy */
/* bring-up knobs: output inversion and the two symbols for a 0 and a 1 bit */
void rgb_config(bool invert, uint8_t sym0, uint8_t sym1);
#endif
