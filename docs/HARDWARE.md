# FB200 Hardware Notes

Hardware-level observations for the FLAMMA FB200 (Mooer-based) bass
multi-effects pedal: its USB topology, the evidence for its MCU family, memory
map, startup scheme, custom-firmware feasibility, and local verification
commands.

All USB identities, vector-table values and peripheral addresses below were
verified against a real FB200 and its stock `V1.0.1` firmware / stock `.mr`
image. Items that remain unproven are explicitly marked.

## 1. USB topology

| Mode | VID | PID | Composition |
|------|-----|-----|-------------|
| Application | `0x34DB` | `0x800F` | USB Audio (class-compliant, 2 in / 2 out @ 44.1 kHz) + vendor HID interface 3 |
| Update / bootloader | `0x0483` | `0x5703` | Mooer update identity, entered after the `0xC1` command |

- **Audio function.** Class-compliant USB Audio, 2 input channels and
  2 output channels at 44.1 kHz. No vendor driver is needed on macOS or Linux;
  it is usable as a standard audio device.
- **Control function.** Interface **3** is a vendor-specific HID interface
  carrying the 64-byte report protocol documented in
  [`PROTOCOL.md`](PROTOCOL.md): device info, IR import/list/delete and the
  `0xC1` jump to the bootloader.
- **Update mode.** Sending `fn=0xC1` from application mode makes the device
  re-enumerate as `0483:5703`. That VID is the STMicroelectronics USB vendor
  ID and is Mooer's common update identity; by itself it is not evidence of
  the application MCU vendor (see §2). The bootloader speaks the same framing
  with the erase/write/exit commands from
  [`FIRMWARE_FORMAT.md`](FIRMWARE_FORMAT.md) §7.

## 2. MCU: NXP i.MX RT10xx (Cortex-M7)

Static analysis of block 0 (the application image) identifies the main SoC as
an **NXP i.MX RT10xx-class Cortex-M7**:

1. **Cortex-M boot contract.** Block 0 begins with initial stack pointer
   `0x20058000` and reset vector `0x600104d9` (thumb bit set). The reset
   handler sets `SCB->VTOR` (`0xE000ED08`) to `0x60010000`, loads MSP from
   `[0x60010000]`, and executes from external mapped flash.
2. **i.MX RT register fingerprint in the reset path.** The reset handler writes
   `IOMUXC_GPR` registers `0x400AC038` / `0x400AC040` / `0x400AC044` (TCM /
   FlexRAM bank configuration; values `0x00200007` and `0xFFAAAAA9`) before
   touching RAM. This is the standard i.MX RT FlexRAM setup sequence and is
   specific to this family. Other i.MX RT bases found in the image:
   `CCM 0x400FC000` (x8), `ANADIG/PMU 0x400D8000` (x21),
   `IOMUXC_GPR 0x400AC000`, `FlexSPI1 0x402A8000` (x11).
3. **FlexSPI mapping at `0x60000000`.** On i.MX RT10xx parts, `0x60000000` is
   the memory-mapped alias of the FlexSPI controller where external serial NOR
   flash lives. The image is linked at `0x60010000` (the reset handler's VTOR
   literal is exactly `0x60010000`).
4. **Cortex-M7 FPU.** The application code contains double-precision FPU
   instructions (`vcvt.f64.f32`, `vmla.f64`, `vmls.f64`), which requires the
   FPv5 double-precision unit found on Cortex-M7 (not the single-precision-only
   FPU of Cortex-M4). The effects math therefore runs on the M7 itself; there
   is no separate DSP for it.
5. **Peripheral map** (all i.MX RT10xx bases): `SAI1 0x40384000`,
   `SAI2 0x4038C000` (I2S audio), `LPI2C1..4 0x403F0000..0x403FC000` (codec /
   control I2C), `eDMA0/1 0x400E8000/0x400EC000`, `GPIO1..4`
   `0x401B8000..0x401C4000`, `USB1 0x402E0000` (+ PHY `0x402E0200`).
6. **Software audio path.** USB audio + HID (`USB1`) and the SAI/I2S codec path
   are driven by this one M7; the Bluetooth module is a separate device
   controlled over UART with the AT commands catalogued in
   [`FIRMWARE_ANALYSIS.md`](FIRMWARE_ANALYSIS.md) §1.3.

The exact part number within the RT10xx family (e.g. RT1052 vs RT1062) and the
external flash chip are not established from the image alone; the ≥352 KiB SRAM
implied by the initial stack pointer narrows it to a mid/high-end variant.

## 3. Memory map

| Address range | Size | Status |
|---------------|------|--------|
| `0x00000000`–... | ≤ 200,704 B | ITCM: runtime location of the application code (see §3.1) |
| `0x20000000`–`0x20057FFF` | ≥ 352 KiB | DTCM/OCRAM; initial stack top `0x20058000` |
| `0x60000000`–`0x60007FFF` | 32 KiB | bootloader (pages 0–63 of the container; not part of block 0) |
| `0x60008000`–`0x6000FFFF` | 32 KiB | head of the update page space (container pages 0–63); purpose unverified |
| `0x60010000`–`0x60040FFF` | 200,704 B | application image (container `START_PAGE 0x40`, pages 64–455) |
| elsewhere | 3,286,016 B | model library (block 1, 95.7 % float32 data, `START_PAGE 0x00`) |

The container's `START_PAGE` counts 512-byte pages in the update address space,
whose base is `0x60008000` (immediately after the bootloader). Hence
`START_PAGE 0x40` × 512 = `0x8000` offset → application base `0x60010000`,
matching the reset handler's `VTOR` literal exactly.

Block 1 numbering restarts at page 0 even though page 0 is inside the block 0
page space, and the erase frame selects a target with `ROM_ID`. The likely
explanation is that block 1 is erased/written on a **different `ROM_ID`
target** (a second flash region or device) whose page numbering starts at zero.
The actual `ROM_ID` values and address mapping have not been verified.

### 3.1 Startup and relocation

The image contains an unusual vector table that is explained by a
copy-to-ITCM boot scheme:

- Entry `[0]` is the stack pointer and entry `[1]` is an absolute flash reset
  address (`0x600104d9`) — the only two entries the bootloader needs.
- The remaining entries are **0-based addresses** (NMI `0x1e91`,
  HardFault `0x9ab5`, SysTick `0x10b31`, IRQ stubs `0x1ea3`...`0x1f5f`),
  i.e. the addresses the handlers take once the image is copied to ITCM at
  `0x00000000`.
- A routine at image offset `0x1a390` copies a 256-entry vector table to
  address `0x00000000` and then writes `0` to `SCB->VTOR`
  (`0xE000ED08`), confirming the runtime vector table lives in ITCM. Early
  startup (called from the flash reset stub) also performs the FlexRAM/TCM
  configuration needed before ITCM can be used.

### 3.2 Custom-firmware feasibility

**Verdict: realistic.** The flash contract is simple and already proven:

- The bootloader accepts arbitrary erase/write pages with **no signature or
  checksum**, and a byte-patched image still booted on hardware (see
  [`UPDATE_AND_RECOVERY.md`](UPDATE_AND_RECOVERY.md) §7), so modified images
  are not rejected.
- The only observed boot contract is `[SP][reset]` at the start of the
  application region; after reset the SoC is a plain i.MX RT10xx running the
  image's own startup code.
- The SoC family has mature open tooling (MCUXpresso SDK, Zephyr, GCC), a
  documented peripheral map (SAI, LPI2C, USB, eDMA, GPIO), and a proven
  recovery path (`fw flash stock.mr --yes --no-jump`).

Remaining unknowns before writing custom firmware are listed in §5.

## 4. Verify locally (macOS)

With the pedal connected in application mode and powered on:

```bash
# 1. USB device and interface tree (application identity)
ioreg -p IOUSB -l -w 0 | grep -i -E "FB200|34db|800f"

# 2. Audio endpoints, channel counts and sample rate
system_profiler SPAudioDataType

# 3. HID devices (vendor control interface)
hidutil list
```

Expected observations:

- `ioreg`: a USB device whose `idVendor`/`idProduct` are `13531`/`32783`
  (`0x34DB`/`0x800F`), with several interfaces, one of them the vendor HID
  interface (interface 3). In update mode the same command shows
  `1155`/`22275` (`0x0483`/`0x5703`) instead.
- `system_profiler SPAudioDataType`: the FB200 appears as a USB audio device
  with **2 input channels**, **2 output channels**, transport `USB`, and a
  default/current sample rate of **44100 Hz**.
- `hidutil list`: a HID device row for vendor `0x34db`, product `0x800f`
  (the vendor control interface). If the interface exposes no HID elements,
  `hidutil` may omit it; the `ioreg` output above still shows the interface.

Project-native equivalents that exercise the same hardware:

```bash
fb200 info        # product and versions over the HID control interface
fb200 probe --listen 2   # print raw HID reports for 2 seconds
```

`fb200 info` should return product `FB200`, firmware `V1.0.1`, application
`V1.0.0`, Bluetooth `V1.0.0`, hardware revision `A` (see
[`PROTOCOL.md`](PROTOCOL.md) §5.1).

## 5. Board photos

Teardown photos of the main board are in [`pcb/`](pcb/README.md). They
establish the SoC part number (`MIMXRT1062DVL6A`, §2) and show the audio codec
area: a Nuvoton NAU88-series part marked `NAU88BL21` on the LPI2C bus next to
the 24 MHz crystal. The SWD header (TCK/TMS/GND/VCC) and a second 4-wire
header (RST/CLK/D1/MOSI) are visible as well.

## 6. Open questions

- Exact MCU part number within the i.MX RT10xx family and the external flash
  chip (size, vendor, QSPI/OSPI mode).
- Audio codec chip on the LPI2C/SAI bus, and the display/button wiring.
- Whether `ROM_ID` selects a second physical flash device or a region of the
  same one, and where block 1 (models) is actually stored.
- Whether the bootloader performs any image integrity check (no signature or
  checksum is present in the container or update protocol, and a patched image
  booted — but an explicit check is not ruled out).
- The exact ITCM copy strategy of the stock startup (which sections are copied,
  which stay in flash XIP) — only the vector-table copy is confirmed.

## References

- USB protocol and identities: [`PROTOCOL.md`](PROTOCOL.md)
- Container format: [`FIRMWARE_FORMAT.md`](FIRMWARE_FORMAT.md)
- Firmware analysis: [`FIRMWARE_ANALYSIS.md`](FIRMWARE_ANALYSIS.md)
- Design notes: `docs/superpowers/specs/2026-09-25-fb200-tools-design.md` §2.1, §2.6
