# FB200 Firmware Static Analysis

Static analysis of the stock FLAMMA FB200 `FB200.mr` firmware image supplied
inside the official `FB200_V1.0.1_Mac.dmg` editor. The image was unpacked with
the container parser (`src/fb200/firmware.py`) and its two block payloads were
examined with byte/string inspection and manual vector-table decoding.

Only observed values are listed. Hypotheses are marked as such, and questions
that remain open are collected in §5. The image itself is not redistributed in
this repository.

File offsets below are absolute in the `.mr`; block payload offsets are
relative to the start of the extracted payload. For block 0, payload offset `X`
is file offset `128 + 512 + X`.

## 1. Block 0 — application image

Block 0 is the application: 200,704 bytes (`SEND_CMD 0x04`,
`START_PAGE 0x40`). It starts with a Cortex-M interrupt vector table.

### 1.1 Vector table

| Vector | Payload offset | Value | Interpretation |
|--------|----------------|-------|----------------|
| Initial stack pointer | `0x00` | `0x20058000` | top of SRAM stack |
| Reset handler | `0x04` | `0x600104d9` | thumb address (`bit 0` set) inside the application |

Implications:

- The CPU is a Cortex-M core: the first two words are the initial SP and the
  reset vector.
- The stack top `0x20058000` places at least 352 KiB of SRAM below that
  address.
- The reset handler executes from `0x60000000`-based memory. `0x60000000` is
  the memory-mapped (FlexSPI) alias where i.MX RT-class parts map external
  serial NOR flash, i.e. code runs execute-in-place from flash.
- The application is linked at flash **`0x60008000`**. The 32 KiB region
  `0x60000000`–`0x60007FFF` that precedes it is the bootloader area. The erase
  page math cross-checks this: `START_PAGE 0x40` × 512 bytes = `0x8000`.
- The reset handler `0x600104d9` lies inside the application window
  (`0x60008000` upward), not in the bootloader.

### 1.2 Version strings

Block 0 embeds the version strings that the running device reports over the
`fn=0x00` get-version command ([`PROTOCOL.md`](PROTOCOL.md) §5.1):

| String | Reported as |
|--------|-------------|
| `V1.0.1` | firmware version |
| `V1.0.0` | application version |
| `V1.0.0` | Bluetooth module version |
| `A` | hardware revision |

The `.mr` header also carries a separate one-byte `VERSION` field that is not
exposed over USB.

### 1.3 Bluetooth module AT commands

The image contains AT-style command strings for the Bluetooth module, the same
module whose version is reported as `V1.0.0`:

- `AT+BD%-15.15s` — a `printf` template that formats and truncates the device
  name to at most 15 characters (`%-15.15s`). This is consistent with setting
  the BLE friendly name.
- `AT+BM…` — additional `AT+BM` command variants are present; their exact
  suffixes were not itemized.
- `FB200 Audio` — the Bluetooth friendly name string. This is the string used
  for the visible proof patch: replacing it same-length (e.g. with
  `FB200 Tools`) changes the advertised name without moving any code.

No other power/control strings were catalogued.

### 1.4 Copyright and UI/preset strings

- `Copyright 2016 Mooer Audio Corporation. All rights reserved` — vendor
  copyright notice.
- UI/preset name strings, for example:
  - `Fat BassX`
  - `Tri Chorus`
  - strings beginning `Ampog…` (the capture shows the prefix only)

These are display/model names, consistent with the amp-model and cab library
described in §2. The complete preset list was not exhaustively enumerated.

## 2. Block 1 — model library

Block 1 is the model/resource image: 3,286,016 bytes (`SEND_CMD 0x06`,
`START_PAGE 0x00`). It begins with a table describing model entries.

| Property | Observed value |
|----------|----------------|
| Entry count | 20 |
| Typical entry size | 176,400 bytes |
| Smaller entries | a few |
| Data type | `float32` coefficient/audio tables |

`176,400` bytes is exactly one second of mono `float32` audio at 44.1 kHz:

```
44,100 samples × 4 bytes/sample = 176,400 bytes
```

The entry count of 20 and the mixture of one-second and shorter tables are
consistent with a library of **10 amp models + 10 cabinet models**, with the
one-second entries being impulse-response/cabinet data. The entry table format
(size fields, offsets, names) has not been fully decoded; this is an open
question (§5).

## 3. Reproducing the analysis

The container-level inspection is reproducible with the shipped tooling:

```bash
fb200 fw inspect FB200.mr                 # header and block summary
fb200 fw inspect FB200.mr --strings       # printable strings per block
fb200 fw extract-block FB200.mr 0 app.bin # block 0 payload
fb200 fw extract-block FB200.mr 1 models.bin
```

The vector table is then the first 8 bytes of `app.bin` (`SP` at offset `0x00`,
reset at `0x04`); model table entries are examined inside `models.bin`.

## 4. Cross-checks

- `START_PAGE 0x40` × 512 = `0x8000` matches the application base
  `0x60008000` derived from the reset vector — the container page numbering is
  consistent with the flash layout.
- Bootloader region size (`0x8000` = 32 KiB) and the page size match the
  erase/write unit used by the update protocol.
- Container `PRODUCT_TAG` (`FB200`) and the version reply product string agree.

## 5. Open questions

1. **Exact MCU part number.** The vector table and `0x60000000` FlexSPI alias
   identify an i.MX RT-class part, but the exact RT10xx variant (and thus the
   SRAM/flash sizes) is not confirmed. See [`HARDWARE.md`](HARDWARE.md).
2. **Integrity checks.** Neither the `.mr` container nor the update protocol
   carries a signature or per-image checksum field; the official updater sends
   none. Whether the bootloader or application independently verifies the
   flashed image (e.g. a CRC or length check) is unknown.
3. **512 vs 1024 IR points.** The user manual claims "512 points, 24-bit"; the
   official renderer and the working USB upload path use 1024 `float32`
   samples (4096 bytes) per IR. The relationship (if any) between the on-device
   DSP resolution and the upload format is unresolved.
4. **Block 1 model format.** The 20-entry table layout, naming, and the exact
   role of each entry (amp coefficients vs. cab/IR data) are not decoded.

## References

- Container layout: [`FIRMWARE_FORMAT.md`](FIRMWARE_FORMAT.md)
- USB protocol: [`PROTOCOL.md`](PROTOCOL.md)
- Design notes: `docs/superpowers/specs/2026-09-25-fb200-tools-design.md` §2.6
