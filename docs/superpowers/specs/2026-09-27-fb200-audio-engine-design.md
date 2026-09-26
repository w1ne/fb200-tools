# FB200 Open Firmware — Milestone 1: Audio Engine Bring-Up

Design spec for the first milestone of the open-source firmware effort for the
FLAMMA FB200. Approved through the brainstorming process on 2026-09-27.

## 1. Context

`fb200-hello` (v0.4.0) proved the vendor boot contract and USB enumeration.
The next step is the **open firmware**: an open-source replacement for the
stock firmware that sounds as good or better, is simple and user-friendly, and
integrates the best available open-source DSP.

The overall effort was decomposed into sub-projects (approved 2026-09-27):

1. **Audio engine bring-up** — this document.
2. **DSP framework + effects** — chain, presets, SOTA open-source DSP
   (neural amp captures, cab IRs, algorithms).
3. **Controls & UI** — footswitches, LCD, LEDs, on-device preset browsing.
4. **IR/model management** — upload/list/delete over USB, flash layout.
5. **BLE + open companion app** — reverse the BLE protocol, open app.
6. **Content** — open-licensed cab IRs, amp captures, factory presets.

Order: 1 → 2, with 3/4/5 later and 6 ongoing.

## 2. Goal and acceptance criteria

**Deliverable:** a new firmware project `firmware/audio/` (working name) that
boots in the vendor image format, brings up the codec + SAI + USB audio, and
passes audio with the DSP hook in place. Effects, UI, BLE and IR management are
out of scope.

| # | Criterion | Target |
|---|-----------|--------|
| 1 | Analog in → analog out passthrough, 48 kHz, mono→stereo, gain node active | works, unity gain ±0.2 dB |
| 2 | USB audio: playback audible on the analog outs; capture records the processed input; class-compliant on macOS | works at 48 kHz |
| 3 | Frequency response 20 Hz–20 kHz | ±0.5 dB or no worse than stock |
| 4 | THD+N @ 1 kHz, −6 dBFS | ≤ 0.03 % (−70 dB) or no worse than stock |
| 5 | Noise floor (A-weighted) | ≤ −85 dBFS or no worse than stock |
| 6 | Analog round-trip latency (ADC→DSP→DAC, 32-frame blocks) | ≤ 2 ms |
| 7 | Cold boot, vendor-format flashing, A+D recovery | unchanged (regression) |
| 8 | Not worse than the stock firmware on the same measurements | measured baseline |
| 9 | Guitar listening: clean, no artifacts or zipper noise | user confirms |

**Baseline definition:** "stock" means the stock firmware with a flat/clean
preset (its true-bypass, if analog, is a different path and is measured
separately), same rig, noting stock runs at 44.1 kHz while ours runs 48 kHz.
Absolute targets may be limited by the analog front-end; the hard requirement
is matching or beating stock.

**Out of scope:** effects, presets, screen/UI, footswitches, BLE, HID
protocol, IR/model upload, audio over the analog path beyond passthrough.

## 3. Approach

**Hybrid (approach C):** reverse-engineer the stock firmware's board-specific
init first, then implement on proven MCUXpresso drivers.

- **Drivers:** `fsl_sai`, `fsl_edma`, `fsl_dmamux`, `fsl_lpi2c`, `fsl_clock`
  from the already-fetched TinyUSB/MCUXpresso dependency tree (BSD-3).
- **USB:** TinyUSB UAC2 (device) with a feedback endpoint, plus CDC for debug.
- **DSP:** our own small float32, zero-allocation block framework; effects
  arrive in milestone 2.
- Rationale: fastest path to verified audio; the only board-specific unknown
  (codec/clock/GPIO init) is extracted by RE and isolated in small testable
  units.

## 4. Phase 0 — stock firmware RE pass

Facts to extract before writing init code. Methods: static analysis of the
stock ITCM image, Unicorn emulation of the stock boot, I²C bus scanning and
register read-back from our own firmware, and datasheet cross-checks.
Measurements (bus scans, register dumps) are preferred over trial-and-error;
RE is the primary source, hardware measurements fill gaps RE cannot reach.

1. **Codec**: which LPI2C bus and address (the stock app has per-instance
   LPI2C driver descriptors); part number and register map (board marking
   `NAU88BL21`, Nuvoton NAU88 series — confirm against silicon); the stock
   init register sequence. Primary method: **register read-back** - with the
   stock app running, send the `0xC1` jump (no power cycle), flash a probe
   firmware, and read the codec's registers over I²C; if the codec's reset
   is not coupled to the SoC reset this dumps the stock's exact
   configuration. Secondary: address scan, static decode of the init code,
   emulator traces (the stock app's emulation currently stalls at display
   init, so the audio init may be unreachable there).
2. **Clocking**: MCLK source and rate, SAI dividers, PLL settings for the
   audio and USB clocks.
3. **GPIO/analog routing**: mute/enable pins, jack detect, true-bypass/relay,
   output amplifier enable, LED pins if relevant to audio bring-up.
4. **Audio path behavior**: whether USB playback passes through the DSP,
   whether the analog input mixes into the DAC path, and what the stock
   bypass does.

**Deliverables:** `docs/AUDIO_PATH.md` (facts + evidence, like
`FIRMWARE_BRINGUP.md`), and the codec init sequence captured as a test fixture
used by `audio/codec.c` unit tests.

## 5. Architecture

### 5.1 Image layout (vendor format, extends `fb200-hello`)

- Block 0 offset 0x000–0x400: our 256-entry vector table ([0] = SP, [1] =
  vendor stub `0x600104d9`).
- Offset 0x400–0x7D4: vendor boot region, byte-identical to stock (stub,
  position-independent loader, load table, thunks).
- Entry 0 payload (offset 0x7D4, ≤ 0x1DBC8 = 121,800 B): our ITCM payload
  (code + rodata), copied to ITCM 0x400, entry point at ITCM 0x4D6
  (`stage2.S`).
- Entry 4 (memset, stock): DTCM 0x20018B44 + 0x358A4 = our `.bss` (219 KB).
- Entries 1–3 (stock): leave stock data in DTCM 0x20000000–0x20018B44 and
  OCRAM 0x20200000–0x20205AA0; do not place our data there.
- Stack top 0x20058000 (image[0], as stock).
- **Safety by construction:** every RAM range we use is a range the stock
  firmware itself uses at boot (payload -> ITCM 0x400..0x1DFC8, bss -> the
  stock memset range, stack -> stock's stack). The exact FlexRAM bank split
  behind `IOMUXC_GPR17` is not verified; the firmware must not use ranges
  beyond stock's usage. Milestone 1 adds a boot-time check that the bss and
  stack ranges are writable and logs the result.
- `.data` lives in the ITCM payload (copied by entry 0). The vendor loader
  already zeroes bss (entry 4), so stage2's old ITCM-copy and bss-zero steps
  are reduced to no-op guards.
- Payload budget: hello is 27.8 KB; UAC2 + SAI/eDMA/LPI2C drivers are
  estimated at 70-90 KB total - expected to fit. CI fails the build if the
  payload exceeds 121,800 B; if it ever does, stage2 gains a relocation step
  that copies additional regions from flash into ITCM/DTCM/OCRAM (own loader
  table) - not needed for milestone 1.
- Flashing: `pack_vendor_image.py` (moved to `firmware/tools/` so both
  firmwares share it), `--app-only` to leave the model block untouched.

### 5.2 Audio pipeline

- **Sample rate 48 kHz**, 32-frame blocks (0.67 ms), float32 throughout.
- **Clock topology is determined in Phase 0** (who drives MCLK/BCLK/LRCLK:
  SoC SAI master with codec MCLK input, or codec master with the SAI as
  slave - `fsl_sai` supports both). The plan targets SAI-master with
  12.288 MHz MCLK (256xFs at 48 kHz); the MCUXpresso EVK clock config
  (PLL4 = 786.432 MHz / 64) is the starting point and must be verified
  against the board and the codec datasheet. USB is asynchronous with a
  feedback endpoint.
- SAI1 RX/TX with eDMA ping-pong; the DSP runs on each completed block.
- Milestone-1 signal flow:
  - `dac_out = dsp(guitar_in) + usb_playback` (clean monitor mix),
  - `usb_capture = dsp(guitar_in)`,
  - guitar input (mono) is duplicated to both channels; both channels are
    processed generically so later stereo effects work unchanged.
- **Latency budget** (analog in -> analog out): 32-frame blocks 0.67 ms +
  DMA double buffering ~0.67 ms + codec ADC/DAC group delay ~0.5-1 ms =
  ~1.8-2.3 ms. If the 2 ms target is missed, drop to 16-frame blocks
  (0.33 ms, higher per-sample CPU overhead) - measured, not assumed.
- No allocation or locks in the audio path; all state is preallocated.
- ISRs only from SAI/eDMA and USB; cross-context traffic uses single-producer
  single-consumer ring buffers.

### 5.3 DSP hook

```c
typedef struct { float in[2][DSP_BLOCK]; float out[2][DSP_BLOCK]; } dsp_block_t;
typedef struct dsp_node {
    void (*process)(void *ctx, const dsp_block_t *b, size_t n);
    void *ctx;
} dsp_node;
```

A chain is an array of nodes (gain, bypass, debug test generator in milestone
1). Nodes are pure functions of their state plus the block; the framework has
no knowledge of specific effects. The chain is host-compilable so DSP code is
unit-tested in CI without hardware.

### 5.4 USB and debug

- TinyUSB UAC2: 2 channels in, 2 channels out at 48 kHz, feedback endpoint
  driven by measured FIFO fill.
- CDC: non-blocking debug log, level meters, and counters (FIFO
  over/underruns, DMA errors) for development and measurement.
- Test generator (sine/sweep/impulse/white) selected by a one-byte CDC
  command (no command parser) - makes loopback measurements possible without
  external signal sources.

### 5.5 Error handling

- FIFO drift: drop/insert samples at bounds + counter (never unbounded
  latency).
- DMA error: mute + counter; SAI/DMA restarted.
- Codec init failure: boot muted with a log line; never hang.
- Mute-on-fault: detected faults (FIFO, DMA, codec errors) mute the DAC via
  the codec's soft-mute, not just zero samples.
- **Hard hangs:** a hang stops DMA refills and the SAI FIFO underruns; the
  resulting DAC behavior (silence vs stale samples) is verified during
  bring-up. If unsafe, a hardware watchdog (RT1062 WDOG, unused by stock) is
  added in a later milestone.

## 6. Components

```
firmware/audio/
  Makefile, tinyusb.lock, linker.ld
  src/main.c                 # init order + superloop
  src/stage2.S, src/startup.c, src/vectors.c
  src/audio/sai.c            # SAI1 + eDMA ping-pong
  src/audio/codec.c          # LPI2C + codec init/volume/mute
  src/audio/usb_audio.c      # TinyUSB UAC2 callbacks, feedback, FIFOs
  src/audio/engine.c         # block loop, mixing, drift, meters
  src/dsp/dsp.c/.h           # framework + chain
  src/dsp/gain.c             # gain node
  src/dsp/testgen.c          # sine/sweep/impulse/white
  src/debug/cdc_log.c        # non-blocking log over CDC
  board/                     # pins, clocks (from Phase 0)
  tools/                     # (shared packer lives in firmware/tools/)
```

Interfaces:

| Unit | Interface |
|------|-----------|
| `audio/sai.c` | `sai_init(48000)`, `sai_start()`, TX/RX block callbacks |
| `audio/codec.c` | `codec_init()`, `codec_set_volume()`, `codec_mute()` |
| `audio/usb_audio.c` | `usb_audio_init()`, SPSC ring push/pull |
| `audio/engine.c` | `engine_run()` (called on block completion) |
| `dsp/*` | `process(ctx, in[2][N], out[2][N], n)`; host-compilable |
| `debug/cdc_log.c` | `log_printf()` |

Each unit has one purpose and is testable independently: DSP on the host,
codec init against a recorded fixture, packer with synthetic templates,
SAI/engine via on-hardware loopback.

## 7. Verification

**Baseline:** measure the stock firmware's bypass path with the same rig
before measuring ours; acceptance requires matching or beating it.

**Unit/CI:** host-compiled DSP tests (pytest), packer tests, firmware build
with ITCM-payload size gate, artifact upload.

**Pre-flash gate:** every image is dry-run through the Unicorn boot emulator
(the v0.4 practice) before touching hardware.

**Hardware rig:** pedal USB to the Mac; Mac → TrueMix600 mixer → pedal analog
in; pedal analog out → mixer line in → Mac; headphones for monitoring.
Measurements with numpy/scipy scripts:

1. USB loopback: sweep/sine out → pedal → USB capture in; compute frequency
   response, THD+N, noise, latency (impulse).
2. Analog loopback: same signals through the mixer/analog path.
3. Testgen mode: pedal generates signals; measure at the mixer.
4. Guitar listening test.
5. Regression: cold boot, `fb200 fw flash` (A+D), recovery to stock.

**Metrics:** the acceptance table in section 2, reported in
`docs/AUDIO_PATH.md` or a measurement log committed with the firmware.

## 8. Risks and mitigations

| Risk | Mitigation |
|------|------------|
| Codec part/init unknown | Phase 0 RE + datasheet + I²C scan; loopback verifies; fallback is a deeper RE of the stock init |
| **UAC2 feedback + FIFO sync is the highest-uncertainty software piece** | Dedicated early bring-up task with its own USB loopback test; fallback: adaptive USB mode (no feedback) |
| Clock topology misread (master/slave) | Phase 0 determination; SAI supports both; rate measurement verifies |
| Analog front-end limits THD/noise | Targets are relative to stock; absolute values reported |
| Hang leaves the DAC in an unknown state | Verify underrun behavior; codec soft-mute on detected faults; WDOG as later safety net |
| ITCM 121,800 B payload limit | CI size gate; stage2 relocation fallback |
| Analog routing quirks (mute/relay/jack detect) | Phase 0 GPIO RE; loopback tests reveal silence/mute states |
| SOTA component licensing | Prefer MIT/BSD/Apache (CMSIS-DSP, NAM, RTNeural, airwindows, ChowDSP); GPL components only if the firmware accepts GPL — decision deferred to milestone 2 |
| DSP CPU headroom for later effects | Milestone 1 reports measured CPU headroom of the passthrough |

## 9. SOTA outlook (milestone 2, architected now)

- **CMSIS-DSP (Apache-2.0)**: FFT/FIR/biquad primitives for the cab-IR
  convolution engine and EQ.
- **NAM / NeuralAmpModelerCore (MIT)** and **RTNeural (BSD-3)**: neural amp
  captures (small models only; the RT1062 has no SDRAM, so model size and
  inference cost are hard constraints).
- **airwindows (MIT)**, **ChowDSP (BSD-3)**: high-quality effects algorithms.
- **guitarix / LV2 (GPL)**: only if we accept GPL for the firmware; the
  decision is explicitly deferred.

The engine's block size, float32 path and zero-allocation rules are chosen so
these can be integrated without redesign.

## 10. References

- Boot contract: [`docs/FIRMWARE_BRINGUP.md`](../../FIRMWARE_BRINGUP.md)
- Hardware: [`docs/HARDWARE.md`](../../HARDWARE.md), photos in
  [`docs/pcb/`](../../pcb/README.md)
- Update/recovery: [`docs/UPDATE_AND_RECOVERY.md`](../../UPDATE_AND_RECOVERY.md)
- Firmware format: [`docs/FIRMWARE_FORMAT.md`](../../FIRMWARE_FORMAT.md)
