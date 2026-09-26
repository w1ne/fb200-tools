# fb200-hello

Minimal custom firmware for the FLAMMA FB200 (NXP i.MX RT10xx, Cortex-M7).
It is the first milestone of the custom-firmware effort: it proves only the
boot + USB path. The firmware boots through the **stock bootloader**, which
stays untouched, and enumerates over USB as a CDC-ACM device:

- VID/PID `0xCAFE:0x4001` (a development placeholder, never a product ID)
- manufacturer `fb200-tools`, product string `FB200 Hello`, serial `HELLO-0001`
- on DTR assertion the serial port prints the banner
  `FB200 hello - fb200-tools custom firmware`, and input is echoed back

The raw image is 28,708 B; `fb200 fw pack` pads it to a single-block,
200,704-byte block-0 image — exactly the stock application region, so the
model library is never written.

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

- `build/fb200-hello.bin` — raw application image
- `build/fb200-hello.elf`, `build/fb200-hello.map` — symbols and link map
- `build/fb200-hello.mr` — flashable image (after packing, below)

## Pack and flash

The packer needs a template image that supplies the container header and page
math. For a real flash, use your stock `FB200.mr`; the checked-in synthetic
generator builds a vendor-free stand-in with the same fields and is what CI
uses (the template itself is never flashed — only the packed output is):

```bash
python firmware/hello/tools/synthetic_template.py -o firmware/hello/build/synthetic-template.mr
fb200 fw pack --template firmware/hello/build/synthetic-template.mr firmware/hello/build/fb200-hello.bin -o firmware/hello/build/fb200-hello.mr
fb200 fw flash firmware/hello/build/fb200-hello.mr            # dry run
fb200 fw flash firmware/hello/build/fb200-hello.mr --yes      # write; recover before unplugging
```

`fb200 fw flash` is dry-run by default; only `--yes` writes. The packed image
is app-only: it erases and writes the 392 application pages (200,704 B) and
never touches the model library. Never unplug the pedal until the tool reports
it back online.

## Verification

With the pedal running this firmware and connected over USB:

```bash
# macOS (ioreg prints VID/PID in decimal; 51966 = 0xCAFE)
ioreg -p IOUSB -l -w 0 | grep -i cafe
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

Flash the stock image to restore the pedal (the tool sends `0xC1` to jump from
the hello firmware); add `--no-jump` if the pedal is already in update mode:

```bash
fb200 fw flash fb200-stock.mr --yes
fb200 fw flash fb200-stock.mr --yes --no-jump
```

Keep the stock image locally and never commit or redistribute it. The full
recovery procedure, including the power-on footswitch combinations, is in
[`docs/UPDATE_AND_RECOVERY.md`](../../docs/UPDATE_AND_RECOVERY.md).

## How it boots

The stock bootloader consumes the first 8 bytes of the image at flash
`0x60010000` as `[initial SP][absolute thumb reset address]`, exactly as it
does for the stock application. Our reset stub (flash-resident) then mirrors
the stock startup: it configures the FlexRAM/TCM GPRs with the stock values,
sets MSP from `image[0]`, copies `.vectors`/`.text`/`.rodata` from flash into
ITCM and `.data`/`.bss` into DTCM, sets `SCB->VTOR` to `0`, and branches into
`app_main()` running from ITCM. The evidence and stock values are in
[`docs/HARDWARE.md`](../../docs/HARDWARE.md) §2–§3.

## Layout

```
firmware/hello/
  Makefile              # arm-none-eabi-gcc build; pinned dependency fetching
  tinyusb.lock          # TinyUSB tag/URL/SHA-256 pin
  linker.ld             # ITCM VMA / flash LMA layout
  src/startup.c         # flash-resident reset stub and copy stage
  src/boot_header.S     # 8-byte [SP][reset] boot header
  src/vectors.c         # 256-entry vector table (runtime copy in ITCM)
  src/main.c            # TinyUSB polled loop, stdout banner and echo
  src/usb_descriptors.c # device/config/string descriptors (0xCAFE:0x4001)
  src/usb_descriptors.h # string descriptor indices
  src/tusb_config.h     # TinyUSB configuration (device, CDC-ACM only)
  src/memfuncs.c        # freestanding memcpy/memset/memmove/strlen
  src/system_clock.c    # SystemCoreClock symbols for the BSP
  src/compat/           # freestanding libc shims (assert, stdio, stdlib,
                        # string, inttypes) for toolchains without newlib
  board/board_config.h  # FB200 facts: crystal 24 MHz, VID/PID, strings
  board/README.md       # BSP composition and licensing notes
  tools/synthetic_template.py  # vendor-free FB200 template for CI/pack tests
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
