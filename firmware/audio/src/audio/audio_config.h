/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_AUDIO_CONFIG_H
#define FB200_AUDIO_CONFIG_H
/* One sample rate for the whole audio path (SAI, codec, USB, DSP). The
 * stock runs 44.1 kHz and its amp/cab/tone coefficients are designed for it,
 * so parity runs there too. MCLK = 256 fs from PLL4 = 722.5344 MHz / 64. */
#define AUDIO_FS      44100u
#define AUDIO_MCLK_HZ 11289600u
#endif
