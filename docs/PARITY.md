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
| Amp/cab models | MISSING (the big one) |
| Gate, comp, MOD (12 types), reverb (5 types) | ported from the stock DSP, host parity tests (see below); not wired into the engine yet |
| Presets (storage + UI) | MISSING |
| Display UI | MISSING; hardware fully mapped (`UI_AND_STORAGE.md`) |
| Bluetooth (AT + audio) | MISSING (needs UART RE + BT audio path RE) |
| Tuner / looper / drum machine | MISSING (verify stock first) |
| MIDI | MISSING (verify stock first) |
| PC-side tooling | `fb200` CLI + `measure.py`; better than stock |

## Stock effect ports

The stock DSP was reverse-engineered from the vendor image and runs in
emulation (`tests/stock_emu.py`: Unicorn + a software f64 FPU; the image is
rebuilt at run time from `fb200-stock.mr`, never committed). Our ports live in
`firmware/audio/src/dsp/`, API `x_init(ctx, fs)`, `x_set_params(ctx, <stock
knob units 0..100>)`, `x_process(ctx, x, n)` in place (reverb: mono in, L/R
out), n <= `DSP_BLOCK`. Knobs are smoothed like the stock (`dsp_knob_t`).

Stock chain (ITCM 0x7b60, 44.1 kHz, float, mono until the reverb): input
(L+R) x input gain -> gate -> comp -> amp -> cab -> MOD -> reverb -> master
volume -> clip +-0.95.

| Module | Stock | Preset fields | Port |
| --- | --- | --- | --- |
| Level detector | 0x7a08 | - | `detector.c` |
| Noise gate | 0xb5a0 | 0x5c enable, 0x60 threshold | `gate.c` |
| Compressor "CS Comp" | 0x2300 | 0x14 enable, 0x16 type (unused), 0x18 attack, 0x1a threshold, 0x1c ratio, 0x1e level | `comp.c` |
| MOD | table 0x20010bfc | 0x74 enable, 0x76 type 0-11, 0x78 rate, 0x7a mix, 0x7c p3, 0x7e p4 | `mod.c` |
| Reverb | table 0x20010ca0 | 0xa4 enable, 0xa6 type 0-4, 0xaa level, 0xac decay, 0xae tone, 0xa8 no effect | `reverb.c` |

Parity (`tests/test_fx_parity.py`): the stock callback runs with a preset that
enables only the module under test; its chain output is compared with our C
module fed the same chain input. Error = RMS(ours - stock) / RMS(stock),
target <= -60 dB; -240 dB = bit-exact.

| Case | Error (dB) |
| --- | --- |
| gate-thr20 | -240.0 |
| gate-thr60 | -240.0 |
| gate-thr100 | -240.0 |
| comp-a50-t50-r50-l50 | -240.0 |
| comp-a0-t20-r100-l80 | -143.6 |
| comp-a100-t80-r10-l30 | -135.7 |
| mod-phaser | -114.7 |
| mod-stepphaser | -105.8 |
| mod-flanger | -148.2 |
| mod-jetflanger | -143.3 |
| mod-tremolo | -114.4 |
| mod-stutter | -240.0 |
| mod-vibrato | -240.0 |
| mod-rotary | -240.0 |
| mod-achorus | -117.6 |
| mod-mchorus | -118.1 |
| mod-ringmod | -145.4 |
| mod-filter | -119.7 |
| mod-phaser-max | -106.2 |
| mod-stepphaser-max | -113.0 |
| mod-flanger-max | -150.9 |
| mod-jetflanger-max | -142.5 |
| mod-tremolo-max | -128.4 |
| mod-stutter-max | -240.0 |
| mod-vibrato-max | -240.0 |
| mod-rotary-max | -240.0 |
| mod-achorus-max | -121.4 |
| mod-mchorus-max | -118.8 |
| mod-ringmod-max | -149.7 |
| mod-filter-max | -122.5 |
| reverb-room | -111.6 |
| reverb-hall | -113.7 |
| reverb-plate | -127.0 |
| reverb-spring | -240.0 |
| reverb-mod | -113.5 |
| reverb-room-l100-d0-t0 | -110.0 |
| reverb-hall-l100-d100-t100 | -112.1 |
| reverb-spring-l40-d100-t0 | -240.0 |

Where we are not bit-exact the cause is known: the comp threshold table and
the reverb/MOD LFO wavetables are regenerated from formulas (no stock data in
the repo), and some MOD types use our own sin/cos/tan kernels and a different
multiply-add order (1-2 ulp per sample).

Stock quirks kept for parity: the detector steps twice per sample when the
gate is on (hold 400 samples); the comp envelope does not fall during silence;
the flanger dry path is x(1-x) (`mod_t.fix_flanger_dry = 1` fixes it); preset
field 0xa8 does nothing. The host tests (`fx_host_test.c`) also run every
module at 48 kHz, where times, delay lengths and LFO rates are scaled but some
fixed filters (comp HP/shelf, detector shelf, reverb high-pass/damping) keep
their 44.1 kHz coefficients.

Memory (arm-none-eabi-gcc -O2, measured by linking the app with one static
instance of each; nothing is linked until the engine uses them):

| Module | ITCM code | .bss (DTCM) |
| --- | --- | --- |
| gate + detector | 1.1 kB | 0.5 kB |
| comp | 1.3 kB | 1.2 kB |
| MOD (`sizeof(mod_t)` 8192 + tables 4.1 kB) | 8.6 kB | 12.7 kB |
| reverb (`sizeof(reverb_t)` 46912 + tables 2 kB) | 4.7 kB | 49.3 kB |
| all four | 15.7 kB | 62.6 kB |

The app's .bss grows from 36 kB to 99 kB of the 219 kB DTCM region: no OCRAM
needed yet.

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
