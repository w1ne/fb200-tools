# FB200 Hardware Notes

Hardware-level observations for the FLAMMA FB200 (Mooer-based) bass
multi-effects pedal: its USB topology, the evidence for its MCU family, memory
map hypotheses, and local verification commands.

All USB identities and the vector-table values were verified against a real
FB200 and its stock `V1.0.1` firmware. Memory-map entries are hypotheses and
are labelled as such.

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

## 2. Evidence for an i.MX RT-class MCU

Static analysis of block 0 (the application image) gives three independent
pointers to an NXP i.MX RT-class Cortex-M part:

1. **Cortex-M vector table.** Block 0 begins with the standard two-word table:
   initial stack pointer `0x20058000` and reset vector `0x600104d9` (thumb bit
   set). The reset handler therefore executes from a `0x60000000`-based
   address, i.e. externally mapped flash, not from on-chip flash or RAM.
2. **FlexSPI mapping at `0x60000000`.** On i.MX RT10xx parts, `0x60000000` is
   the memory-mapped alias of the FlexSPI controller where external serial NOR
   flash lives. Code fetched there runs execute-in-place. The stock image's
   page math agrees: `START_PAGE 0x40` × 512 = `0x8000`, matching the
   application base `0x60008000` that follows a 32 KiB
   (`0x60000000`–`0x60007FFF`) bootloader region.
3. **SRAM size hint.** The initial stack pointer `0x20058000` shows at least
   352 KiB of SRAM below `0x20058000`, which is compatible with mid/high-end
   RT10xx variants.

The exact part number (e.g. RT1010 vs RT1015 vs RT1020) has not been
established: all of them share the `0x60000000` FlexSPI alias, so the
firmware alone does not distinguish them.

## 3. Memory-map hypotheses

Everything in this table is a **hypothesis** derived from the container values
and vector table; only the vector-table words and the block sizes/pages are
observed facts.

| Address range | Size | Hypothesis |
|---------------|------|------------|
| `0x20000000`–`0x20057FFF` | ≥ 352 KiB | on-chip SRAM; stack starts at `0x20058000` |
| `0x60000000`–`0x60007FFF` | 32 KiB | bootloader (pages 0–63; never part of block 0) |
| `0x60008000`–`0x60038FFF` | 200,704 B | application image (`START_PAGE 0x40`, pages 64–455) |
| elsewhere | 3,286,016 B | model library (block 1, `START_PAGE 0x00`) |

Block 1 numbering restarts at page 0 even though page 0 is inside the block 0
bootloader area, and the erase frame selects a target with `ROM_ID`. The
likely explanation is that block 1 is erased/written on a **different
`ROM_ID` target** (a second flash region or device) whose page numbering
starts at zero. The actual `ROM_ID` values and address mapping have not been
verified.

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

## 5. Open questions

- Exact MCU part number and flash part (i.MX RT10xx variant).
- Whether `ROM_ID` selects a second physical flash device or a region of the
  same one.
- Whether the bootloader performs any image integrity check (no signature or
  checksum is present in the container or update protocol).

## References

- USB protocol and identities: [`PROTOCOL.md`](PROTOCOL.md)
- Container format: [`FIRMWARE_FORMAT.md`](FIRMWARE_FORMAT.md)
- Firmware analysis: [`FIRMWARE_ANALYSIS.md`](FIRMWARE_ANALYSIS.md)
- Design notes: `docs/superpowers/specs/2026-09-25-fb200-tools-design.md` §2.1, §2.6
