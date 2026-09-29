# FB200 App Protocol (USB HID and BLE)

Reverse-engineered documentation of the protocol the FLAMMA FB200 (a
Mooer-based bass multi-effects pedal) speaks to the official Electron editor
(USB HID) and the phone app (BLE).

The framing, CRC, version and IR commands were verified against a real FB200
(application firmware V1.0.1), the official Electron editor, and the commands
`fb200-tools` sends and receives. The full command set in §5 comes from the
stock firmware image (static analysis plus emulation of its dispatcher; marked
**(E)** where emulated) and is implemented by our firmware
(`firmware/audio/src/proto`). Facts that are known but not yet implemented (firmware update mode) are
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
(`0x11` = PC, `0x10` = BLE). The stock firmware does **not** support it: its
dispatcher (ITCM `0x46c4`) reads `fn` from frame byte 4 and there is no
command `0x10`/`0x11`, so an identify frame is silently dropped over USB and
BLE alike (this is why an identify `0x11` frame gets no reply). Replies never
carry it. `fb200-tools` and our firmware use identify-less frames; our
firmware drops identify frames like the stock.

### Transports (stock firmware)

Both transports feed the same reassembly state machine (ITCM `0x7264`, one
instance and 1 KB buffer per transport) and the same dispatcher:

| id / reply mask | Transport | Receive | Send |
|---|---|---|---|
| `1` | USB HID interface 3 | report `[len][len bytes]` appended to a 2 KB ring | frame split into 63-byte chunks, each a 64-byte report `[len][chunk][zero pad]` (`0x1b384`) |
| `2` | BLE via the Bluetooth module (LPUART5, 115200 8N1, transparent UART) | every RX byte into a 2 KB ring (IRQ `0xaf70`) | raw frame bytes in 70-byte UART chunks; after each chunk the stock waits up to 14 ms for a module-ready flag (`0x115f0`) |

Reassembly: wait for `AA`, then `55` (else resync), read `len`, which must
be `1..0x3FA` (else resync); wait for `len + 6` bytes; check the CRC; on any
mismatch drop one byte and resync on the next `AA`. A partial frame waits for
more bytes, so a frame may arrive in any number of pieces.

Replies are queued (200 entries, ITCM `0xbd58`) with a transport mask and
built by a task (`0xfad0`). A command's reply goes to the transport it came
from; **unsolicited notifications go to BLE only** (the phone app is the live
editor; the Electron editor only uses info and IR commands). A few replies
are hard-wired to USB (noted in §5).

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

Reverse-engineered from the stock `V1.0.1` image: dispatcher ITCM `0x46c4`,
reply builder `0xfad0`. Layouts marked **(E)** were confirmed by emulating the
stock dispatcher + reply builder with crafted frames (Unicorn, RAM images from
the vendor loader); the others are from static reading of the same code.
Payload offsets exclude `fn`. All u16 are little endian. "edit" = the live
edit buffer (the current preset, 256 bytes, layout in
[`UI_AND_STORAGE.md`](UI_AND_STORAGE.md) §5 / `firmware/audio/src/preset/preset.h`);
"S" = the global settings block (49 bytes, flash `0x80000`).

### 5.1 Summary

| fn | Request payload | Effect | Reply (to requester unless noted) |
|---|---|---|---|
| `00` | - | get version | `01` version (55 B) **(E)** |
| `FA` | - | short version | `FB` (41 B), **to USB only** **(E)** |
| `C3` | - | device info | `C4` (47 B), **to USB only** **(E)** |
| `61` | IR upload frame | assemble an IR, commit on the last frame | `62` per frame **(E)** |
| `63` | `[type=1][slot u16][op]` | IR name (op 0) or data chunk op (1..20) | `64` **(E)** |
| `65` | `[type=1][first u16][last u16]` | IR slot list, slots first..last (<= 9) | `66` **(E)** |
| `67` | `[type=1][slot u16][op][name 50]` | op 1 delete, op 2 rename | `68` + BLE `69` **(E)** |
| `80`..`86` | module block (u16s) | write one effect module of edit | none; BLE echo of the block when a model change loaded defaults **(E)** |
| `94` | - | app connect: dump state | `A1 B0 B7 BA BB B5 83 C9` **(E)** |
| `96` | `[index]` | read preset `index` from flash | `97 [index][256 B]` **(E)** |
| `97` | `[index][256 B]` | store preset (index `FF`: edit only, no store) | `99 [1]`, `98 [index]` |
| `98` | `[index]` | select preset 0..39 | BLE `98 [index]`, BLE `83` **(E)** |
| `99` | `[index][name 20]` | rename preset (selects it, stores it) | BLE `97 [index][256 B]`, BLE `A1` |
| `A0` | `[7 B]` | module order -> edit `+0xBC..0xC2` | none |
| `A3` | `[u16]` | stored in RAM (`0x2000844c`), purpose unknown | none (queues `A4`, which the builder does not emit) **(E)** |
| `B0` | `[13 B]` settings block | write settings (see 5.5) | BLE `83` if the global cab switch changed **(E)** |
| `B2` | - | **factory reset**: all 40 presets + settings + rhythm | `B2 [1]` to requester **and** USB **(E)** |
| `B3` | `[name 20]` | Bluetooth name: S`+0x02..0x15`, flash `0x83000`, AT commands | none **(E)** |
| `B7` | `[4 B]` | S`+0x1B..0x1E` (each > 72 -> 9) | none **(E)** |
| `B8` | `[a][-][b][c]` | S`+0x2C` = a, `+0x2D` = b, `+0x2E` = c | none **(E)** |
| `BA` | `[6 B]` rhythm | rhythm block (see 5.6) | none **(E)** |
| `C1`, `C4` | - | enter the updater: flash `0x86000` = `00`, `AT+B500`, `AT+CZ`, `C2 [1]` to USB, SYSRESETREQ | `C2 [1]` |
| `C9` | `[b]` | S`+0x20` (rhythm mode) = b | none **(E)** |
| `D6` | `[b]` | BLE flow flag (RAM) | none; from BLE the stock also sends `9B 00` on USB |
| `D9` | - | read the 30-byte block at flash `0x85000` | `DA [30 B]` **(E)** |
| `DA` | `[30 B]` | write the block at flash `0x85000` | none **(E)** |
| other | - | ignored (incl. identify-byte frames) | none |

### 5.2 Version and info replies

- `01` (55 B): product `[0:32]` NUL padded (`FB200`), application version
  `[32:39]`, firmware `[39:46]`, Bluetooth `[46:53]`, hardware rev `[53:55]`
  (stock `V1.0.0` / `V1.0.1` / `V1.0.0` / `A`). Captured reply in §7.
- `FB` (41 B): product `[0:32]`, firmware `[32:39]`, hardware rev `[39:41]`.
- `C4` (47 B): product `[0:32]`, `'A'`, `01`, firmware `[34:41]`, then
  `05 00 0A 0A 00 00` (meaning unknown).

### 5.3 Effect modules `80`..`86`

Request = the module's u16 fields in order; the edit buffer is written, the
values are clamped, and there is no reply. When the model/type field changes,
the stock loads that model's default parameters (tables at DTCM `0x2000845c`
amp, `0x2000875e` mod, `0x20008868` delay, `0x20008830` reverb; copied into
`firmware/audio/src/proto/proto.c`) and echoes the resulting block to BLE with
the same `fn`. The pedal sends the same `80`..`86` blocks to BLE when a knob
or footswitch changes a module.

| fn | edit offset | fields (u16) | clamps (value > max -> replacement) | defaults on type change |
|---|---|---|---|---|
| `80` | `0x14` | en, type, p1..p4 | en>1->1, type>21->1, p>100->100 | no |
| `81` | `0x5C` | en, type, level | en>1->1, type>4->1, level>100->100 | no |
| `82` amp | `0x2C` | en, model, gain, bass, mid, midfreq, treble, volume | model>120->1, p>100->100; en && model 0 -> 1 | 6 params (models 1..55) |
| `83` cab | `0x44` | en, type, p1..p4 | type>120->1, p1>4->4, p2,p3>100->100, p4>9->9; en && type 0 -> 1 | no (type >= 11 selects user IR `type - 11`; reloads the IR) |
| `84` mod | `0x74` | en, type, p1..p5 | type>21->1, p1..p4>100->100, p5>240->100 | 5 params |
| `85` delay | `0x8C` | en, type, p1 (mix), p2 (feedback), time | type>6->1, p>100->100, time clamped to 40..2500 ms | 3 params |
| `86` reverb | `0xA4` | en, type, p1..p4 | type>5->1, p1>200->200, p2..p4>100->100 | 4 params |

The stock DSP ignores the `85` delay block. Our firmware plays it only in a
preset that also has our marker at `P+0x96` (`PARITY.md` M4); `85` writes
`0x8C..0x95` and keeps the marker. Our EQ (marker + settings at
`P+0xC4..0xDD`, after the module order) is outside every block: no `80`..`86`
or `A0` write touches it.

### 5.4 Presets

- `96 [i]` -> `97 [i][256 B]` from flash (`0x71000 + i * 0x200`).
- `97 [i][256 B]`, i < 40: write flash, make it the current preset
  (S`+0x16` = i, `+0x21` = i & 3, `+0x22` = i >> 2), reload it, then reply
  `99 [01]` and `98 [i]`. `i = FF` replaces the edit buffer only.
- `98 [i]`: select; notifications `98 [i]` and `83` (cab block) go to BLE only.
- `99 [i][name 20]`: select i, set the name, store; BLE `97 [i][preset]` and
  `A1 [i][edit]`.
- `A1 [i][256 B]` (notification / connect dump): the edit buffer with the
  current index.

### 5.5 Settings

`B0` payload / reply (13 B): `[0]` S`+0x16` current preset (ignored on write),
`[1]` S`+0x19` global cab switch (0 forces the edit cab off, else restores it
from the stored preset; BLE `83` follows), `[2..6]` S`+0x1A..0x1E` (on write
`+0x1B..0x1E` > 72 -> 9), `[7..9]` S`+0x2C..0x2E` (`+0x2D` = tuner on),
`[10]` S`+0x17` Bluetooth audio on (a change sends `AT+B501`/`AT+B500`,
`AT+CZ`), `[11]` S`[0x24 + slot]`, `[12]` S`[0x28 + slot]` (the current slot's
light-ring colour 0..9 and level 0..100, default 0 = red and 100). `B7` (4 B) = S`+0x1B..0x1E`. `C9 [b]` = S`+0x20`.

### 5.6 Rhythm, battery, connect dump

- `BA` (6 B): `[on 0/1][? 0/1][pattern 0..39][volume 0..100][tempo u16]`,
  flash `0x81000`, default `00 00 00 64 6E 00` (tempo 110). The stock clamps on
  load and always boots with it off.
- `BB` (2 B): `[battery level * 25][charging]` (BLE on change).
- `B5 [1]`: sent in the connect dump (the stock sets a "connected" flag).
- `94` (connect) -> `A1 [S+0x16][edit]`, `B0`, `B7`, `BA`, `BB`, `B5 [1]`,
  `83` (cab block), `C9 [S+0x20]`, all to the requester.

### 5.7 IR slots

Slots are 1-based `1..9` (the stock range-checks the slot against 50 but only
stores 9). Storage: names 9 x 50 B at flash `0x87000`, used flags at
`0x88000`, data `0x2800` B per slot at `0x89000 + (slot-1) * 0x2800`.

- `61` upload: `[type=1][slot u16][total][index][len u16][data]`. Index 0
  carries the name (<= 50 B, printable), indices 1.. carry data appended to a
  `0x2800` buffer; when `index == total - 1` and all indices arrived in
  order, the name, flag and full `0x2800` data are written to flash and BLE
  gets `69`. Every frame is answered `62 [01][slot][00][index]` (the slot's
  high byte is always 0). The official app sends 1 + 8 frames (4096 B).
- `63 [1][slot u16][op]` -> `64`: empty slot `[1][slot u16][00]`; used slot,
  op 0 `[1][slot u16][01][21][00][50 u16][name 50]`; op k = 1..20
  `[1][slot u16][01][21][k][0x200 u16][512 B = data chunk k]`.
- `65 [1][first u16][last u16]` -> `66`: one 59-byte record per slot.
- `67 [1][slot u16][op][name 50]`: op 1 delete (flag 0, name "Empty"),
  op 2 rename (only if used). Reply `68 [1][slot u16][ok]`, then BLE `69`.
- `69` record (59 B): `[1][slot u16][used][used ? 2 : 0][name 50][used ? 50 : 0][0 0 0]`.
- `9B [b]` (stock, to USB): sent when a BLE IR upload starts (`00`) / ends
  (`01`), and for a BLE `D6`.

### 5.8 Notifications the pedal sends by itself (BLE only in the stock)

| Trigger | fn |
|---|---|
| knob changes a module parameter | `80`..`86` block |
| footswitch / bank selects a preset | `98 [index]`, `83` |
| long-A save | `97 [index][preset]`, `98 [index]`, `B0` |
| tuner / stomp-mode / rhythm changes | `B0`, `BA`, `C9 [S+0x20]` |
| battery level change | `BB` |
| IR stored / renamed / deleted | `69` |

`B2` is the **factory reset** acknowledgement (the earlier reading of `B2` as
an "IR list changed" notification was wrong: the stock only sends it in reply
to a `B2` request).

### 5.9 Our firmware

`firmware/audio/src/proto/proto.{c,h}` implements all of the above
transport-independently (`proto_feed`, per-transport senders, notification
API, weak hooks for Bluetooth/AT, updater and factory reset); host tests in
`tests/test_proto_host.py`. Differences from the stock: `FA`/`C3` reply to the
requester, not USB only; slots above 9 are ignored instead of corrupting
state; the notification mask is configurable (`proto_set_notify_mask`,
default BLE like the stock); `9B` is not sent; the version reply reports our
firmware version (`PROTO_FW_VERSION`).

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

- **Stock firmware emulation (2026-09-27).** The command set in §5 was read
  from the stock `V1.0.1` image and confirmed by running its dispatcher and
  reply builder in Unicorn on the RAM images produced by the vendor loader,
  feeding crafted frames and capturing the frames it sends (USB and BLE
  senders hooked) and its flash writes.

## References

- Design notes: `docs/superpowers/specs/2026-09-25-fb200-tools-design.md`
- FF20 community project: https://github.com/wattsline/Flamma-FF20
- Similar Mooer projects: [ThijsWithaar/MooerManager](https://github.com/ThijsWithaar/MooerManager),
  [shpala/MooerLooperManager](https://github.com/shpala/MooerLooperManager),
  [utajum/mooer-drummer-x2](https://github.com/utajum/mooer-drummer-x2)
