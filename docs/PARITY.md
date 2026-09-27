# Feature parity with the stock firmware

Goal: everything the stock FB200 does, done better. This is the working gap
list; evidence for the stock side is in `FIRMWARE_ANALYSIS.md`,
`HARDWARE.md`, `PROTOCOL.md` and `AUDIO_PATH.md`.

## Stock feature inventory (what we know)

| # | Feature | Evidence |
| --- | --- | --- |
| 1 | Amp/cab model library: 20 entries (10 amps + 10 cabs), float32 tables at flash `0x60041000`; one-second entries = cab IRs at 44.1 kHz | `FIRMWARE_ANALYSIS.md` §2 |
| 2 | Presets with names (`Fat BassX`, `Tri Chorus`, ...), stored in flash | `FIRMWARE_ANALYSIS.md` §1.4 |
| 3 | USB audio, UAC1, 44.1 kHz, playback + capture | USB descriptors in the stock image |
| 4 | Bluetooth module over UART (AT command set, `AT+BDFB200 Audio`, module `V1.0.0`) | `FIRMWARE_ANALYSIS.md` §1.3, `HARDWARE.md` §4 |
| 5 | LCD display + 4 footswitches (A-D) + buttons | stock display init (emulation stalls there); buttons on GPIO3_IO12 / GPIO2_IO24 |
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
| Display UI | MISSING (needs RE of the LCD controller/pins) |
| Bluetooth (AT + audio) | MISSING (needs UART RE + BT audio path RE) |
| Tuner / looper / drum machine | MISSING (verify stock first) |
| MIDI | MISSING (verify stock first) |
| PC-side tooling | `fb200` CLI + `measure.py`; better than stock |

## Roadmap

- **M1 — audio engine bring-up** (current): 48 kHz pass-through, DSP hook,
  USB UAC2, codec, SAI/eDMA, console, LED. Remaining: hardware verification.
- **M2 — the sound**: open DSP chain (noise gate, EQ/biquads, drive/amp
  stage, cab IR convolution, modulation/delay/reverb), presets of our own,
  measured against stock (`docs/MEASUREMENTS.md`).
- **M3 — the UI**: LCD driver (RE the stock controller/pins), footswitch
  handling, preset browse/edit, tuner.
- **M4 — storage**: preset + config persistence in flash with wear leveling;
  model/IR management (open formats, not the stock blob).
- **M5 — connectivity**: USB MIDI, Bluetooth module (AT control + BT audio
  path once RE'd), optional USB host/audio interface modes.
- **M6 — extras**: looper, drum machine/metronome, web-based editor over USB
  CDC, measurement suite integration.

## Why ours is better

- 48 kHz throughout (stock: 44.1 kHz) with a 12.288 MHz clock tree and UAC2.
- Open, SOTA DSP: IR convolution and models in the open, no proprietary blob.
- USB-only flashing + recovery (`handover`), live console, self-update.
- Everything scriptable and testable: host tests, dry-run gate before every
  flash, measurement suite for objective comparisons.
- Open preset/IR formats and a web editor, no vendor tooling required.
