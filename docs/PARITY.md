# Feature parity with the stock firmware

Goal: everything the stock FB200 (V1.0.1) does, then better. This page is the
status and the gap list. The stock feature list and its evidence (manual, stock
code, emulation) are in [`STOCK_FEATURES.md`](STOCK_FEATURES.md). Last audit:
2026-09-27, firmware 0.6.x.

Status words:

- **DONE**: implemented, and a parity test or a check on the pedal shows it.
- **PARTIAL**: works, but part of the stock behaviour is missing or different.
- **MISSING**: not implemented.
- **UNKNOWN**: implemented or not needed, but nobody has checked it.
- **BETTER**: does more than the stock.

## Where we are

### Sound

| Stock feature | Status | Our implementation and evidence |
| --- | --- | --- |
| Fixed chain gate -> comp -> amp -> cab -> MOD -> reverb -> master -> clip | DONE | `audio/engine.c` `engine_task`; played on the pedal (M2) |
| 10 amp models + tone stack (bass, mid, mid freq, treble, gain, vol) | DONE | `dsp/amp.c`, `dsp/tone.c`; bit-exact, `tests/test_stock_dsp_parity.py` |
| 10 cabs (512-tap FIR) | DONE | `dsp/cab.c`; -110 dB, `tests/test_stock_dsp_parity.py` |
| 9 user IR slots (cab 11-19) | DONE, BETTER | `engine.c` `load_user_ir`; an empty slot bypasses the cab (stock: silence) |
| Noise gate | DONE | `dsp/gate.c`; bit-exact, `tests/test_fx_parity.py` |
| Compressor "CS Comp" | DONE | `dsp/comp.c`; <= -135 dB, `tests/test_fx_parity.py` |
| MOD, 12 types | DONE | `dsp/mod.c`; <= -105 dB, `tests/test_fx_parity.py` |
| Reverb, 5 types | DONE | `dsp/reverb.c`; <= -110 dB, `tests/test_fx_parity.py` |
| Master volume, smoothed | DONE | `engine.c` (`s_master`), knob k15 |
| Input gain (global `S+0x1a`, app, -55..+5 dB) | MISSING | engine uses unity gain; default (0 dB) matches the stock |
| Delay (preset fields `0x8c..0x94`, protocol `0x85`) | not needed | the stock DSP ignores them (emulation); stored and echoed by `proto.c` |
| Module order (`0xbc`) | not needed | the stock ignores it (emulation); stored by `proto.c` `0xA0` |
| Sample rate 44.1 kHz | DONE | `audio_config.h` `AUDIO_FS`; stock clock tree |

### Presets, knobs, footswitches, display

| Stock feature | Status | Our implementation and evidence |
| --- | --- | --- |
| 40 presets in the stock flash format | DONE | `preset/preset.c`; stock presets load on the pedal ("Fat Bass", "Clean Pick") |
| Slot select A-D, bank up C+D / down A+B | DONE | `ui/ui.c` `action_single`, `action_chord`; footswitch map checked on the pedal |
| Live (stomp) mode B+C: A reverb, B MOD, C amp+cab, D comp | DONE | `ui.c` `toggle_module`; matches stock code 0x9fca-0xa6f8. Edge case: amp off + cab on -> stock turns both off, ours turns both on |
| Save: hold **any** switch to save to **that** slot, in preset and live mode | PARTIAL | `ui.c`: only hold A, only in preset mode, only to the current slot |
| Save to another bank (BANK +/- then hold) | UNKNOWN | ours: a bank chord loads the preset, so edits are lost; the stock behaviour is from the manual only |
| Save feedback (light bar blinks 1 s) | PARTIAL | ours shows `SAV` on the display; no light bar |
| 16 knobs with pickup, knob LEDs (on / blink / off) | DONE, BETTER | `ui.c` `knobs`, `leds`; knob table measured on the pedal; display names the knob and marks "not picked up" |
| Knob LEDs off in tuner mode | MISSING | `ui.c` `leds` ignores tuner mode |
| Display: `P`/`L` + bank + slot, `d`, 0-100 values, tuner | DONE | `ui/display.c`, `ui.c` |
| 40-LED RGB light bar (switch status, app colours, save blink, tempo flash) | MISSING | `ui/rgb.c` driver only: `rgb_init` clears it and nothing else writes it; LED layout unknown |
| Status LED (battery level, charging) | PARTIAL | `ui/power.c`: level colours from stock thresholds; LED is **off** while charging, manual says solid red (charging) / solid green (full) |
| Factory reset (app `B2`) | MISSING | `proto.c` calls `proto_hook_factory_reset`, which has no target implementation (weak stub returns -1: the app gets "failed"); the 20 factory presets are vendor data and are not in the stock data blob yet |
| Test mode (boot with D held) | MISSING | low priority, meaning not known |

### Tuner and drums

| Stock feature | Status | Our implementation and evidence |
| --- | --- | --- |
| Tuner (YIN), hold A+B | DONE | `dsp/tuner.c`; same readings as the stock, `tests/test_drums_tuner.py`; `ui.c` B held + A long |
| Tuner calibration (`S+0x2c`, A4 = 435 + v) | MISSING | `engine.c` `tuner_init(&s_tuner, 440)`; `tuner_set_a4` exists, nothing calls it |
| Tuner mute option (`S+0x2e`) | MISSING | engine always mutes while tuning (the stock default) |
| Drum machine: 40 rhythms, 40-260 BPM, level, stock samples | DONE, BETTER | `dsp/drums.c`; bit-exact, `tests/test_drums_tuner.py`; drums in the USB recording; hits on time |
| Rhythm mode (hold B+C, display `d`) | DONE | `ui.c` C held + B long |
| Buttons in rhythm mode: A/B rhythm, C tap tempo, D start/stop | PARTIAL | ours: A start/stop, B/C rhythm, D tap. Stock: manual p.14 and code 0x9fec |
| Tempo flash on the light bar | MISSING | needs the light bar |
| Count-in | UNKNOWN | `drums_count_in` exists, nothing calls it; how the stock starts it is not known (rhythm byte 1?) |
| Knob control of drums (LEVEL/RATE/MOD) | BETTER | `ui.c` `knobs`; the stock has only the app |

### Bluetooth, app protocol, USB, power

| Stock feature | Status | Our implementation and evidence |
| --- | --- | --- |
| BT module start-up (AT) | DONE | `bt/bt.c`; module answers on the pedal |
| BT audio on/off kept over a reboot (`S+0x17`) | PARTIAL | the app toggle works (`proto_hook_bt_enable`); at boot `bt.c` always sends `AT+B501` |
| BT rename (`B3`) | DONE | `proto_port.c` `proto_hook_bt_name` |
| BT audio in (SAI3), mixed x1.3 | PARTIAL | `audio/bt_audio.c`; SAI3 runs on the pedal; audible playback from a phone not logged |
| App protocol: all commands (`00`..`FA`) | DONE | `proto/proto.c`; `tests/test_proto_host.py` |
| App: live edit, presets, IRs, settings over BLE | UNKNOWN | code path runs; not tried with the Flamma Manager phone app |
| App: drums (`BA`), rhythm mode (`C9`), tuner (`B8`/`B0`) | PARTIAL | `proto.c` stores them in flash/settings, but the engine and `ui.c` never read them: app play/stop does nothing, and the pedal's `BA` notifications send stale values (`proto.c` `rhythm[]` is not updated by `ui.c`) |
| Light-bar colours from the app (`B7`, probably `S+0x1b..0x1e`) | MISSING | stored only |
| USB identity 34DB:800F + vendor HID | DONE | `usb_descriptors.c`, `usb_hid.c`; `fb200 info`, `fb200 ir list` on the pedal |
| PC editor (official Electron app) | UNKNOWN | same protocol; not tried with the official app |
| USB audio 44.1 kHz, 2 in / 2 out | DONE, BETTER | UAC2 (stock UAC1), `audio/usb_audio.c`; recording and playback on the pedal (M1) |
| USB OTG recording to a phone | UNKNOWN | class-compliant, not tried on a phone; the stock "OTG volume" setting is not identified |
| Battery level (4 steps) and charger sense, `BB` to the app | DONE | `ui/power.c`, `proto_port.c` `proto_battery` |
| Battery notification on change | MISSING | `proto_notify_battery` exists, nothing calls it |
| Power-fail settings save | DONE (different) | the switch is a hard cut (checked on the pedal); settings are written 3 s after a change |
| Firmware update | DONE, BETTER | USB recovery + app slot, `fb200 update`, browser updater (v0.6.0); no A+D. The official PC updater (`C1`) goes to our recovery, not the vendor DFU: going back to stock needs A+D |

Not in the stock (so not gaps): looper, MIDI, delay, auto power-off.

## Gaps, ranked by user impact

| # | Gap | Evidence | Size |
| --- | --- | --- | --- |
| 1 | **Save**: hold any switch A-D saves to that slot, also in live mode; bank chords before saving should keep the edits (save-as) | manual p.13; stock code sets the save slot 0-3 at 0x9f12, 0xa17c, 0xa3e6, 0xa62c; ours: `ui.c` long A only, preset mode only | S (any switch) / M (save-as: confirm the stock first) |
| 2 | **Rhythm-mode buttons** differ: stock A/B = rhythm, C = tap tempo, D = start/stop | manual p.14; stock code 0x9fec (A = rhythm - 1); ours `ui.c` `rhythm_single` | S |
| 3 | **App drum and mode commands do nothing live**: `BA` (play/stop, rhythm, level, tempo), `C9` (rhythm mode), `B8`/`B0` tuner; `BA` notifications are stale | `proto.c` dispatch `0xBA`, `0xC9`, `0xB8`; `ui.c` keeps `rhythm_mode`, `tuner_mode` and drum state apart | S |
| 4 | **Light bar** (40 RGB LEDs) is dark: switch status, colours from the app, save blink, tempo flash | manual p.6, p.13-14; `ui/rgb.c` only cleared at init; LED layout unknown | M |
| 5 | **Factory reset** from the app fails | `proto_hook_factory_reset` not implemented; needs the 20 factory presets added to the stock data blob (`stockdata.py`) | M |
| 6 | **Global settings ignored**: input gain `S+0x1a`, tuner calibration `S+0x2c`, tuner mute `S+0x2e`, BT on/off at boot `S+0x17` | emulation (input gain, mute); stock code 0x913c (calibration), 0x190e4 (BT at boot); `engine.c`, `bt.c` | S |
| 7 | **Status LED while charging** is off; stock: solid red charging, solid green full | manual p.6; `ui/power.c` `power_task` | S (check the colours on the pedal) |
| 8 | Small UI details: knob LEDs off in tuner mode; stomp C edge case; battery notification on change | `ui.c` `leds`, `toggle_module`; `proto_notify_battery` unused | S |
| 9 | **Not verified**: phone app over BLE, official PC editor, BT audio by ear, OTG to a phone, count-in, meaning of `S+0x1b..0x1e`, `S+0x24..0x2b`, rhythm byte 1 | see the UNKNOWN rows | S each (a test session) |

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

Memory (arm-none-eabi-gcc -O2, measured when the modules were ported, one
static instance of each):

| Module | ITCM code | .bss (DTCM) |
| --- | --- | --- |
| gate + detector | 1.1 kB | 0.5 kB |
| comp | 1.3 kB | 1.2 kB |
| MOD (`sizeof(mod_t)` 8192 + tables 4.1 kB) | 8.6 kB | 12.7 kB |
| reverb (`sizeof(reverb_t)` 46912 + tables 2 kB) | 4.7 kB | 49.3 kB |
| all four | 15.7 kB | 62.6 kB |

With the full chain in the engine (commit 189c2e0), DTCM is 211 of 219 kB
(reverb and MOD delay lines): RAM is the tight limit.

## Roadmap

Research behind the "better" items (sources, budgets): `ROADMAP_RESEARCH.md`.
Budgets on this chip: 600 MHz / 44.1 kHz = 13.6k cycles per sample; RAM
(512 KB FlexRAM, no external RAM) is the tighter limit.

- **M1 - audio bring-up: DONE on hardware.** Guitar/bass in, headphone out,
  USB UAC2 in/out, codec, SAI/eDMA, 44.1 kHz stock clock tree, console,
  recovery + USB self-update, crash dumps.
- **M2 - parity sound: DONE on hardware** (2026-09-27; the user played
  through it and stepped presets). Stock chain ported with parity tests
  against an emulation of the stock DSP: gate, compressor, amp
  (Wiener-Hammerstein, 10 models) + tone stack, cab (512-tap FIR, 10 +
  user IRs), 12 modulations, 5 reverbs. Stock data comes from the user's own
  stock image (`fb200 update stock`), never committed. Open: input gain
  (gap 6).
- **M3 - parity UI: mostly DONE.** Done: display, footswitches (stock
  chords, live mode, tuner, rhythm mode), knobs with pickup, knob LEDs,
  presets in the stock format, battery/charger monitor, settings save,
  tuner, drum machine, Bluetooth (AT, app protocol, BT audio on SAI3). Open,
  in order: gaps 1-3 (save, rhythm buttons, app drum/mode commands), gap 4
  (light bar), gap 5 (factory reset), gaps 6-8, then the test session (gap 9).
- **M4 - better core (after parity):** 48 kHz / 24-bit engine (needed for
  NAM; stock assets resampled offline), latency <3 ms, CPU/RAM profiler;
  bass chain additions: crossover clean-blend drive, 5-7 band EQ + HPF/LPF,
  delay (the stock has none: its delay fields do nothing), better tuner.
- **M5 - IR engine:** up to 4096 taps (partitioned convolution, already in
  `dsp/conv.c`), WAV import, 50+ slots, low/high cut, dual-IR blend.
- **M6 - open ecosystem:** done: documented protocol (`PROTOCOL.md`),
  browser firmware update (v0.6.0, WebHID + Web Serial). Open:
  class-compliant USB MIDI, WebMIDI/WebHID editor (self-describing blocks),
  JSON presets, multichannel UAC2 (dry DI + processed + re-amp).
- **M7 - NAM A2-Lite player** (~50 % CPU on this class of M7; MIT stack:
  NeuralAmpModelerCore, nam-binary-loader, nam-pedal), TONE3000 browsing in
  the editor, A1 -> A2-Lite distillation tool on the host.
- **M8 - bass effects:** mono octaver (poly later), envelope filter/synth,
  multiband compressor; ADPCM looper (~8-16 s in RAM); AIDA-X/RTNeural.

## Why ours is better

- Already: USB updates with no A+D (also from the browser), a resident
  recovery, live console, crash dumps; the display names the knob and shows
  pickup; drums from the knobs and in the USB recording; drum hits on time;
  an empty IR slot never silences the pedal.
- Parity first at the stock 44.1 kHz, then a 48 kHz/24-bit core (M4).
- Open DSP: IR convolution and models in the open, no proprietary blob.
- Everything scriptable and testable: host tests, parity tests against the
  emulated stock, dry-run gate before every flash, measurement suite.
- Open preset/IR formats and a web editor, no vendor tooling required.
