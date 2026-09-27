#ifndef FB200_SAI_H
#define FB200_SAI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAI_BLOCK_FRAMES 32

void sai_audio_init(void);

/* Engine side (stereo interleaved int16). */
size_t sai_pull(int16_t *dst, size_t frames);       /* codec ADC -> engine */
size_t sai_push(const int16_t *src, size_t frames); /* engine -> codec DAC */

void sai_stats(uint32_t *rx_fill, uint32_t *tx_fill, uint32_t *rx_blocks,
               uint32_t *tx_blocks, uint32_t *over, uint32_t *under);

#endif
