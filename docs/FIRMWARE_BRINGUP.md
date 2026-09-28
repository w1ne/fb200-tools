# Custom Firmware Bring-Up: Vendor Boot Contract (SOLVED)

Status: **solved and verified on hardware** (2026-09-27). `fb200-hello` boots on
the FB200 through the stock bootloader, enumerates as `0xCAFE:0x4001`
("FB200 Hello" / `fb200-tools`) and its CDC echo works. This document records
the vendor boot contract, how it was reverse-engineered, and how our image
follows it.

## 1. Result

- `fb200-hello` is built in the vendor's own image format and flashed with
  `fb200 fw flash`; no stock bootloader changes are needed.
- Verified on hardware: **cold boot** (USB power cycle) through the stock
  bootloader, USB enumeration, CDC banner
  `FB200 hello - fb200-tools custom firmware`, echo.
- Recovery (A+D + stock reflash) is unaffected and remains the safety net.

## 2. The vendor boot contract

Block 0 of a stock `.mr` is a self-loading image. Offsets are block-0
offsets; flash address = `0x60010000 + offset`.

```
0x000..0x400   vector table (ITCM-form handler addresses; [0] = SP,
               [1] = 0x600104d9 -> the flash stub at 0x4d8)
0x400..0x7d4   vendor boot region (raw, executed from flash):
                 0x400  loader (position-independent)
                 0x434  load-table header (sl/fp offsets)
                 0x43c  LZ-style decompressor
                 0x4a0  memcpy
                 0x4bc  memset
                 0x4d8  reset stub: VTOR=0x60010000, SP=[0x60010000],
                        IOMUXC_GPR14/16/17 (FlexRAM), SystemInit(0x66c)
                        then jumps to the loader at 0x60010400
                 0x528  thunk: bx ITCM 0x4d6
                 0x532  thunk: bx ITCM 0x1672
0x784..0x7d4   load table, 5 entries of (src, dst, len, fn):
                 [0] memcpy   flash 0x600107d4 -> ITCM 0x400  len 0x1dbc8
                 [1] memcpy   flash 0x6002e39c -> DTCM 0x20000000 len 0x49a0
                 [2] decompress flash 0x60032d3c -> DTCM 0x200049a0 len 0x141a4
                 [3] decompress flash 0x60040e74 -> OCRAM 0x20200000 len 0x5aa0
                 [4] memset   DTCM 0x20018b44 len 0x358a4 (bss)
0x7d4..0x1e39c entry 0 payload (the ITCM image)
0x1e39c..      entries 1-3 payloads (DTCM/OCRAM data, compressed)
```

After the table walk the loader executes `bx 0x4d6` (via the thunk at 0x528):
**the entry contract is a fixed jump to ITCM 0x4d6**, which is payload offset
`0xd6` (payload is copied to ITCM 0x400). The stock code at ITCM 0x4d6 is the
app's C-runtime startup. Interrupts are disabled only around the stub; the
loader and the payload entry run with interrupts enabled and VTOR pointing at
flash block 0 until the app switches VTOR to 0.

How this was established:

- Static analysis of the stock image (loader is position-independent: table
  pointers are `adr`-relative, entry 0's source equals the ITCM image).
- **Unicorn emulation of the stock boot region**: started at flash
  0x600104d9 with block 0 mapped, traced the table walk (5 entries,
  614,693 instructions) and the final jump to ITCM 0x4d6, then the app.
- The same emulation of our own image as a dry-run before flashing.

## 3. Our image format

`firmware/hello` builds two artifacts and `firmware/tools/pack_vendor_image.py`
assembles block 0 from the stock template:

```
block 0 = stock block 0 with:
  [0x000:0x400]  replaced by our vector table   (build/fb200-hello.vectors.bin)
  [0x7d4:0x1e39c] replaced by our ITCM payload  (build/fb200-hello.blob.bin,
                  padded with 0xff to 0x1dbc8)
  everything else (vendor boot region, load table, stock DTCM/OCRAM
  payloads) byte-identical to stock
block 1 = stock model block, unchanged
```

The payload is linked at ITCM 0x400; `src/stage2.S` places the 2-byte-aligned
entry at payload offset 0xd6 (ITCM 0x4d6) which branches to `stage2_main`
(`src/startup.c`). stage2 mirrors the stock stub (FlexRAM, SP from image[0],
FPU), copies the vector table from flash block 0 to ITCM 0x0, zeroes `.bss`,
sets `VTOR = 0` and calls `app_main`. Vector `[1]` keeps the vendor stub
address so the stock boot path stays intact.

The stock loader keeps working on our image because the load table, the
vendor boot region and entries 1-4 payloads are untouched; only the bytes the
loader copies to ITCM 0x400 are ours.

### Memory map (audio app)

The chip is an **i.MX RT1052** (MIMXRT1052DVL6B), not an RT1062: 512 kB of
FlexRAM, no separate OCRAM2. `stage2_main` sets the FlexRAM split as the stock
stub does: `IOMUXC_GPR16 = 0x00200007`, `IOMUXC_GPR17 = 0xFFAAAAA9` (bank 0
OCRAM, banks 1-11 DTCM, banks 12-15 ITCM). Measured on the pedal on
2026-09-28 (`peek32`/`poke32` on the running app, D-cache lines evicted
before each read):

| Region | Address | Size | Our use |
|---|---|---|---|
| ITCM | `0x00000000..0x0001FFFF` | 128 kB (4 banks) | vectors, all code and const data (`.blob`, limit 0x1dbc8 B), recovery's copier at `0x1F000` |
| DTCM | `0x20000000..0x20057FFF` | 352 kB (11 banks) | `.dtcmdata` + `.bss` from `0x20018B44` (the stock memset region), stack below `0x20058000`; below `0x20018B44`: stock data unpacked at boot, crash dump (`recovery.c`) |
| OCRAM | `0x20200000..0x20207FFF` | 32 kB (1 bank) | `.ocramdata` (empty now) and `.ocram`: user IR staging (2 kB), EQ (508 B), delay line (30 kB) |
| - | `0x20208000..` | none | data writes are dropped, reads return 0, no fault; an instruction fetch runs through zeros to `0x20280000` and bus-faults |

- In the stock image the vendor loader unpacks 0x5AA0 bytes of stock data
  to OCRAM `0x20200000` (load table entry 3). Our images do not: the packer
  turns entries 1-3 into copies of the `.bss` memset (`src/fb200/images.py`,
  [BOOTLOADER.md](BOOTLOADER.md) section 4), and nothing of ours reads the
  range before it writes it (`.ocram` is NOLOAD, and each buffer is written before it is read:
  `delay_init` clears the line, `eq_init` sets the EQ, a user IR is copied in
  before the cab reads it), so all 32 kB are ours.
- Up to PR #13 our linker script declared `OCRAM 0x20210000, 512 kB` (the
  RT1062 layout). Everything placed there did not exist: the delay line
  (v0.8.0: the repeats never played), the user IR staging, the EQ state, the
  long-IR tail and its FFT tables, and in PR #13 the cold code (the app
  crashed at boot).
- Guards: `linker.ld` (region length and an `ASSERT`),
  `tests/test_audio_image.py::test_every_section_is_in_real_memory` (every
  allocated section of both ELFs in the table above), and the emulators
  (`firmware/tools/boot_dry_run.py`, the stock emulators, the LabWired chip
  model) map only these three windows, so an access outside fails the run.
- The RAM-bound sizes are one define each: `ENGINE_IR_TAPS`
  (`src/audio/engine.h`; 512 = long IRs off) and `DELAY_MS_MAX`
  (`src/dsp/delay.h`; 342 ms).

### Hot and cold code (audio app)

All app code runs from ITCM; the ITCM payload is 121024 of its 121800-byte
limit (0x1dbc8, the vendor loader's entry 0). Moving code that is not on the
audio path out of ITCM (for example to flash XIP) needs a rule for what may
move: nothing reachable from an ISR, from `engine_task` or `usb_audio_task`,
or through a driver callback. `firmware/tools/hot_path.py` follows direct
calls, tail calls, veneers and callback addresses from these roots and fails
if any reachable function is outside ITCM; `tests/test_audio_image.py` runs
it, with a negative control.

## 4. Why earlier attempts failed

Before the contract was understood, images were built as plain flash-executable
binaries with a custom reset stub at image[1] or at fixed flash offsets. None
of them ran:

- The bootloader does not jump to image[1]; it runs the vendor stub/loader and
  the load table. A non-vendor-format block 0 fails inside the loader (garbage
  table entries / back-references) before any custom code executes - no USB,
  no fault LED, just silence until recovery.
- The "reset-beacon" (reset loop) produced no USB flapping, confirming our
  code was never entered.

## 5. Verified procedure (hardware)

```
# from stock app, or from the bootloader (A+D) with --no-jump:
fb200 fw flash hello.mr --yes

# verification
ioreg -p IOUSB | grep -i "FB200 Hello"          # 0xCAFE:0x4001
# open /dev/tty.usbmodemHELLO_0001 -> banner, echo
```

Post-flash verification in `fw flash` still expects the stock app, so a
custom image exits 3 ("device did not re-enumerate"); that is expected. After
flashing, the bootloader's exit jumps straight into the image; cold boots
(power cycles) also boot it, no host interaction needed.

Recovery: power off, hold **A+D**, power on (~3 s), then
`fb200 fw flash fb200-stock.mr --yes --no-jump`.

## 6. References

- Image layout: [`HARDWARE.md`](HARDWARE.md) sections 2-3
- Update/recovery: [`UPDATE_AND_RECOVERY.md`](UPDATE_AND_RECOVERY.md)
- Firmware project: [`../firmware/hello/README.md`](../firmware/hello/README.md)
