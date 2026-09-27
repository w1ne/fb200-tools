#ifndef FB200_BT_AUDIO_H
#define FB200_BT_AUDIO_H
#include <stddef.h>
#include <stdint.h>
/* Bluetooth audio in: the module sends I2S to SAI3 (pads GPIO_EMC_33 RX data,
 * EMC_38 BCLK, EMC_39 frame sync; no MCLK). Like the stock the MCU is the
 * I2S master at 44.1 kHz, 2 x 32-bit slots, RX synchronous to TX
 * (docs/AUDIO_PATH.md). The stock mixes it as BT * master * 1.3 into the
 * output and adds it to the USB capture. */
void bt_audio_init(void);
size_t bt_audio_pull(float *l, float *r, size_t frames);   /* 0 when nothing arrived */
void bt_audio_stats(uint32_t *blocks, uint32_t *fill, int32_t *peak);
#endif
