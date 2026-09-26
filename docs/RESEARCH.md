# Research Notes

How the FB200 reverse-engineering work was carried out: the investigation
narrative, the method and safety rules used, the projects it cross-checks
against, and what is verified versus still pending.

Protocol, container and firmware details live in
[`PROTOCOL.md`](PROTOCOL.md), [`FIRMWARE_FORMAT.md`](FIRMWARE_FORMAT.md) and
[`FIRMWARE_ANALYSIS.md`](FIRMWARE_ANALYSIS.md); hardware evidence is in
[`HARDWARE.md`](HARDWARE.md). This document is the story behind them.

## 1. Narrative

1. **USB enumeration.** The pedal was connected and enumerated as a composite
   device at `34DB:800F`: a class-compliant USB Audio function (2 in / 2 out @
   44.1 kHz) plus a vendor-specific HID interface (interface 3). The audio
   function needed no driver; interface 3 became the research target.
2. **Read-only HID probing.** `fb200 probe --listen` captured raw 64-byte
   reports. Nothing was written until the framing was understood: the first
   write was the `fn=0x00` get-version frame, whose reply decoded cleanly into
   the product and version strings.
3. **Official app extraction.** The official editor ships as a macOS DMG
   (`FB200_V1.0.1_Mac.dmg`). It was extracted into a local working copy
   (`app-extracted/`), and the Electron main-process bundle
   (`app-extracted/out/main/index.js`) was read for protocol constants, CRC
   implementation, command payload layouts and the update sequence. No vendor
   files are committed.
4. **Framing and CRC discovery.** The `AA 55 | len u16 LE | fn | data | CRC16`
   layout, the 64-byte HID report chunking and the table-driven CRC were
   confirmed against captured frames and cross-checked against the FF20
   community project, which uses the same framing and CRC table.
5. **`.mr` container analysis.** The bundled `FB200.mr` was parsed into a
   128-byte header plus two tagged block records (application and models);
   `fw inspect`, `fw extract-block` and string scanning produced
   [`FIRMWARE_FORMAT.md`](FIRMWARE_FORMAT.md) and
   [`FIRMWARE_ANALYSIS.md`](FIRMWARE_ANALYSIS.md).
6. **Byte-exact update replay.** The erase/write/exit frame stream of the
   official updater was reconstructed, then compared byte-for-byte with the
   frames our `FirmwareUpdater` plan builder produces. The replay caught the
   big-endian page field and the `VERSION == 0` erase payload; both now match
   the official stream, and the plan is covered by
   `tests/test_updater_plan.py`.

## 2. Method

- **Read-only probing first.** Enumeration, HID report captures and string
  scans came before any write. The only application-mode command sent while
  the protocol was unknown was `fn=0x00` (get version), which is safe and
  idempotent.
- **No blind writes.** Erase/write/exit frames were derived from the official
  app and validated as a plan before any hardware write; flashing remains
  dry-run by default.
- **CRC anchors everything.** Every captured frame was checked against
  `crc16(01 00 00) == C8 CF` and its own trailing CRC before being trusted.
- **Identify byte.** The official app prepends an identify byte (`0x11` = PC,
  `0x10` = BLE) before `fn`. The USB HID interface ignores/accepts frames
  without it, and silently ignores identify-`0x11` frames, so `fb200-tools`
  sends identify-less frames.
- **Cross-checks over single sources.** Protocol findings were confirmed
  against a live pedal *and* the official app *and*, where applicable, the
  FF20 project.

## 3. Similar projects (prior art)

| Project | Relation |
|---------|----------|
| [wattsline/Flamma-FF20](https://github.com/wattsline/Flamma-FF20) | Sibling Flamma FF20 pedal; documents the same `AA 55` framing and CCITT-style CRC, used as an independent cross-check of the protocol work here. |
| [ThijsWithaar/MooerManager](https://github.com/ThijsWithaar/MooerManager) | USB control of Mooer pedals; prior art for talking to Mooer-based hardware from a host tool. |
| [shpala/MooerLooperManager](https://github.com/shpala/MooerLooperManager) | Manager for Mooer GL100/GL200 loopers; shows the family of Mooer device tools that exist. |

Other open-source tools exist for the Mooer family (for example GL100/GL200
loopers); the FB200 had no dedicated open-source tooling before this project.
Where projects overlap, they are credited as prior art and used to cross-check
findings, not copied.

## 4. Verified vs. not yet verified

**Verified against captures or static analysis:**

- USB identities and the composite topology (`34DB:800F`, `0483:5703`).
- Frame layout, report chunking, CRC16 and the `crc16(01 00 00) == C8 CF`
  vector.
- Application-mode command set (info, IR import/list/delete, `0xC1`).
- `.mr` container layout, field semantics that are marked verified, and the
  byte-exact round-trip of the parser/serializer.
- The erase/write/exit flash plan, replayed byte-for-byte against the official
  updater's frame stream.
- Firmware static analysis results (vector table, version strings, Bluetooth
  name, model-table counts).

**Not yet verified (Task 20, hardware validation):**

- Actually flashing a stock image and a patched image over USB, including
  post-flash verification on real hardware.
- The power-on footswitch recovery combos (see
  [`UPDATE_AND_RECOVERY.md`](UPDATE_AND_RECOVERY.md) §5.3).
- Whether the bootloader performs any image integrity check.

## 5. Open questions

1. **Footswitch recovery combos.** Which power-on footswitch combination (if
   any) forces the bootloader (`0483:5703`); see the results table in
   [`UPDATE_AND_RECOVERY.md`](UPDATE_AND_RECOVERY.md) §5.3.
2. **IR resampling best practice.** The official renderer decodes to 44.1 kHz
   and 1024 `float32` samples; the user manual says "512 points, 24-bit". The
   best resampling strategy for imported IRs is unresolved (see
   [`PROTOCOL.md`](PROTOCOL.md) §6 and [`FIRMWARE_ANALYSIS.md`](FIRMWARE_ANALYSIS.md) §5).
3. **Bluetooth name and bonded hosts.** Whether changing the Bluetooth
   friendly name (the `FB200 Audio` → `FB200 Tools` proof patch) breaks
   existing pairings on already-bonded hosts, or requires re-pairing.

## 6. Timeline

| Date (2026) | Milestone |
|-------------|-----------|
| 09-25, 18:19 | Design spec committed from the brainstorming session |
| 09-25, 18:31 | v0.1–v0.3 implementation plans committed |
| 09-25, 18:38–19:53 | v0.1 core: transport, CRC/framing, pedal info, IR list/import/delete, WAV conversion |
| 09-25, 20:03–20:23 | `info` / `ir` CLI, README and `PROTOCOL.md` published |
| 09-25, 20:25–20:54 | v0.2: `.mr` parser/serializer, `fw inspect` / `extract-block` / `patch-string`, format/analysis/hardware docs |
| 09-25, 20:56–21:18 | v0.3: flash-plan builder, `FirmwareUpdater`, `fw flash` CLI |
| 09-26, 03:02 | `fw flash` hardening: transport closure, target identification, `--yes` path coverage |
| 09-26 | This document set: update/recovery, patching and research notes |

## References

- USB protocol: [`PROTOCOL.md`](PROTOCOL.md)
- Container format: [`FIRMWARE_FORMAT.md`](FIRMWARE_FORMAT.md)
- Firmware static analysis: [`FIRMWARE_ANALYSIS.md`](FIRMWARE_ANALYSIS.md)
- Hardware evidence: [`HARDWARE.md`](HARDWARE.md)
- Update and recovery: [`UPDATE_AND_RECOVERY.md`](UPDATE_AND_RECOVERY.md)
- Design notes: `docs/superpowers/specs/2026-09-25-fb200-tools-design.md`
- FF20 community project: https://github.com/wattsline/Flamma-FF20
