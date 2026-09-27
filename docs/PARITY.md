# Feature parity with the stock firmware

Goal: everything the stock FB200 does, done better. This is the working gap
list; evidence for the stock side is in `FIRMWARE_ANALYSIS.md`,
`HARDWARE.md`, `PROTOCOL.md` and `AUDIO_PATH.md`.

## Stock feature inventory (what we know)

| # | Feature | Evidence |
| --- | --- | --- |
| 1 | Amp/cab model library: 20 entries (10 amps + 10 cabs), float32 tables at flash `0x60041000`; one-second entries = cab IRs at 44.1 kHz | `FIRMWARE_ANALYSIS.md` §2 |
| 2 | 40 presets (10 banks x 4) of 256 B at F:0x71000 + i x 0x200, global settings at F:0x80000, 9 user IR slots; stock data is still on the pedal (verified) | `UI_AND_STORAGE.md` §5 |
| 3 | USB audio, UAC1, 44.1 kHz, playback + capture | USB descriptors in the stock image |
| 4 | Bluetooth module over UART (AT command set, `AT+BDFB200 Audio`, module `V1.0.0`) | `FIRMWARE_ANALYSIS.md` §1.3, `HARDWARE.md` §4 |
| 5 | 3-digit 14-segment LED display, 4 footswitches (active low), 16 knobs (2x 74HC4051 -> ADC1), 16 knob LEDs, 40 RGB LEDs (WS2812 on FlexIO2), status LED | `UI_AND_STORAGE.md` §1-3 (no LCD) |
| 6 | Firmware update: USB DFU `0483:5703` + `.mr` container | `FIRMWARE_BRINGUP.md`, `UPDATE_AND_RECOVERY.md` |
| 7 | Device info protocol over USB (version strings, BT version, hw rev) | `PROTOCOL.md` |
| 8 | Headphone out (class-G) + instrument input (codec PGA) | `AUDIO_PATH.md` |
| 9 | Tuner / looper / drum machine | NOT verified in the image yet (Mooer family tools exist; open question) |
| 10 | MIDI | NOT verified yet |

## Where we are

| Area | Status |
| --- | --- |
| Boot format, vendor image, flashing, recovery | DONE (`FIRMWARE_BRINGUP.md`), USB-only handover pending |
| Console (USB CDC): debug, registers, I2C, memory, FW self-update | DONE, better than stock (stock has no console) |
| USB audio | UAC2, 48 kHz, stereo playback + capture implemented; hardware bring-up pending |
| Codec driver (NAU88L21) | DONE (reset/power-up/volume, host-tested); hardware verification pending |
| SAI1 + eDMA audio path | DONE (ping-pong, drift, meters, faults); hardware bring-up pending |
| LED | scan + status implemented; pin identification pending hardware |
| DSP framework | chain, smoothing, gain, testgen, no-libm math shims; host tests |
| Amp/cab models + effects | MISSING (the big one) |
| Presets (storage + UI) | MISSING |
| Display UI | MISSING; hardware fully mapped (`UI_AND_STORAGE.md`) |
| Bluetooth (AT + audio) | MISSING (needs UART RE + BT audio path RE) |
| Tuner / looper / drum machine | MISSING (verify stock first) |
| MIDI | MISSING (verify stock first) |
| PC-side tooling | `fb200` CLI + `measure.py`; better than stock |

## Roadmap

Research behind the "better" items (sources, budgets): `ROADMAP_RESEARCH.md`.
Budgets on this chip: 600 MHz / 44.1 kHz = 13.6k cycles per sample; RAM
(512 KB FlexRAM, no external RAM) is the tighter limit.

- **M1 - audio bring-up: DONE on hardware.** Guitar/bass in, headphone out,
  USB UAC2 in/out, codec, SAI/eDMA, 44.1 kHz stock clock tree, console,
  recovery + USB self-update, crash dumps.
- **M2 - parity sound (in progress).** Stock chain ported with parity tests
  against a bit-exact emulation of the stock DSP: gate, compressor, amp
  (Wiener-Hammerstein, 10 models) + tone stack, cab (512-tap FIR, 10 +
  user IRs), 11 modulations, 5 reverbs. Stock coefficients are extracted at
  build time from the user's own stock image, never committed.
- **M3 - parity UI: mostly DONE.** Display, footswitches (stock chords),
  knobs with pickup, knob LEDs, presets in the stock format, battery/charger
  monitor. Open: power-fail save + latch, RGB LED ring, tuner, drum machine,
  Bluetooth (AT + app protocol + BT audio on SAI3).
- **M4 - better core (after parity):** 48 kHz / 24-bit engine (needed for
  NAM; stock assets resampled offline), latency <3 ms, CPU/RAM profiler;
  bass chain additions: crossover clean-blend drive, 5-7 band EQ + HPF/LPF,
  delay (the stock has none), better tuner, drum level fix.
- **M5 - IR engine:** up to 4096 taps (partitioned convolution, already in
  `dsp/conv.c`), WAV import, 50+ slots, low/high cut, dual-IR blend.
- **M6 - open ecosystem:** class-compliant USB MIDI, documented protocol,
  WebMIDI editor (self-describing blocks), JSON presets, browser firmware
  update, multichannel UAC2 (dry DI + processed + re-amp).
- **M7 - NAM A2-Lite player** (~50 % CPU on this class of M7; MIT stack:
  NeuralAmpModelerCore, nam-binary-loader, nam-pedal), TONE3000 browsing in
  the editor, A1 -> A2-Lite distillation tool on the host.
- **M8 - bass effects:** mono octaver (poly later), envelope filter/synth,
  multiband compressor; ADPCM looper (~8-16 s in RAM); AIDA-X/RTNeural.

## Why ours is better

- Parity first at the stock 44.1 kHz, then a 48 kHz/24-bit core (M4).
- Open, SOTA DSP: IR convolution and models in the open, no proprietary blob.
- USB-only flashing with a resident recovery (no A+D), live console, crash
  dumps, self-update (`docs/BOOTLOADER.md` §4).
- Everything scriptable and testable: host tests, dry-run gate before every
  flash, measurement suite for objective comparisons.
- Open preset/IR formats and a web editor, no vendor tooling required.
