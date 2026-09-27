/* NAU88L21 codec driver: software reset, clocking, power-up sequence and
 * volume control. The signal path is codec ADC -> SAI RX and SAI TX -> codec
 * DAC; the codec is the I2S clock slave (the SAI drives MCLK/BCLK/FS).
 *
 * Power-up follows the vendor power sequence: bias/VMID first, analog ADC
 * power with a 125 ms settle, then clock/digital enables, DAC path, output
 * stages, charge pump ramp, class-G headphone amp, TESTDAC off, unmute. */
#include <string.h>
#include "audio/codec.h"
#include "audio/nau8821.h"

#ifndef CODEC_HOST_TEST
#include "fsl_clock.h"
#include "fsl_common_arm.h"
#include "fsl_lpi2c.h"
#endif

static const codec_write_t kInitSequence[] = {
    /* Software reset, written twice per the datasheet reset procedure. */
    {NAU8821_R00_RESET, 0xFFFF, 0},
    {NAU8821_R00_RESET, 0xFFFF, 0},
    /* Clocking: MCLK = 256*Fs (12.288 MHz at 48 kHz), no dividers. */
    {NAU8821_R03_CLK_DIVIDER, NAU8821_CLK_DIV_MCLK_DIRECT, 0},
    /* Digital audio interface: I2S format, 16-bit data, clock slave. */
    {NAU8821_R1C_I2S_PCM_CTRL1, NAU8821_I2S_DL_16 | NAU8821_I2S_DF_I2S, 0},
    {NAU8821_R1D_I2S_PCM_CTRL2, NAU8821_I2S_SLAVE, 0},
    /* Analog bias and VMID bring-up. */
    {NAU8821_R66_BIAS_ADJ, NAU8821_BIAS_VMID, 0},
    {NAU8821_R76_BOOST, NAU8821_GLOBAL_BIAS_EN, 0},
    {NAU8821_R66_BIAS_ADJ,
     NAU8821_BIAS_VMID | NAU8821_BIAS_VMID_SEL_2 | NAU8821_BIAS_TESTDAC_EN, 0},
    {NAU8821_R76_BOOST,
     NAU8821_GLOBAL_BIAS_EN | NAU8821_PRECHARGE_DIS |
         NAU8821_HP_BOOST_DIS | NAU8821_HP_BOOST_G_DIS |
         NAU8821_SHORT_SHUTDOWN_EN, 0},
    {NAU8821_R1E_LEFT_TIME_SLOT, NAU8821_DIS_FS_SHORT_DET, 0},
    {NAU8821_R4B_CLASSG_CTRL, NAU8821_CLASSG_TIMER_64MS, 0},
    {NAU8821_R6A_ANALOG_CONTROL_2,
     NAU8821_HP_NON_CLASSG_CURRENT_2xADJ | NAU8821_DAC_CAPACITOR_MSB |
         NAU8821_DAC_CAPACITOR_LSB, 0},
    {NAU8821_R73_RDAC, NAU8821_DAC_CLK_DELAY_2NS | NAU8821_DAC_VREF_1V7, 0},
    /* 64x oversampling both ways; the reset defaults are hissy. */
    {NAU8821_R2B_ADC_RATE, NAU8821_ADC_SYNC_DOWN_64, 0},
    {NAU8821_R2C_DAC_CTRL1, NAU8821_DAC_OVERSAMPLE_64, 0},
    /* Analog ADC power, then let it settle before the digital path. */
    {NAU8821_R72_ANALOG_ADC_2, NAU8821_POWERUP_ADCL | NAU8821_POWERUP_ADCR, 0},
    {NAU8821_R72_ANALOG_ADC_2, NAU8821_POWERUP_ADCL | NAU8821_POWERUP_ADCR,
     125},
    /* Clock and digital path enables. */
    {NAU8821_R01_ENA_CTRL,
     NAU8821_EN_I2S_CLK | NAU8821_EN_DAC_CLK | NAU8821_EN_ADC_CLK |
         NAU8821_EN_ADCL | NAU8821_EN_ADCR | NAU8821_EN_DACL |
         NAU8821_EN_DACR, 0},
    /* DAC enables and clocks, output stages, charge pump, class G. */
    {NAU8821_R73_RDAC,
     NAU8821_DAC_EN_L | NAU8821_DAC_EN_R | NAU8821_DAC_CLK_EN_L |
         NAU8821_DAC_CLK_EN_R | NAU8821_DAC_CLK_DELAY_2NS |
         NAU8821_DAC_VREF_1V7, 0},
    {NAU8821_R7F_POWER_UP_CONTROL,
     NAU8821_PUP_PGA_L | NAU8821_PUP_PGA_R | NAU8821_PUP_MAIN_DRV_L |
         NAU8821_PUP_MAIN_DRV_R | NAU8821_PUP_DRV_INSTG_L |
         NAU8821_PUP_DRV_INSTG_R | NAU8821_PUP_INTEG_L | NAU8821_PUP_INTEG_R,
     0},
    {NAU8821_R80_CHARGE_PUMP, NAU8821_CHARGE_PUMP_EN, 0},
    {NAU8821_R80_CHARGE_PUMP, NAU8821_CHARGE_PUMP_EN | NAU8821_JAMNODCLOW,
     20},
    {NAU8821_R4B_CLASSG_CTRL,
     NAU8821_CLASSG_TIMER_64MS | NAU8821_CLASSG_EN | NAU8821_CLASSG_LDAC_EN |
         NAU8821_CLASSG_RDAC_EN, 0},
    /* TESTDAC off so the DAC signal passes through; unmute. */
    {NAU8821_R66_BIAS_ADJ, NAU8821_BIAS_VMID | NAU8821_BIAS_VMID_SEL_2, 0},
    {NAU8821_R31_MUTE_CTRL, 0x0000, 0},
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
    codec_write_t seq[64];
    size_t n = codec_build_init_sequence(seq, 64);
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
