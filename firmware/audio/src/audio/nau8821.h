/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* NAU88L21 (Linux name: nau8821) register map - the subset the driver uses.
 * 16-bit register addresses, 16-bit data words, big-endian on the wire.
 * Facts from the Nuvoton NAU88L21 datasheet (Rev 3.3, section 10) and the
 * register/bit definitions in the mainline Linux nau8821 driver. */
#pragma once

#define NAU8821_I2C_ADDR 0x54u /* CSB strapped high */

#define NAU8821_R00_RESET            0x00
#define NAU8821_R01_ENA_CTRL         0x01
#define NAU8821_R03_CLK_DIVIDER      0x03
#define NAU8821_R0D_JACK_DET_CTRL    0x0D
#define NAU8821_R1C_I2S_PCM_CTRL1    0x1C
#define NAU8821_R1D_I2S_PCM_CTRL2    0x1D
#define NAU8821_R1E_LEFT_TIME_SLOT   0x1E
#define NAU8821_R2B_ADC_RATE         0x2B
#define NAU8821_R2C_DAC_CTRL1        0x2C
#define NAU8821_R31_MUTE_CTRL        0x31
#define NAU8821_R32_HSVOL_CTRL       0x32
#define NAU8821_R34_DACR_CTRL        0x34
#define NAU8821_R35_ADC_DGAIN_CTRL1  0x35
#define NAU8821_R4B_CLASSG_CTRL      0x4B
#define NAU8821_R58_I2C_DEVICE_ID    0x58
#define NAU8821_R66_BIAS_ADJ         0x66
#define NAU8821_R6A_ANALOG_CONTROL_2 0x6A
#define NAU8821_R71_ANALOG_ADC_1     0x71
#define NAU8821_R72_ANALOG_ADC_2     0x72
#define NAU8821_R73_RDAC             0x73
#define NAU8821_R74_MIC_BIAS         0x74
#define NAU8821_R76_BOOST            0x76
#define NAU8821_R7E_PGA_GAIN         0x7E
#define NAU8821_R7F_POWER_UP_CONTROL 0x7F
#define NAU8821_R80_CHARGE_PUMP      0x80

/* ENA_CTRL (0x01) */
#define NAU8821_EN_DRC_CLK (1u << 0)
#define NAU8821_EN_I2S_CLK (1u << 4)
#define NAU8821_EN_DAC_CLK (1u << 6)
#define NAU8821_EN_ADC_CLK (1u << 7)
#define NAU8821_EN_ADCL    (1u << 8)
#define NAU8821_EN_ADCR    (1u << 9)
#define NAU8821_EN_DACL    (1u << 10)
#define NAU8821_EN_DACR    (1u << 11)

/* CLK_DIVIDER (0x03): all dividers off, everything from MCLK. */
#define NAU8821_CLK_DIV_MCLK_DIRECT 0x0000u

/* I2S_PCM_CTRL1 (0x1C) */
#define NAU8821_I2S_DL_16 (0x0u << 2)
#define NAU8821_I2S_DF_I2S 0x2u

/* I2S_PCM_CTRL2 (0x1D): codec is clock slave (SAI is master). */
#define NAU8821_I2S_SLAVE 0x0000u

/* LEFT_TIME_SLOT (0x1E) */
#define NAU8821_DIS_FS_SHORT_DET (1u << 13)

/* ADC_RATE (0x2B) */
#define NAU8821_ADC_SYNC_DOWN_64 0x1u

/* DAC_CTRL1 (0x2C) */
#define NAU8821_DAC_OVERSAMPLE_64 0x0u

/* MUTE_CTRL (0x31): set = muted. */
#define NAU8821_DAC_SOFT_MUTE (1u << 9)
#define NAU8821_ADC_SOFT_MUTE (1u << 1)

/* BIAS_ADJ (0x66) */
#define NAU8821_BIAS_VMID     (1u << 6)
#define NAU8821_BIAS_TESTDAC_EN (0x3u << 8)
#define NAU8821_BIAS_VMID_SEL_2 (0x2u << 4)

/* ANALOG_CONTROL_2 (0x6A) */
#define NAU8821_HP_NON_CLASSG_CURRENT_2xADJ (1u << 12)
#define NAU8821_DAC_CAPACITOR_MSB (1u << 1)
#define NAU8821_DAC_CAPACITOR_LSB (1u << 0)

/* ANALOG_ADC_2 (0x72) */
#define NAU8821_POWERUP_ADCL (1u << 6)
#define NAU8821_POWERUP_ADCR (1u << 4)

/* RDAC (0x73) */
#define NAU8821_DAC_EN_L          (1u << 12)
#define NAU8821_DAC_EN_R          (1u << 13)
#define NAU8821_DAC_CLK_EN_L      (1u << 8)
#define NAU8821_DAC_CLK_EN_R      (1u << 9)
#define NAU8821_DAC_CLK_DELAY_2NS (0x2u << 4)
#define NAU8821_DAC_VREF_1V7      (0x3u << 2)

/* BOOST (0x76) */
#define NAU8821_GLOBAL_BIAS_EN   (1u << 12)
#define NAU8821_PRECHARGE_DIS    (1u << 13)
#define NAU8821_HP_BOOST_DIS     (1u << 9)
#define NAU8821_HP_BOOST_G_DIS   (1u << 8)
#define NAU8821_SHORT_SHUTDOWN_EN (1u << 6)

/* POWER_UP_CONTROL (0x7F) */
#define NAU8821_PUP_MAIN_DRV_L   (1u << 0)
#define NAU8821_PUP_MAIN_DRV_R   (1u << 1)
#define NAU8821_PUP_DRV_INSTG_L  (1u << 2)
#define NAU8821_PUP_DRV_INSTG_R  (1u << 3)
#define NAU8821_PUP_INTEG_L      (1u << 4)
#define NAU8821_PUP_INTEG_R      (1u << 5)
#define NAU8821_PUP_PGA_L        (1u << 15)
#define NAU8821_PUP_PGA_R        (1u << 14)

/* CHARGE_PUMP (0x80) */
#define NAU8821_JAMNODCLOW        (1u << 10)
#define NAU8821_POWER_DOWN_DACL   (1u << 8)
#define NAU8821_POWER_DOWN_DACR   (1u << 9)
#define NAU8821_CHARGE_PUMP_EN    (1u << 5)

/* CLASSG_CTRL (0x4B) */
#define NAU8821_CLASSG_EN      (1u << 0)
#define NAU8821_CLASSG_LDAC_EN (1u << 1)
#define NAU8821_CLASSG_RDAC_EN (1u << 2)
#define NAU8821_CLASSG_TIMER_64MS (0x20u << 8)
