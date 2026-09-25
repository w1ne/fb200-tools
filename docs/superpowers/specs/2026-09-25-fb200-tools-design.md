# fb200-tools — Design

Date: 2026-09-25
Status: approved (brainstorming complete, ready for implementation plan)
Repo: github.com/w1ne/fb200-tools
License: MIT

## 1. Purpose

An open-source, cross-platform utility and research repository for the FLAMMA FB200
bass multi-effects pedal (rebranded Mooer hardware). It provides:

1. A Python library and CLI for device info, IR (impulse response) management, and
   firmware inspection/patching/flashing.
2. Deep reverse-engineering documentation of the USB HID protocol, the `.mr`
   firmware container, and the firmware internals.
3. A safe, recovery-first firmware modding workflow, culminating in a visible
   proof patch flashed and verified on real hardware.

Motivation: the official editor is a rosetta-era Electron app; there is no
Linux support, no scripting, and no public documentation. Comparable community
tools exist for sibling devices (Flamma FF20, Mooer GE200/GL100, Mooer
Drummer X2), but nothing for the FB200.

## 2. Verified research (foundation)

All findings below were verified against a real FB200, the official
`FB200_V1.0.1_Mac.dmg` editor (Electron) and its bundled `FB200.mr` firmware
image.

### 2.1 USB identities

| Mode | VID | PID | Notes |
|------|-----|-----|-------|
| Application | `0x34DB` | `0x800F` | USB Audio (2 in / 2 out @ 44.1 kHz, class-compliant) + vendor HID interface (interface 3) |
| Update/bootloader | `0x0483` | `0x5703` | Entered after the `0xC1` command; common Mooer update identity |

### 2.2 HID wire format

- HID reports are 64 bytes. The first byte is the number of valid payload bytes
  that follow; remaining bytes are zero padding.
- Payloads larger than 63 bytes are chunked into 63-byte HID reports.
- Frame: `AA 55 | len(u16 LE) | fn | data | CRC16`
  - `len` = length of `fn + data`.
  - `CRC16` = table-driven CCITT-style CRC with XOR-0xFFFF final (table in
    Appendix A, identical in the official app and the FF20 community project).
  - The official app writes an identify byte (`11` = PC, `10` = BLE) before
    `fn`; the FB200's USB HID interface accepts frames without it. Identify
    `11` frames receive no reply over USB. The tool therefore sends
    identify-less frames.
- Device replies use the same framing (without an identify byte).

### 2.3 Command set

| fn (hex) | Direction | Meaning | Payload / reply |
|----------|-----------|---------|-----------------|
| `00` | host→dev | Get version | reply `fn=01`: product `FB200` at `[0:32]`, `V1.0.0` at `[32:39]`, `V1.0.1` at `[39:45]`, `V1.0.0` at `[46:53]`, `A` at `[53:]` |
| `61` | host→dev | Upload IR (9 frames) | `[fileType=1, index u16 LE, totalFrame, frameIdx, dataLen u16, name or 512B of float32 LE]`; reply `fn=62` per frame |
| `63` | host→dev | Query IR slot | `[1, index u16 LE, 0]`; reply `fn=64`; `data[3]==0` empty, else name length at `[6:8]`, name at `[8:8+len]` |
| `67` | host→dev | Delete IR slot | `[1, index u16 LE, 1]`; reply `fn=68`; `data[3]==0` failure |
| `C1` | host→dev | Jump to bootloader | device re-enumerates as `0483:5703` |
| `B2` | dev→host | Device notification | client should re-query IR list |

IR slots are 1-based (1..9). Names are sanitized to printable ASCII, max 50 chars.
Offsets in the table above are within the reply payload (after the `fn` byte).

### 2.4 IR format

The official renderer decodes any WAV to 44.1 kHz, takes channel 0, truncates
or zero-pads to exactly **1024 float32 samples** (4096 bytes), and uploads it
in 8 data frames plus 1 metadata frame. The user manual claims "512 points
24-bit" — discrepancy to document as an open question; the tool follows the
app's 1024-float format because it is proven to work.

### 2.5 Firmware container (`.mr`)

Layout parsed by the official updater:

- 128-byte header: `TAG` (`Mooer_TAG`), `PRODUCT_TAG` (32 bytes, `FB200`),
  `SEND_CMD`, `REC_CMD`, `TIMEOUT` (u32), `UPDATE_BLOCK` (u8), `UPDATE_ADDR`
  (4 bytes), `VERSION` (u8).
- For each block: 512-byte tag (`TAG`, `START_ADDR` u32, `STOP_ADDR` u32,
  `BLOCK_SIZE` u32, `SEND_CMD` u8, `REC_CMD` u8, `TIMEOUT` u32,
  `START_PAGE` u32, `ROM_ID` u8) followed by `BLOCK_SIZE` data bytes.
- FB200 `V1.0.1` image: header SEND_CMD `0x02`; block 0 (app) 200,704 bytes,
  SEND_CMD `0x04`, START_PAGE `0x40`; block 1 (models) 3,286,016 bytes,
  SEND_CMD `0x06`, START_PAGE `0`.

### 2.6 Firmware internals (static analysis)

- Block 0: Cortex-M vector table at file offset 0 — SP `0x20058000`, reset
  `0x600104d9` → application linked at flash `0x60008000` (i.MX RT-style
  FlexSPI mapping at `0x60000000`), implying a 32 KB bootloader at
  `0x60000000`. Contains UI/preset strings ("Fat BassX", "Tri Chorus", …),
  Bluetooth module AT commands (`AT+BD%-15.15s`, `AT+BM…`, name
  `FB200 Audio`), and "Copyright 2016 Mooer Audio Corporation".
- Block 1: begins with a table (`count=20`) of model entries, mostly
  176,400 bytes (exactly 1 s of 44.1 kHz float32) plus a few smaller ones;
  data is float32 coefficient/audio tables. Likely the 10 amp + 10 cab model
  library.
- Open questions: exact MCU part (i.MX RT10xx variant), whether the
  bootloader verifies image integrity (no signature field observed and the
  official updater sends none), 512-vs-1024 IR points, block 1 model format.

### 2.7 Update protocol

Replayed from the official app:

1. `fn=0xC1` → device re-enumerates as `0483:5703`.
2. Erase: frame `fn=header.SEND_CMD (0x02)`. For `VERSION == 0` (stock
   FB200) the payload is the 4-byte `UPDATE_ADDR`; otherwise
   `[ROM_ID, START_PAGE(4 LE), BLOCK_SIZE(4 LE)] × blocks`; wait for reply
   `0x03`.
3. Per block: frames `fn=block.SEND_CMD` (0x04/0x06) with
   `[page u16 BE, 512-byte chunk]`, one frame per 512 bytes, waiting for reply
   `SEND_CMD+1` each time. The page is the low 16 bits of the u32 LE
   `START_PAGE + chunk index`, sent big-endian.
4. `fn=0xFF` to exit/reboot.

No image signature or per-image checksum is transmitted by the official
updater, which makes same-length patched images plausible. This must be
validated by a stock round-trip before any patched write.

## 3. Scope

### In scope (v1)

- `fb200` Python package: transport, protocol, pedal, firmware, updater, wav, cli.
- CLI: `info`, `ir list|import|delete|backup`, `fw inspect|extract-block|patch-string|flash`,
  `probe`.
- Firmware flashing client with dry-run default, explicit `--yes`, product-tag
  validation, progress reporting.
- Proof patch: same-length replacement of `FB200 Audio` (Bluetooth name) in
  block 0; flashed and verified on hardware.
- Research documentation (protocol, container, analysis, hardware, recovery,
  patching, narrative, credits).
- Tests with a mock transport; CI on Linux/macOS/Windows for Python 3.10–3.14.
- MIT license, disclaimers, GitHub publish under `w1ne`.

### Out of scope (v1)

- Desktop GUI (possible later).
- Preset backup/restore over USB (no commands discovered; likely BLE-only).
- Firmware feature unlocks (IR slot count, model replacement) — research v0.4+.
- Custom firmware from scratch.
- Redistribution of stock firmware or vendor application files.

## 4. Architecture

```
CLI (cli.py)
  └── pedal.py (FB200Device)        updater.py (FirmwareUpdater)
        └── protocol.py (frames, CRC, commands)
              └── transport.py (Transport interface, HidapiTransport)
firmware.py (MrFile) — pure data, no USB
wav.py (WAV → 1024 float32) — pure stdlib, no USB
```

- `Transport` is an interface: `open()`, `close()`, `write_frame(bytes)`,
  `read_frame(timeout)`, `enumerate()`. `MockTransport` replays scripted
  frames for tests.
- `pedal.FB200Device` exposes `info()`, `ir_list()`, `ir_import()`,
  `ir_delete()`, `enter_bootloader()`.
- `firmware.MrFile` parses/serializes `.mr`; provides block access,
  `find_strings()`, and `patch_string_equal_length()`.
- `updater.FirmwareUpdater` builds erase/write/exit frames from an `MrFile`
  and drives either the app-mode device (jump first) or an already-booted
  device; `dry_run=True` by default.

## 5. CLI

| Command | Description |
|---------|-------------|
| `fb200 info` | Device versions and USB IDs |
| `fb200 ir list` | List 9 slots (name/empty) |
| `fb200 ir import <slot> <file.wav> [--name NAME]` | Convert and upload |
| `fb200 ir delete <slot>` | Delete slot |
| `fb200 ir backup <dir>` | Write manifest JSON of slot names (payloads not downloadable) |
| `fb200 fw inspect <file.mr> [--strings] [--json]` | Container structure; optional string dump |
| `fb200 fw extract-block <file.mr> <n> <out.bin>` | Extract a block payload |
| `fb200 fw patch-string <file.mr> --find S --replace S [-o out.mr]` | Same-length string patch |
| `fb200 fw flash <file.mr> [--yes] [--no-jump]` | Validate and print the flash plan; writes only with `--yes` |
| `fb200 probe [--send HEX] [--listen N]` | Advanced raw research tool |

Exit codes: 0 success, 1 user/device error, 2 usage error, 3 post-flash
verification failure (device does not re-enumerate or does not answer the
version command after reboot).

## 6. Safety and recovery

- Flash is dry-run unless `--yes`; prints block plan, target device, and image
  summary before writing.
- Only images with `PRODUCT_TAG == FB200` are accepted for flashing.
- Stock image is never modified in place; patches write to a new file unless
  `-o` is explicitly given.
- On write failure the pedal remains in bootloader mode; re-running
  `fb200 fw flash` recovers. Documented.
- Hardware recovery spike: test hold-footswitch combos at power-up to detect
  bootloader re-enumeration (non-destructive; power-cycle to exit). Result
  documented in `UPDATE_AND_RECOVERY.md`.
- Repo contains no stock firmware or vendor binaries.

## 7. Testing

- Unit: CRC vectors cross-checked against captured frames; frame
  pack/parse/chunking; `.mr` fixture round-trip (synthetic image); WAV
  conversion edge cases; updater command sequence against `MockTransport`.
- Hardware tests under `tests/hardware/`, marked `hardware`, skipped by default;
  run manually with a connected pedal.
- CI: `ruff check`, `pytest -m "not hardware"` on ubuntu/macos/windows ×
  Python 3.10–3.14.

## 8. Documentation set

- `README.md` — overview, install, quickstart, screenshots, warnings, credits.
- `docs/PROTOCOL.md` — framing, CRC, command table, examples.
- `docs/FIRMWARE_FORMAT.md` — `.mr` container spec and page math.
- `docs/FIRMWARE_ANALYSIS.md` — block 0/1 analysis, model table, strings.
- `docs/HARDWARE.md` — USB topology, MCU evidence, memory map hypotheses.
- `docs/UPDATE_AND_RECOVERY.md` — official update flow, our client, recovery.
- `docs/PATCHING.md` — how to make safe same-length patches.
- `docs/RESEARCH.md` — narrative, method, similar projects, references.
- `DISCLAIMER.md` — not affiliated with Flamma/Mooer; warranty/risk notice.

## 9. Images and branding

- Self-made banner and SVG diagrams committed to `images/`.
- Product photos: only photos taken by the maintainer, or external links with
  credit. No vendor marketing images committed.
- Trademark names used nominatively only.

## 10. Milestones

| Version | Content |
|---------|---------|
| v0.1 | transport/protocol/pedal + `info`/`ir` + WAV conversion + tests + docs skeleton |
| v0.2 | `firmware.py` + `fw inspect/extract-block/patch-string` + format/analysis docs |
| v0.3 | `updater.py` + `fw flash` + recovery docs + stock round-trip on hardware + proof patch verified |
| v0.4 | Research-led mods: IR slot count, model data experiments |

## 11. Risks

| Risk | Mitigation |
|------|------------|
| Patched image rejected or bricks app | Stock round-trip first; bootloader is separate from app area; re-flash recovery |
| Hardware combo for recovery unknown | Spike + document; app-mode `0xC1` path always available while app boots |
| Firmware write interrupted (power) | Document; keep last stock image; retry from bootloader |
| Vendor DMCA/copyright concerns | No vendor binaries redistributed; documentation and clean-room tooling only |
| Wrong `.mr` variant flashed | Strict `PRODUCT_TAG` check, size/page validation, dry-run |
| IR format discrepancy (512 vs 1024) | Follow proven app behavior; document open question |

## Appendix A — CRC16

Table-driven, 256-entry table (as shipped in the official app and the FF20
Tools project). Algorithm:

```
crc = 0
for b in data:
    crc = (TABLE[((crc >> 8) ^ b) & 0xFF] ^ ((crc << 8) & 0xFFFF)) & 0xFFFF
crc ^= 0xFFFF
```

Verified sample: frame `AA 55 01 00 00` → CRC `C8 CF`.

## Appendix B — Captured version reply

```
3e aa 55 38 00 01 46 42 32 30 30 00 … 56 31 2e 30 2e 30 00 56 31 2e 30 2e 31 00
56 31 2e 30 2e 30 00 41 00 8f 34 00
```

Decodes to: product `FB200`, versions `V1.0.0` / `V1.0.1` / `V1.0.0` / `A`.
