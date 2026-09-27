# Stock FB200 feature inventory (V1.0.1): the evidence

The raw evidence behind [`PARITY.md`](PARITY.md). Audit of 2026-09-27.

Sources, with the tag used below:

- **[M]** the owner's manual, `FB200_Manual_EN_V01_2025.05.22` (PDF linked from
  flamma.shop/pages/manual), page numbers as printed.
- **[W]** the flamma.shop product page and shop listings (Thomann): marketing.
- **[C]** the stock image, V1.0.1: static reading of the ITCM code. Addresses are
  ITCM run addresses; `S+x` is the 49-byte global settings block (RAM
  `0x2001DD40`, flash F:0x80000); `P+x` is the preset / edit buffer (`0x2001DC40`).
- **[E]** the stock DSP run in emulation (`tests/stock_emu_fx.py`, Unicorn, RAM
  image from the user's own `.mr`). "No effect" means the output was bit-identical
  with the field at two different values.
- **[D]** earlier reverse-engineering notes: `UI_AND_STORAGE.md`, `PROTOCOL.md`,
  `AUDIO_PATH.md`, `HARDWARE.md`.

No vendor bytes are copied here, only addresses, field offsets and behaviour.

## 1. Sound

| Feature | Stock | Evidence |
| --- | --- | --- |
| Signal chain | fixed: input gain -> gate -> comp -> amp (+ tone) -> cab -> MOD -> reverb -> master -> clip | [C] 0x7b60, [E] |
| Module order field `P+0xbc` | stored and sent by the app, **no effect** on the sound | [E] order [4,3,2,1] vs default |
| Amp | 10 models: Ampog 2OD, Ampog B18 CL, Ampog SVT4, Ampog SVT VALVE, Mvrkbass 500, Mvrkbass 501, Akuila 750 CL, Akuila 750 DS, Akuila 751, BASSER CRUNCH. Knobs GAIN, BASS, MID, TREBLE, VOL; mid frequency `P+0x36` from the app only | [M] p.18, [E] (`P+0x36` changes the output) |
| Cab | 10 cabs (SV810U47 ... Akuila121) + 9 user IR slots (IR1-IR9) | [M] p.19, p.10 |
| Cab params `P+0x48..0x4e` | clamped by the protocol, **no effect** on the sound | [E] |
| User IR | WAV, 44.1 kHz; manual says 512 points 24-bit; the app sends 1024 float32; the DSP uses 512 taps | [M] p.22, [D] PROTOCOL §6 |
| Gate | on/off + threshold; `P+0x5e` type **no effect** | [M] p.10, [E] |
| Compressor | one type, "CS Comp": THRESH, LEVEL knobs; attack/ratio from the app; `P+0x16` type **no effect** | [M] p.10, [E] |
| MOD | 12 types: Phaser, Step Phaser, Flanger, Jet Flanger, Tremolo, Stutter Tremolo, Vibrato, Rotary, Analog Chorus, Multi Chorus, Ring Mod, Filter; RATE, MIX knobs; p3/p4 from the app; `P+0x80` **no effect** | [M] p.20, [E] |
| Reverb | 5 types: Room, Hall, Plate, Spring, Mod; LEVEL knob; decay/tone from the app | [M] p.21 |
| Delay | **none audible.** Protocol `0x85`, preset fields `P+0x8c..0x94` (7 types, 40-2500 ms) exist and every factory preset has it "on", but the DSP output does not change | [D] PROTOCOL §5.3, [E] (all 7 types), [M] (no delay listed) |
| Effect count | "19 effect models" = gate + comp + 12 MOD + 5 reverb | [M] p.4 |
| Input gain | `S+0x1a`, table -55..+5 dB in 5/0.5 dB steps, default index 13 = 0 dB; set by the app (`B0`) | [E] (`IN_GAIN_TAB`, output changes), [D] |
| Master | MASTER knob, `S+0x18` | [D] |
| Looper, MIDI | none | [M], reviews |

## 2. Presets and storage

| Feature | Stock | Evidence |
| --- | --- | --- |
| Presets | 40 = 10 banks x 4 (A-D), 256 B each at F:0x71000 | [M] p.4, [D] §5 |
| Factory presets | 20 named (Fat Bass ... Classic Spring) + 20 "EMPTY"; the 20 are in the image (RAM `0x20004E40`) | [C], [D] |
| Factory reset | app command `B2`; also at boot when a flash magic is missing. No front-panel reset | [D] PROTOCOL §5.1, [M] (none documented) |
| Save | hold **any** footswitch 1 s: saves the edit buffer to that switch's slot of the current bank (in preset and live mode, not in tuner or rhythm mode). The 40-LED light bar blinks for 1 s, then the preset is written and `97`, `98`, `B0` go to BLE | [M] p.13, [C] save flag set with slot 0/1/2/3 at 0x9f12, 0xa17c, 0xa3e6, 0xa62c (tuner and rhythm checked first); save routine 0x67e0 |
| Save to another bank | manual: "press BANK +/- to choose the target group, the screen flashes", then hold a footswitch | [M] p.13; not yet confirmed in [C] whether BANK +/- keeps the edit buffer |

## 3. Front panel

| Feature | Stock | Evidence |
| --- | --- | --- |
| Display | 3-digit 14-segment: `P<bank><slot>`, `L<bank><slot>`, `d` (rhythm), tuner, 0-100 while a knob moves | [D] §1, [M] |
| Preset mode | A-D select a slot; A+B bank down, C+D bank up | [M] p.13, [D] |
| Live (stomp) mode | B+C toggles preset/live. A = reverb, B = MOD, C = amp + cab, D = comp on/off. C: if amp **or** cab is on, both go off; else both on | [M] p.10-13, [C] 0x9fca (A, `P+0xa4`), 0xa2b6 (B, `P+0x74`), 0xa4ea (C, `P+0x2c`/`0x44`), 0xa6f8 (D, `P+0x14`) |
| Tuner | hold A+B in any mode; same again to exit | [M] p.15, [D] |
| Rhythm (drum) mode | hold B+C until `d`. **A and B change the rhythm, C tapped twice or more sets the BPM (the light flashes the tempo), D starts/stops**; hold B+C again to leave | [M] p.14, [C] 0x9fec (rhythm-mode switch A: pattern - 1, wraps to 39, sends `BA`) |
| Knobs | 16, pickup: a knob acts once it reaches the stored value; its LED blinks until then; LED off when the module is off or in tuner mode | [M] p.13, [D] §2-3 |
| Light bar | 40 RGB LEDs: footswitch status, colour set in the app, blinks on save, flashes the drum tempo | [M] p.6, p.13-14, [C] 0x67e0 (save blink over LEDs 0..39) |
| Light-bar colours | probably `S+0x1b..0x1e` (4 values, one per footswitch, `B7`, > 72 -> 9); **no effect** on the sound | hypothesis; [E] |
| Status LED | green/yellow/red = battery level, red flashing = charge now, solid red = charging, solid green = full | [M] p.6 |
| Test mode | boot with D held (L) | [D] §2 |

## 4. Tuner and drums

| Feature | Stock | Evidence |
| --- | --- | --- |
| Tuner | YIN, A4 430-450 Hz. Calibration `S+0x2c`: A4 = 435 + value (default 5 -> 440) | [C] 0x913c passes `S+0x2c` + 5 to 0x17a48 (cal = A4 - 430) |
| Tuner mute | `S+0x2e` (default 1): 1 = output muted while tuning, 0 = sound passes | [E] `S+0x2d` = 1: output 0 with `S+0x2e` = 1, unchanged with 0 |
| Tuner on | `S+0x2d`, also from the app (`B8`, `B0`) | [D], [C] |
| Drum machine | 40 rhythms, 40-260 BPM, level 0-100, count-in lists; settings at F:0x81000 `[on][?][rhythm][level][bpm u16]`, always boots off | [M] p.14, [D] PROTOCOL §5.6, `dsp/drums.h` |
| Drums from the app | `BA` sets on/rhythm/level/tempo, `C9` rhythm mode | [D] PROTOCOL §5.1 |
| Drums in USB recording | no | [D] (commit 84e9f95) |

## 5. Connectivity and power

| Feature | Stock | Evidence |
| --- | --- | --- |
| Bluetooth audio | BT 5.0 playback from a phone, on/off from the app (`S+0x17`); applied at boot | [M] p.16, [C] 0x190e4 -> 0x1ba7c(`S+0x17`) |
| BLE app | "Flamma Manager": live edit, presets, drums, settings, cloud tones, light-bar colours | [M] p.4, p.17, [D] PROTOCOL §5 |
| BT name | app `B3`, `AT+BD<name> Audio` / `AT+BM<name>`; re-sent at every boot | [D] §4 |
| USB | USB-C: charge or OTG recording to a phone/PC; stock UAC1, 44.1 kHz, 2 in / 2 out; vendor HID for the PC editor (IR import, update) | [M] p.7, [D] |
| OTG recording volume | "adjust via the app"; field not identified (candidates `S+0x24+slot`, `S+0x28+slot`, both **no effect** in the DSP callback) | [M] p.7, [E] |
| Firmware update | PC software: `C1` -> vendor DFU `0483:5703`; A+D at power-on | [D] PROTOCOL §8, BOOTLOADER |
| Battery | 2000 mAh, ~6 h, charge ~2.5 h at 5 V/2 A; 4 levels (ADC9 thresholds at 0x188dc) | [M] p.22, [D] |
| Power-fail save | settings saved from the hold-up capacitors when ADC7 drops | [D] §3 |
| Auto power-off | none | [M] |
| Outputs | 1/4" out (100 ohm), 3.5 mm phones; input 2 Mohm | [M] p.22 |

## 6. Settings block `S` (49 bytes) as far as known

| Offset | Meaning | Default | Evidence |
| --- | --- | --- | --- |
| 0x00 | "B1" magic | | [D] |
| 0x02 | BLE name (20) | | [D] |
| 0x16 | current preset | | [D] |
| 0x17 | Bluetooth audio on | 1 | [D], [C] |
| 0x18 | master volume | | [D] |
| 0x19 | global cab switch (0 forces the cab off) | 1 | [D] PROTOCOL §5.5 |
| 0x1a | input gain index | 13 (0 dB) | [E] |
| 0x1b-0x1e | unknown, probably light-bar colours | 13 | [D], hypothesis |
| 0x1f | live (stomp) mode | 0 | [D] |
| 0x20 | rhythm mode | 0 | [D] |
| 0x21, 0x22 | slot, bank | | [D] |
| 0x23 | unknown (UI state, written in rhythm/live paths) | 0 | [C] |
| 0x24-0x27 | per-slot byte, unknown | 0 | [D] |
| 0x28-0x2b | per-slot level, unknown use | 100 | [D], [E] no effect |
| 0x2c | tuner calibration, A4 = 435 + v | 5 | [C] |
| 0x2d | tuner on | 0 | [D], [E] |
| 0x2e | tuner mute | 1 | [E] |
| 0x2f, 0x30 | unknown | 0 | [C] |
