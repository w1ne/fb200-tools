# FB200 `.mr` Firmware Container Format

Reverse-engineered specification of the `Mooer_TAG` firmware container used by
the FLAMMA FB200 (Mooer-based) bass multi-effects pedal. It was derived from
the official `FB200_V1.0.1_Mac.dmg` Electron editor and its bundled `FB200.mr`
image, and is implemented by `src/fb200/firmware.py`.

Everything marked "verified" was checked against the stock `V1.0.1` image;
field semantics that have not been confirmed are called out explicitly.

## 1. Overview

A `.mr` file is a flat, uncompressed, little-endian container. It holds a
128-byte header followed by `UPDATE_BLOCK` records, each a 512-byte tag
followed immediately by its payload:

```
offset 0
+---------------------------------+
| header                  128 B   |
+---------------------------------+
| block 0 tag             512 B   |
| block 0 payload    BLOCK_SIZE   |
+---------------------------------+
| block 1 tag             512 B   |
| block 1 payload    BLOCK_SIZE   |
+---------------------------------+
| ...            UPDATE_BLOCK records
```

- The magic `Mooer_TAG` (9 bytes) starts the header and every block tag.
- All multi-byte integers are little-endian.
- A payload directly follows its tag: no padding, no alignment.
- File size is `128 + Σ (512 + BLOCK_SIZE_i)`. The stock FB200 `V1.0.1` image
  contains two blocks and is **3,487,872 bytes**:
  `128 + 512 + 200,704 + 512 + 3,286,016`.

## 2. 128-byte header

Fields run from offset 0 through offset 52; bytes 53–127 are unused by the
parser.

| Offset | Size | Field | Type | Stock value | Notes |
|--------|------|-------|------|-------------|-------|
| 0 | 9 | `TAG` | bytes | `Mooer_TAG` | magic |
| 9 | 32 | `PRODUCT_TAG` | ASCII | `FB200` | NUL-padded |
| 41 | 1 | `SEND_CMD` | u8 | `0x02` | erase command for this image |
| 42 | 1 | `REC_CMD` | u8 | `0x03` | expected erase reply |
| 43 | 4 | `TIMEOUT` | u32 LE | — | per-frame timeout hint; unit/use unverified |
| 47 | 1 | `UPDATE_BLOCK` | u8 | `2` | number of block records |
| 48 | 4 | `UPDATE_ADDR` | bytes | — | semantics unverified; preserved verbatim |
| 52 | 1 | `VERSION` | u8 | — | image version field; the USB version reply reports separate version strings |
| 53 | 75 | reserved | bytes | — | not interpreted; preserved byte-exact |

`PRODUCT_TAG` is what the flash tooling validates before writing (`FB200`).

## 3. 512-byte block tag

Fields run from offset 0 through offset 31; bytes 32–511 are unused by the
parser.

| Offset | Size | Field | Type | Notes |
|--------|------|-------|------|-------|
| 0 | 9 | `TAG` | bytes | magic `Mooer_TAG` |
| 9 | 4 | `START_ADDR` | u32 LE | semantics unverified |
| 13 | 4 | `STOP_ADDR` | u32 LE | semantics unverified |
| 17 | 4 | `BLOCK_SIZE` | u32 LE | payload length in bytes |
| 21 | 1 | `SEND_CMD` | u8 | write command for this block |
| 22 | 1 | `REC_CMD` | u8 | expected write reply (`SEND_CMD + 1` in stock image) |
| 23 | 4 | `TIMEOUT` | u32 LE | per-frame timeout hint; unit/use unverified |
| 27 | 4 | `START_PAGE` | u32 LE | first page number of the payload |
| 31 | 1 | `ROM_ID` | u8 | erase target selector sent in the erase frame |
| 32 | 480 | reserved | bytes | not interpreted; preserved byte-exact |

## 4. Block payload

Each block payload is exactly `BLOCK_SIZE` bytes copied verbatim from the tag
to the flash target. Payloads are opaque to the container layer: in the stock
image block 0 is the application image (Cortex-M code and data) and block 1 is
the model library. See [`FIRMWARE_ANALYSIS.md`](FIRMWARE_ANALYSIS.md).

## 5. Stock FB200 V1.0.1 reference values

| | Header | Block 0 (application) | Block 1 (models) |
|--|--------|-----------------------|------------------|
| `TAG` | `Mooer_TAG` | `Mooer_TAG` | `Mooer_TAG` |
| `PRODUCT_TAG` | `FB200` | — | — |
| `SEND_CMD` | `0x02` | `0x04` | `0x06` |
| `REC_CMD` | `0x03` | `0x05` | `0x07` |
| `START_PAGE` | — | `0x40` (64) | `0x00` (0) |
| payload size | — | 200,704 bytes | 3,286,016 bytes |
| 512-byte pages | — | 392 (pages 64–455) | 6,418 (pages 0–6,417) |
| `UPDATE_BLOCK` | `2` | — | — |

Exact file layout of the stock image:

| File offset | Size | Content |
|-------------|------|---------|
| 0 | 128 | header |
| 128 | 512 | block 0 tag |
| 640 | 200,704 | block 0 payload (also the Cortex-M vector table at its start) |
| 201,344 | 512 | block 1 tag |
| 201,856 | 3,286,016 | block 1 payload |
| 3,487,872 | — | end of file |

## 6. Page math

- The flash page size is **512 bytes** (`0x200`).
- `START_PAGE` is a page number in the target selected by `ROM_ID`, stored as
  a full u32 in the tag and sent as a full u32 in the erase frame. In the
  Python API it is `MrBlockTag.start_page` (i.e. `block.tag.start_page`).
- Each payload is written in 512-byte chunks, one write frame per chunk
  (`ceil(BLOCK_SIZE / 512)` frames). `page` is the zero-based chunk index.
- Write frames carry the page number as a **u16 LE**, so only the low 16 bits
  of the sum are sent:

  ```
  page_number = (START_PAGE + page) & 0xFFFF
  ```

- Block 0 therefore uses page numbers `0x0040..0x01C7` and block 1
  `0x0000..0x1911`.
- Cross-check: `START_PAGE 0x40` × 512 = `0x8000`, which matches the
  application base address derived from the vector table
  (`0x60000000 + 0x8000 = 0x60008000`); the pages before it are the
  bootloader area.

## 7. Bootloader frames built from a container

The full update sequence (jump, erase, write, exit) is described in
[`PROTOCOL.md`](PROTOCOL.md) §8. The payloads the container contributes are:

**Erase** — one frame for the whole image:

```
fn      = header.SEND_CMD                      (0x02)
payload = for each block:
          [ROM_ID u8][START_PAGE u32 LE][BLOCK_SIZE u32 LE]
reply   = header.REC_CMD                       (0x03)
```

**Write** — one frame per 512-byte chunk:

```
fn      = block.SEND_CMD                       (0x04 or 0x06)
payload = [page u16 LE][512-byte payload chunk]
reply   = block.REC_CMD                        (SEND_CMD + 1: 0x05 or 0x07)
```

**Exit** — one frame after all blocks:

```
fn      = 0xFF
payload = none
```

`0xFF` reboots the device back into application mode. Every frame is carried
over 64-byte HID reports (chunked per [`PROTOCOL.md`](PROTOCOL.md) §2); a
520-byte write frame becomes 9 reports of 63 bytes.

## 8. Byte-exact round-trip guarantee

`MrFile` (in `src/fb200/firmware.py`) is lossless:

- `from_bytes()` keeps the raw 128-byte header and each raw 512-byte tag on
  the parsed objects (`MrHeader.raw`, `MrBlockTag.raw`) in addition to the
  decoded fields.
- `to_bytes()` starts from those raw records and overwrites only the defined
  fields, writing `BLOCK_SIZE` from the actual payload length. Bytes the
  parser does not interpret — header offset 53–127, tag offset 32–511, and any
  nonzero values inside them — are preserved verbatim.
- Consequence: for any file accepted by `from_bytes()`,
  `MrFile.from_bytes(data).to_bytes() == data` byte for byte. Same-length
  string patching (`patch_string()`) cannot change the file size, so a patched
  image keeps the original layout and page plan.

Validation performed while parsing: header magic and block magic must be
`Mooer_TAG`, the file must contain a full header, every declared block tag and
payload must be present, and no trailing bytes are allowed. `to_bytes()`
additionally requires `UPDATE_BLOCK` to equal the number of blocks.

## References

- USB protocol and update sequence: [`PROTOCOL.md`](PROTOCOL.md)
- Static analysis of the stock image: [`FIRMWARE_ANALYSIS.md`](FIRMWARE_ANALYSIS.md)
- Hardware evidence: [`HARDWARE.md`](HARDWARE.md)
- Parser/serializer: `src/fb200/firmware.py`
- Design notes: `docs/superpowers/specs/2026-09-25-fb200-tools-design.md` §2.5, §2.7
