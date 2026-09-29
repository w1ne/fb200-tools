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
| `labwired/system.yaml` | the FB200 board: NAU88L21 codec, knob multiplexers and knobs, ADC inputs, FlexIO2 clock, footswitches, Bluetooth module on LPUART5 |
| `labwired/smoke.yaml` | gate for the open smoke firmware |
| `labwired/stock-boot.yaml` | gate for the unmodified vendor firmware, boot to USB (short) |
| `labwired/stock-knobs.yaml` | gate for the vendor firmware: it reads all 16 knobs through the 74HC4051 muxes, and a turned knob (long: 3.4 G cycles) |
| `labwired/stock-first-boot.yaml` | gate for the vendor firmware from a blank flash: factory reset, Bluetooth AT sequence and module replies, a scripted phone (long: about 30 min of CPU time) |
| `firmware/labwired-smoke/` | the open smoke firmware (bare registers, no SDK) |
| `tools/labwired_elf.py` | puts raw blobs into one ARM ELF, one PT_LOAD per blob |
| `tools/labwired_stock.py` | builds `build/labwired/stock.elf` from your `.mr` |

## 2. Get the LabWired CLI

The i.MX RT parts and the `peripheral_log` and `fidelity_clean` assertions
are on core `main` (PRs #1254 and #1255) and are not released yet. The
NAU88L21 codec part and device logs (`peripheral_log` with a device id) are
on core `main` (PR #1272). The Bluetooth module (`bt201`) needs core PR #1274
(branch `feat/bt201-module`) until it is merged. Until the next core release,
build the CLI from `main`:

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

**The vendor image is not redistributable.** Never commit `fb200-stock.mr`,
`stock.elf` or any block extracted from it. `build/` and `*.mr` are in
`.gitignore`.

## 4. Run the gates

```bash
labwired test --script labwired/smoke.yaml
labwired test --script labwired/stock-boot.yaml
labwired test --script labwired/stock-knobs.yaml        # long, see 5
labwired test --script labwired/stock-first-boot.yaml   # long, see 5
```

Expected result:

```
PASS  4/4 checks · smoke · 2000000 steps · 0.27s
PASS  37/37 checks · stock-boot · 90000000 steps · 15.26s
PASS  20/20 checks · stock-knobs · 3400000000 steps · 4754.33s
PASS  23/23 checks · stock-first-boot · 6800000000 steps · 8190.29s
```

The stock gate asserts `fidelity_clean: true`: an unmapped MMIO access or an
undecoded instruction anywhere in the run fails it (the gaps are also in
`result.json`, key `fidelity`). Use `--output-dir DIR` to keep the artifacts.

A nightly CI job with the released `labwired-test` action follows after the
next core release. The stock gate cannot run in public CI, because it needs
the vendor image.

## 5. What the gates prove

### Smoke gate

The open smoke firmware boots from FlexSPI at `0x60010000`, prints
`RT1052 SMOKE OK` on LPUART5, makes knob LED 1 (GPIO4_IO00) an output and
toggles it. The gate reads GPIO4 GDIR and DR, which printed text cannot fake.

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

The only board part not modelled yet is the 14-segment display. The knobs
and their multiplexers are modelled (see the stock knob gate below), and so
is the Bluetooth module (see the first-boot gate). The stock boot gate does
not see the knobs: the firmware scans them only in its main loop, after
3.2 G cycles.

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
On a busy Mac it took 61 min of wall time. It is not in the default loop.

Timeline, measured on the twin (SysTick is 1 ms = 600 000 cycles):

| Cycles | Device time | What the firmware does |
|--------|-------------|------------------------|
| 0 .. 0.09 G | 0 .. 0.15 s | boot to USB enumeration (the stock boot gate) |
| 0.03 .. 1.40 G | 0.05 .. 2.3 s | factory reset: storage format through FlexSPI IP commands (sector erase `0x20`, quad page program `0x32`, status poll `0x05`) |
| 1.40 .. 3.20 G | 2.3 .. 5.3 s | a fixed `delay_ms(3000)` before the main loop (ITCM `0x17774`; the wait loop is `0x1A01A..0x1A020`, it polls the SysTick ms counter) |
| 3.20 .. 6.15 G | 5.3 .. 10.3 s | main loop; a software countdown starts the Bluetooth bring-up |
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
(`FAIL 2/11`; only `fidelity_clean` and the stop reason pass).

Notes on `result.json` for long runs:

- `metrics.exceptions` counts only exceptions that end the run with an
  error. It is not an interrupt count. It is 0 in a good run; SysTick, SAI1
  and GPT1 interrupts are taken.
- `cycles` in `result.json` is the sum of the instruction cost model
  (for example 1 for MOV, 2 for BL). `max_cycles` and all device time
  (SysTick, peripheral timers) use the machine clock, which is
  `stop_reason_details.observed`. A `max_cycles: 3000000000` run reports
  `cycles` of about 3.67 G.

## 6. SVD provenance

The files in `labwired/chip/peripherals/` and the base addresses and IRQs in
`labwired/chip/mimxrt1052.yaml` come from the NXP CMSIS SVD for the
MIMXRT1052 (vendor `nxp.com`, version 1.0, device MIMXRT1052DVL6B,
BSD-3-Clause, from the NXP MCUXpresso SDK). The SVD is not in this
repository. The copy used was vendored in labwired-core at commit `da773a4d`
as `tests/fixtures/real_world/mimxrt1052.svd`:

- source: `https://github.com/w1ne/labwired-core/blob/da773a4d/tests/fixtures/real_world/mimxrt1052.svd`
- sha256: `8151227fba04784dfb2c0750d87cc641866b7f3801324fc6c20772560869ff40`
