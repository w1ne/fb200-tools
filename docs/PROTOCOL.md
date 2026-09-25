# FB200 USB HID Protocol

Reverse-engineered documentation of the USB HID protocol used by the FLAMMA FB200
(a Mooer-based bass multi-effects pedal), as implemented by `fb200-tools` v0.1.

Everything below was verified against a real FB200 (application firmware V1.0.1),
the official Electron editor, and the commands `fb200-tools` sends and receives
today. Facts that are known but not yet implemented (firmware update mode) are
marked as such.

## 1. USB identities

| Mode | VID | PID | Notes |
|------|-----|-----|-------|
| Application | `0x34DB` | `0x800F` | USB Audio (2 in / 2 out @ 44.1 kHz, class-compliant) + vendor HID interface (interface 3) |
| Update / bootloader | `0x0483` | `0x5703` | Entered after the `0xC1` command; common Mooer update identity |

All device control in this document happens over the vendor HID interface
(interface 3) of the application identity.

## 2. HID reports

- HID reports are **64 bytes**.
- The **first byte is the number of valid payload bytes** that follow; the
  remaining bytes are zero padding.
- Payloads (frames) larger than 63 bytes are **chunked into 63-byte reports**:
  for a frame of `N` bytes, `ceil(N / 63)` reports are written, each one
  `[len][chunk][zero padding]`.
- To reassemble, concatenate the first `len` bytes of each report in order.

Example: the 7-byte frame `AA 55 01 00 00 C8 CF` is sent as:

```
07 AA 55 01 00 00 C8 CF 00 00 ... 00     (64 bytes total)
```

`src/fb200/protocol.py` implements this as `iter_reports()` (write side) and
`FrameReader` (read side, including resynchronization on `AA 55` and CRC checks).

## 3. Frame layout

```
offset  size  field
0       2     sync        AA 55
2       2     len         u16 little-endian, length of (fn + data)
4       1     fn          command / reply id
5       len-1 data        command or reply payload
4+len   2     CRC16       high byte first
```

Total frame length is `6 + len`. `len` counts `fn + data` only: it does **not**
include the sync bytes, the length field itself, or the CRC.

Example: get version (`fn = 0x00`, no payload) is
`AA 55 01 00 00 C8 CF`.

### Identify byte

The official Electron app writes an **identify byte** before `fn`
(`0x11` = PC, `0x10` = BLE). The FB200's USB HID interface accepts frames
**without** it; an identify `0x11` frame receives **no reply over USB**
(BLE presumably requires it). `fb200-tools` therefore always sends
identify-less frames, and all offsets in this document describe identify-less
framing. Device replies use the same framing, also without an identify byte.

## 4. CRC16

Table-driven CCITT-style CRC16 with a final XOR of `0xFFFF`, identical in the
official app and the FF20 community project. The 256-entry table lives in
`src/fb200/protocol.py` (`CRC_TABLE`).

```
crc = 0
for b in data:
    crc = (TABLE[((crc >> 8) ^ b) & 0xFF] ^ ((crc << 8) & 0xFFFF)) & 0xFFFF
crc ^= 0xFFFF
```

The CRC is computed over `len | fn | data` (bytes 2 through `4 + len` of the
frame, i.e. excluding `AA 55` and the CRC itself) and appended high byte first.

Verified vectors:

- `crc16(01 00 00) == C8 CF`
- end to end: `pack_frame(0x00) == AA 55 01 00 00 C8 CF`

## 5. Command set

Application-mode commands (device `34DB:800F`, vendor HID interface):

| fn | Direction | Meaning | Payload / reply |
|----|-----------|---------|-----------------|
| `00` | host → dev | Get version | reply `fn=01`, see §5.1 |
| `61` | host → dev | Upload IR | 9 frames, see §5.2; reply `fn=62` per frame |
| `63` | host → dev | Query IR slot | `[01, slot u16 LE, 00]`; reply `fn=64`, see §5.3 |
| `67` | host → dev | Delete IR slot | `[01, slot u16 LE, 01]`; reply `fn=68`, see §5.4 |
| `C1` | host → dev | Jump to bootloader | device re-enumerates as `0483:5703` |
| `B2` | dev → host | Device notification | client should re-query the IR list |

IR slots are **1-based** (`1..9`). Offsets are within the reply payload, i.e.
after the `fn` byte.

### 5.1 Get version (`00` → reply `01`)

Request payload: none. Reply payload: 55 bytes.

| Payload offset | Size | Field | Captured value |
|----------------|------|-------|----------------|
| `0` | 32 | product id, NUL-padded ASCII | `FB200` |
| `32` | 7 | application version, NUL-terminated | `V1.0.0` |
| `39` | 7 | firmware version, NUL-terminated | `V1.0.1` |
| `46` | 7 | Bluetooth module version, NUL-terminated | `V1.0.0` |
| `53` | 2 | hardware revision, NUL-terminated | `A` |

A full annotated capture is in §7.

### 5.2 Upload IR (`61` → reply `62` per frame)

One import is **9 frames**: 1 metadata frame followed by 8 data frames. Each
frame is acknowledged with `fn=62` before the next one is sent.

Frame payload layout:

| Offset | Size | Field |
|--------|------|-------|
| `0` | 1 | file type, always `01` |
| `1` | 2 | slot index, u16 LE, `1..9` |
| `3` | 1 | total frame count (`09`) |
| `4` | 1 | frame index, `0` = metadata, `1..8` = data |
| `5` | 2 | data length, u16 LE |
| `7` | n | metadata frame: IR name; data frames: 512 bytes of samples |

- Metadata frame (index `0`): the payload is the IR name, printable ASCII,
  maximum 50 characters.
- Data frames (index `1..8`): 512 bytes each of `float32` little-endian
  samples — 4096 bytes / 1024 samples total.

See §6 for the sample format.

### 5.3 Query IR slot (`63` → reply `64`)

Request payload: `[01, slot u16 LE, 00]` (4 bytes).

Reply payload offsets:

| Offset | Size | Field |
|--------|------|-------|
| `3` | 1 | `00` = slot empty, non-zero = occupied |
| `6` | 2 | name length, u16 LE |
| `8` | n | name, printable ASCII |

### 5.4 Delete IR slot (`67` → reply `68`)

Request payload: `[01, slot u16 LE, 01]` (4 bytes).

Reply payload offset `3`: `00` = failure, non-zero = success.

### 5.5 Notifications (`B2`)

The device can send `fn=B2` spontaneously to tell the client that the IR list
changed; the client should re-query the slots. `fb200-tools` v0.1 ignores it.

## 6. IR format

The official renderer decodes any WAV to **44.1 kHz**, takes **channel 0**,
and truncates or zero-pads to exactly **1024 `float32` samples** (4096 bytes).
It then uploads 8 data frames plus 1 metadata frame (9 frames total).

Names are sanitized to printable ASCII (`0x20..0x7E`) and limited to 50
characters.

The user manual describes "512 points, 24-bit"; the app's proven 1024-float
format is what actually works and is what `fb200-tools` implements. The
discrepancy remains an open question.

## 7. Captured version reply

Reply to `AA 55 01 00 00 C8 CF` (from the design notes, Appendix B):

```
3e aa 55 38 00 01 46 42 32 30 30 00 ... 56 31 2e 30 2e 30 00 56 31 2e 30 2e 31 00
56 31 2e 30 2e 30 00 41 00 8f 34 00
```

Annotated against the 64-byte HID report:

| Report offset | Bytes | Meaning |
|---------------|-------|---------|
| `0` | `3e` | HID valid-length byte: 62 frame bytes follow |
| `1:3` | `aa 55` | frame sync |
| `3:5` | `38 00` | `len` = `0x0038` = 56 = `fn` (1) + payload (55) |
| `5` | `01` | reply `fn` |
| `6:38` | `46 42 32 30 30 00 00...` | payload `[0:32]`: product `FB200`, NUL-padded |
| `38:45` | `56 31 2e 30 2e 30 00` | payload `[32:39]`: app `V1.0.0` |
| `45:52` | `56 31 2e 30 2e 31 00` | payload `[39:46]`: firmware `V1.0.1`, NUL-terminated |
| `52:59` | `56 31 2e 30 2e 30 00` | payload `[46:53]`: Bluetooth `V1.0.0` |
| `59:61` | `41 00` | payload `[53:55]`: hardware revision `A` |
| `61:63` | `8f 34` | CRC16 over frame bytes `[2:60]` (report bytes `[3:61]`) |
| `63` | `00` | HID report padding |

Decoded: product `FB200`, application `V1.0.0`, firmware `V1.0.1`,
Bluetooth `V1.0.0`, hardware revision `A`.

## 8. Firmware update mode (`0483:5703`)

Not implemented in `fb200-tools` v0.1; this section documents the official
updater's behavior for the planned flashing client. Full update flow:

1. From application mode, send `fn=0xC1` (no payload); the device
   re-enumerates as `0483:5703`.
2. **Erase**: frame `fn=header.SEND_CMD` (`0x02`). If `header.VERSION == 0`
   the payload is the 4-byte `header.UPDATE_ADDR`; otherwise it is
   `[ROM_ID u8, START_PAGE u32 LE, BLOCK_SIZE u32 LE]` repeated for every
   block in the image. Wait for reply `0x03`. The stock FB200 `V1.0.1` image
   uses `VERSION 0` with `UPDATE_ADDR` = `03 00 00 00`.
3. **Write**: for each block, frames `fn=block.SEND_CMD` with
   `[page u16 BE, 512-byte chunk]`, one frame per 512 bytes (the official app
   sends the low 16 bits of the u32 LE page number reversed); wait for reply
   `SEND_CMD + 1` for each frame.
4. `fn=0xFF` to exit and reboot.

Function codes in the stock FB200 `V1.0.1` image:

| fn | Usage | Reply |
|----|-------|-------|
| `02` | erase (header `SEND_CMD`) | `03` |
| `04` | write app block (block 0 `SEND_CMD`) | `05` |
| `06` | write models block (block 1 `SEND_CMD`) | `07` |
| `FF` | exit / reboot | — |

`SEND_CMD` comes from the 128-byte `.mr` header and from each block tag; the
container format itself is documented separately (v0.2). The official updater
sends **no image signature or per-image checksum**, so image validation
(`PRODUCT_TAG == FB200`, sizes, page math) is the client's responsibility.
Any flashing must be validated with a stock round-trip first.

## 9. How this was derived

- **Official Electron app inspection.** The protocol constants, CRC table,
  identify bytes, command payload layouts, and the update sequence were read
  out of the official editor (`FB200_V1.0.1_Mac.dmg`) and replayed against a
  real pedal.
- **FF20 community project.** [wattsline/Flamma-FF20](https://github.com/wattsline/Flamma-FF20)
  documents the sibling Flamma FF20 and uses the same framing and CRC table,
  which cross-checks the findings here.
- **Live hardware verification.** Frames were captured from a real FB200, and
  every command implemented by `fb200-tools` is exercised against it.

No vendor binaries are redistributed in this repository: this documentation is
a description of observed behavior, and the code is an independent client
implementation. Official firmware and applications must be obtained through
official channels.

## References

- Design notes: `docs/superpowers/specs/2026-09-25-fb200-tools-design.md`
- FF20 community project: https://github.com/wattsline/Flamma-FF20
- Similar Mooer projects: [ThijsWithaar/MooerManager](https://github.com/ThijsWithaar/MooerManager),
  [shpala/MooerLooperManager](https://github.com/shpala/MooerLooperManager),
  [utajum/mooer-drummer-x2](https://github.com/utajum/mooer-drummer-x2)
