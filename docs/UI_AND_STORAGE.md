# FB200 user interface, LEDs, Bluetooth and storage

Reverse-engineered from the stock image (Unicorn emulation of the loader and
app with peripheral models, plus static analysis) on 2026-09-27. Addresses
are stock ITCM run addresses unless marked DTCM (`0x2000xxxx`) or `F:` (flash
offset from 0x60000000). Confidence: **H** seen in code and emulation,
**M** in code with meaning inferred, **L** hypothesis. Items marked
**verified** were read back on a real pedal over the USB console.

The SoC close-up photos read **MIMXRT1052 DVL6B**; the ROM API tree also has
the RT1050 layout (`docs/BOOTLOADER.md`). Pads/peripherals used here are the
same on RT1062.

## 1. Display (H): 3-digit 14-segment LED, GPIO-multiplexed

No LCD, no controller, no frame buffer; the "font" is a code switch
(0x16574, 25 glyphs).

- Segments: GPIO4 (pads `GPIO_EMC_16..30`, ALT5, pad 0x10B0), active high:
  IO25 a, IO24 b, IO22 c, IO21 d, IO16 e, IO30 f, IO26 g1, IO20 g2,
  IO18/IO28 centre verticals, IO17 a diagonal (only 'R'), IO23 dp;
  IO19/27/29 never lit (other diagonals?).
- Digit selects (active high): d0 = GPIO4_IO31 (`EMC_31`), d1 = GPIO3_IO18
  (`EMC_32`), d2 = GPIO3_IO21 (`EMC_35`).
- Glyph codes: 0-9 digits, 10 O, 11 A, 12 b, 13 C, 14 d, 15 P, 16 L, 17 -,
  18 dp only, 19 blank, 20 E, 21 F, 22 G, 23 I, 24 R.
- Refresh from SysTick (1 kHz): next digit every 3 ms (~111 Hz frame).
- Screens: preset `P<bank><slot>` (e.g. `P3C`), stomp `L<bank><slot>`,
  rhythm `d`, tuner (note letter, dp = sharp, `-` arrows), 0-100 overlay
  while a knob moves.

## 2. Controls (H)

Footswitches: pads `GPIO_SD_B0_00/01/02` and `GPIO_B1_08`, ALT5, pad
**0xF0B0** (22k pull-up), **active low**.

**Verified on the pedal** (A-D select slots A-D, chords work):

| GPIO / pad | slot | label |
| --- | --- | --- |
| GPIO3_IO12 / SD_B0_00 | 3 | D |
| GPIO3_IO13 / SD_B0_01 | 1 | B |
| GPIO3_IO14 / SD_B0_02 | 2 | C |
| GPIO2_IO24 / B1_08 | 0 | A |

Scanner every 10 ms, 2-sample debounce, long press 1 s then repeat every
100 ms. Stock actions: A-D (on release) select slot; C+D bank up; A+B bank
down (the chords only browse the bank, see `STOCK_FEATURES.md`); B+C
toggles preset/stomp mode; B held + A long = tuner; C held + B long = rhythm
mode; any switch long = save to that slot (H); in stomp mode the switches
toggle modules; boot with D held = test mode (L).

Knobs: 16, through two 74HC4051 muxes. Select lines GPIO2_IO17/18/19
(pads `B1_01/02/03`); outputs ADC1 IN3 (`AD_B0_14`, k0-k7) and IN4
(`AD_B0_15`, k8-k15). One channel per 10 ms (full scan 80 ms), 8 samples,
drop min/max, average; change threshold 48 counts. The notes said the
stock inverts (4095 - v); on the pedal that turned every knob the wrong
way (user report), so our firmware uses the raw reading.

Our display feedback (better than the stock's bare 0-100): touching a knob
shows its name for 0.6 s (OUt, rLE, rEU, nIH, rAt, nOd, CAb, UOL, bAS, nid,
trE, GAn, AnP, CLE, tHr, GAt), then its value; a dot after the value means
the knob has not picked up the preset value yet (it is not acting).

**Measured on the pedal** (the user turned every knob left to right and read
the panel labels; the channel table recovered from the stock code was
wrong, its knob-LED pairing right):

| panel (left -> right) | mux channel | target | knob LED |
| --- | --- | --- | --- |
| MASTER | k15 | master volume (settings+0x18) | 14 |
| LEVEL | k14 | reverb level (+0xaa) | 13 |
| REVERB | k12 | reverb type 0-4 (+0xa6) | 15 |
| MIX | k11 | mod p2 (+0x7a) | 9 |
| RATE | k8 | mod p1 (+0x78) | 4 |
| MOD | k9 | mod type 0-11 (+0x76) | 10 |
| CAB | k13 | cab 1-19 (+0x46; 11-19 user IR) | 5 |
| VOL | k10 | amp volume (+0x3a) | 12 |
| BASS | k4 | +0x32 | 0 |
| MID | k6 | +0x34 | 1 |
| TREBLE | k7 | +0x38 | 2 |
| GAIN | k5 | +0x30 | 11 |
| AMP | k2 | amp model 1-10 (+0x2e) | 3 |
| LEVEL | k1 | comp level (+0x1e) | 6 |
| THRESH | k0 | comp threshold (+0x1a) | 7 |
| GATE | k3 | gate threshold (+0x60) | 8 |

## 3. LEDs

- 16 knob LEDs, inside the knob caps (blue/green/red/blue groups):
  GPIO4_IO0..15 (`EMC_00..15`), active low, **powered only while
  `GPIO_AD_B0_02` (GPIO1_IO2) is high** - verified on the pedal with a
  camera. Stock: on = knob matches the stored value, blink = mismatch, off =
  module off / tuner. Physical order left to right (verified, camera):
  LED `8 7 6 3 11 2 1 0 12 5 10 4 9 15 13 14`, i.e. knobs
  `k2 k7 k0 k1 k4 k6 k5 k3 k8 k11 k15 k14 k9 k10 k12 (?)`; the purple knob
  is master volume (k13, no LED).
- 40 RGB LEDs (WS2812-type, G-R-B): `GPIO_B0_02` ALT4 = FLEXIO2_D02, SDK
  FlexIO UART at 6.6 Mbaud + eDMA, 24 UART chars per LED (0xC0 = 0,
  0xFC = 1), 25 % brightness (the stock sends every byte >> 2).
  **They are rings inside the 4 footswitch domes, 10 per dome** (camera,
  2026-09-28): LEDs 0-9 = footswitch A, 10-19 = B, 20-29 = C, 30-39 = D,
  as the stock code addresses them (checked: selecting slot A lights the
  0-9 dome). The order inside a ring is not mapped: each ring is driven as
  one colour. The white light bars on the panel are separate and not
  driven by these LEDs. Behaviour (stock LED task
  0x67e0, ours `ui/lightbar.c`): `docs/STOCK_FEATURES.md` "Light rings".
- Status RGB LED: GPIO2_IO0/1/3 (`B0_00/01/03`), active low; battery on
  ADC channel 9 (`AD_B1_04`), charger sense GPIO1_IO19 (`AD_B1_03`).
- GPIO2_IO30 (`B1_14`) toggles every 500 ms: heartbeat LED or external
  watchdog (L).
- **GPIO2_IO26 (`B1_10`) / IO31 (`B1_15`) are a latch/mute pair, always
  opposite**; the stock watches ADC channel 7 (`AD_B1_02`) and on a
  power-fail threshold saves settings and flips them. `frontend.c` sets the
  stock's normal running state.
- **The power switch is a hard cut** (verified on the pedal, on USB: switch
  off = full power-on reset, the SRC_GPR crumbs are cleared; ADC7 never
  dipped before the cut at 250 ms polling). The stock's ADC7 path is a
  last-milliseconds save from the hold-up capacitors, which needs the
  settings sector pre-erased. Ours instead writes settings 3 s after any
  change, so a switch-off loses at most the last 3 s of changes and never
  the stored settings.
  `B1_09`, `B1_11`, `AD_B0_02` are set high at init (enables/resets, L).

## 4. Bluetooth (H)

LPUART5 (`B1_12` TX / `B1_13` RX, ALT1), 115200 8N1; also carries the
AA 55 app protocol (BLE transparent UART). Boot AT sequence: `AT+TM`, names
(`AT+BD<name> Audio`, `AT+BM<name>`, name from F:0x83000), `AT+CN00`,
`AT+B501`, `AT+B401`; BT off = `AT+B500` + `AT+CZ`. Jieli "BT201"-style
(L). BT audio arrives digitally on SAI3 (MCU = I2S master, RX only, 32-bit
slots) and is mixed into the output.

## 5. Flash map and storage (H; verified entries read on the pedal)

No wear levelling, CRC or journal; writes are read-modify-write of a 4 KB
sector through FlexSPI IP commands (the stock and ours alike).

Power loss during a write: the sector is erased, then programmed page by
page (about 50 ms in all). Cut in that window, the whole sector is lost:
8 presets (a preset sector holds 8 records at a 0x200 stride), or the global
settings, the rhythm block, the BT name, the IR names or flags, or 4 kB of an
IR. Our firmware checks what it reads before it plays it
(`src/preset/preset_check.c`, `stock_check()`): an erased preset loads as the
stock blank "EMPTY" preset, out-of-range fields of a preset are clamped to the
stock's app-write limits (a reverb decay of 655 % made the reverb run away),
erased settings load the stock defaults (master 0), an out-of-range setting
its default. The flash is not rewritten by these checks, so the stock app and
firmware read the same bytes; the next save stores the checked record. No
crash and no boot loop from any record (fuzzed: `tests/test_fuzz_host.py`).

Not done: a journal or a second copy. It needs free sectors the stock never
touches; 0x76000..0x7F000 and 0xA2000..0xAF000 look unused, but that is not
verified against the stock firmware, and a stock firmware that used them
would read our copy as its data. The stock format itself has no spare room
(a record's second 0x100 bytes share the sector, so they are lost with it).

| F: offset | size | contents |
| --- | --- | --- |
| 0x10000 | 0x31000 | app (block 0; ours: recovery + app slot) |
| 0x41000 | 0x20000 | ours: app const tables |
| 0x61000 | 0x10000 | ours: stock sound data (`FBSD`, written once from the user's `.mr`) |
| 0x71000 + i x 0x200 | 0x100 | 40 presets (10 banks x 4) - **verified** ("Fat Bass", "Clean Pick") |
| 0x80000 | 0x31 | global settings - **verified** ("B1", name, preset, BT, volume) |
| 0x80100 | 4 | ours: power settings (`PW`, idle minutes, LED level; [POWER.md](POWER.md)) |
| 0x81000 | 6 | rhythm settings (tempo 110?) |
| 0x82000 | 0x20 | magic "FB200" - **verified** |
| 0x83000 | 20 | BLE name - **verified** |
| 0x85000 | 0x1E | unknown |
| 0x86000 | 1 | updater flag (0xFF) - **verified** |
| 0x87000 | 9 x 50 | IR names - **verified** ("Empty") |
| 0x88000 | 9 | IR slot used flags |
| 0x89000 + s x 0x2800 | 0x2800 | user IR data |
| 0xB0000 | 8 | magic "B01" - **verified** |
| 0xD0000 | 3286016 | model library (block 1) - **verified** (count 0x14 = 20) |
| 0x50F000 | 0x1000 | ours: kept for a looper `loop save` header (not written yet) |
| 0x510000 | to the chip end (0x2F0000 on 8 MB) | ours: the looper's loops (`loopstore/loopstore.h`; lost at power off) |

Preset record (u16 LE fields): name[20] @0x00; module 0x80 enable/type
0x14/0x16, params 0x18-0x1e; amp 0x2c/0x2e, params 0x30-0x3a; cab
0x44/0x46, params 0x48-0x4e; module 0x81 0x5c-0x60; mod 0x74/0x76, params
0x78-0x80; module 0x85 (delay: en, type, mix, feedback, time ms) 0x8c-0x94, our delay marker and params 0x96-0x9a (PARITY.md M4); reverb 0xa4/0xa6, params
0xa8-0xae; effect order[8] @0xbc; our EQ marker and settings 0xc4-0xdd
(PARITY.md M4); 0xde-0xff unused.

Global settings: +0x00 "B1", +0x02 BLE name, +0x16 current preset, +0x17 BT
on, +0x18 master volume, +0x1f stomp mode, +0x20 rhythm mode, +0x21 slot,
+0x22 bank, +0x24 + slot light-ring colour, +0x28 + slot light-ring level,
+0x2d tuner.

Factory reset writes defaults + 40 factory presets (20 named, 20 "EMPTY")
when a magic is missing.

App protocol (HID/BLE, dispatcher 0x46c4): 0x80-0x86 set a module of the
edit buffer (also sent as notifications), 0x97 store preset, 0x98/0x99
select preset, 0xB3 rename BT, 0xB7/0xB8 settings, 0xBA rhythm, 0xC1/0xC4
enter updater, 0xC9 rhythm mode, 0xFA ping (0xFB). See `PROTOCOL.md`.

## Open (needs the pedal and a person)

Physical knob/LED positions, which footswitch is which, segment geometry and
digit order, status LED colours, the LED order inside a light ring, roles of `B1_09/B1_11/
AD_B0_02/B1_14`, the BT module part and AT semantics.
