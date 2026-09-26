# fb200-hello

> **Status: verified on hardware (2026-09-27).** The image boots on the
> pedal through the stock bootloader, enumerates as `0xCAFE:0x4001`
> ("FB200 Hello" / `fb200-tools`) and its CDC banner and echo work. The
> vendor boot contract and the vendor-format build are documented in
> [`docs/FIRMWARE_BRINGUP.md`](../../docs/FIRMWARE_BRINGUP.md).

Minimal custom firmware for the FLAMMA FB200 (NXP i.MX RT10xx, Cortex-M7).
It is the first milestone of the custom-firmware effort: it targets only the
boot + USB path. The firmware is meant to boot through the **stock bootloader**,
which stays untouched, and enumerate over USB as a CDC-ACM device:

- VID/PID `0xCAFE:0x4001` (a development placeholder, never a product ID)
- manufacturer `fb200-tools`, product string `FB200 Hello`, serial `HELLO-0001`
- on DTR assertion the serial port prints the banner
  `FB200 hello - fb200-tools custom firmware`, and input is echoed back

The firmware is built in the vendor's own image format (see
[`docs/FIRMWARE_BRINGUP.md`](../../docs/FIRMWARE_BRINGUP.md)): a 1,024-byte
vector table at block-0 offset 0, a 27,804-byte ITCM payload at block-0
offset 0x7d4, and the stock loader region/table untouched. The payload entry
sits at the fixed address the vendor loader jumps to (ITCM 0x4d6).

> **Experimental and unofficial.** This firmware is not affiliated with,
> endorsed by, or supported by FLAMMA Innovation or MOOER Audio. A bad flash
> leaves the pedal in its bootloader: it is still recoverable, but keep your
> stock `.mr` image locally and read
> [`docs/UPDATE_AND_RECOVERY.md`](../../docs/UPDATE_AND_RECOVERY.md) before
> flashing anything.

## Requirements

- `arm-none-eabi-gcc` — macOS: `brew install --cask gcc-arm-embedded`;
  Debian/Ubuntu: `gcc-arm-none-eabi`
- `make`
- `curl`
- `git`
- `python3`

## Build

From `firmware/hello/`:

```bash
make deps build
```

`make deps` downloads the pinned TinyUSB 0.21.0 release tarball, verifies its
SHA-256 against `tinyusb.lock`, and then fetches the MCUXpresso SDK subset at
the commits pinned inside that tarball. `make build` compiles the firmware.
Artifacts land in `build/`:

- `build/fb200-hello.vectors.bin` — 1,024-byte vector table (block-0 offset 0)
- `build/fb200-hello.blob.bin` — ITCM payload (block-0 offset 0x7d4)
- `build/fb200-hello.elf`, `build/fb200-hello.map` — symbols and link map
- `build/fb200-hello.mr` — flashable image (after packing, below)

## Pack and flash

Packing needs your local **stock** `.mr` image as the template: the vendor
boot region, load table and the stock DTCM/OCRAM payloads are copied from it
byte-for-byte. Never commit or redistribute the stock image. Run these from
the repo root and prefix `fb200` with `.venv/bin/` outside an active venv:

```bash
python3 firmware/tools/pack_vendor_image.py fb200-stock.mr \
  firmware/hello/build/fb200-hello.vectors.bin \
  firmware/hello/build/fb200-hello.blob.bin \
  -o firmware/hello/build/fb200-hello.mr --app-only
fb200 fw flash firmware/hello/build/fb200-hello.mr            # dry run
fb200 fw flash firmware/hello/build/fb200-hello.mr --yes      # write; recover before unplugging
```

`fb200 fw flash` is dry-run by default; only `--yes` writes. With
`--app-only` the image writes just the 392 application pages (200,704 B) and
never touches the model library; omit it to also rewrite the model block
(byte-identical stock data). Never unplug the pedal until the tool reports it
back online.

Expected outcome: `fb200 fw flash fb200-hello.mr --yes` writes the image
successfully but then exits with code `3` ("did not re-enumerate"), because
post-flash verification looks for the stock `34DB:800F` application. The write
itself succeeded; confirm the CDC device appears as described in Verification.
To flash while the pedal is already in the bootloader (A+D), add `--no-jump`.

## Verification

With the pedal running this firmware and connected over USB:

```bash
# macOS (ioreg prints VID/PID in decimal; 51966 = 0xCAFE)
ioreg -p IOUSB -l -w 0 | grep -iE "cafe|FB200 Hello"
ioreg -p IOUSB -l -w 0 | grep 51966
# Linux
lsusb | grep -i cafe
```

The CDC port appears as `/dev/tty.usbmodem*` on macOS and `/dev/ttyACM*` on
Linux. Open it with any terminal that asserts DTR; the banner is printed at
that moment, and typed characters are echoed back:

```bash
screen /dev/tty.usbmodem* 115200
```

While `fb200-hello` runs, `fb200 info` and the `ir` commands do **not** work:
this firmware exposes no HID interface and no audio.

## Recovery

`fb200-hello` exposes only a CDC interface, not the vendor HID interface, so
the `0xC1` jump that stock tooling uses to enter update mode is unavailable
while hello runs. `fb200 fw flash fb200-stock.mr --yes` will fail with
`FB200 not found` for the same reason. To return to stock:

1. Power the pedal off, hold footswitches **A + D**, power it on while holding
   them, keep holding for about 3 seconds, then release. The pedal enumerates
   as `0483:5703` (update mode). This combination was verified on hardware on
   2026-09-26; see
   [`docs/UPDATE_AND_RECOVERY.md`](../../docs/UPDATE_AND_RECOVERY.md) §5.3.
2. Flash the stock image without the jump:

```bash
fb200 fw flash fb200-stock.mr --yes --no-jump
```

Keep the stock image locally and never commit or redistribute it. The full
recovery procedure is in
[`docs/UPDATE_AND_RECOVERY.md`](../../docs/UPDATE_AND_RECOVERY.md).

## How it boots

The stock bootloader runs the vendor boot region in block 0 (offsets
0x400..0x7d4): a flash stub that configures FlexRAM and jumps to a
position-independent loader, which walks the load table at 0x784
(memcpy/decompress/memset into ITCM, DTCM and OCRAM) and then jumps to the
fixed address ITCM 0x4d6. Our payload is linked at ITCM 0x400, and
`src/stage2.S` places a 2-byte stub at payload offset 0xd6 (ITCM 0x4d6) that
branches to `stage2_main` (`src/startup.c`). stage2 mirrors the stock stub,
copies the vector table to ITCM 0x0, zeroes `.bss`, sets `VTOR = 0` and calls
`app_main`. The full contract, load table and reverse-engineering evidence are
in [`docs/FIRMWARE_BRINGUP.md`](../../docs/FIRMWARE_BRINGUP.md).

## Layout

```
firmware/hello/
  Makefile              # arm-none-eabi-gcc build; pinned dependency fetching
  tinyusb.lock          # TinyUSB tag/URL/SHA-256 pin
  linker.ld             # ITCM 0x0 vectors / ITCM 0x400 payload layout
  src/stage2.S          # 2-byte stub at the vendor entry ITCM 0x4d6
  src/startup.c         # stage2_main: vectors, bss, VTOR, app_main
  src/vectors.c         # 256-entry vector table (stored at block-0 offset 0)
  src/main.c            # TinyUSB polled loop, stdout banner and echo
  src/usb_descriptors.c # device/config/string descriptors (0xCAFE:0x4001)
  src/usb_descriptors.h # string descriptor indices
  src/tusb_config.h     # TinyUSB configuration (device, CDC-ACM only)
  src/memfuncs.c        # freestanding memcpy/memset/memmove/strlen
  src/system_clock.c    # SystemCoreClock symbols for the BSP
  src/compat/           # freestanding libc shims (assert, stdio, stdlib,
                        # string, inttypes); used unconditionally and shadow
                        # newlib where present so builds are identical
  board/board_config.h  # FB200 facts: crystal 24 MHz, VID/PID, strings
  board/README.md       # BSP composition and licensing notes
  tools/synthetic_template.py  # vendor-free FB200 template for CI/pack tests
  (the shared packer lives in firmware/tools/)
```

`board/` holds only configuration and notes: the BSP (`family.c`,
`clock_config.c`, `pin_mux.c`, `board.c`) is compiled directly from the pinned
`.deps/tinyusb` tree (`hw/bsp/imxrt`, reference board `mimxrt1060_evk`, ci_hs
port) so exactly one copy of each file exists.

## Licensing

- Firmware code in this directory is MIT, same as the project — see
  [`LICENSE`](../../LICENSE).
- TinyUSB is MIT.
- Files derived from the NXP MCUXpresso SDK inside the TinyUSB tree are
  BSD-3-Clause, with their headers intact.

No third-party code is vendored here: it is compiled from `.deps/`, which is
fetched from the checksum-verified TinyUSB release.
