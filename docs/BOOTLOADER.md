# FB200 Boot Chain and the DFU Handover

Reverse-engineered 2026-09-27 by dumping the bootloader region over the USB
console (`dumpmem`) from the running open firmware. This is how the pedal
boots, how it decides between the application and update (DFU) mode, and why
the handover into the vendor DFU failed, and the USB self-update that
replaces it.

## 1. Flash layout (verified by dump)

| Address | Contents |
|---------|----------|
| `0x60000000` | FlexSPI Configuration Block (`FCFB`, 512 B, lookup table) |
| `0x60001000` | IVT (header `0x412000D1`, entry `0x60002000`, DCD `0x60001030`, boot data `0x60001020`) |
| `0x60002000` | Bootloader vector table (SP `0x20050000`, reset `0x600024D1`) |
| `0x600024D0` | Bootloader reset handler: VTOR `0x60002000`, SP from `[0x60002000]`, FlexRAM GPRs (`0x00AA0000` / `0x00200007` / **`0xFFEAAAA9`**), SystemInit `0x600027E8`, then jumps to `0x60002400` |
| `0x60002400` | Bootloader main: the **same self-loading loader format** as the app (table + LZ decompressor + memcpy + memset) |
| `0x60008000`+ | Update/DFU code (USB descriptors for `0483:5703` at `0x6000AF40`), executed after the loader runs |
| `0x60010000` | Application image (the `.mr` block 0) |
| `0x60071000`.. | Presets, settings, IRs (see `UI_AND_STORAGE.md` §5) |
| `0x600D0000` | Model library (block 1; verified on the pedal: count 20) |
| `0x60086000` | Handover flag byte (see below) |

Note: the bootloader configures FlexRAM with `0xFFEAAAA9`; the stock
application reconfigures it with `0xFFAAAAA9` in its own reset stub.

## 2. The DFU decision block (`0x60008D50`–`0x60008D84`)

```
flag  = *(0x60086000)                      ; 0xFF = erased = "update allowed"
pin_a = read_pin(GPIO3, 12)                ; GPIO3_IO12 (pad GPIO_SD_B0_00)
pin_d = read_pin(GPIO2, 24)                ; GPIO2_IO24 (pad GPIO_B1_08)
if (pin_a || pin_d) {                      ; active high
    if (*(0x600CF000) == *(0x20010174) &&  ; flash == RAM handshake slot 1
        *(0x60003004) == *(0x2001051C) &&  ; flash == RAM handshake slot 2
        flag == 0xFF)
        enter_update_mode();               ; 0x6000333C (DFU: 0483:5703)
}
boot_application();
```

`read_pin` returns the raw GPIO `DR` bit (active high). `0x6000333C` is the
only DFU entry point in the bootloader, and this block is its only caller.

Interpretation: the two flash↔RAM equalities are a runtime handshake — the
application (or the updater) copies the two flash words into the RAM slots
before resetting, so only a cooperating firmware can trigger DFU. The flag
byte is simply erased flash (`0xFF`), so it does not gate a normal unit.

## 3. Handover into the vendor DFU: NOT solved (parked)

Four attempts failed on hardware (2026-09-27):

1. RAM handshake + driving `GPIO_B1_08` high + `NVIC_SystemReset()`: the
   bootloader boots the application anyway.
2. Jump to `0x6000333C`: this is the boot-the-app trampoline, not DFU.
3. Write the flag byte at `0x60086000` over FlexSPI IP commands: the write
   failed (no write-enable sequence; and the decision block wants `0xFF`
   there anyway).
4. Call the updater entry `0x600091BC` directly: the pedal hangs (it needs
   the bootloader's runtime state) and drops off USB.

These commands are removed from the console.

## 4. Two-stage boot: resident recovery + app slot (verified 2026-09-27)

A+D is needed only if the recovery image itself breaks. Every other update,
and every app failure, stays on USB.

| Flash | Block 0 offset | Contents |
|-------|----------------|----------|
| `0x60010000` | `0x00000` | recovery vectors |
| `0x60010400` | `0x00400` | vendor stub + loader (verbatim); load table at `0x784` |
| `0x600107D4` | `0x007D4` | recovery blob (loader entry 0 -> ITCM `0x400`, entry `0x4D6`) |
| `0x6001F000` | `0x0F000` | copier (staged by recovery to ITCM `0x1F000`) |
| `0x60020000` | `0x10000` | app slot: header `FBAP`/len/CRC32 (0x100), vectors (0x400), blob |
| `0x60041000` | - | app data blob: cold code (`.xiptext`, run in place from flash) and const tables (CMSIS), copied to OCRAM and DTCM at boot (slot header v2; a slot without them goes back to recovery; [hot and cold code](FIRMWARE_BRINGUP.md#hot-and-cold-code-audio-app), [memory map](FIRMWARE_BRINGUP.md#memory-map-audio-app)) |
| `0x60061000` | - | stock sound data `FBSD` (`src/dsp/stock_data.h`), written once by `fwstock` |

- The published images contain no vendor bytes. The recovery image leaves
  `0x400..0x7D4` erased; the first-install `.mr` gets the vendor stub and
  loader from the user's stock `.mr` (`src/fb200/images.py`). The stock
  sound data (amp models, cab IRs, tone stack, drum rhythms) is built from
  the same `.mr` (`src/fb200/stockdata.py`) and has its own flash area, so
  app updates never need the `.mr` again. Without it the app runs, but the
  amp, cab and tone pass audio through and the drums are silent.

- Load-table entries 1-3 decompressed stock data from `0x6002E39C..` (now
  the app slot) into DTCM/OCRAM; the packer turns them into copies of entry
  4 (the `.bss` memset). Our firmware uses none of that data.
- Recovery (`make VARIANT=recovery`) checks the slot and launches the app
  through the copier, unless it has to stay: slot invalid, the app faulted,
  the app hung, or the app ran `recovery`. It then keeps the USB console.
- Reasons live in `SRC_GPR3..6` (survive a warm reset, cleared at power-on):
  fault = `0xFA0000xx` + CFSR/HFSR/PC, request = `0x5EC0FEED`, and the app's
  alive marker `0xA11FE000`. A leftover alive marker means the app died
  without a clean reset (WDOG1, 8 s, fed by the app main loop).
- Console: `crumbs`, `recovery`, `boot` (recovery), `crash`/`hang` (app
  tests), `fwinfo`, `fwtest`, `fwbegin` (app slot), `fwrec` (recovery),
  `fwstock` (stock data), `stock` (app: stock data status), `stack` (the
  high-water of the 8 kB stack reserve, painted at boot).
- Flash writes use plain SPI-NOR commands in FlexSPI LUT slots 12-15 as IP
  commands; the boot configuration is left alone, so memory-mapped reads keep
  working.
- Host: `firmware/tools/pack_images.py` -> `fb200-recovery.bin`,
  `fb200-app.slot`, `fb200-twostage.mr` (DFU); `usb_update.py
  app|recovery|block0`; `boot_dry_run.py --app-elf` emulates vendor loader ->
  recovery -> copier -> app and fails on any peripheral access before
  `board_init` outside CCM/SRC/IOMUXC_GPR/WDOG.

Verified on the pedal: app and recovery rewritten over USB; `reset` -> app;
`hang` -> recovery (SRSR wdog bit); `crash` -> recovery with the faulting
PC; `recovery` -> recovery; `boot` -> app.

### Dead ends (do not retry)

- **ROM FlexSPI driver** (`init`/`erase`/`program`): after its `init`, any
  AHB read of flash, speculative ones included, hangs the core.
- **ROM serial downloader**: this chip's ROM API tree (`*0x0020001C` =
  `0x002012E8`) has the RT1050 layout; `runBootloader` (entry 0,
  `0x00201287`) is a 10-instruction `SYSRESETREQ` stub that ignores its
  argument. The RT1062 SDK header put it at entry 2 (the copyright string).
- **SNVS LPGPR** ignores writes on this board; SRC_GPR is used instead.
- **Touching USB before `board_init`**: a read of a clock-gated peripheral
  stalls the bus. The first two-stage image hung this way before arming the
  watchdog (A+D needed).
- **WDOG1 WRSR** as the hang signal: its timeout flag survives later
  software resets and kept recovery from launching a good app.

## 5. Reliability

- **Watchdog** (WDOG1, 8 s): armed by recovery just before it starts the app;
  the app feeds it once per main-loop pass and in every long wait: the flash
  busy-wait (`flash_wait_idle` runs `flash_pump`), each erased sector of an
  update, each preset of a factory reset, the update session. Every other
  loop is bounded (I2C: `I2C_RETRY_TIMES`; the CDC log drops rather than
  waits). So an app hang ends in recovery ("hang/watchdog"). Recovery itself
  runs without the watchdog: a hang there (only I2C `scan`/`dump` or a
  FlexSPI command could) needs a power cycle, which boots recovery again.
- **App update session**: after an app update erased the old app's cold code,
  only `reset` leaves the session. If the host is gone (USB unplugged for 5 s,
  or no console input for 2 min after the stream), the pedal resets by itself:
  the new app starts, or recovery if the slot is invalid.
- **Stack**: 8 kB below `_estack`. `firmware/tools/stack_usage.py` bounds the
  worst case from gcc's `-fstack-usage` frames and the call graph (main loop +
  two nested interrupts, each with a 108-byte exception frame);
  `tests/test_audio_image.py` fails below 512 B of headroom. v0.9.1: bound
  6.9 kB (app; the deepest chains are the cab's user-IR gain FFT, 4 kB of
  locals, and a factory reset from the app that re-enters `tud_task`),
  1.4 kB (recovery). `stack` shows the measured high-water.
- **Untrusted input** (app protocol over HID/BLE, console lines, presets and
  settings from flash, the stock data blob) is fuzzed on the host under
  ASan/UBSan: `tests/test_fuzz_host.py` (smoke in the default suite;
  `pytest -m fuzz` for long runs, and a gcc 14 build in docker).
- **Soak** on a pedal: `tools/soak.py --minutes 30` cycles presets, parameter
  writes, console commands, the IR list and audio captures, and fails on new
  skipped blocks, SAI over/underruns, a new crumb, a missing reply or a low
  stack (CSV + log).

## 6. Practical notes

- After flashing, the updater's exit jumps straight into the image; no power
  cycle is needed.
- A+D at power-on still works as the last-resort path (same decision block);
  flash `fb200-twostage.mr` with `fb200 fw flash --yes --no-jump`.
- The bootloader region is read-only from the application's point of view;
  the flag byte at `0x60086000` is in flash and was already `0xFF`.
- `src` (console) reads `SRC_SRSR`/`SBMR1`/`SBMR2` for reset-cause debugging.
