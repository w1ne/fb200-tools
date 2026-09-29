# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.10.0] - 2026-09-29

Faster, cleaner, safer and easier on the battery. Measured on a pedal: the DSP
engine at 8 % of the CPU (was 14-15 %), no ticks in USB playback, a 30-minute
soak without faults.

### Changed

- **Amp 30 % faster, bit-exact** (`tone_df1`: biquad stages in pairs, fused
  oversampler): 21.4k -> 15.0k cycles per block on the pedal.
- **Cab 2x faster:** the head convolver did two FFT steps per block after any short
  audio block. It re-phases once now, and the engine passes only whole 32-frame blocks
  (`src/audio/sai_ring.h`). 512 taps 20.9k -> 10.7k, 4096 taps 37.7k -> 27.0k cycles.
- **Sound:** DAC and USB capture round to 16 bit (were truncated; `dither on` for
  TPDF), the EQ runs in double (no added low-band noise), the delay line stores a
  16-bit float format (cleaner quiet repeats), and USB playback is resampled to the
  codec clock: no ticks (was one about every 2.5 s).
- **Init and main-loop functions run from flash** (`COLD` marker, `src/cold.h`) to
  keep ITCM for the audio path.

### Added

- **Host fuzzing of the firmware parsers** (`tests/test_fuzz_host.py`,
  `firmware/audio/tests/fuzz_host_test.c`): the app protocol (HID/BLE frames,
  every command incl. IR upload, preset writes, rename, settings), the console
  (every command's arguments), presets/settings from flash and the stock data
  blob, under ASan/UBSan with clang (edge coverage) and gcc 14 (docker). A short
  fixed-seed run is in the default suite; `pytest -m fuzz` runs long ones.
- **Soak test** `tools/soak.py`: minutes of preset changes, parameter writes,
  console commands, IR lists and audio captures on a pedal; fails on skipped
  blocks, SAI over/underruns, new crumbs, missing replies or a low stack.
- **Console `stack`**: high-water of the 8 kB stack reserve (painted at boot).
  `firmware/tools/stack_usage.py` bounds the worst case from `-fstack-usage`
  and the call graph; the image tests keep 512 B of headroom.

### Fixed

- **Erased or corrupt presets/settings played as they were:** a power loss
  during a save erases a whole sector (8 presets, or the settings). An erased
  preset played every module at 655 % (a reverb that ran away to NaN); now it
  loads as the stock blank preset, and out-of-range fields are clamped to the
  stock's limits, also for whole-preset writes from the app (0x97). Erased
  settings load the stock defaults (they played at full master volume).
- **Stock data blob:** a CRC-valid blob with bad drum tables or NaN/Inf
  coefficients is refused (the drums walked past their event lists).
- **Console:** `peek`/`dumpmem`/`crc` checked only the ends of a range (reads
  of the unmapped ITCM/DTCM space, a `crc` spanning ITCM to flash: bus fault);
  `poke` wrote into the flash window; `gain`/`testgen` took values that
  overflow (infinite gain); commands without an argument read a NULL pointer.
- **Update session:** after an app update the pedal resets by itself when the
  host is gone (it stayed half alive until a power cycle).
- **Host tools:** a pedal reset mid-command gives a clear error at once, not a
  hang (a sub-ms HID timeout blocked forever); a busy console port says which
  port and what to do; read-only queries retry once.
- **BT name:** a control byte ends the name sent in AT commands.
- **LabWired twin: knobs.** `labwired/system.yaml` models the 16 knobs as
  potentiometers behind the two 74HC4051 multiplexers (select GPIO2_IO17..19,
  ADC1 IN3/IN4). New long gate `labwired/stock-knobs.yaml`: the unmodified
  stock firmware reads every knob into its knob table (DTCM `0x2001DED6` /
  `0x2001DEE6`) and follows a knob turned mid-run. Needs labwired-core with
  the `74hc4051` part.
- **Battery operation** ([docs/POWER.md](docs/POWER.md)): the main loop sleeps
  (`WFI`) when there is no audio block or USB event (the stock busy-loops at
  600 MHz); `cpu` shows the loop busy %. Unused PLLs, the second USB PHY and
  unused clock gates are off. `power led 100|66|33` dims the display, knob LEDs
  and light rings; `power idle <min>` (off by default) darkens the panel after
  idle minutes. `power clock 528|396` for measurements (not saved). Battery
  gauge: filtered, with hysteresis, an mV/% estimate, a "LOb" warning, and a
  critical state that saves the settings and stops flash writes.

## [0.9.1] - 2026-09-29

### Fixed

- **Audio drops out during flash writes:** a preset save, a settings write, an IR
  import or delete stopped the audio for tens of ms (the main loop waited for the
  flash, and the audio engine runs in it). The flash busy-wait now keeps the audio
  running (`src/debug/flash_rmw.c`); drums keep time but are silent during a write
  (their samples are in flash). Measured on the pedal: 0 skipped blocks for a save,
  a settings write and an IR import + delete (were 13 and 38).

## [0.9.0] - 2026-09-29

Bass EQ, 1 s delay that plays on the pedal, IRs up to 4096 taps on an FFT cab,
reamping over USB, an MCP server and a PoC app so an AI agent can drive the
pedal. The memory map now matches the chip (OCRAM is 32 kB on the RT1052).
Verified on a pedal.

### Added

- **Desktop app PoC (`app/`, `fb200-app`):** a local web UI (Starlette, 127.0.0.1) to
  edit the pedal like the vendor app - presets (list, select, rename, save), the 7
  effect blocks, delay, EQ, user IRs (import with the `process_ir` options, delete),
  drums, tuner, output and global settings - and an assistant panel: type "make it
  brighter" and a Claude agent (default `claude-sonnet-5`, or `claude-opus-5-5`) calls
  the MCP tools, shows each step, measures with `audio_test` and charts the octave
  bands. Tools that store to flash (`save_preset`, `rename_preset`, `ir_import`,
  `ir_delete`, `settings`, console `save`/`factory`) wait for a user click. The tool
  schema is the MCP server's `tools/list` (in process). Install: `pip install
  '.[app]'`. See `app/README.md`. Not yet tried on a real pedal.
- **MCP tools:** `parameter_docs` (also the resource `fb200://parameter-docs`: every
  field's range, unit and sound meaning, plus tone recipes, from `fb200/params.py`),
  `preset_list` (40 names, HID `0x96`), `rename_preset` (`0x99`), `ir_delete`,
  `settings` (the `0xB0` block: input gain, global cab, BT audio, light ring);
  `ir_import` takes the `process_ir` options (channel, trim, lowcut, highcut,
  minphase, normalize).
- **Reamping over USB:** console `usb in` routes the computer's USB playback into the
  effects chain input instead of the instrument (`usb mix`: summed with it; `usb out`:
  to the analog output only, the default and the stock behaviour). Play a DI track or a
  test sweep and record the processed sound from the USB capture. The MCP `audio_test`
  with `source="usb"` uses it (and restores the routing) and reports the round-trip
  `delay_ms`.
- **Bass EQ** (not in the stock): HPF 20-200 Hz, 5 peaking bands (30-10000 Hz,
  +-15 dB, Q 0.3-4), LPF 2-20 kHz, after the cab. Changes glide in 12 ms, no clicks.
  Bit-exact when off or flat. Console: `eq [on|off]`, `eq hpf|lpf <hz>`,
  `eq <band> <hz> <dB> [q]` (docs/PARITY.md M4).
- **Bass EQ in the preset:** the console `eq` writes the EQ into the edit buffer and
  `save` stores it: marker "EQ" at preset `0xc4` and 24 bytes of settings at
  `0xc6..0xdd` (the unused tail after the module order; gain in 1/8 dB, Q x 50).
  A preset change glides to its EQ (no click). Presets without the marker (all
  factory presets, presets from the stock app) play with the EQ off; the stock
  firmware and the stock app ignore the bytes and the app's module edits keep them.
- **MCP tools** `set_eq` (on/off, HPF, LPF, band freq/gain/q; returns the EQ state),
  `usb_route` (`out`/`in`/`mix`) and `cab_long` (N-tap synthetic cab IR for
  measurements, 0 = back to the preset's cab); the `console` tool lists `eq` and
  `cab long`.
- **Two-stage convolver for long IRs (M5, library only):** `dsp/conv2.c` runs
  IRs up to 4096 taps with no added latency: the 512-tap head on the current
  convolver, the rest in 256-sample partitions spread over the 8 blocks of each
  frame. Host tests against a direct FIR.
- **Cab on the two-stage convolver (M5 step P1b):** the cab can play IRs up to
  4096 taps (`cab_set_ir_len`; stock and user slots stay 512 taps for now). With
  <= 512 taps the tail does no work: same sound (bit-identical) and same CPU as
  before. A long IR loads without an audio stall: one tail FFT per block, then an
  exact swap at a frame boundary (~15-25 ms after the change). From a short IR the
  new tail starts empty and fades in over its length. Console: `cab long <taps>`
  loads a synthetic test IR (0: back to the preset's cab), for `prof` on the pedal.
- **Better IR import** (host): `fb200 ir import` resamples with a Kaiser windowed
  sinc (aliasing below -60 dB; the linear resampler is gone) and takes
  `--channel`, `--trim`, `--taps N` (up to 4096, half-Hann fade-out),
  `--lowcut`/`--highcut` (2nd-order Butterworth), `--blend other.wav:MIX`,
  `--minphase` (numpy, extra `[ir]`) and `--normalize`. `fb200 ir process -o out.wav`
  writes the processed IR without a pedal. API: `fb200.wav.process_ir()`. With no
  options a 44.1 kHz WAV gives the same samples as before.
- **MCP server** (`fb200 mcp`, `pip install 'fb200-tools[mcp]'`): an AI agent (Claude
  Code, Claude Desktop) drives the pedal over USB. Tools for status, the console, effect
  blocks, delay, drums, tuner, CPU profile, crash dump, user IRs and an audio test
  (test signal in, USB capture out: RMS, peak, THD, octave-band response).
- **LabWired stock first-boot gate** (`labwired/stock-first-boot.yaml`, long:
  about 30 min of CPU time). The unmodified vendor firmware boots from a blank
  flash, does its factory reset (sector erase and page program of F:0x82000
  and F:0xB0000, magics `FB200` and `B01`) and sends the Bluetooth AT
  sequence on LPUART5 (`AT+TM` .. `AT+B401`) at about 11 s of device time.
  See `docs/LABWIRED.md`.

### Fixed

- **Memory map: OCRAM is 32 kB.** The pedal's chip is an i.MX RT1052: its FlexRAM
  split (IOMUXC_GPR17 = 0xFFAAAAA9, read back on the pedal) gives ITCM 128 kB, DTCM
  352 kB and OCRAM 32 kB at 0x20200000. Our linker script declared 512 kB of OCRAM at
  0x20210000 (an RT1062 layout), where the pedal has nothing: writes are dropped,
  reads return 0, code there faults. Now `linker.ld` has the real regions (with
  `ASSERT`s), a test checks that every section of both images lies in real memory,
  and the boot emulation, the stock emulators and the LabWired chip model map only
  the real RAM (docs/FIRMWARE_BRINGUP.md, "Memory map").
- **The app crashed at boot** (unreleased): its cold code ran from the missing OCRAM.
  The cold code (console, UI, preset storage, protocol, clock and pin setup, init
  drivers) now runs in place from flash (XIP, the slot data area); the audio path and
  the flash write path stay in ITCM (`firmware/tools/hot_path.py` checks both). An app
  update runs from RAM only once it starts erasing (`fw_session`): send `reset` when it
  is done, as before. A slot without its data still goes back to recovery.
- **RAM reclaimed:** the low DTCM that the stock data used to fill (our images load
  none there) holds the long-IR tail, the DTCM above `.bss` the delay line (8 kB kept
  for the stack), the ITCM above the code the reverb state. The crash dump moved to
  0x20018A00 (an older recovery shows "no crash dump" for a newer app).
- **Bass delay: the repeats never played on the pedal in v0.8.0** (its line was in
  the missing OCRAM: only the dry signal came out). The line is now in DTCM, max time
  1000 ms (sized for 44.1 kHz, the pedal's only rate). `delay on` on a stock preset
  now sets 300 ms (was 350).
- **User IR slots (cab 11-19)** (unreleased): the IR was staged in the missing OCRAM,
  so the cab got the IR only while the D-cache still held it, else zeros (silence).
  The staging buffer is in the real OCRAM now.
- **Bass EQ** (unreleased): its state was in the missing OCRAM (it held only while
  in the D-cache). Now in the real OCRAM.
- **Long IRs** fit in the real RAM (tail in the low DTCM, FFT tables in OCRAM):
  `ENGINE_IR_TAPS` (`src/audio/engine.h`) is 4096. A build with 512 links neither the
  tail nor the tables; there `cab long` over 512 answers "not available" (the MCP
  `cab_long` reports an error).

### Changed

- **Cab IR on the FFT convolver:** the 512-tap cab runs as a partitioned FFT
  convolution, not a direct FIR. No added latency. About a quarter of the cab CPU
  (estimate; measure with `prof`). Sound unchanged (stock parity gates). An IR change
  keeps the input history, as before. It uses 4 kB more DTCM.

## [0.8.0] - 2026-09-28

Bass delay, footswitch light rings as the stock, and the DSP chain at less than
half the CPU (CMSIS-DSP loop unrolling). Sound unchanged.

### Added

- **Bass delay** (not in the stock): 20-1000 ms, feedback, mix, low cut on the
  repeats (20-500 Hz), tone. Between MOD and reverb. Stock presets keep their sound:
  the stock delay fields play only in presets with our marker (docs/PARITY.md M4).
  Console: `delay [on|off] [time] [fb] [mix] [lowcut] [tone]`.
- **LabWired twin** of the board as YAML (`labwired/`): MIMXRT1052 chip, FB200
  board, a smoke gate for the open smoke firmware and a stock-boot gate that
  boots the unmodified vendor firmware to USB enumeration. `tools/labwired_stock.py`
  builds the stock ELF from your own `.mr`. See `docs/LABWIRED.md`.
- **Footswitch light rings** (the 40 RGB LEDs, 10 in each footswitch dome), as the
  stock LED code: preset mode lights the loaded slot's dome in its colour from the
  app (colour and level per slot), live mode lights a dome per module that is on,
  the tuner turns them off, rhythm mode shows A/B held, the tempo on C and play on
  D. A save blinks the saved slot's dome for 1 s. Frames go out only on a change.

### Changed

- **DSP 2.4x less CPU:** CMSIS-DSP is built with its loop unrolling; the cab IR
  runs 3.2x faster. Preset 0 (amp + cab) 36% -> 15% CPU; the heaviest preset 27%
  average, 31% peak. Sound unchanged (amp + tone bit-exact with the stock, amp +
  tone + cab -110 dB). Console: `prof` shows the cycles per chain stage.
- Tests: `pytest` runs in under a minute; the slow stock-DSP parity tests are
  `pytest -m stock`, run nightly by `tools/nightly_stock.sh`.

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
