# fb200-hello Custom Firmware — Design

Date: 2026-09-26
Status: approved (brainstorming)
Related: [`docs/HARDWARE.md`](../../HARDWARE.md) §2–§3, [`docs/UPDATE_AND_RECOVERY.md`](../../UPDATE_AND_RECOVERY.md)

## 1. Purpose

Prove that the FB200 can run non-vendor firmware. `fb200-hello` is a minimal
application that boots on the pedal and enumerates over USB as our own device
(custom VID/PID and product string) with a CDC-ACM serial interface that prints
a banner. It is built from source in CI and flashed with the existing
`fb200 fw flash` tooling, with proven recovery to the stock image.

This is the first milestone of a possible open-source firmware effort
(see [`docs/HARDWARE.md`](../../HARDWARE.md) §3.2). It deliberately proves the
boot + USB path only.

## 2. Success criteria

1. CI builds `firmware/hello/` with `arm-none-eabi-gcc` (Linux job) and packs a
   flashable `.mr` artifact.
2. `fb200 fw pack` produces a valid app-only (single-block) image that
   `fb200 fw inspect` parses and whose flash plan is ~392 write frames
   (200,704 B) instead of 6,810.
3. On hardware: the pedal re-enumerates as `0xCAFE:0x4001` with product string
   `FB200 Hello` and exposes a CDC-ACM port that prints the banner.
4. Immediately after, flashing the stock image restores the normal device:
   `fb200 info` reports `FB200 V1.0.1`, USB audio + HID return, `pytest -m
   hardware` passes.
5. Docs, CHANGELOG entry and CI are updated and pushed; CI green.

## 3. Non-goals

- No audio path, display, effects, Bluetooth, or model-library access.
- No bootloader changes, no DFU, no secure-boot work.
- No writes outside the application region: the models block is never touched
  by `fb200-hello` images.
- No upstreaming into TinyUSB/Zephyr/SDK in this milestone.

## 4. Verified facts this design relies on

From static analysis of the stock `V1.0.1` image (all in
[`docs/HARDWARE.md`](../../HARDWARE.md)) and hardware validation:

- Main SoC: NXP i.MX RT10xx (Cortex-M7, FPv5 double-precision FPU).
- Boot contract: application region starts at flash `0x60010000`; the
  bootloader consumes `image[0]` as the initial stack pointer and `image[1]` as
  an absolute (thumb) reset address. The stock values are
  `0x20058000` / `0x600104d9`.
- Stock startup configures `IOMUXC_GPR` `0x400AC038` = `0x00AA0000`,
  `0x400AC040` = `0x00200007`, `0x400AC044` = `0xFFAAAAA9` (TCM/FlexRAM setup),
  then a routine at image offset `0x1a390` copies 256 words (the vector table)
  from `0x60010000` to `0x00000000` and sets `SCB->VTOR` (`0xE000ED08`) to `0`.
  Remaining vector entries are 0-based ITCM addresses, i.e. the image runs
  from ITCM.
- The bootloader accepts unsigned images (a byte-patched image booted).
- Recovery: `fb200 fw flash stock.mr --yes [--no-jump]` is proven on hardware.
- Update page space base is `0x60008000`; block 0 uses `START_PAGE 0x40`
  (pages 64–455), 200,704 bytes, 512-byte pages.

## 5. Architecture

### 5.1 Repository layout

```
firmware/hello/
  Makefile            # arm-none-eabi-gcc build; `make deps` extracts TinyUSB
  tinyusb.lock        # pinned URL + SHA-256 of the TinyUSB release tarball
  linker.ld           # ITCM VMA / flash LMA layout (see 5.3)
  src/startup.c       # flash-resident reset stub + copy stage
  src/vectors.c       # 256-entry vector table (ITCM runtime copy)
  src/main.c          # TinyUSB CDC device + banner/echo
  board/              # adapted TinyUSB MIMXRT10xx BSP (clock, MPU, FlexRAM)
  tools/
    synthetic_template.py  # builds a vendor-free FB200 template for CI
  README.md           # build instructions, pin, licensing notes, warnings
  .gitignore          # .deps/, build/
```

`firmware/hello/.deps/` holds the extracted TinyUSB sources and is ignored by
git; `tinyusb.lock` (checked in) pins the exact release URL and SHA-256 so the
build is reproducible offline once fetched. `make` fetches on first use;
`make deps` only fetches.

### 5.2 Firmware composition

- TinyUSB in device mode with a custom descriptor set:
  - VID/PID `0xCAFE:0x4001` (development placeholder; never shipped as a
    product ID), manufacturer `fb200-tools`, product `FB200 Hello`, serial
    `HELLO-0001`.
  - One CDC-ACM interface. On DTR assertion the firmware writes the banner
    (`FB200 hello - fb200-tools custom firmware`, ASCII only); each received
    line is echoed back.
- Board files adapted from TinyUSB's MIMXRT10xx BSP (MIT; files derived from
  NXP SDK remain BSD-3 with their headers intact).
- Crystal frequency is a single `BOARD_XTAL_HZ` define (default 24 MHz),
  confirmed against PCB photos before the hardware run.
- Clock init brings up PLL3 (480 MHz) for USB1 as in the reference BSP; only
  the peripherals needed for USB are enabled.

### 5.3 Memory layout and startup (mimics stock)

- Linker: `.vectors`, `.text` and `.rodata` have VMA in ITCM
  (`0x00000000`…) with LMA in flash (`0x60010000`…); `.data`/`.bss` live in
  DTCM at `0x20000000`; `_estack = 0x20058000` (mirrors stock).
- `image[0..8]` at LMA `0x60010000` is the boot header: `[SP]` and
  `[absolute flash reset stub]`; the reset stub address is also the vector
  table entry `[1]` so both the bootloader and (harmlessly) a post-copy reset
  exception resolve correctly.
- Flash-resident reset stub, in order:
  1. `cpsid i`.
  2. Configure FlexRAM/TCM GPRs with the stock values (`0x00AA0000`,
     `0x00200007`, `0xFFAAAAA9`).
  3. Set MSP from `image[0]`.
  4. Copy `.vectors` + `.text` + `.rodata` from flash LMA to ITCM;
     copy `.data`; zero `.bss`.
  5. `SCB->VTOR = 0`.
  6. Branch to `Reset_Handler` in ITCM (`cpsie i` first).
- Vector table: 256 entries; implemented handlers are default loops
  (NMI/HardFault/…); TinyUSB runs polled in `main()` for this milestone, so no
  SysTick/IRQ handlers are required.

### 5.4 `fb200 fw pack` (new library/CLI feature)

Command:

```
fb200 fw pack --template stock.mr app.bin --app-only -o hello.mr
```

`--app-only` is the default and only behavior in this milestone; the flag is
accepted explicitly for clarity.

Behavior (`fb200.firmware.pack_app_image`):

1. Parse `--template` as `MrFile`; require a `FB200` product tag.
2. Require `1 <= len(app.bin) <= template block 0 size` (200,704 B for the
   stock image); otherwise raise `FirmwareError`.
3. Build a one-block `MrFile`:
   - header: copy the template header verbatim, then set the block count to 1;
     product tag, `SEND_CMD`/`REC_CMD`, `UPDATE_ADDR`, `VERSION` (0) and
     timeouts are unchanged;
   - block tag: copy the template block-0 tag verbatim (`SEND_CMD 0x04`,
     `REC_CMD 0x05`, `START_PAGE 0x40`);
   - payload: `app.bin` padded with `0xFF` to the template block-0 size, so the
     flash plan covers exactly the stock application pages.
4. Serialize with `MrFile.to_bytes()`.

The packed image uses the existing verified flash plan path; no updater
changes are needed. `fb200 fw flash` keeps its dry-run default, `--yes` gate
and product-tag check.

Errors: validation failures raise `FirmwareError`, the CLI prints `error:` to
stderr and exits 1.

## 6. CI

New job `firmware` in `.github/workflows/ci.yml` (Linux only, added to the
existing workflow):

1. Install `gcc-arm-none-eabi` (Ubuntu package).
2. `make deps build` in `firmware/hello/`.
3. `.venv/bin/fb200 fw pack --template build/synthetic-template.mr \
   firmware/hello/build/fb200-hello.bin -o build/fb200-hello.mr`.
   `firmware/hello/tools/synthetic_template.py` (checked in) generates
   `synthetic-template.mr`: a valid FB200 container built from the public
   format in [`docs/FIRMWARE_FORMAT.md`](../../FIRMWARE_FORMAT.md) with the
   same header/tag fields, block-0 size 200,704 and a zeroed payload. No
   vendor firmware enters CI or the repository.
4. Upload `fb200-hello.bin` and `fb200-hello.mr` with
   `actions/upload-artifact` as workflow artifacts.

Pack unit tests run in the normal test matrix (`tests/test_fw_pack.py`), so
the tool is covered on all three OSes; only the cross build is Linux-only.

## 7. Verification and safety

Local (before any hardware):

```bash
.venv/bin/python -m pytest -q                 # includes new pack tests
cd firmware/hello && make deps build
.venv/bin/fb200 fw inspect build/fb200-hello.mr
.venv/bin/fb200 fw flash build/fb200-hello.mr            # dry run only
```

Hardware (user-supervised, same protocol as Task 20):

```bash
.venv/bin/fb200 fw flash build/fb200-hello.mr --yes
ioreg -p IOUSB -l -w 0 | grep -iE "cafe|FB200 Hello"     # 0xCAFE = 51966
ls /dev/tty.usbmodem*                                    # macOS CDC port
# Linux: ls /dev/ttyACM*
# read the banner, then recover immediately:
.venv/bin/fb200 fw flash fb200-stock.mr --yes
.venv/bin/fb200 info                                     # FB200 V1.0.1
```

- The stock `.mr` stays local and is never committed.
- If the app-only erase turns out to affect the models block, the stock full
  flash restores everything (proven).
- If the device does not enumerate, the pedal stays in application mode and is
  recovered with the stock image; the `--no-jump` path is available if it ends
  up in the bootloader.

## 8. Risks and open items

| Risk | Impact | Mitigation |
|------|--------|------------|
| Crystal frequency differs from 24 MHz | USB clock wrong, no enumeration | Confirm from PCB photos before flashing; single `BOARD_XTAL_HZ` define |
| Exact RT10xx part / FlexRAM config differs | Startup copy or stack placement wrong | Reuse the stock GPR values; photos to confirm part; adjust defines |
| Bootloader rejects 1-block images or app-only erase touches models | Flash aborts or models lost | Recoverable by stock reflash; fallback to full 2-block pack |
| TinyUSB BSP assumes EVK board details | USB misconfigures | Only USB1 (dedicated pins) is used; clock/MPU kept minimal |
| New USB VID/PID confuses host tooling | Cosmetic | Placeholder `0xCAFE` never used for products; documented |

Open items to close with PCB photos: exact SoC marking, crystal(s), QSPI
flash, audio codec (for later milestones), and the model-block destination
(`ROM_ID` question in [`docs/HARDWARE.md`](../../HARDWARE.md) §5).

## 9. Deliverables

- `fb200.firmware.pack_app_image` + `fb200 fw pack` CLI + tests.
- `firmware/hello/` complete buildable project with pinned TinyUSB lock file.
- CI `firmware` job producing artifacts.
- Docs: `firmware/hello/README.md`, README status/features update,
  `docs/HARDWARE.md` §3.2 pointer, `docs/RESEARCH.md` timeline entry,
  CHANGELOG `Unreleased` entry.
- Hardware validation recorded in `docs/UPDATE_AND_RECOVERY.md` §7 (same
  table format as the v0.3 validation).

## 10. Release

After hardware validation, cut `v0.4.0` with the pack tool and the firmware
milestone in the changelog, following the v0.3 release process (tag, push,
`gh release create`).
