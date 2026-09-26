# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.4.0] - 2026-09-27

### Added

- `fb200 fw pack` for building app-only (single-block) `.mr` images.
- `firmware/hello`: a minimal custom firmware for the i.MX RT1062 (pinned
  TinyUSB 0.21.0, CDC-ACM device) that builds in CI and packs into a flashable
  `.mr`.
- `docs/FIRMWARE_BRINGUP.md`: vendor loader/relocation findings and the
  hardware bring-up investigation.

### Notes

- `fb200-hello` boots on hardware in the vendor image format: USB
  `0xCAFE:0x4001` ("FB200 Hello"), CDC banner and echo verified. The vendor
  boot contract (block-0 loader/table, fixed entry at ITCM 0x4d6) is
  documented in `docs/FIRMWARE_BRINGUP.md`.

## [0.3.0] - 2026-09-26

### Added

- `fb200 fw flash`: firmware flashing client with dry-run default, `--yes`
  opt-in and `--no-jump` recovery mode.
- `fb200.updater`: flash-plan builder and `FirmwareUpdater` (erase, page
  writes, exit), validated byte-for-byte against the official updater and on
  hardware.
- Documentation: `docs/UPDATE_AND_RECOVERY.md`, `docs/PATCHING.md`,
  `docs/RESEARCH.md`.
- Hardware smoke tests (`pytest -m hardware`).
- Hardware-verified proof patch: `V1.0.1` → `V9.9.9` reported by `fb200 info`
  after flashing, then reverted with the stock image.

### Fixed

- Close HID handles on the `fw flash` jump path; identify the target device
  before flashing; cover the `--yes` paths with tests.
- Allow up to 60 s for the bootloader erase acknowledgement (measured ~11.5 s
  on hardware; the previous 10 s timeout aborted a valid erase).

### Safety

- Flashing writes firmware to hardware and can leave the pedal in its
  bootloader. Always keep a stock `FB200.mr` locally and read
  `docs/UPDATE_AND_RECOVERY.md` before flashing.

## [0.2.0] - 2026-09-25

### Added

- `.mr` firmware container parser/serializer (`fb200.firmware`) that preserves
  uninterpreted bytes verbatim.
- CLI: `fb200 fw inspect`, `fb200 fw extract-block`, `fb200 fw patch-string`
  (same-length patches only).
- Documentation: `docs/FIRMWARE_FORMAT.md`, `docs/FIRMWARE_ANALYSIS.md`,
  `docs/HARDWARE.md`.

## [0.1.0] - 2026-09-25

### Added

- HID transport (`fb200.transport`) and framing/CRC16 protocol
  (`fb200.protocol`, documented in `docs/PROTOCOL.md`).
- Device API (`fb200.pedal`): product/version info, IR list/import/delete.
- WAV to IR conversion (`fb200.wav`).
- CLI: `fb200 info`, `fb200 ir list/import/delete/backup`, `fb200 probe`.
- Test suite and GitHub Actions CI.
