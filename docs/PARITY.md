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
| 10 cabs (512-tap FIR) | DONE | `dsp/cab.c`; -110 dB, `tests/test_stock_dsp_parity.py` |
| 9 user IR slots (cab 11-19) | DONE, BETTER | `engine.c` `load_user_ir`; an empty slot bypasses the cab (stock: silence) |
| Noise gate | DONE | `dsp/gate.c`; bit-exact, `tests/test_fx_parity.py` |
| Compressor "CS Comp" | DONE | `dsp/comp.c`; <= -135 dB, `tests/test_fx_parity.py` |
| MOD, 12 types | DONE | `dsp/mod.c`; <= -105 dB, `tests/test_fx_parity.py` |
| Reverb, 5 types | DONE | `dsp/reverb.c`; <= -110 dB, `tests/test_fx_parity.py` |
| Master volume, smoothed | DONE | `engine.c` (`s_master`), knob k15 |
| Input gain (global `S+0x1a`, app, -55..+6 dB) | DONE | `dsp/gain.c` `gain_input_stock` (stock table dB steps, within 1 ulp; `dsp_host_test.c`), stock smoother in `engine.c`; 0 dB keeps the bit-parity value |
| Delay (preset fields `0x8c..0x94`, protocol `0x85`) | BETTER | the stock DSP ignores them (emulation). Ours plays them only in presets with our marker, so stock presets sound the same: [M4 delay](#m4-bass-delay) |
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
| Battery level (4 steps) and charger sense, `BB` to the app | DONE | `ui/power.c`, `proto_port.c` `proto_battery` |
| Battery notification on change | DONE | `ui/power.c`: `BB` when the level or the charger changes (stock 0x18a5e); level 4 while charging (0x1897c) |
| Power-fail settings save | DONE (different) | the switch is a hard cut (checked on the pedal); settings are written 3 s after a change |
| Firmware update | DONE, BETTER | USB recovery + app slot, `fb200 update`, browser updater (v0.6.0); no A+D. The official PC updater (`C1`) goes to our recovery, not the vendor DFU: going back to stock needs A+D |

Not in the stock (so not gaps): looper, MIDI, auto power-off. (Delay: ours, M4.)

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
  bass chain additions: crossover clean-blend drive, 5-7 band EQ + HPF/LPF,
  delay (the stock has none: its delay fields do nothing; first version
  done, see [below](#m4-bass-delay)), better tuner.
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

### M4: bass delay

Status: first version on the host (2026-09-28), not tried on the pedal.

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
20-1000 ms (RAM, below).

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
so the tail dies out to exact zeros): 1 s at up to 48 kHz = 96,008 B. DTCM
has ~5.5 kB free (`.bss` ends at 0x2004CE68, the limit is 0x2004E3E8), so
the line is in OCRAM2 (`linker.ld` `.ocram`, 0x20210000, NOLOAD, cleared by
`delay_init`). OCRAM2 (512 kB) is free after boot: the vendor loader only
unpacks the stock's 0x5AA0-byte OCRAM data to 0x20200000, which our code does
not read. OCRAM is cached (D-cache on, default memory map); the delay
touches it 3 times per sample. `delay_t` (116 B) stays in DTCM. ITCM code:
+2.9 kB.

**CPU.** Measured by instruction count (Cortex-M7 build, `-O2`, Unicorn):
92 instructions per sample with low cut and tone on = 0.7 % of the 600 MHz
budget at CPI 1, 1.4 % at CPI 2. Check on the pedal with `cpu`.

**Off = the stock chain.** The engine calls `delay_process` only when
`preset_delay_on`; otherwise the chain is the same code as before. A switch
from off to on clears the line first (no old audio).

**Console** (for tests on the pedal): `delay [on|off] [time 20-1000 ms] [fb
0-100] [mix 0-100] [lowcut 0-100] [tone 0-100]`. It edits the edit buffer
(save with `save` or a held footswitch). The first edit of a preset without
the marker writes the marker and our defaults (350 ms, fb 30, mix 35, low
cut 63, tone 70). `delay off` on a stock preset writes nothing.

**Tests** (`tests/test_delay.py`, `audio/tests/delay_host_test.c`, at 44.1
and 48 kHz): first echo at exactly `time * fs`, echo ratio = feedback
(0.475 +- 0.001), nothing between the echoes; 40 Hz through a 150 Hz low
cut comes back at -23.2 dB (Butterworth -23.2 dB), 1 kHz at 0 dB; full
feedback then 30 s of silence: exact zeros after ~1 s, no NaN, no subnormal;
a time change glides and settles on the exact sample count; the preset rule
(stock values, erased flash, byte-swapped marker, en off).

Open: footswitch/knob control of the delay (none now), the light bar for
it, a type map for `0x8e`, tap tempo.

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
