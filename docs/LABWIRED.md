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
| `labwired/system.yaml` | the FB200 board: ADC inputs, FlexIO2 clock, footswitches, UART, 14-segment display |
| `labwired/smoke.yaml` | gate for the open smoke firmware |
| `labwired/stock-boot.yaml` | gate for the unmodified vendor firmware, boot to USB (short) |
| `labwired/stock-first-boot.yaml` | gate for the vendor firmware from a blank flash: factory reset and Bluetooth AT sequence (long: about 30 min of CPU time) |
| `firmware/labwired-smoke/` | the open smoke firmware (bare registers, no SDK) |
| `tools/labwired_elf.py` | puts raw blobs into one ARM ELF, one PT_LOAD per blob |
| `tools/labwired_stock.py` | builds `build/labwired/stock.elf` from your `.mr` |

## 2. Get the LabWired CLI

The i.MX RT parts and the `peripheral_log` and `fidelity_clean` assertions
are on core `main` (PRs #1254 and #1255) and are not released yet. The
14-segment display model (`segment-display-mux`) needs core PR
[#1273](https://github.com/w1ne/labwired-core/pull/1273). Until the
next core release, build the CLI from `main`:

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
labwired test --script labwired/stock-first-boot.yaml   # long, see 5
```

Expected result:

```
PASS  5/5 checks · smoke · 40000000 steps · SMOKE_TIME_PLACEHOLDER
PASS  24/24 checks · stock-boot · 90000000 steps · 15.26s
PASS  11/11 checks · stock-first-boot · 6800000000 steps · 3669.59s
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

Then the firmware multiplexes `LAb` on the 14-segment display, the way the
open firmware's display driver does (all selects off, segments, next
select), about 0.1 ms per digit. The gate reads the text from the display
model's `text` log:

```yaml
- peripheral_log: {peripheral: display, log: text, contains: "\"LAb\""}
```

The display runs for 40 M cycles, because the model decides what is visible
once per 20 ms window (12 M cycles).

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
| 4 | LPI2C1 probes the NAU88L21 at `0x54` and gets a NACK | yes (MSR.NDF and the bus trace `addr 0x54 W nack`) |
| 5 | WS2812 frame on FlexIO2 through eDMA: at least 960 8-bit words on pin 2, encoded `0xC0` / `0xFC` | yes (FlexIO `wire` log) |
| 6 | USB1 enumeration: device mode, running, port enabled at high speed, DEVICEADDR = 5, endpoints 1/4/5 enabled; VID:PID `34DB:800F`, HID interface 3, product string `FB200` | yes (registers and USB `host` log) |
| 7 | 14-segment display pins are outputs (GPIO4 16..31, GPIO3 18 and 21) | yes |
| 8 | no unmapped MMIO, no undecoded instruction; no memory violation or decode error | yes (`fidelity_clean: true`, stop reason `max_cycles`) |

Not covered: **stage 1.** To check the ITCM copy, the gate must compare ITCM
words with bytes of the vendor image. Those bytes cannot go in this
repository. All later stages run from ITCM, so a failed copy fails them too.

The stage 5 check counts words and finds both encodings; it does not prove
that every word is `0xC0` or `0xFC`.

The board parts are not modelled yet: NAU88L21 codec, 74HC4051 knob
multiplexers, Bluetooth module. The 14-segment display is modelled (see
section 6), but the stock firmware does not refresh it inside 90 M cycles.

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
| no fidelity gap, run not stopped early | `fidelity_clean`, stop reason `max_cycles` |

The Bluetooth module is not modelled: nothing answers the AT commands. The
firmware does not wait for `OK`, so the sequence is complete anyway.

Negative control: the same run with each new expected value changed (an
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

STOCK_DISPLAY_PLACEHOLDER

## 7. SVD provenance

The files in `labwired/chip/peripherals/` and the base addresses and IRQs in
`labwired/chip/mimxrt1052.yaml` come from the NXP CMSIS SVD for the
MIMXRT1052 (vendor `nxp.com`, version 1.0, device MIMXRT1052DVL6B,
BSD-3-Clause, from the NXP MCUXpresso SDK). The SVD is not in this
repository. The copy used was vendored in labwired-core at commit `da773a4d`
as `tests/fixtures/real_world/mimxrt1052.svd`:

- source: `https://github.com/w1ne/labwired-core/blob/da773a4d/tests/fixtures/real_world/mimxrt1052.svd`
- sha256: `8151227fba04784dfb2c0750d87cc641866b7f3801324fc6c20772560869ff40`
