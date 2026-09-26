# FB200 Board Photos

Teardown photos of the FLAMMA FB200 main board, kept here as the hardware
reference for the custom firmware work. All photos were taken on 2026-09-26
and downscaled/re-encoded for repository size (the originals are larger).

| File | What it shows |
|------|---------------|
| `PXL_20260926_191849588.jpg` … `PXL_20260926_191923581.jpg` | Board overview shots (PCB, connectors, shielding, both areas of the main board) |
| `p3_soc.jpg` | Main SoC close-up: **MIMXRT1062DVL6A** (NXP i.MX RT1062, Cortex-M7) |
| `p3_top_center.jpg` | SWD debug header (TCK/TMS/GND/VCC) and the audio codec area: chip marked **NAU88BL21** (Nuvoton NAU88-series codec; exact part number to be confirmed), next to the 24 MHz crystal |
| `p2_flash.jpg` | Second 4-wire programming header (RST/CLK/D1/MOSI) and an 8-pin SOIC (candidate QSPI NOR flash) |
| `p3_crystal_zone.jpg`, `p3_xtal.jpg` | Crystal / clock area close-ups |

## What the photos established

- **SoC:** `MIMXRT1062DVL6A` — 600 MHz Cortex-M7 with FPv5 double-precision
  FPU, 1 MB total on-chip RAM (FlexRAM + OCRAM), external QSPI NOR flash at
  `0x60000000` (memory-mapped). See [`../HARDWARE.md`](../HARDWARE.md) §2–§3.
- **Codec:** Nuvoton NAU88-series part (marking `NAU88BL21`) on an LPI2C bus
  with SAI I²S audio. Exact part number, I²C address and register map are
  being confirmed by scanning the bus from our own firmware and by
  reverse-engineering the stock firmware's init sequence.
- **Debug access:** an SWD header (TCK/TMS/GND/VCC) is present; no probe was
  available during bring-up, so all firmware work is done over USB.

## Notes

- The photos are of a retail unit; board revisions may differ.
- Component identification from photos is best-effort: confirm against the
  silicon (I²C scans, register reads) before relying on it.
