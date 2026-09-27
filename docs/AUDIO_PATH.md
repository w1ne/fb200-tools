# FB200 audio path

What we know about the analog/digital audio hardware, and what our firmware
configures. Sources: probing the live device (I2C scan, register reads), the
stock firmware image (static RE, see `FIRMWARE_ANALYSIS.md`), the Nuvoton
NAU88L21 datasheet (Rev 3.3), and the mainline Linux `nau8821` driver.

## Codec

- **Nuvoton NAU88L21** (Linux name `nau8821`), QFN32, ultra-low-power stereo
  codec with class-G headphone amp. Board marking `NAU88BL21`.
- **I2C address 0x54** (CSB strapped high) on **LPI2C1**, pads
  `GPIO_SD_B1_04` (SCL) / `GPIO_SD_B1_05` (SDA), mux ALT2, verified by probing
  the live device.
- Control protocol: **16-bit register address + 16-bit data**, both
  big-endian. (One-byte-address reads return 0x00, which is why the early
  `scan` dumps looked empty.)
- Key registers: `0x00` reset, `0x01` clock/digital-path enables, `0x03`
  clock dividers, `0x1C/0x1D` I2S format + master/slave, `0x2B/0x2C`
  ADC/DAC rates, `0x31` soft mute, `0x34/0x35` DAC/ADC digital volume,
  `0x58` device ID, `0x66/0x76` bias/boost, `0x73` DAC enable + VREF,
  `0x7F` output driver power, `0x80` charge pump, `0x4B` class-G.
- Clocking: the codec wants **256*Fs on MCLK** (12.288 MHz at 48 kHz) and is
  the **I2S clock slave** (SAI drives MCLK/BCLK/FS).
- Digital audio interface: I2S, 16-bit data (DL=16).

## SAI1 + pins

The stock firmware configures SAI1 (44100 Hz) with these pads (recovered from
the literal pool of the stock SAI init at ITCM ~0x1abdc, cross-checked against
`fsl_iomuxc.h`):

| Signal | Pad | Mux | Pad config |
| --- | --- | --- | --- |
| MCLK | `GPIO_SD_B1_03` | ALT3 | 0x10B0 |
| RX_DATA00 (codec ADCOUT) | `GPIO_AD_B1_12` | ALT3 | 0x10B0 |
| TX_DATA00 (codec DACIN) | `GPIO_AD_B1_13` | ALT3 | 0x10B0 |
| TX_BCLK | `GPIO_AD_B1_14` | ALT3 | 0x10B0 |
| TX_SYNC | `GPIO_AD_B1_15` | ALT3 | 0x10B0 |

The codec uses a single BCLK/FS pair for both directions, so our SAI runs TX
asynchronous (it generates the clocks) and RX synchronous with TX. The stock
image also contains a second, separate SAI3 init (pads `GPIO_EMC_33/38/39`,
RX_DATA/TX_BCLK/TX_SYNC) — presumably the instrument-input path; not used by
our firmware.

## Clocks

- **PLL4 (audio PLL)**: 24 MHz * (32 + 768/1000) = **786.432 MHz**.
- **SAI1 clock root**: PLL4 / 8 / 8 = **12.288 MHz** (`CSCMR1` SAI1 mux = 2,
  `CSCDR1` SAI1 pre-div = /8, div = /8).
- SAI: MCLK = root / 1 = 12.288 MHz = 256 * 48 kHz; BCLK = 1.536 MHz
  (16-bit * 2 channels * 48 kHz); FS = 48 kHz.

## Our firmware

- `src/audio/sai.c`: SAI1 + eDMA (ping-pong, 32-frame blocks, 512-frame SPSC
  rings), `sai_pull`/`sai_push` for the engine.
- `src/audio/codec.c`: NAU88L21 reset + power-up sequence + volume; the
  sequence builder is host-tested (`tests/test_codec_init.py`).
- `src/audio/engine.c`: codec ADC -> (DSP chain, later) -> codec DAC, USB
  capture fed from the same stream, host playback monitored into the DAC.
- Console: `codec` (dump key registers + re-init), `creg <reg> [val]`
  (16-bit register access), `usb` and (soon) `sai` stats.
