# LabWired Twin of the FB200

A LabWired twin is a simulated FB200 board: the MIMXRT1052 chip, the board
wiring and a test gate, all as YAML in this repository. The simulator runs
firmware ELF files on it and checks the result.

The twin lives in this repository. LabWired core supplies only generic parts
(the `imxrt_*` peripheral models, the Cortex-M7 CPU, the test runner). Core
has no FB200 special cases.

## 1. Files

| File | What it is |
|------|------------|
| `labwired/chip/mimxrt1052.yaml` | the chip: memory map, pins, peripherals |
| `labwired/chip/peripherals/*.yaml` | register files ingested from the NXP SVD |
| `labwired/system.yaml` | the FB200 board: NAU88L21 codec, knob multiplexers and knobs, 14-segment display, ADC inputs, FlexIO2 clock, footswitches, Bluetooth module on LPUART5 |
| `labwired/smoke.yaml` | gate for the open smoke firmware |
| `labwired/stock-boot.yaml` | gate for the unmodified vendor firmware, boot to USB (short) |
| `labwired/stock-knobs.yaml` | gate for the vendor firmware: it reads all 16 knobs through the 74HC4051 muxes, and a turned knob (long: 3.4 G cycles) |
| `labwired/stock-first-boot.yaml` | gate for the vendor firmware from a blank flash: factory reset, Bluetooth AT sequence and module replies, a scripted phone (long: about 30 min of CPU time) |
| `firmware/labwired-smoke/` | the open smoke firmware (bare registers, no SDK) |
| `firmware/audio/` | the open pedal firmware (recovery and the app) |
| `tools/labwired_elf.py` | puts raw blobs into one ARM ELF, one PT_LOAD per blob |
| `tools/labwired_stock.py` | builds `build/labwired/stock.elf` from your `.mr` |
| `tools/labwired_open_fw.py` | builds `build/labwired/open.elf` and writes `build/labwired/open-boot.yaml` with the knob-table address |
| `labwired/open-boot.yaml` | gate template for the open firmware: USB product string, display text, one footswitch, codec setup, Bluetooth startup, 16 raw knob counts, and one turned knob |

## 2. Get the LabWired CLI

The i.MX RT parts, the `peripheral_log` and `fidelity_clean` assertions,
the NAU88L21 codec part with device logs (PR #1272), the 14-segment
display part `segment-display-mux` (PR
[#1273](https://github.com/w1ne/labwired-core/pull/1273)) and the BT201
Bluetooth module `bt201` (PR #1274) are on core `main` and are not
released yet. This image also needs two interpreter fixes. A store drops
a cached decode of the instruction bytes it overwrites. A VLDR or VSTR
literal uses Align(PC+4, 4). A released `labwired` binary does not boot
this image. Until the next core release, build the CLI from a core
commit that contains those fixes:

```bash
git clone https://github.com/w1ne/labwired-core.git
cd labwired-core
cargo build --release -p labwired-cli
export PATH="$PWD/target/release:$PATH"   # gives `labwired`
```

After the release, use the released `labwired` binary.

## 3. Build the firmware

Smoke firmware (needs `arm-none-eabi-gcc`):

```bash
make -C firmware/labwired-smoke
```

Stock ELF, from your own copy of the vendor image:

```bash
python3 tools/labwired_stock.py path/to/fb200-stock.mr
# or: FB200_STOCK_MR=path/to/fb200-stock.mr python3 tools/labwired_stock.py
```

The script reads the `.mr` with `fb200.firmware.MrFile` and writes
`build/labwired/stock.elf`: block 0 (application) at `0x60010000`, block 1
(model library) at `0x600D0000`. The entry is block 0's reset vector
(`0x600104D9`).

Open firmware (recovery and the app). This image has no vendor bytes:

```bash
make -C firmware/audio build VARIANT=recovery
make -C firmware/audio build VARIANT=app
python3 tools/labwired_open_fw.py
```

The script writes `build/labwired/open.elf`. Recovery sits at `0x60010000`.
The app slot sits at `0x60020000`. The reset vector still points at
`0x600104D9`. The script writes a short loader at that address. The loader
copies the recovery program into ITCM and branches to it. Recovery then
launches the app.

The script also reads the `knobs` symbol in `fb200-app.elf`. It writes
`build/labwired/open-boot.yaml`. Run that file. The
template `labwired/open-boot.yaml` names the array. The tool fills in the
address for this build. The linker moves the array on every build.

**The vendor image is not redistributable.** Never commit `fb200-stock.mr`,
`stock.elf` or any block extracted from it. `build/` and `*.mr` are in
`.gitignore`.

## 4. Run the gates

```bash
labwired test --script labwired/smoke.yaml
labwired test --script build/labwired/open-boot.yaml   # open firmware, see 5
labwired test --script labwired/stock-boot.yaml
labwired test --script labwired/stock-knobs.yaml        # long, see 5
labwired test --script labwired/stock-first-boot.yaml   # long, see 5
```

Expected result:

```
PASS  5/5 checks · smoke · 40000000 steps · 36.87s
PASS  29/29 checks · open-boot · 640000000 steps · 30.40s
PASS  37/37 checks · stock-boot · 90000000 steps · 15.26s
PASS  20/20 checks · stock-knobs · 3400000000 steps · 4754.33s
PASS  24/24 checks · stock-first-boot · 6800000000 steps · 6031.15s
```

The stock gate asserts `fidelity_clean: true`: an unmapped MMIO access or an
undecoded instruction anywhere in the run fails it (the gaps are also in
`result.json`, key `fidelity`). Use `--output-dir DIR` to keep the artifacts.

The `open-firmware` job in `.github/workflows/ci.yml` builds this image and
runs the open gate on every push and every pull request. The job builds
the CLI from the pinned core commit. The stock gate cannot run in public
CI, because it needs the vendor image.

## 5. What the gates prove

### Open firmware gate

The open gate runs `firmware/audio`. Recovery starts, checks the app slot,
and launches the app. The simulated USB host reads the string descriptors.
The product string is `FB200 Audio`. The stock image sends `FB200`.
`fidelity_clean: true` rejects an unmapped access or an undecoded instruction.

The display shows the name of the MASTER knob. The display model reads
the letter O as the digit 0. The text log contains `0Ut`.

At 100 M cycles the gate presses footswitch B. At 140 M cycles the gate
releases footswitch B. The app loads slot B. The display then shows `P0b`.

The app writes codec register `0x001C` with the value `0x0002`. This
value selects 16-bit I2S. The state log contains `dai slave i2s 16-bit`.
The state log contains `adcout driven`. The state log contains
`enable dac_l dac_r adc_l adc_r`.

The Bluetooth module answers `AT+TM` with `TM+BT201-BLE`. The module
answers `AT+CN00`. The module answers `AT+B501`. The module answers
`AT+B401`. The run stops at 640 M cycles. The stop is after `AT+B401`.

Looper mode needs footswitch D held and footswitch C held for about one
second. That hold is longer than this budget. The gate does not check a
loop record.

The app stores one raw ADC count per knob. The count is a `uint16_t` in
the array `knobs`. A count of 0 is fully counter-clockwise. The firmware
does not subtract the count from 4095. The board starts each knob at
`8 + 5 N` %. Those 16 counts are all different. None of them is the
mid-scale count 2048.

At 80 M cycles the gate turns the MASTER knob from 83 % to 20 %. The app
scans that knob again. At 640 M cycles the stored count is 819 (`0x333`).
The start count 3399 (`0xd47`) is no longer in that slot. The run stops
at `max_cycles` 640000000.

The gate sets `peripheral_tick_interval` to 16. The machine ticks
peripherals every 16 CPU cycles. Two consecutive runs reported the same
29 checks and the same stop reason, `max_cycles`. The first run printed
this line. The second run took 30.83 s.

```
PASS  29/29 checks · open-boot · 640000000 steps · 30.40s
```

The runs used a local core build. That core is not released yet.

### Smoke gate

The open smoke firmware boots from FlexSPI at `0x60010000`, prints
`RT1052 SMOKE OK` on LPUART5, makes knob LED 1 (GPIO4_IO00) an output and
toggles it. The gate reads GPIO4 GDIR and DR, which printed text cannot fake.

Then the firmware multiplexes `LAb` on the 14-segment display, the way the
open firmware's display driver does (all selects off, segments, next
select), about 0.1 ms per digit. The gate reads the text from the display
model's `text` log:

```yaml
- peripheral_log: {peripheral: display, log: text, contains: "\"LAb\""}
```

The display runs for 40 M cycles, because the model decides what is visible
once per 20 ms window (12 M cycles).

Negative control: with `"LAB"` in place of `"LAb"` the gate fails that
check (`FAIL 4/5`): `B` and `b` are different glyphs.

### Stock boot gate

The unmodified vendor firmware V1.0.1 runs for 90 M cycles from a cold
start, through its own loader, to USB enumeration. The chip boot ROM and the
FB200 bootloader (`0x60000000..0x60008000`) are not simulated: the run starts
at the application's vector table, where the bootloader hands over
(see [`FIRMWARE_BRINGUP.md`](FIRMWARE_BRINGUP.md)).

Each `memory_value` assertion reads a register that the firmware or the
simulated USB host changed from its reset value. Each `peripheral_log`
assertion reads a log that a part model records: the FlexSPI IP commands,
the I2C bus trace, the FlexIO shifter words, the simulated USB host. The
expected values are register facts from the reference manual and USB
descriptor values, not bytes of the vendor image.

| Stage | What | Covered |
|-------|------|---------|
| 1 | loader copies its code to ITCM and enters ITCM `0x4D6` | indirect only (see below) |
| 2 | clocks: VDD_SOC raised (DCDC REG3.TRG), DCDC STS_DC_OK, ARM PLL powered and locked at DIV_SELECT 100 | yes |
| 3 | FlexSPI driver reads the NOR JEDEC ID (`0x9F`) and quad-reads (`0x6B`) the configuration sector at `0xB0000` | yes (FlexSPI `ip` log) |
| 4 | LPI2C1 finds the NAU88L21 at `0x54`, reads its ID and writes its 76-register init table, each write read back | yes (MSR, the bus trace, the codec's `reads`, `writes` and `state` logs) |
| 5 | WS2812 frame on FlexIO2 through eDMA: at least 960 8-bit words on pin 2, encoded `0xC0` / `0xFC` | yes (FlexIO `wire` log) |
| 6 | USB1 enumeration: device mode, running, port enabled at high speed, DEVICEADDR = 5, endpoints 1/4/5 enabled; VID:PID `34DB:800F`, HID interface 3, product string `FB200` | yes (registers and USB `host` log) |
| 7 | 14-segment display pins are outputs (GPIO4 16..31, GPIO3 18 and 21) | yes |
| 8 | no unmapped MMIO, no undecoded instruction; no memory violation or decode error | yes (`fidelity_clean: true`, stop reason `max_cycles`) |

Not covered: **stage 1.** To check the ITCM copy, the gate must compare ITCM
words with bytes of the vendor image. Those bytes cannot go in this
repository. All later stages run from ITCM, so a failed copy fails them too.

The stage 5 check counts words and finds both encodings; it does not prove
that every word is `0xC0` or `0xFC`.

The 40 WS2812 LEDs are not modelled (stage 5 checks their FlexIO2 frame).
The knobs and their
multiplexers are modelled (see the stock knob gate below), and so are the
14-segment display (section 6) and the Bluetooth module (see the first-boot
gate). The stock boot gate sees none of them: the firmware scans the knobs
and refreshes the display only in its main loop, after 3.2 G cycles; the
first-boot gate checks the display and the Bluetooth module.

#### Stage 4: the codec

The NAU88L21 is the generic core part `nau88l21` (control port and register
file, reset values from the datasheet Rev 3.3 section 10), attached in
`system.yaml` as `codec` on `lpi2c1` at `0x54` (CSB high). The gate reads
the codec's own logs by that id (`peripheral_log: {peripheral: codec, ...}`).

What the unmodified firmware does on the twin, at 60.75 M .. 64.66 M cycles
(about 6.5 ms):

1. Probe: read R58 `I2C_DEVICE_ID` (the codec returns `0x1A20`).
2. Write R00 = `0x0000` (reset), then 75 more registers in address order,
   each one read back after the write: R01 = `0x0FFF` (DAC and ADC L/R on),
   R03 = `0x0050`, R1C = `0x000E` (I2S, 32-bit), R1D = `0x0000` (clock
   slave, ADCOUT driven), R4B = `0x2007` (class G), R66 = `0x0060`,
   R72 = `0x0170`, R73 = `0x3308`, R74 = `0x0502`, R76 = `0x3140`,
   R7E = `0x0101`, R7F = `0xC03F`, R80 = `0x0720`, and the others. This is
   the table the open firmware replays (`firmware/audio/src/audio/codec.c`);
   only R1C differs there (16-bit).

Without the codec the firmware sent three transfers to `0x54` (write, read,
write), got a NACK on each and went on. With the codec it does the init
above. Nothing else changes, in the 90 M cycles of the gate and also in a
700 M-cycle probe run: SAI1 and SAI3 stay disabled (TCSR/RCSR at reset), and
the GPIO1..3 data, eDMA ERQ and IOMUXC_GPR1 values are the same with and
without the codec. The stock does not wait on the codec or retry it.
The codec is configured before audio starts; the SAI data path is not
modelled.

Negative control: the same run with the codec strapped to `0x1B`
(`i2c_address: 0x1b`) fails every stage 4 check (`FAIL 22/37`); only the
other stages pass.

### Knobs: two 74HC4051 multiplexers

`system.yaml` models the 16 knobs as generic core parts: a `potentiometer`
per knob, behind two `74hc4051` analog multiplexers (core descriptor
`configs/devices/74hc4051.yaml`, primitive `analog_mux`). Wiring, from
[`UI_AND_STORAGE.md`](UI_AND_STORAGE.md) section 2 and cross-checked in the
stock code and in our firmware (`firmware/audio/src/ui/controls.c`):

| Signal | Pad | GPIO | Notes |
|--------|-----|------|-------|
| S0 / S1 / S2 (both muxes) | `GPIO_B1_01/02/03` | GPIO2_IO17/18/19 | the stock select routine (ITCM `0x1BAD0`) writes these three pins only |
| mux A Z | `GPIO_AD_B0_14` | ADC1 IN3 | knobs k0..k7 on Y0..Y7 |
| mux B Z | `GPIO_AD_B0_15` | ADC1 IN4 | knobs k8..k15 on Y0..Y7 |
| E (enable) | - | - | no firmware write: tied to GND (no `e_pin`) |

Knob `kN` is mux channel `N mod 8`; the panel names come from the measured
table in `UI_AND_STORAGE.md`. Each knob starts at a distinct position,
`8 + 5 N` %, so a gate can tell every knob apart. No knob starts at 50 %,
the fixed mid-scale level that the twin used before the knobs existed.
`position` is in %, 0 = fully left. Turn a knob in a test script with a stimulus on its id, for
example `target: { component: "knob_k15_master", channel: "position" }`.

The multiplexer re-reads its select pads inside every GPIO register write,
so the conversion that the firmware starts two instructions after the
select write converts the new channel. The other ADC1 inputs (IN7 supply,
IN9 battery) stay fixed levels.

### Stock knob gate (long)

The stock firmware scans the knobs only from its main loop. On a blank
flash the main loop starts at about 3.22 G cycles (factory reset, then a
fixed 3 s delay, see the first-boot timeline below), so this gate runs
3.4 G cycles. It took 34 to 79 min of wall time on an Apple M4 (three such runs in
parallel, on a Mac with other builds running). It is not in the default
loop.

**Where the firmware keeps the knob values, and how this was found.**

1. Static: two functions load the ADC1 base (`0x400C4000`): the ADC init
   (ITCM `0x19DA8`) and a blocking read at ITCM `0x19D7C` (write HC0, poll
   HS.COCO0, read R0). The read has four callers: channel 9 (battery,
   `0x18900`), channel 7 (supply, `0x19558`) and channels 3 and 4 in one
   function, ITCM `0x1B538`,
   called from the main loop every 10 ms. Per call it takes scan slot
   `i` (a counter), sets the select lines for channel `order[i]` (routine
   ITCM `0x1BAD0`, GPIO2 pins 17..19 only), converts IN3 and IN4 8 times
   each, drops the minimum and the maximum, averages the other 6, and
   stores `4095 - average` (0 below 10, 4095 above 4085) as u16 at
   DTCM `0x2001DED6 + 2 i` (IN3) and `0x2001DEE6 + 2 i` (IN4). With
   `--watch-gpio gpio2:17` (and 18, 19) the select pins are set once at
   0.74 M cycles (init) and then do not move until 3.22 G cycles, when the
   main loop starts to scan.
2. Dynamic: a 3.4 G-cycle run with the knobs at the distinct positions
   above and `memory_value` probes on both tables. The failed probes
   reported the values the firmware wrote (`Memory assertion failed ...
   got`). Every value was an inverted knob reading, and slot `i` held
   mux channel `(i + 1) mod 8`: the scan order table is `1..7, 0`, not
   `0..7`. Slot 6 of IN4 held MASTER after the stimulus had turned it.
   The inversion (`4095 - v`) confirms the note in `UI_AND_STORAGE.md`.

A second function (ITCM `0x18838`) copies each slot to a filtered table at
DTCM `0x2001DE84` when it moved by more than 48 counts. The gate does not
assert it: its mux-B half is rotated by a free-running counter.

**What the gate asserts.** ADC1 in 12-bit mode, the select pins as outputs,
and all 16 slots with the value of the knob that the table above puts
there. The expected value of a knob at P % is
`4095 - round(trunc(3300 P / 100) mV * 4095 / 3300)`: potentiometer and
ADC arithmetic, not bytes of the vendor image. At 3.30 G cycles a stimulus
turns MASTER (k15) from 83 % to 20 %; at 3.40 G its slot must hold
`0xCCC` (20 %), not `0x2B8` (83 %).

Negative controls (same 3.4 G-cycle run):

| Change | Result |
|--------|--------|
| no stimulus (MASTER stays at 83 %) | `FAIL 19/20`: only MASTER's slot fails, `expected 0xccc, got 0x2b8` |
| no muxes and no knobs: ADC1 IN3/IN4 fixed at 1650 mV, as before this change | `FAIL 4/20`: all 16 slots fail, each `got 0x7ff` (mid-scale) |

The stock firmware does not care what drives IN3/IN4: with fixed levels it
stores one mid-scale value in every slot. Only the per-knob positions,
routed by the select lines, give each slot its own value.

### Stock first-boot gate (long)

The unmodified vendor firmware boots from a blank flash (all `0xFF`), as a
board fresh from the factory, for 6.8 G cycles (11.3 s of device time at
600 MHz). It needs about 29 min of CPU time (Apple M4) and 1.1 GB of
memory at peak (most of it is the text of the FlexSPI `ip` log at the end
of the run: about 12 M status-poll lines).
On a busy Mac it took 61 min of wall time, and 142 min with the display
model while two other simulations ran (`wall_time_ms` is 4 h). It is not in
the default loop.

Timeline, measured on the twin (SysTick is 1 ms = 600 000 cycles):

| Cycles | Device time | What the firmware does |
|--------|-------------|------------------------|
| 0 .. 0.09 G | 0 .. 0.15 s | boot to USB enumeration (the stock boot gate) |
| 0.03 .. 1.40 G | 0.05 .. 2.3 s | factory reset: storage format through FlexSPI IP commands (sector erase `0x20`, quad page program `0x32`, status poll `0x05`) |
| 1.40 .. 3.20 G | 2.3 .. 5.3 s | a fixed `delay_ms(3000)` before the main loop (ITCM `0x17774`; the wait loop is `0x1A01A..0x1A020`, it polls the SysTick ms counter) |
| 3.20 .. 6.15 G | 5.3 .. 10.3 s | main loop; a software countdown starts the Bluetooth bring-up |
| 3.23 G | 5.4 s | the display shows `P.0.A.` (then `0.8.3.` at 3.42 G, the knob overlay) |
| 6.15 .. 6.70 G | 10.3 .. 11.2 s | Bluetooth AT sequence on LPUART5, 150 ms apart |

A run that stops before 6.2 G cycles sees no AT command. (A 3 G probe
stopped inside the 3 s delay. That is why its `uart.log` was empty.)

| What | Assertion |
|------|-----------|
| sector erase and page program of the settings sector F:0x82000 and of F:0xB0000 | `peripheral_log` FlexSPI `ip`: `cmd 0x20 addr 0x00082000`, `cmd 0x32 addr 0x00082000`, same for `0x000b0000` |
| magic `FB200` at F:0x82000 and `B01` at F:0xB0000 | `memory_value` at `0x60082000`, `0x60082004`, `0x600B0000` (the NOR array, read through the FlexSPI AHB window) |
| Bluetooth AT sequence `AT+TM`, `AT+BD..`, `AT+BM..`, `AT+CN00`, `AT+B501`, `AT+B401`, in this order | `uart_ordered`, `uart_contains "AT+B401"` |
| the module got each command and answered as the BT201 manual says (`TM+BT201-BLE`, then `OK` for each setting) | `peripheral_log` LPUART5 `at`: `AT+TM -> TM+BT201-BLE`, `AT+BDFB200 Audio -> OK`, ... |
| a phone connects (script, 6.72 G): classic `TS+01`, BLE `TL+03` | `peripheral_log` LPUART5 `link` |
| the firmware parsed the module's `TS+01`: DTCM `0x2000782D` = `'1'` (the display-dot state, UI_AND_STORAGE.md section 4) | `memory_value` `0x2000782C` mask `0xFF00` = `0x3100` |
| the phone sends "get version" over BLE (6.73 G); the firmware answers on the BLE transport and the module sends the answer to the phone | `peripheral_log` LPUART5 `air`: `phone->mcu aa 55 01 00 00 c8 cf`, `mcu->phone aa 55 38 00 01 46 42 32 30 30 00` |
| the first display screen of the main loop, `P.0.A.` | `peripheral_log` display `text`: `"P.0.A."` |
| no fidelity gap, run not stopped early | `fidelity_clean`, stop reason `max_cycles` |

The Bluetooth module is the core `bt201` part (system.yaml, id `bt`): a
BT201 (Jieli KT1025A) as its V2.3 manual describes it. It answers the AT
commands, pushes its link status (`TS+..`, `TL+..`) and passes BLE data
through while a phone is connected. The phone is scripted in the gate:
`stimuli` set the links (`edr_link`, `ble_link`) and `uart_injections` with
`device: bt` is data the phone writes. The frame bytes in the gate are
`fb200.protocol.pack_frame` output; `tests/test_labwired_gates.py` checks
that. The console connector stays as a tap: `uart.log` has what the
firmware sent to the module.

What the stock does with the module's answers (UI_AND_STORAGE.md section 4):
it parses `TS+nn` / `TL+nn` in its LPUART5 RX handler, and the `TS` state
drives the display dot. It does not check `OK`. It compares the `AT+TM`
answer with `FB200` to skip the name commands, which never matches a BT201
(`TM+<name>`), so it sends the whole sequence at every boot.

Negative control for the Bluetooth checks (2026-09-29): the same run
without the phone stimuli and with `AT+TM -> TM+FB200FB200` expected (a name
the module uses only after a reset) fails exactly the six checks that need
the module or the phone (`FAIL 17/23`): the `AT+TM` answer, `TS+01` and
`TL+03`, the `TS` byte (still `'0'`, from the module's `TS+00` at power-on),
and both `air` lines (the phone's frame is logged `phone->mcu dropped (no ble
link)`). The passing run took 8190 s of wall time (2033 s CPU, 1.15 GB peak)
on a loaded Mac.

Negative control (earlier): the same run with each new expected value changed (an
erase and a program of F:0x10000 and F:0xB1000, `FB21`, `1`, `B02`,
`AT+BD` before `AT+TM`, `AT+B402`) fails all nine of these checks
(`FAIL 2/11`; only `fidelity_clean` and the stop reason pass). The same
run with `"P0A"` (the text without the dp) in addition to `"P.0.A."` fails
only that check (`FAIL 12/13`), and the message shows the three lines of
the `text` log.

Notes on `result.json` for long runs:

- `metrics.exceptions` counts only exceptions that end the run with an
  error. It is not an interrupt count. It is 0 in a good run; SysTick, SAI1
  and GPT1 interrupts are taken.
- `cycles` in `result.json` is the sum of the instruction cost model
  (for example 1 for MOV, 2 for BL). `max_cycles` and all device time
  (SysTick, peripheral timers) use the machine clock, which is
  `stop_reason_details.observed`. A `max_cycles: 3000000000` run reports
  `cycles` of about 3.67 G.

## 6. The 14-segment display

`labwired/system.yaml` places the core part `segment-display-mux` with the
id `display`:

- segments: GPIO4_IO16..30 (pads `GPIO_EMC_16..30`), active high;
- digit selects, left to right: GPIO4_IO31, GPIO3_IO18, GPIO3_IO21 (pads
  `GPIO_EMC_31`, `GPIO_EMC_32`, `GPIO_EMC_35`), active high;
- segment names per pin and the glyphs: from
  [`UI_AND_STORAGE.md`](UI_AND_STORAGE.md) section 1 and the font of the open
  firmware (`firmware/audio/src/ui/display.c`), which copies the stock font
  (ITCM `0x16574`). Glyphs with the same segments decode to the first one:
  `O` shows as `0`, `S` as `5`, `B` as `b`, `D` as `d`.

The model lights a segment LED only while its segment line AND its digit
select are active. It integrates the lit time exactly, per GPIO store, and
every 20 ms decides what a human sees: an LED is visible when it is lit at
least 50 % as long as the brightest LED, and at least 1 % of the window. A
short ghost stays dark; a display that is not refreshed goes blank. An
unknown pattern shows as `?`, a lit `dp` as `.` after the character.

It records two logs for `peripheral_log`:

| Log | One line per | Example |
|-----|--------------|---------|
| `text` | change of the visible text | `"LAb" at cycle 12000001` |
| `frames` | change of the visible segment masks (bit = IO16..IO30) | `0x4021 0x4751 0x4471 at cycle 12000001` |

A failed `peripheral_log` check prints the last lines of the log.

### What the stock firmware shows

In the first-boot run (codec, knobs and display all attached) the `text` log
has six lines; a failed check prints the last five:

```
"P.0.A." at cycle 3240000001
"?.0.A." at cycle 3408000001
"0.8.3." at cycle 3420000001
"0.7.7." at cycle 5820000001
"0.8.3." at cycle 5844000001
```

The first line (not shown) is the screen's first window at 3.228 G. Before
the codec was modelled, `"P.0.A."` came at exactly 3228000001 and nothing
changed after 3.42 G.

- `P0A` is preset bank 0, slot A (`P<bank><slot>`, UI_AND_STORAGE.md
  section 1). 180 ms later the firmware shows `083`: the 0-100 overlay of
  a knob that moved. The knob scan starts with the main loop and the first
  scan reads MASTER (k15) at its start position, 83 % (see the knob
  section). Before the knobs were modelled, all knobs read mid-scale and
  this screen was `050`.
- `077` for 24 ms at 5.82 G is not explained yet: no knob starts at 77 %
  (starts are `8 + 5 N`), and the stimulus that turns MASTER is only in the
  knob gate.
- The dp is on after every digit. This is what the firmware drives, not a
  model error: the stock glyph writer (the code switch) never writes the dp
  line GPIO4_IO23; a separate routine holds it high in this mode (and blinks
  it, or clears it, in others; the tuner uses it as the sharp sign). The
  model shows a segment that is lit under every select.
- `?.0.A.` is one 20 ms window in which the screen changed from `P0A` to
  `083`: the two texts share the window, and some segments are lit for less
  than half of it. It is visible for 20 ms only.

A run with a second display instance at `threshold_pct: 95` confirms that
the dp is driven, not a ghost: in every window the dp is visible together
with the segments of its digit, at the same duty.

The same run shows a limit of the 20 ms window. The stock firmware lights
each digit for 3 ms, so a window holds 6.67 digit slots: one digit gets 3
slots, the other two get 2, and they measure at 67 % of the brightest. At a
95 % threshold only one digit is visible per window (`" 5. "`, `"  0."`,
`"0.  "`). Keep `threshold_pct` below 67 for this display; the default, 50,
gives a stable text.

## 7. SVD provenance

The files in `labwired/chip/peripherals/` and the base addresses and IRQs in
`labwired/chip/mimxrt1052.yaml` come from the NXP CMSIS SVD for the
MIMXRT1052 (vendor `nxp.com`, version 1.0, device MIMXRT1052DVL6B,
BSD-3-Clause, from the NXP MCUXpresso SDK). The SVD is not in this
repository. The copy used was vendored in labwired-core at commit `da773a4d`
as `tests/fixtures/real_world/mimxrt1052.svd`:

- source: `https://github.com/w1ne/labwired-core/blob/da773a4d/tests/fixtures/real_world/mimxrt1052.svd`
- sha256: `8151227fba04784dfb2c0750d87cc641866b7f3801324fc6c20772560869ff40`
