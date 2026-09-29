# Feature parity with the stock firmware

Goal: everything the stock FB200 (V1.0.1) does, then better. This page is the
status and the gap list. The stock feature list and its evidence (manual, stock
code, emulation) are in [`STOCK_FEATURES.md`](STOCK_FEATURES.md). Last audit:
2026-09-27, firmware 0.6.x. Gaps 1-3 and 5-8 of that audit were closed the same
day; their evidence is host tests (`tests/test_proto_host.py`, `test_stockdata.py`,
`test_dsp_host.py`) against the stock code. The checks on the pedal are listed
under [Pedal checks](#pedal-checks).

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
| 10 cabs (512-tap FIR) | DONE | `dsp/cab.c` on the partitioned FFT convolver `dsp/conv.c` (no latency); -110 dB, `tests/test_stock_dsp_parity.py` |
| 9 user IR slots (cab 11-19) | DONE, BETTER | `engine.c` `load_user_ir`; an empty slot bypasses the cab (stock: silence) |
| Noise gate | DONE | `dsp/gate.c`; bit-exact, `tests/test_fx_parity.py` |
| Compressor "CS Comp" | DONE | `dsp/comp.c`; <= -135 dB, `tests/test_fx_parity.py` |
| MOD, 12 types | DONE | `dsp/mod.c`; <= -105 dB, `tests/test_fx_parity.py` |
| Reverb, 5 types | DONE | `dsp/reverb.c`; <= -110 dB, `tests/test_fx_parity.py` |
| Master volume, smoothed | DONE | `engine.c` (`s_master`), knob k15 |
| Input gain (global `S+0x1a`, app, -55..+6 dB) | DONE | `dsp/gain.c` `gain_input_stock` (stock table dB steps, within 1 ulp; `dsp_host_test.c`), stock smoother in `engine.c`; 0 dB keeps the bit-parity value |
| Delay (preset fields `0x8c..0x94`, protocol `0x85`) | BETTER | the stock DSP ignores them (emulation). Ours plays them only in presets with our marker, so stock presets sound the same: [M4 delay](#m4-bass-delay) |
| Looper (not in the stock) | BETTER (host) | ours: up to 108 s mono in the flash, overdub (whole-loop dubs up to 54 s), undo/redo of the last dub, single-switch looper mode (hold D + C long); the delay and long IRs stay: [M8 looper](#m8-looper) |
| Bass EQ (not in the stock) | BETTER (host) | ours: HPF + 5 bands + LPF after the cab, set from the console, stored in the preset with our marker (`0xc4`); stock presets keep it off: [M4 EQ](#m4-bass-eq) |
| Module order (`0xbc`) | not needed | the stock ignores it (emulation); stored by `proto.c` `0xA0` |
| Sample rate 44.1 kHz | DONE | `audio_config.h` `AUDIO_FS`; stock clock tree |

### Presets, knobs, footswitches, display

| Stock feature | Status | Our implementation and evidence |
| --- | --- | --- |
| 40 presets in the stock flash format | DONE | `preset/preset.c`; stock presets load on the pedal ("Fat Bass", "Clean Pick") |
| Slot select A-D, bank up C+D / down A+B | DONE | `ui/ui.c` `action_single`, `action_chord`; footswitch map checked on the pedal. A bank chord browses (below) |
| Live (stomp) mode B+C: A reverb, B MOD, C amp+cab, D comp | DONE | `ui.c` `toggle_module`; stock code 0x9fca-0xa6f8, incl. C: amp or cab on -> both off (0xa4e2); host test |
| Save: hold **any** switch to save to **that** slot, in preset and live mode | DONE | `ui.c` `save_to`; stock 0x9f12, 0xa17c, 0xa3e6, 0xa62c; notifications 97, 98, B0 (0x67e0); host test |
| Save to another bank (BANK +/- then hold) | DONE | stock 0x1bc0c: a bank chord only moves the shown bank (edit buffer kept); 0x19af4: in preset mode the display flashes (133 ms on/off) and the browse ends after 1666 ticks (~1.7 s); A-D load from the shown bank. `ui.c` browse; host test |
| Save feedback (light ring blinks 1 s) | DONE (different) | `ui/lightbar.c` `lightbar_save`: the saved slot's ring off 0-200 ms, on 200-400, off, on 600-800, off, as the stock (0x67e0). The stock writes after the blink; ours writes at once (and shows `SAV`) with the blink in parallel; host test |
| 16 knobs with pickup, knob LEDs (on / blink / off) | DONE, BETTER | `ui.c` `knobs`, `leds`; knob table measured on the pedal; display names the knob and marks "not picked up" |
| Knob LEDs off in tuner mode | DONE | `ui.c` `leds`; host test |
| Display: `P`/`L` + bank + slot, `d`, 0-100 values, tuner | DONE | `ui/display.c`, `ui.c` |
| Footswitch light rings (40 RGB LEDs, 10 per dome): switch status, app colours, save blink, tempo flash | DONE (host) | `ui/lightbar.c` from the stock LED task 0x67e0: preset mode = only the loaded slot's ring, in its app colour and level (0x19414); live mode = a ring per switch while its module is on, fixed colours (0x6a94); tuner = all off; rhythm = A/B lit while held, C tempo flash, D lit while playing (0x68ca). Frames only on change, at most every 20 ms. Host tests. Ring order as the stock (switch A = LEDs 0-9); checked on the pedal: slot A lights that dome |
| Status LED (battery level, charging) | DONE | `ui/power.c`: level colours from stock thresholds. While charging the stock code turns the LED off too (0x1897c); the manual's red/green must come from the charger chip. Check on the pedal |
| Factory reset (app `B2`, console `factory yes`) | DONE | stock routine 0x18fe0: 40 presets (20 factory + 20 EMPTY), default settings, rhythm block, IR list; factory presets from the stock data blob v2; `ui_factory_reset`, `proto_factory_reset`; host test. Keeps the master volume (stock default 0) |
| Test mode (boot with D held) | MISSING | low priority, meaning not known |

### Tuner and drums

| Stock feature | Status | Our implementation and evidence |
| --- | --- | --- |
| Tuner (YIN), hold A+B | DONE | `dsp/tuner.c`; same readings as the stock, `tests/test_drums_tuner.py`; `ui.c` B held + A long |
| Tuner calibration (`S+0x2c`, A4 = 435 + v) | DONE | `engine.c` `engine_apply_settings` |
| Tuner mute option (`S+0x2e`) | DONE | `engine.c`: 0 lets the sound through while tuning (drums stay off) |
| Tuner on/off from the app (`B8`, `B0` `S+0x2d`) | DONE | `ui.c` `ui_settings_changed`; host test |
| Drum machine: 40 rhythms, 40-260 BPM, level, stock samples | DONE, BETTER | `dsp/drums.c`; bit-exact, `tests/test_drums_tuner.py`; drums in the USB recording; hits on time |
| Rhythm mode (hold B+C, display `d`) | DONE | `ui.c` C held + B long |
| Buttons in rhythm mode: A/B rhythm, C tap tempo, D start/stop | DONE | `ui.c` `rhythm_single`; stock 0x9fec (A: - 1, wraps to 40), 0xa2d6 (B), 0xa54e (C), 0xa714 (D); BA sent on every button; host test |
| Tempo flash on the light ring | DONE (host) | `ui/lightbar.c`: ring C off for the first half of each beat (60000 / BPM ms), red for the second half, free-running like the stock counter (0x68fc); host test |
| Count-in | UNKNOWN | `drums_count_in` exists, nothing calls it; how the stock starts it is not known (rhythm byte 1?) |
| Knob control of drums (LEVEL/RATE/MOD) | BETTER | `ui.c` `knobs`; the stock has only the app |

### Bluetooth, app protocol, USB, power

| Stock feature | Status | Our implementation and evidence |
| --- | --- | --- |
| BT module start-up (AT) | DONE | `bt/bt.c`; module answers on the pedal |
| BT audio on/off kept over a reboot (`S+0x17`) | BETTER | `bt.c` sends `AT+B500` at boot when off. The stock always sends `AT+B501` at boot (0x1b8f0); its 0x190e4 call is inside the factory reset |
| BT rename (`B3`) | DONE | `proto_port.c` `proto_hook_bt_name` |
| BT audio in (SAI3), mixed x1.3 | PARTIAL | `audio/bt_audio.c`; SAI3 runs on the pedal; audible playback from a phone not logged |
| App protocol: all commands (`00`..`FA`) | DONE | `proto/proto.c`; `tests/test_proto_host.py` |
| App: live edit, presets, IRs, settings over BLE | UNKNOWN | code path runs; not tried with the Flamma Manager phone app |
| App: drums (`BA`), rhythm mode (`C9`), tuner (`B8`/`B0`) | DONE | `BA` drives the drum machine at once (`ui_rhythm_set`); the drum machine is the one copy of the rhythm state, so `BA` notifications are current; `C9`/`B8`/`B0` switch the modes at once; host test. Not tried with the phone app |
| Light-ring colours from the app | DONE (host) | the colour is `S+0x24+slot` (palette 0..9, > 9 -> 8) and the level `S+0x28+slot` (0..100 -> 30..100 %), both per slot, written by `B0` bytes 11 and 12 for the current slot (stock 0x19414 -> 0x17908). `S+0x1b..0x1e` (`B7`) are **not** read by the LED code; still unknown. Host test |
| USB identity 34DB:800F + vendor HID | DONE | `usb_descriptors.c`, `usb_hid.c`; `fb200 info`, `fb200 ir list` on the pedal |
| PC editor (official Electron app) | UNKNOWN | same protocol; not tried with the official app |
| USB audio 44.1 kHz, 2 in / 2 out | DONE, BETTER | UAC2 (stock UAC1), `audio/usb_audio.c`; recording and playback on the pedal (M1) |
| USB OTG recording to a phone | UNKNOWN | class-compliant, not tried on a phone; the stock "OTG volume" setting is not identified |
| Battery level (4 steps) and charger sense, `BB` to the app | DONE | `ui/power.c`, `proto_port.c` `proto_battery`; the stock thresholds on a filtered reading with hysteresis (`ui/power_logic.c`) |
| Battery notification on change | DONE | `ui/power.c`: `BB` when the level or the charger changes (stock 0x18a5e); level 4 while charging (0x1897c) |
| Power-fail settings save | DONE (different) | the switch is a hard cut (checked on the pedal); settings are written 3 s after a change |
| Firmware update | DONE, BETTER | USB recovery + app slot, `fb200 update`, browser updater (v0.6.0); no A+D. The official PC updater (`C1`) goes to our recovery, not the vendor DFU: going back to stock needs A+D |

Not in the stock (so not gaps): looper, MIDI, auto power-off. (Delay: ours, M4;
looper: ours, M8.)
Ours, off by default: idle standby (panel dark), LED level, clock switch; on:
CPU sleep, unused clocks off, low/critical battery handling ([POWER.md](POWER.md)).

## Gaps, ranked by user impact

| # | Gap | Evidence | Size |
| --- | --- | --- | --- |
| 1 | **Not verified**: phone app over BLE, official PC editor, BT audio by ear, OTG to a phone, count-in, meaning of `S+0x1b..0x1e`, `S+0x1d` (indexes the input-gain table: a second level?), rhythm byte 1 | see the UNKNOWN rows | S each (a test session) |
| 2 | Light rings on the pedal: which dome the stock lights for switch A (code: LEDs 0-9; camera: LEDs 0-9 are in D), the LED order inside a ring | pedal check 10 | S |
| 3 | Test mode (boot with D held) | meaning not known | S |

Closed on 2026-09-27 (host tests; pedal checks below): save to any slot and to
another bank, rhythm-mode buttons, app drum/mode/tuner commands, factory reset,
input gain, tuner calibration and mute, BT audio at boot, battery notification,
knob LEDs in tuner mode, live-mode C. The status LED while charging already
matched the stock code.

### Pedal checks

The changes of 2026-09-27 are tested on the host only. On the pedal:

1. Hold B for 1 s in preset mode: `SAV`, the display then shows `P<bank>b`;
   select another slot and back: the edits are there.
2. Change a knob, press C+D: the display flashes the next bank and the sound
   does not change; hold A: saved to slot A of that bank. Without a choice the
   display returns to the current bank after about 2 s.
3. Live mode (B+C), hold D: saves; the compressor does not toggle.
4. Rhythm mode: A/B step the rhythm (from `d01`, A goes to `d40`), C tapped
   twice sets the tempo, D starts/stops.
5. Tuner: knob LEDs go dark.
6. Live mode with amp off and cab on (set in the app): C turns both off.
7. App: play/stop and rhythm from the drum page act at once; the tuner and
   rhythm-mode buttons switch the pedal; input gain changes the level; the
   tuner calibration moves the reading; tuner mute off lets the sound through;
   BT audio off survives a power cycle; the battery page updates when the
   charger is plugged in.
8. Factory reset (`fb200 console "factory yes"` or the app): 20 named + 20
   EMPTY presets, preset `P0A` "Fat Bass", IR list empty. With a v1 sound
   data blob it must refuse and change nothing.
9. Charging: status LED red while charging, green when full (charger chip;
   see the TODO in `ui/power.c`).
10. Light rings (10 LEDs in each footswitch dome, about 25 % brightness):
    - preset mode: only the loaded slot's dome is lit, red by default; A-D
      move it; a bank chord does not move it;
    - app: a new colour or level for the current slot changes that dome at
      once (palette: red, orange, yellow, green, cyan, blue, magenta, pink,
      white);
    - hold a switch to save: that dome blinks twice in 1 s (off, on, off, on,
      off), then stays lit;
    - live mode: A purple while the reverb is on, B orange (mod), C red (amp
      or cab), D green (comp); each switch toggles its own dome;
    - tuner: all domes dark;
    - rhythm mode: A and B red while held, C flashes red at the tempo (dark
      for the first half of each beat), D red while the drums play.

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
(reverb and MOD delay lines): RAM is the tight limit. With the cab on the
FFT convolver it is 218 of 219 kB (the user IR staging buffer moved to OCRAM).

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
  stock image (`fb200 update stock`), never committed. Input gain done
  (host test).
- **M3 - parity UI: mostly DONE.** Done: display, footswitches (stock
  chords with bank browse, live mode, save to any slot, tuner, rhythm mode
  with the stock buttons), knobs with pickup, knob LEDs, presets in the stock
  format, factory reset, battery/charger monitor, settings save, tuner, drum
  machine, Bluetooth (AT, app protocol, BT audio on SAI3). Open: the pedal
  checks above (incl. the light rings), then the test session (gap 1).
- **M4 - better core (after parity):** 48 kHz / 24-bit engine (needed for
  NAM; stock assets resampled offline), latency <3 ms, CPU/RAM profiler;
  bass chain additions: crossover clean-blend drive, 5-7 band EQ + HPF/LPF
  (first version done: 5 bands + HPF/LPF, see [below](#m4-bass-eq)),
  delay (the stock has none: its delay fields do nothing; first version
  done, see [below](#m4-bass-delay)), better tuner.
- **M5 - IR engine:** up to 4096 taps (the cab runs on the two-stage
  convolver, [below](#m5-long-irs-in-the-cab)); 64 long-IR slots with
  storage and transfer (P2, [below](#m5-p2-long-ir-storage), open: the
  pedal checks), WAV import, low/high cut, dual-IR blend (host, `process_ir`).
- **M6 - open ecosystem:** done: documented protocol (`PROTOCOL.md`),
  browser firmware update (v0.6.0, WebHID + Web Serial). Open:
  class-compliant USB MIDI, WebMIDI/WebHID editor (self-describing blocks),
  JSON presets, multichannel UAC2 (dry DI + processed + re-amp).
- **M7 - NAM A2-Lite player** (~50 % CPU on this class of M7; MIT stack:
  NeuralAmpModelerCore, nam-binary-loader, nam-pedal), TONE3000 browsing in
  the editor, A1 -> A2-Lite distillation tool on the host.
- **M8 - bass effects:** mono octaver (poly later), envelope filter/synth,
  multiband compressor; looper (done: up to 108 s in the flash, see
  [below](#m8-looper)); AIDA-X/RTNeural.

### M5: long IRs in the cab

Status: step P1b on the host (2026-09-28); on the pedal from the RAM
reclaim (2026-09-29, not yet measured on hardware). The tail (~96 kB) is in
the low DTCM and its FFT tables (4.9 kB) in OCRAM
([memory map](FIRMWARE_BRINGUP.md#memory-map-audio-app)). The firmware is
built with `ENGINE_IR_TAPS` = 4096 (`src/audio/engine.h`); with 512 there is
no tail, no 512-point tables, `cab_set_ir_len` over 512 taps fails and
`cab long` over 512 answers "not available". Stock and user slots are 512
taps.

**Design** (`dsp/conv2.c`, `dsp/cab.c`). The cab is a `conv2_t`: head =
taps 0..511 on the 32-sample partitioned convolver (`conv.c`, as before),
tail = taps 512..4095 in 256-sample partitions on a 512-point FFT, one
slice of the work per block, 512 samples late = no added latency.

- **Tail off (<= 512 taps, every stock cab and user slot):** no tail work at
  all, not even the input history. Output bit-identical to the old cab,
  cost the same (instruction counts, `tools/conv2_cycles.py`: 17507 per
  block vs 17490).
- **Tail on, 4096 taps:** per block (slice 0..7) 44.5k 27.1k 27.1k 27.1k
  27.1k 31.8k 31.8k 46.9k instructions; the block budget is 435k cycles
  (600 MHz, 44.1 kHz, 32 samples). Tail data is in OCRAM (D-cache): expect
  more cycles than instructions there; check with `prof`.
- **IR change:** `conv2_set_ir` costs ~60-86k instructions (the 16 head
  FFTs, as a 512-tap change before; it was ~510k for 4096 taps: an audio
  dropout). <= 512 taps: at once, exact, tail off. Longer: the tail spectra
  are computed one 512-point FFT per block into the idle half of a double
  buffer (never in the two blocks that already run an FFT), then head and
  tail swap together. Tail on before: exact swap at a frame boundary (the
  new tail starts to accumulate at slice 0, one frame before the head
  swaps), ~15-25 ms after the call; worst block while loading 52.5k.
  Tail off before (short -> long): swap after 14 blocks, and the new tail
  starts with an empty history: taps 512.. fade in over the tail length
  (up to 81 ms). Accepted: it only happens at a cab change. A long IR's
  gain changes with the IR.
- **Why not a crossfade or a main-loop job:** a crossfade needs two
  convolvers running; a main-loop job needs a hook in the main loop and has
  no fixed swap point. Doing one FFT per block in the audio path is
  deterministic and host-testable (exact swaps are tested against a
  direct FIR).

**Memory.** DTCM: +64 B (`cab_t` +84 B; TinyUSB's 2 kB-aligned `_dcd_data`
now comes first in `.bss`, where the section alignment pads anyway, so it
no longer costs up to 2 kB of padding). Tail 96 kB (`s_cab_tail`) in the
low DTCM (`linker.ld` `.dtcm_lo`); in OCRAM (32 kB) the IR staging 16 kB
(`s_ir`, 4096 taps for `cab long`) and the 512-point rfft tables (4.9 kB,
`.ocramdata`: copied from the slot data at boot, with the DTCM tables).
See [the memory map](FIRMWARE_BRINGUP.md#memory-map-audio-app). A build with
`ENGINE_IR_TAPS` = 512 (long IRs off) needs IR staging 2 kB and links
neither the tail nor the tables (`cab_init` uses `conv2_init_head`). ITCM
code: +2.6 kB.

**Console:** `cab long <taps>` puts a synthetic IR (noise, -60 dB at 4096
taps) in the cab until the next cab change; `cab long 0` goes back to the
preset's cab. With `prof` it measures the real cost on the pedal. Up to
`ENGINE_IR_TAPS` taps (over: "not available").

### M5 P2: long IR storage

Status: on a pedal (2026-09-30). `loop stats` read JEDEC `ef 40 17` (8 MB).
`fb200 ir put` stored a 1651-tap IR in slot 20, and `cab 20` stayed selected
while a loop existed (cab 11333 cycles/block over the following blocks).
The sound has not been listened to.

- **Slots:** 64, cab types 20..83 (the preset's cab field; the stock app
  protocol clamps only above 120). Up to 4096 taps, float32, 44.1 kHz.
  Flash F:0x400000..0x502000, above the model library, only on a chip that
  holds it (`flash_capacity`: JEDEC size and FlexSPI window, run-time check).
  Two table copies (seq, CRC), a data CRC per slot
  ([flash map](UI_AND_STORAGE.md#5-flash-map-and-storage-h-verified-entries-read-on-the-pedal)).
- **Transfer:** console `irput` streams the taps into the IR staging buffer
  (`s_ir`, 16 kB OCRAM: no new buffer; cab changes wait meanwhile), checks the
  CRC, then writes one 4 kB sector per main-loop pass with `flash_store`
  (`flash_rmw`: the audio keeps running while the flash is busy; its range
  check takes the store's range only on a chip that holds it). The table is
  built in the same buffer. `irls`, `irdel`; host `fb200 ir put`,
  `ir ls --long`, MCP `long_ir_*` ([PROTOCOL.md §5.10](PROTOCOL.md#510-long-ir-store-our-firmware-usb-console)).
- **Load:** `engine_apply_preset` checks the data CRC in flash (~0.8 ms for
  4096 taps, main loop), copies the taps into `s_ir` and calls
  `cab_set_ir_len`. Empty or bad: cab bypass, as an empty stock slot.
- **With the looper (M8):** both stay. The loop is in its own flash and does
  not borrow the delay line or the long-IR tail. A stored long IR plays all
  of its taps while a loop exists. `cab long`, `cab <20-83>` and `irput`
  work during a loop. `cab long` answers -4 while an upload holds the IR
  buffer.
- **Gain:** the stock user-IR rule (`cab_user_ir_gain`, first 512 taps),
  computed once at upload and stored. Why: the head of a cab IR carries its
  level; the same IR plays at the same level from a stock slot (512 taps) and
  a long slot, and switching between them gives no jump. A long tail adds
  little energy (it decays).
- **Code placement:** `irstore.c` and its glue are cold (XIP), as `proto.c`:
  they run only in the main loop and write only through `flash_store` (RAM).
  `flash_probe` reads the JEDEC ID once at boot (`flash_read_id`, cold) and
  stores the size. `flash_capacity` is that RAM word, so a store does not
  send a JEDEC command. The image test keeps `flash_capacity` in ITCM.
  RAM: the upload state in OCRAM (`s_irput`).
- **Tests:** `tests/test_irstore_host.py` (irstore.c on a fake flash: upload,
  table copies, torn writes, bad CRC, full table, small chip, delete, `irls`
  output; also under gcc 14 ASan/UBSan with `IRSTORE_CFLAGS`),
  `tests/test_longir_console.py` (host client, CLI and MCP against a pty pedal
  that runs irstore.c).

### M8: looper

Status: on a pedal (2026-09-30), driven from the console (the same engine
actions as the footswitches; the switches were not pressed). A 103.4 s
record of the 108.3 s maximum. A dub started at 54.6 s and ended on its own
when the erased flash ran out (`cut=2`; no underruns, overruns or drops).
Undo was sent during play and the label became redo after the fade.
`loop save 1` and `loop save 2` both came back after `reset` (`loop load`,
stopped, same lengths). With that loop playing, delay on at 320 ms and cab
20 (a stored 1651-tap IR): engine 9% average and 16% peak, the loop task
22% busy, cab 11333 / delay 393 / loop 2848 cycles per block. Stack
high-water 5144 of 8192 bytes. The long record counted 13 extra output
skips; that playback added none. No crash dump. The sound has not been
listened to. Host tests still cover the simulated flash. It replaces the
first version (16 s of ADPCM in borrowed RAM, PR #32).

**Where.** `dsp/looper.c`, mono, after the reverb and before the master
volume: it records the processed sound, the loop plays into both sides and
the master scales it with the live signal; the USB capture has it, the drums
are not recorded. Live sound passes at unity.

**Storage.** The loop is in the external NOR flash (W25Q64JV, 8 MB, JEDEC
`ef 40 17`), F:0x510000 to the end of the chip (`flash_capacity`, from the
JEDEC ID and the FlexSPI window; at most F:0x800000): 2.94 MB. The bounds
are in `debug/flash_rmw.h` (`FLASH_LOOP_*`); `flash_rmw` accepts the area.
F:0x50F000 (one sector) holds two `loop save` records (the chunk maps, the
length and the undo flag). RAM holds
only two rings of 16 frames between the audio and the flash side
(`loopstore/loopio.h`, DTCM) and the flash side's maps (OCRAM). The delay and
the long-IR cab keep their RAM.

**Format.** 22.05 kHz: the input decimated by 2 with a 27-tap half-band FIR
(flat +-0.03 dB to 8 kHz, <= -64 dB from 15 kHz), the output interpolated
with the same filter. Frames of 32 samples in 41 bytes (`dsp/loopcodec.c`):
a scale byte in quarter-octave steps and 32 x 10-bit mantissas, NICAM-like
block floating point. Every frame decodes on its own; 28.3 kB/s. Why 10
bits, not 8: an 8-bit block (even with the best scale per block) tops out at
50 dB for a sine that fills it (1.76 + 6.02 x 8, less the scale step), 5 dB
short of the 55 dB target at 5 kHz; prediction across frames would help but
breaks random access (the loop start, a dub start, undo). 10 bits cost 20 %
of the length.

**Layout.** The area is 376 slots of 8 kB (2 sectors). The loop is a list
of chunks of 199 frames (289 ms); map `cur[]` gives each chunk's slot. NOR
cannot be rewritten in place, so a write (the first record, the closing
crossfade, a dub) writes each chunk it touches as a new version into an
erased slot: the frames it does not touch are copied from the old version
(the head of the first chunk is copied last, so a dub never waits for it),
and the new version replaces the old in the map when it is complete.
Undo keeps the map from before the last dub (`alt[]`): undo and redo swap
the maps; a new dub drops the redo. Chunks a dub did not touch are in both
maps, so a short dub on a long loop needs little space. New versions go to
the next erased slot round the area: wear levelling.

**Erase ahead and the audio.** Slots in no map are erased in the background
(64 kB blocks where 8 free slots line up, else 4 kB sectors) from looper
mode on (or the first `loop` command). An erase runs in slices of 3 ms:
`lsio_erase_run` runs the audio pump while the flash is busy, then suspends
the erase (75h; SUS in status register 2) so the main loop (XIP code) runs
again; while it is suspended the flash reads and programs other sectors,
so the streams never wait for an erase. Page programs (256 B) wait with the
pump running (~0.4 ms). Every other flash writer first ends a suspended
erase (`lsio_quiesce` in `flash_cmd_init` and `fw_begin`). All of this is
ITCM code (`tools/hot_path.py` roots `lsio_*`); the flash side's logic
(`loopstore.c`) is cold and runs only while the flash is idle or suspended.
Reads are IP commands (fast read 0Bh) into the rings, never through the AHB
cache. The main loop does not sleep while the flash side has work.

**Throughput** (W25Q64JV typical / max): page program 0.4 / 3 ms, sector
erase 45 / 400 ms, 64 kB block erase 150 / 2000 ms. A record needs 28.3
kB/s written (110 pages/s: 4 % of the time typical, 33 % at the max) and
the same erased (6.5 % of the time typical with 64 kB blocks; at the max
2 s per block the erase alone takes 88 %). A dub adds 28.3 kB/s of reads
(IP reads, well under 1 %). So typical chips erase 15 times faster than a
record fills; a chip at every datasheet maximum cannot erase and program
28 kB/s at once. Hence the erase starts with looper mode (the whole area in
7 s typical, 96 s at the max, simulated), and a record or a dub that finds
no erased slot ends cleanly: the record closes at a chunk end (with the
crossfade), the dub fades out before the chunk end. A record needs 3 erased
slots to start (`loop rec` answers "preparing the flash" before that,
~0.3 s after looper mode on).

**Lengths.** Record: up to 375 chunks = **108.3 s** (one slot is kept for
the closing crossfade). Dub over the whole loop: a loop up to 188 chunks =
**54.3 s** (it needs as many free slots as it touches); a longer loop dubs
until the free slots run out. The shortest loop is 0.5 s. Power off: a
loop that was not saved is lost. `loop save 1|2` writes the maps into
F:0x50F000; those slots are not erased by `loop clear` or by erase-ahead.
`loop load 1|2` installs that loop, stopped at the start. At boot the
records are read and their slots stay protected. A new recording takes
erased slots only, so a saved loop remains until that record is saved over.

**Behaviour.** First record: A press to A press sets the length
(sample-accurate at 22.05 kHz; the wrap is at that sample, no drift, host
test over 10 passes). The close crossfades: the first 8 frames (11.6 ms) are
written again as the loop start fading in while what was played right after
the close fades out, so the wrap has no click. Overdub: new = old x (1 -
0.05 g) + in x g, g the punch gain (5 ms ramp); old layers lose 0.45 dB per
dubbed pass. Undo takes back the whole last dub (all its passes). Stop, play,
undo and clear fade over 5 ms. While a loop exists the settings autosaves
wait; `flash_store` refuses a write during a record or a dub (-6).

**Measured on the host** (`tests/test_looper.py`: `looper_host_test.c` on a
simulated W25Q64, `loopflash_sim.c`, which also fails any read or program
while busy, a program over unerased bits, an erase during a suspend and a
read of bytes not written since boot). Codec alone: SNR >= 60.3 dB for sines
100 Hz..5 kHz from 0 to -80 dBFS. Through the whole path (record, flash,
play; SNR = fundamental vs everything else, THD = harmonics 2-5):

| Sine | SNR | THD |
| --- | --- | --- |
| 100 Hz, -6 / -46 dBFS | 62.6 / 62.5 dB | -83 / -82 dB |
| 440 Hz, -6 / -46 dBFS | 60.7 / 61.4 dB | -84 / -85 dB |
| 1 kHz, -6 / -46 dBFS | 61.3 / 61.1 dB | -78 / -74 dB |
| 2 kHz, -6 / -46 dBFS | 61.1 / 61.0 dB | -80 / -78 dB |
| 5 kHz, -6 / -46 dBFS | 60.4 / 60.4 dB | -89 / -91 dB |

(The ADPCM version: 26..58 dB, falling 6 dB per octave.) Flash timing, 20 s
record + 10 s dub + undo: typical: 0 skips, main loop blocked at most 4.0
ms; datasheet max: 0 skips, 9.3 ms; no erase suspend: 0 skips but 150 ms
UI stalls and 46 read underruns (the chip has suspend). Undo is bit exact
in every case. An erase slower than the record (3.5 s per block): the record
closes by itself at 2.0 s and plays. The whole area: the record closes at
108.3 s; a dub over it ends when the flash is full; a 54.0 s loop dubs
whole.

**CPU** (`tools/engine_cycles.py --looper rec|play|dub`, the M7 model,
cycles per 32-sample block): record 2.5k, play 2.2k, dub 2.8k (worst block
3.5k) of 435k: under 1 %. The flash side runs in the main loop. ITCM: the
audio side and codec ~2.8 kB, the flash operations ~0.8 kB, freed by
moving the ADPCM code out and more control code to flash (`COLD`: the
amp/tone/mod/comp/EQ/cab parameter setters, `fw_info`, `fw_test`, the JEDEC
probe); 0.9 kB of ITCM left.

**Controls.** Looper mode: hold D, then C long (display `LP-`; the same
again leaves; rhythm mode and looper mode exclude each other). A acts when
pressed (record, close, punch out, restart), a tap of A while playing dubs
(on release: a held A is the undo), hold A = undo/redo, tap B = stop/play,
hold B = clear. Display `rEC`, `PLY`, `odb`, `StP`, `Und`/`rdo`, `CLr`,
`PrP` (the flash is not ready yet); ring A red/green/orange with a white
flash at each loop start, ring B blue while a loop exists. Console `loop`
(`prep_ms` = erased flash ready), `loop save|load <1-2>` (two loops kept
across power-off), `loop stats` (JEDEC, slots, programs,
erases, suspends, underruns), MCP `looper`. `loop hq` is gone (the flash
format is better than hq was).

Open: drum sync (quantise the loop length to bars); a stereo loop. The
sound of a loop has not been listened to.

### M4: bass delay

Status: first version on the host (2026-09-28). **Max time 1000 ms.** In
v0.8.0 the line was in memory the pedal does not have (OCRAM above
0x20208000: writes dropped, reads 0), so the repeats never played on the
pedal, only the dry signal. Then the line was in the real 32 kB OCRAM (max
342 ms); now it is in DTCM (see RAM below).

**The problem.** The stock preset has a delay block (app command `0x85`,
fields `P+0x8c` en, `0x8e` type 0-6, `0x90` mix, `0x92` feedback, `0x94`
time 40-2500 ms). The stock DSP ignores it (emulation, all 7 types), and
all 20 factory presets have it on with the same values (mix 9, feedback
18, 490 ms). If we play the delay when `0x8c` is on, every stock preset
changes its sound.

**Choice: option (a), the stock fields plus our marker.** A preset plays the
delay only when `0x8c` is on **and** the u16 at `P+0x96` is `0x4c44` ("DL")
(`preset.h` `preset_delay_on`). Why:

- `0x96..0xa3` is the unused tail of the delay block (each module has 0x18
  bytes; the delay uses 5 words). It is 0 in every factory preset and in
  the "EMPTY" preset (`tests/test_delay.py` reads them from the stock image).
- The stock audio path ignores it: with our marker and our words in
  `0x96..0x9b`, the emulated stock output is bit-identical
  (`test_stock_dsp_ignores_the_marker`; a control edit of the amp gain in
  the same test does change the output).
- Presets stay in the stock format, so they still load on the stock
  firmware (which ignores the delay) and in the app.
- The app's `0x85` block writes only `0x8c..0x95`: it keeps our marker. So
  after our first edit, the app can switch the delay and set mix, feedback
  and time. A preset the app writes whole (`0x97`) with a 0 at `0x96`
  plays no delay: the safe side.
- Option (b), a flag in another unused byte, is the same thing with a less
  obvious place. Option (c), a global switch, would add the delay to every
  preset at once: that is the problem we must not have.

Our words: `0x96` marker, `0x98` low cut (knob 0-100), `0x9a` tone (knob
0-100). Mapping of the stock fields: `0x90` = mix (the stock callback
smooths `0x90` as the delay mix, `tests/stock_emu_fx.py`), `0x92` =
feedback, `0x94` = time in ms. The type field `0x8e` is kept but not used
(one delay voice). The app clamps time to 40-2500 ms; our DSP clamps to
20-1000 ms (`DELAY_MS_MAX`, RAM, below). The preset format is unchanged: a
preset keeps its stored time and plays it clamped to `DELAY_MS_MAX`; the console
shows the clamped time, and its next edit of that preset stores it.

**The official app.** `PROTOCOL.md` has the `0x85` block and per-type
defaults (stock table `0x20008868`); the app files are not in this
checkout, so it is not known if the app shows a delay page. Check with the
phone app (test session, gap 2).

**DSP** (`dsp/delay.c`): mono, in place, after MOD and before the reverb.
`line = LP(HP(x + fb * y))`, `out = x + mix * y` (dry stays at unity). Time
20-1000 ms with a glide (no clicks, a short pitch bend like tape), feedback
knob x 0.95, mix knob x 1.0, low cut 12 dB/oct at 20-500 Hz (knob 63 =
150 Hz, 0 = off), tone 6 dB/oct low-pass 1-10 kHz (100 = off). The loop gain
is at most 0.95 at every frequency (Butterworth high-pass, no peak).

**RAM.** The line is int16 (x 16384, +-2.0 full scale, truncated toward zero
so the tail dies out to exact zeros): 1 s at 44.1 kHz (`AUDIO_FS`, the only
rate) = 88,208 B (`DELAY_MS_MAX` in `dsp/delay.h`, the line sized for
`DELAY_FS_MAX` = 44.1 kHz; the link fails if it does not fit). The line is
in the DTCM between `.bss` and the stack (`linker.ld` `.dtcm_hi`, NOLOAD,
cleared at boot and by `delay_init`); see
[the memory map](FIRMWARE_BRINGUP.md#memory-map-audio-app). (v0.8.0 placed
it at OCRAM 0x20210000, where the pedal has no RAM; main then had 342 ms in
the real 32 kB OCRAM.) `delay_t` (116 B) stays in `.bss`. ITCM code: +2.9 kB.

**CPU.** Measured by instruction count (Cortex-M7 build, `-O2`, Unicorn):
92 instructions per sample with low cut and tone on = 0.7 % of the 600 MHz
budget at CPI 1, 1.4 % at CPI 2. Check on the pedal with `cpu`.

**Off = the stock chain.** The engine calls `delay_process` only when
`preset_delay_on`; otherwise the chain is the same code as before. A switch
from off to on clears the line first (no old audio).

**Console** (for tests on the pedal): `delay [on|off] [time 20-1000 ms] [fb
0-100] [mix 0-100] [lowcut 0-100] [tone 0-100]` (a longer time clamps). It edits the edit buffer
(save with `save` or a held footswitch). The first edit of a preset without
the marker writes the marker and our defaults (300 ms, fb 30, mix 35, low
cut 63, tone 70). `delay off` on a stock preset writes nothing.

**Tests** (`tests/test_delay.py`, `audio/tests/delay_host_test.c`, at 44.1
and 32 kHz): first echo at exactly `time * fs` (also at 20 ms and at
`DELAY_MS_MAX`, and a longer time clamps to it), echo ratio = feedback
(0.475 +- 0.001), nothing between the echoes; 40 Hz through a 150 Hz low
cut comes back at -23.2 dB (Butterworth -23.2 dB), 1 kHz at 0 dB; full
feedback then 30 s of silence: exact zeros after ~1 s, no NaN, no subnormal;
a time change glides and settles on the exact sample count; the preset rule
(stock values, erased flash, byte-swapped marker, en off).

Open: footswitch/knob control of the delay (none now), the light bar for
it, a type map for `0x8e`, tap tempo.

### M4: bass EQ

Status: first version on the host (2026-09-28), not tried on the pedal.

**Chain.** `dsp/eq.c`, mono, in place, **after the cab, before MOD** (the
usual place on a bass pedal: it shapes the amp + cab tone, and MOD, delay and
reverb get the shaped signal). 7 biquads: HPF 12 dB/oct 20-200 Hz, 5 peaking
bands (30-10000 Hz, +-15 dB, Q 0.3-4; defaults 40, 100, 250, 800, 3000 Hz at
0 dB, Q 1), LPF 12 dB/oct 2-20 kHz. RBJ cookbook designs in double, rounded
to float once. No shelves: a band at 30-40 Hz with a low Q or the HPF does
that job; add them if players ask.

**In the preset, with a marker** (the delay rule). The record has a free
tail after the module order (`0xbc..0xc3`): `0xc4..0xff` is 0 in all 21
factory presets, no app command writes it (`0x80..0x86` write inside their
0x18-byte module blocks, `0xA0` writes `0xbc..0xc2`, `0x99` the name), and
the stock DSP ignores it. Layout (`preset.h` `P_EQ_MARK`, `eq.h` `eq_save`):

| offset | size | field |
|---|---|---|
| `0xc4` | u16 | marker `0x5145` ("EQ") |
| `0xc6` | u8 | on |
| `0xc7` | u8 | HPF Hz (0 = off, 20-200) |
| `0xc8` | u16 | LPF Hz (0 = off, 2000-20000) |
| `0xca + 4 b` | u16, s8, u8 | band b 0..4: Hz, gain in 1/8 dB (+-120 = +-15 dB), Q x 50 (15-200) |

`0xde..0xff` stays free. No marker (every stock preset, erased flash, a
whole preset from the app with 0 there) = EQ off with the default settings.
`engine_apply_preset` loads it every time (`eq_load`): only the stages that
change glide (the same 16-block glide as the console), so a preset change
does not click and a knob edit restarts nothing. The console `eq` writes the
marker and the record into the edit buffer (on the grid: the state it shows
is what `save` keeps). Presets with the EQ still load on the stock firmware
(it ignores the bytes) and in the app; a whole-preset write from the app
keeps them if the app sends back what it read.

**Changes do not click.** Each stage glides to a new setting in 16 blocks
(11.6 ms): frequency and Q in octaves, gain in dB, a new design every block,
and inside the block the coefficients step every sample (stable: the biquad
stability triangle is convex). A settled stage runs CMSIS-DSP
`arm_biquad_cascade_df1_f32`. DF1, not `df2T`: the DF1 state is the signal
itself, so a coefficient change does not leave an old-filter state behind; in
a model of an LPF glide (2 -> 20 kHz, 200 Hz sine) df2T put clicks at
-47 dB, DF1 at -83 dB. A straight line from the old to the new coefficients
(no redesign) was worse than a hard switch for a notch Q change.

**Off = the stock chain, bit-exact.** A band at 0 dB, a filter at 0 and
`eq off` are neutral (b = a; the HPF/LPF fade their numerator to the
denominator). A neutral stage that has settled is skipped: it does not touch
the signal and only keeps its last two samples, so it starts again without a
jump. That also keeps the float rounding noise of idle low stages (~-75 dB
each) out of the signal.

**RAM.** `eq_t` is 508 B, in OCRAM (`engine.c` `s_eq`, `.ocram`). In DTCM
.bss it pushed the 2 kB-aligned USB buffer `_dcd_data` up by 2 kB and DTCM
overflowed: .bss has only ~8 B before that boundary (1408 B free after it).
Build: ITCM `.blob` 112760 -> 117056 B (+4.2 kB), `.dtcmdata` 5632 and `.bss`
210536 unchanged, `.ocram` 98056 -> 98564 B.

**CPU** (instructions per 32-sample block, Cortex-M7 build `-O2`, Unicorn;
budget 435k cycles): off 198; 7 stages settled 2.5k (~0.6 % at CPI 1); all 7
gliding (`eq on` with 7 settings) 8.6k plus 42 double divides, ~2-4 %, for
16 blocks; one band gliding 3.5k. `prof` shows it as `eq`. Check on the pedal
with `prof` and `cpu`.

**Console** (for tests on the pedal): `eq` (state), `eq on|off`,
`eq hpf <20-200 Hz|0>`, `eq lpf <2000-20000 Hz|0>`,
`eq <band 1-5> <hz> <gain dB> [q]` (e.g. `eq 2 100 -4.5 1.4`). Each change
goes into the edit buffer; `save` stores it. MCP: `set_eq`.

**Tests** (`tests/test_dsp_host.py::test_eq_suite`, `audio/tests/eq_host_test.c`,
44.1 and 48 kHz): the measured response (the DFT of the impulse response
through `eq_process`) of every band, the HPF and the LPF vs the RBJ target in
double within 0.1 dB where the target is above -24 dB (worst 0.07 dB over a
grid of 30-10000 Hz, Q 0.3-4, +-15 dB; 0.03 dB for all 7 at once), and the
analog prototype at f0 (the peak gain, the -3.01 dB point) within 0.1 dB;
flat = bit-exact (off, on + flat, back to flat, `eq off`); no clicks: a sine
through 10 parameter changes, the output above 8 kHz stays below -70 dB re
its peak (worst -76 dB; negative controls: a 1 % step added at the change
reads -48 to -63 dB, a hard coefficient switch fails 2 of the 10); stable:
every stage at its limits, 20 s of random changes every 1-40 blocks, a
silence ends in exact zeros. Preset (`tests/test_eq_preset.py`,
`audio/tests/eq_preset_host_test.c`): the marker rule (0, erased,
byte-swapped), the record layout byte for byte, save -> load -> save is the
identity, off-grid values round, garbage clamps and stays finite; a preset
change with a 100 Hz sine: max |second difference| stays at the settled
sine's (0.98 off -> on, 0.90 on -> off; a cleared-state control reads 199x),
then bypassed and bit-exact; all factory presets 0 in `0xc4..0xff`; the
stock DSP output is bit-identical with a full EQ record there (emulation;
an amp-gain control edit does change it); app module/order/name writes keep
the bytes (`test_proto_host.py`).

Open: app control, pedal knobs/footswitch for it,
shelves if wanted, the pedal check.

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
