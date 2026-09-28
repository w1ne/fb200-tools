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
| `labwired/system.yaml` | the FB200 board: ADC inputs, FlexIO2 clock, footswitches, UART |
| `labwired/smoke.yaml` | gate for the open smoke firmware |
| `labwired/stock-boot.yaml` | gate for the unmodified vendor firmware |
| `firmware/labwired-smoke/` | the open smoke firmware (bare registers, no SDK) |
| `tools/labwired_elf.py` | puts raw blobs into one ARM ELF, one PT_LOAD per blob |
| `tools/labwired_stock.py` | builds `build/labwired/stock.elf` from your `.mr` |

## 2. Get the LabWired CLI

The i.MX RT parts and the `peripheral_log` and `fidelity_clean` assertions
are on core `main` (PRs #1254 and #1255) and are not released yet. Until the
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
```

Expected result:

```
PASS  4/4 checks · smoke · 2000000 steps · 0.27s
PASS  24/24 checks · stock-boot · 90000000 steps · 15.26s
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
multiplexers, 14-segment display, Bluetooth module.

## 6. SVD provenance

The files in `labwired/chip/peripherals/` and the base addresses and IRQs in
`labwired/chip/mimxrt1052.yaml` come from the NXP CMSIS SVD for the
MIMXRT1052 (vendor `nxp.com`, version 1.0, device MIMXRT1052DVL6B,
BSD-3-Clause, from the NXP MCUXpresso SDK). The SVD is not in this
repository. The copy used was vendored in labwired-core at commit `da773a4d`
as `tests/fixtures/real_world/mimxrt1052.svd`:

- source: `https://github.com/w1ne/labwired-core/blob/da773a4d/tests/fixtures/real_world/mimxrt1052.svd`
- sha256: `8151227fba04784dfb2c0750d87cc641866b7f3801324fc6c20772560869ff40`
