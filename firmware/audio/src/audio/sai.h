/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_SAI_H
#define FB200_SAI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAI_BLOCK_FRAMES 32

void sai_audio_init(void);

/* DAC latency bound for sai_push_block. At start-up the RX ring overflows
 * before the engine runs; draining that backlog filled the TX ring (10.6 ms)
 * and, with equal rates, it stayed full. Above the target the engine skips
 * a block. */
#define SAI_TX_TARGET_FILL (3u * SAI_BLOCK_FRAMES)

/* Engine side (stereo interleaved int16), whole SAI_BLOCK_FRAMES blocks only
 * (audio/sai_ring.h). */
bool sai_pull_block(int16_t *dst);   /* codec ADC -> engine; false: no whole block yet */
/* engine -> codec DAC; false: skipped (more than max_fill frames queued) */
bool sai_push_block(const int16_t *src, uint32_t max_fill);

uint32_t sai_rx_fill(void);   /* frames the engine has not pulled yet */
uint32_t sai_tx_fill(void);   /* frames queued for the DAC (= output latency) */
void sai_stats(uint32_t *rx_fill, uint32_t *tx_fill, uint32_t *rx_blocks,
               uint32_t *tx_blocks, uint32_t *over, uint32_t *under);

#endif
