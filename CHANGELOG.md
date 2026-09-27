# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.7.0] - 2026-09-27

Stock parity: save, bank browsing, rhythm-mode buttons, app commands, global
settings and factory reset now work as on the stock firmware (docs/PARITY.md).
Host tools ask the pedal which stock data versions it accepts (`fwstock` with no
arguments), so an older app never gets a blob it cannot read.

### Changed

- **Save as the stock:** hold A, B, C or D for 1 s to save to that slot, in preset
  and live mode. Bank chords keep your edits: the display flashes the new bank; A-D
  load from it, a hold saves to it (save to another bank).
- Rhythm mode buttons as the stock: A/B rhythm, C tap tempo, D play/stop.
- App commands act at once: drums (`BA`), rhythm mode (`C9`), tuner (`B8`, `B0`);
  the pedal's drum notifications carry the current values.
- Input gain, tuner calibration and tuner mute from the app are used. The Bluetooth
  audio switch survives a reboot. Battery notification to the app on change.
- Knob LEDs off in tuner mode; live-mode C turns amp and cab off if either is on.
- Stock sound data version 2 adds the factory presets (version 1 still works).

### Added

- Factory reset from the app (`B2`) or the console (`factory yes`). Needs the sound
  data version 2: run `fb200 update stock FB200.mr` again once.

## [0.6.0] - 2026-09-27

### Added

- **Web updater** (GitHub Pages, Chrome/Edge on Windows, macOS, Linux, ChromeOS):
  first install and updates from the browser, with a step-by-step visual guide.
  Nothing to install.
- `fb200 update app latest` downloads the latest release and flashes it.
  `fb200 fw twostage FB200.mr` makes the first-install image.
- Releases publish the firmware images (built in CI) and deploy the web page.
- Buy me a coffee link.

### Changed

- **The firmware images contain no vendor data.** The stock sound data (amp models,
  cab IRs, tone stack, drum rhythms) is built from your stock `.mr` once
  (`fb200 update stock FB200.mr`, console `fwstock`) into its own flash area
  (F:0x61000). App updates never touch it and need no `.mr`. Console `stock` shows
  its state.
- The stock data is unpacked in pure Python (the stock loader's LZ77): building no
  longer needs `unicorn`.
- `fb200 update` checks the flash mapping with a built-in CRC, so it needs no `.mr`.

### Removed

- `extract_stock_dsp.py`, `gen_stock_drums.py` (replaced by `src/fb200/stockdata.py`).
- Finished implementation plans (`docs/superpowers/plans`).

### Fixed

- CI: Windows, Python 3.10 and Linux host builds of the DSP tests.

## [0.5.0] - 2026-09-27

First release of the open FB200 firmware (`firmware/audio`), with host tools.

### Added

- **Open firmware with stock-sound parity.** Gate, compressor, 10 amp models + tone
  stack, 10 cab IRs + 9 user-IR slots, 12 modulations, 5 reverbs. Each module passed
  against an emulation of the stock DSP (amp/tone bit-exact, the rest within -105 dB).
- Drum machine (40 rhythms, stock samples played in place from the pedal's flash) and
  tuner, both matching the stock in emulation.
- Front panel: 3-digit display, 16 knobs with pickup and LEDs, the stock footswitch
  actions (slots, bank chords, stomp mode, tuner, rhythm mode, save), 40 RGB LEDs,
  battery/charger status.
- Presets and settings in the stock flash format. The stock app protocol over
  Bluetooth and USB HID, with the stock USB identity (`34DB:800F`).
- USB audio interface (UAC2, 44.1 kHz) and Bluetooth audio in.
- Two-stage boot: a resident recovery plus an application slot. Updates over USB with
  no button combo; recovery takes over after a fault or a hang; crash dumps survive a
  reset.
- Host tools: `fb200 console`, `fb200 update app|recovery`, `fb200 crash --elf`.
  Firmware tools: `build_images.sh`, `pack_images.py`, and `boot_dry_run.py`
  (full-chain boot emulation that fails on unsafe early peripheral access).
- Docs: `INSTALL.md`, `PARITY.md`, `ROADMAP_RESEARCH.md`, `AUDIO_PATH.md`,
  `UI_AND_STORAGE.md`, `BOOTLOADER.md`, and a full `PROTOCOL.md` command set.

### Better than stock

- The display names the knob being turned and shows whether it has picked up.
- Hold A and turn LEVEL/RATE/MOD to change drum level, tempo and rhythm.
- Drums are included in USB recordings.
- An empty user-IR slot bypasses the cab instead of silencing the pedal.
- Drum hits start on time (the stock ignores the sample bank header).
- The app's "enter updater" command never touches the vendor bootloader flag,
  which the A+D recovery depends on.

### Notes

- Build with your own official firmware file. The repository ships no vendor code
  or data, and the built images must not be redistributed.
- DSP libraries: CMSIS-DSP v1.18.0 (pinned, checksummed). Toolchain: an Arm GNU
  toolchain with newlib.

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
