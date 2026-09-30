/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Host test: the codec init sequence is the stock firmware's (76 writes),
 * adapted to 16-bit I2S at 48 kHz. Pins the registers that made the input
 * read zero when they were wrong. */
#include <assert.h>
#include <stdio.h>
#include "audio/codec.h"
#include "audio/nau8821.h"

static int last(const codec_write_t *seq, size_t n, uint16_t reg)
{
    int v = -1;
    for (size_t i = 0; i < n; i++)
        if (seq[i].reg == reg) v = seq[i].value;
    return v;
}

int main(void)
{
    codec_write_t seq[96];
    size_t n = codec_build_init_sequence(seq, 96);
    assert(n == 76);
    assert(seq[0].reg == NAU8821_R00_RESET);           /* reset first */

    assert(last(seq, n, NAU8821_R03_CLK_DIVIDER) == 0x0050);   /* MCLK/2 <= 6.144 MHz */
    assert(last(seq, n, NAU8821_R2B_ADC_RATE) == 0x0002);      /* OSR 128 */
    assert(last(seq, n, NAU8821_R1C_I2S_PCM_CTRL1) ==
           (NAU8821_I2S_DL_16 | NAU8821_I2S_DF_I2S));          /* 16-bit I2S */
    assert(last(seq, n, NAU8821_R1D_I2S_PCM_CTRL2) == 0x0000); /* ADCOUT driven, slave */
    assert(last(seq, n, NAU8821_R72_ANALOG_ADC_2) == 0x0170);  /* ADC L/R powered */
    assert(last(seq, n, NAU8821_R7E_PGA_GAIN) == 0x0101);      /* PGA 0 dB */
    assert(last(seq, n, NAU8821_R31_MUTE_CTRL) == 0x0000);     /* unmuted */

    int rdac = last(seq, n, NAU8821_R73_RDAC), classg = last(seq, n, NAU8821_R4B_CLASSG_CTRL);
    assert((rdac & (NAU8821_DAC_EN_L | NAU8821_DAC_EN_R)) == (NAU8821_DAC_EN_L | NAU8821_DAC_EN_R));
    assert((classg & NAU8821_CLASSG_EN) && (classg & NAU8821_CLASSG_TIMER_64MS));
    assert((last(seq, n, NAU8821_R66_BIAS_ADJ) & NAU8821_BIAS_TESTDAC_EN) == 0);

    printf("codec init sequence OK (%zu writes)\n", n);
    return 0;
}
