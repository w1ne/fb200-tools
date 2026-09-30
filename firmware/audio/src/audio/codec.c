/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* NAU88L21 codec driver: the stock init sequence, register access and
 * volume/mute. The signal path is codec ADC -> SAI1 RX and SAI1 TX -> codec
 * DAC; the codec is the I2S clock slave (SAI1 drives MCLK/BCLK/FS). */
#include <string.h>
#include "audio/codec.h"
#include "audio/nau8821.h"

#ifndef CODEC_HOST_TEST
#include "fsl_clock.h"
#include "fsl_common_arm.h"
#include "fsl_lpi2c.h"
#endif

/* The stock firmware's codec init, replayed as data: 76 writes in order,
 * no delays (recovered from the stock image: codec_init at ITCM 0x19f34,
 * table at DTCM 0x20007834; see docs/AUDIO_PATH.md). Only R1C differs: the
 * stock runs 32-bit I2S words, our SAI 16-bit. Same rate as the stock:
 * R03=0050 -> ADC/DAC clock MCLK/2 = 5.6448 MHz, R2B/R2C -> OSR 128.
 *
 * Input path (NAU88L21 datasheet / Linux nau8821): R1D=0000 drives ADCOUT
 * (the default 8010 tri-states it), R6B=0000 connects the mic inputs,
 * R72=0170 powers ADC L/R with VREF=VMID, R74=0502 MICBIAS (guitar front
 * end), R7E=0101 PGA 0 dB. Our first hand-built sequence got R03, R1D, R72
 * and R7E wrong and the input read exactly zero. */
static const codec_write_t kInitSequence[] = {
    {0x00, 0x0000, 0},
    {0x01, 0x0FFF, 0},
    {0x03, 0x0050, 0},
    {0x04, 0x0000, 0},
    {0x05, 0x00BC, 0},
    {0x06, 0x0000, 0},
    {0x07, 0x4000, 0},
    {0x08, 0x1000, 0},
    {0x09, 0x0600, 0},
    {0x0A, 0x0031, 0},
    {0x0B, 0x26E9, 0},
    {0x0D, 0xC000, 0},
    {0x0F, 0x0000, 0},
    {0x11, 0x0000, 0},
    {0x12, 0x8053, 0},
    {0x13, 0x0000, 0},
    {0x1A, 0x0000, 0},
    {0x1B, 0x0000, 0},
    {0x1C, 0x0002, 0}, /* stock 000E (32-bit); ours: I2S, 16-bit */
    {0x1D, 0x0000, 0},
    {0x1E, 0x2000, 0},
    {0x1F, 0x0000, 0},
    {0x21, 0x0000, 0},
    {0x22, 0x0000, 0},
    {0x23, 0x0000, 0},
    {0x24, 0x0000, 0},
    {0x25, 0x0000, 0},
    {0x26, 0x0000, 0},
    {0x27, 0x0000, 0},
    {0x28, 0x0000, 0},
    {0x29, 0x0000, 0},
    {0x2A, 0x0000, 0},
    {0x2B, 0x0002, 0},
    {0x2C, 0x0082, 0},
    {0x2D, 0x0000, 0},
    {0x2F, 0x0000, 0},
    {0x30, 0x0000, 0},
    {0x31, 0x0000, 0},
    {0x32, 0x0000, 0},
    {0x34, 0xCFCF, 0},
    {0x35, 0xCFCF, 0},
    {0x36, 0x1486, 0},
    {0x37, 0x0F12, 0},
    {0x38, 0x25FF, 0},
    {0x39, 0x3457, 0},
    {0x3A, 0x1486, 0},
    {0x3B, 0x0F12, 0},
    {0x3C, 0x25F9, 0},
    {0x3D, 0x3457, 0},
    {0x41, 0x0000, 0},
    {0x42, 0x0000, 0},
    {0x43, 0x0000, 0},
    {0x44, 0x0000, 0},
    {0x45, 0x0000, 0},
    {0x46, 0x0000, 0},
    {0x47, 0x0000, 0},
    {0x48, 0x0000, 0},
    {0x49, 0x0000, 0},
    {0x4A, 0x0000, 0},
    {0x4B, 0x2007, 0},
    {0x4C, 0x0000, 0},
    {0x55, 0x0000, 0},
    {0x66, 0x0060, 0},
    {0x68, 0x0000, 0},
    {0x69, 0x0000, 0},
    {0x6A, 0x1003, 0},
    {0x6B, 0x0000, 0},
    {0x71, 0x0011, 0},
    {0x72, 0x0170, 0},
    {0x73, 0x3308, 0},
    {0x74, 0x0502, 0},
    {0x76, 0x3140, 0},
    {0x77, 0x0000, 0},
    {0x7E, 0x0101, 0},
    {0x7F, 0xC03F, 0},
    {0x80, 0x0720, 0},
};

size_t codec_build_init_sequence(codec_write_t *seq, size_t max)
{
    size_t n = sizeof(kInitSequence) / sizeof(kInitSequence[0]);
    if (n > max) {
        n = max;
    }
    memcpy(seq, kInitSequence, n * sizeof(kInitSequence[0]));
    return n;
}

#ifndef CODEC_HOST_TEST

static bool codec_xfer(uint16_t reg, uint8_t *data, size_t n, bool read)
{
    lpi2c_master_transfer_t t = {
        .slaveAddress = NAU8821_I2C_ADDR,
        .direction = kLPI2C_Write,
        .subaddress = reg,
        .subaddressSize = 2,
        .data = data,
        .dataSize = n,
    };
    if (read) {
        t.direction = kLPI2C_Read;
    }
    return LPI2C_MasterTransferBlocking(LPI2C1, &t) == kStatus_Success;
}

bool codec_write(uint16_t reg, uint16_t value)
{
    uint8_t d[2] = {(uint8_t)(value >> 8), (uint8_t)value};
    return codec_xfer(reg, d, 2, false);
}

bool codec_read(uint16_t reg, uint16_t *value)
{
    uint8_t d[2] = {0, 0};
    if (!codec_xfer(reg, d, 2, true)) {
        return false;
    }
    *value = ((uint16_t)d[0] << 8) | d[1];
    return true;
}

static uint32_t s_init_retries;
static uint16_t s_init_first_retry_reg = 0xFFFFu;

/* Each write is retried: at boot the first attempt of some writes NAKs (the
 * codec was still settling), and one NAK used to fail the whole init and
 * leave the pedal muted with an unconfigured codec. */
bool codec_init(void)
{
    const codec_write_t *seq = kInitSequence;
    size_t n = sizeof(kInitSequence) / sizeof(kInitSequence[0]);
    bool ok = true;
    uint32_t cpu = CLOCK_GetFreq(kCLOCK_CpuClk);
    s_init_retries = 0;
    s_init_first_retry_reg = 0xFFFFu;
    for (size_t i = 0; i < n; i++) {
        bool done = false;
        for (int attempt = 0; attempt < 3 && !done; attempt++) {
            if (attempt != 0) {
                s_init_retries++;
                if (s_init_first_retry_reg == 0xFFFFu) s_init_first_retry_reg = seq[i].reg;
                SDK_DelayAtLeastUs(1000u, cpu);
            }
            done = codec_write(seq[i].reg, seq[i].value);
        }
        if (!done) {
            ok = false;
        }
        if (seq[i].delay_ms != 0) {
            SDK_DelayAtLeastUs((uint32_t)seq[i].delay_ms * 1000u, cpu);
        }
    }
    return ok;
}

void codec_init_stats(uint32_t *retries, uint16_t *first_retry_reg)
{
    *retries = s_init_retries;
    *first_retry_reg = s_init_first_retry_reg;
}

bool codec_probe(void)
{
    uint16_t id = 0;
    return codec_read(NAU8821_R58_I2C_DEVICE_ID, &id);
}

void codec_set_dac_volume(uint8_t left, uint8_t right)
{
    codec_write(NAU8821_R34_DACR_CTRL, ((uint16_t)right << 8) | left);
}

void codec_set_adc_volume(uint8_t left, uint8_t right)
{
    codec_write(NAU8821_R35_ADC_DGAIN_CTRL1, ((uint16_t)right << 8) | left);
}

void codec_mute(bool mute)
{
    uint16_t v = mute ? (NAU8821_DAC_SOFT_MUTE | NAU8821_ADC_SOFT_MUTE) : 0u;
    codec_write(NAU8821_R31_MUTE_CTRL, v);
}

#endif /* CODEC_HOST_TEST */
