#ifndef FB200_CODEC_H
#define FB200_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t reg;
    uint16_t value;
    uint8_t delay_ms; /* settle time after this write */
} codec_write_t;

/* Pure sequence builder (host-testable, no hardware access). */
size_t codec_build_init_sequence(codec_write_t *seq, size_t max);

/* Hardware access (LPI2C1 must be initialized, see i2c_probe_init). */
bool codec_write(uint16_t reg, uint16_t value);
bool codec_read(uint16_t reg, uint16_t *value);
bool codec_init(void); /* apply the init sequence; true if all writes ACK */
/* Retries the last codec_init needed, and the first register that needed one
 * (0xFFFF = none). */
void codec_init_stats(uint32_t *retries, uint16_t *first_retry_reg);
bool codec_probe(void); /* read the device ID register (0x58) */

/* DAC/ADC digital volume fields: 0x00 = -127.5 dB ... 0xCF = 0 dB. */
void codec_set_dac_volume(uint8_t left, uint8_t right);
void codec_set_adc_volume(uint8_t left, uint8_t right);
void codec_mute(bool mute);

#endif
