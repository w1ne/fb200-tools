/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_PADS_H
#define FB200_PADS_H
#include <stdint.h>
/* IOMUXC pads by index (mux reg 0x401F8014 + 4 i, pad reg 0x401F8204 + 4 i):
 * EMC_n = n, AD_B0_n = 42 + n, AD_B1_n = 58 + n, B0_n = 74 + n,
 * B1_n = 90 + n, SD_B0_n = 106 + n. See docs/UI_AND_STORAGE.md. */
#define PAD_EMC(n)   (n)
#define PAD_AD_B0(n) (42u + (n))
#define PAD_B0(n)    (74u + (n))
#define PAD_B1(n)    (90u + (n))
#define PAD_SD_B0(n) (106u + (n))
void pad_set(uint32_t pad, uint32_t mux_mode, uint32_t pad_cfg);
#endif
