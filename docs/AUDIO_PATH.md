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
asynchronous (it generates the clocks) and RX synchronous with TX.

**MCLK only leaves the chip with `IOMUXC_GPR1[19]` (SAI1_MCLK_DIR) set.** The
stock sets it (ITCM 0x1ac6c); our first firmware did not, so the codec had no
MCLK: the ADC read exactly zero and the DAC was silent. Check: `peek32
0x400AC004` must show bit 19 (`0x80080000`).

**The guitar input is the codec ADC on SAI1 RX**, not SAI3. The stock SAI3
(pads `GPIO_EMC_33` RX_DATA, `EMC_38` TX_BCLK, `EMC_39` TX_SYNC; no MCLK, no
TX data; 44.1 kHz, 2 x 32-bit I2S, RX sync to TX, interrupt driven) is only
mixed into the output as an aux stream: most likely the Bluetooth module's
audio (the module is controlled over LPUART5, `GPIO_B1_12/13`, 115200, AT
commands). `GPIO_EMC_32/35` are driven high at boot (module enable/reset?).

Stock SAI1 details (for reference; we run 48 kHz/16-bit): 44.1 kHz from
PLL4 = 722.5344 MHz / 64 = 11.2896 MHz MCLK, 32-bit I2S words, BCLK = 64 fs,
FIFO watermark 16, interrupt driven (no DMA), 8 frames per IRQ.

## Board GPIOs around the audio path

Replayed from the stock (`src/audio/frontend.c`): at boot `GPIO_B1_15` high,
`B1_10` low, `B1_09` and `B1_11` high; after the codec and SAI run, `B1_15`
-> 0 and `B1_10` -> 1. Function not known yet (likely output mute release
and input front-end enable).

## Clocks

- **PLL4 (audio PLL)**: 24 MHz * (32 + 768/1000) = **786.432 MHz**.
- **SAI1 clock root**: PLL4 / 8 / 8 = **12.288 MHz** (`CSCMR1` SAI1 mux = 2,
  `CSCDR1` SAI1 pre-div = /8, div = /8).
- SAI: MCLK = root / 1 = 12.288 MHz = 256 * 48 kHz; BCLK = 1.536 MHz
  (16-bit * 2 channels * 48 kHz); FS = 48 kHz.

## Our firmware

- `src/audio/sai.c`: SAI1 + eDMA (ping-pong, 32-frame blocks, 512-frame SPSC
  rings), `sai_pull`/`sai_push` for the engine.
- `src/audio/codec.c`: the **stock init table replayed as data** (76
  writes, from `codec_init` at ITCM 0x19f34; only R1C differs: 16-bit I2S).
  Key input registers: R03=0050 (ADC/DAC clock MCLK/2 = 6.144 MHz; 0 would
  be 2x the datasheet max), R1D=0000 (ADCOUT driven, not tri-stated),
  R6B=0000, R72=0170 (ADC L/R power, VREF=VMID), R74=0502 (MICBIAS),
  R7E=0101 (PGA 0 dB). Host-tested (`tests/test_codec_init.py`). The stock
  never touches the codec after init.
- `src/audio/engine.c`: codec ADC -> DSP chain -> codec DAC, USB
  capture fed from the same stream, host playback monitored into the DAC.
  See "USB playback routing" below.
- Console: `codec` (dump key registers + re-init), `creg <reg> [val]`
  (16-bit register access), `usb` and (soon) `sai` stats.

## USB playback routing (reamping)

Console `usb [out|in|mix]` (`usb` alone prints `route=` with the stats). The
engine pulls the host playback once per block, drift-compensated as before
(`usb_audio_trim` / `usb_audio_pull16`), before the chain input:

| Route | Chain input (the stock L + R of the ADC) | DAC | USB capture |
| --- | --- | --- | --- |
| `out` (default, stock) | instrument | chain + playback | chain |
| `in` (reamping) | playback, (L + R) / 2 | chain | chain |
| `mix` | instrument + playback (L + R) / 2 | chain | chain |

`in` replaces the instrument: a reamp wants only the DI track in the chain,
not the idle instrument noise. The playback mono mean (L + R) / 2 keeps a mono
file played on both channels at its own level. When the host does not stream,
the engine passes zeros: `in` gives silence, `mix` the instrument. `tin`
(test generator into the chain) overrides both. The tuner hears the chain
input, so also the playback. The route is not saved (reboot: `out`).

Latency, USB playback -> USB capture, inside the pedal:

- Playback ring (`usb_audio.c`, drained by the engine): the engine starts
  pulling at once when the host opens the stream, so the ring runs near its
  minimum, about one USB frame of audio + one engine block (44-45 + 32
  frames, 1-2 ms at 44.1 kHz). With a host clock faster than the codec clock
  the fill grows until the trim holds it at `MAX_RING_FILL` = 480 frames
  (10.9 ms). `usb` shows the current `play_fill`.
- Engine: 0. The pull, the chain and the capture push run in the same
  32-frame block; the effects buffer nothing (the cab convolver included):
  only filter group delay, e.g. the amp's 3x oversampling interpolation
  (about one sample).
- Capture: the capture ring is drained on every main-loop pass (up to 64
  frames per pass) into the TinyUSB FIFO: about one USB frame (~1 ms).

So the pedal adds roughly 2-3 ms, at most ~12 ms, and it changes when the
drift compensation inserts or drops a frame. The host (CoreAudio / driver
buffers) adds more. Do not align with a constant: `audio_test` (MCP) returns
`delay_ms` from the cross-correlation of the played and captured signal
(`fb200.audio.delay_frames`).
