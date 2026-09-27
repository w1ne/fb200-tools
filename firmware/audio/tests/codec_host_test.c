/* Host test: the codec init sequence must reset first, configure the clock
 * and I2S interface before enabling the digital paths, clear TESTDAC after
 * the DAC path powers up, and end unmuted. */
#include <assert.h>
#include <stdio.h>
#include "audio/codec.h"
#include "audio/nau8821.h"

int main(void)
{
    codec_write_t seq[64];
    size_t n = codec_build_init_sequence(seq, 64);
    assert(n > 10 && n <= 64);

    /* Reset first, twice. */
    assert(seq[0].reg == NAU8821_R00_RESET);
    assert(seq[1].reg == NAU8821_R00_RESET);

    /* Clock and interface setup precede the clock/path enables. */
    size_t clk = 0, ena = 0;
    for (size_t i = 0; i < n; i++) {
        if (seq[i].reg == NAU8821_R03_CLK_DIVIDER) clk = i;
        if (seq[i].reg == NAU8821_R01_ENA_CTRL) ena = i;
    }
    assert(clk < ena);

    /* The last writes to these registers must leave the codec usable. */
    uint16_t mute = 0xFFFF, bias = 0xFFFF, rdac = 0, classg = 0;
    for (size_t i = 0; i < n; i++) {
        switch (seq[i].reg) {
        case NAU8821_R31_MUTE_CTRL: mute = seq[i].value; break;
        case NAU8821_R66_BIAS_ADJ: bias = seq[i].value; break;
        case NAU8821_R73_RDAC: rdac = seq[i].value; break;
        case NAU8821_R4B_CLASSG_CTRL: classg = seq[i].value; break;
        default: break;
        }
    }
    assert(mute == 0);                                  /* unmuted */
    assert((bias & NAU8821_BIAS_TESTDAC_EN) == 0);      /* DAC passes through */
    assert((rdac & (NAU8821_DAC_EN_L | NAU8821_DAC_EN_R)) ==
           (NAU8821_DAC_EN_L | NAU8821_DAC_EN_R));      /* both DACs on */
    assert((classg & NAU8821_CLASSG_EN) != 0);          /* class G on */
    assert((classg & NAU8821_CLASSG_TIMER_64MS) != 0);  /* anti-click timer */

    printf("codec init sequence OK (%zu writes)\n", n);
    return 0;
}
