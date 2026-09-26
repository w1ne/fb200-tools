# FB200 Update and Recovery

How firmware updates work on the FLAMMA FB200, what `fb200-tools` implements,
and how to recover a pedal whose update was interrupted. The official flow was
replayed from the official Electron editor; `fb200 fw flash` implements the
same frame sequence. Wire details are in [`PROTOCOL.md`](PROTOCOL.md) §8 and
[`FIRMWARE_FORMAT.md`](FIRMWARE_FORMAT.md) §7.

Flashing is inherently risky. Read [`DISCLAIMER.md`](../DISCLAIMER.md) first.

## 1. The two USB modes

| Mode | VID | PID | Composition |
|------|-----|-----|-------------|
| Application | `0x34DB` | `0x800F` | USB Audio (class-compliant, 2 in / 2 out @ 44.1 kHz) + vendor HID interface 3 |
| Update / bootloader | `0x0483` | `0x5703` | Mooer update identity; speaks only the erase/write/exit commands |

The application mode answers `fb200 info` and all `ir` commands. The update mode
also answers `fn=0x00`, but with the bootloader's own identity strings (observed
`FB200` and `V1.0.0` on hardware) rather than the installed application version;
use it only to detect that the device is present. See [`HARDWARE.md`](HARDWARE.md)
§1 for the USB topology evidence.

## 2. Official update flow

Reverse-engineered from the official Electron updater; the sequence below was
replayed byte-for-byte against a capture of the official updater's frame stream.
No write has yet been performed on hardware from this project (that is the
Task 20 validation step).

1. **Jump.** From application mode the host sends `fn=0xC1` (no payload). The
   device re-enumerates as `0483:5703`.
2. **Erase.** The host sends one erase frame, `fn=0x02` (`header.SEND_CMD`). For
   the stock image (`header.VERSION == 0`) the payload is the 4-byte
   `UPDATE_ADDR` (`03 00 00 00`); a nonzero `VERSION` would send a per-block
   `[ROM_ID u8][START_PAGE u32 LE][BLOCK_SIZE u32 LE]` list. The host waits for
   reply `0x03`.
3. **Write.** One frame per 512-byte payload chunk, in page order:
   `fn=0x04` for the application block and `fn=0x06` for the model block, with
   payload `[page u16 BE][512-byte chunk]`. The page field is **big-endian**
   (the low 16 bits of the block's `START_PAGE + chunk index`). Each frame is
   acknowledged with `SEND_CMD + 1` (`0x05` / `0x07`) before the next is sent.
4. **Exit.** After the last block the host sends `fn=0xFF`; the device reboots
   into the application and re-enumerates as `34DB:800F`.

The official updater transmits **no image signature or per-image checksum**;
image validation (product tag, sizes, page math) is entirely the client's
responsibility.

## 3. Our implementation: `fb200 fw flash`

```
fb200 fw flash image.mr                    # dry run: validate and print the plan
fb200 fw flash image.mr --yes              # actually erase, write and exit
fb200 fw flash stock.mr --yes --no-jump    # pedal already in update mode
```

Behaviour:

- **Dry run by default.** Without `--yes` the image is parsed, validated and the
  flash plan is printed; nothing is written.
- **Product check first.** Images whose `PRODUCT_TAG` is not `FB200` are refused
  before any USB traffic.
- **Jump mode (default).** Opens the application device, prints the target
  product and firmware version, sends `0xC1`, then waits for the bootloader
  device to appear.
- **`--no-jump`.** Skips the `0xC1` jump and expects the pedal to already be at
  `0483:5703`. Use this to resume a flash after an interruption, or for a pedal
  that is already in update mode.
- **Post-flash verification.** After `0xFF` the tool waits up to 20 s for the
  application identity to re-enumerate, then queries `fb200 info` (retrying for
  up to 5 s).

Exit codes:

| Code | Meaning |
|------|---------|
| `0` | success (dry run completed, or flash plus verification succeeded) |
| `1` | error (bad image, transport/communication failure, not in update mode) |
| `2` | usage error (argparse) |
| `3` | the write completed, but the device did not re-enumerate or did not answer the version query afterwards |

Code `3` means the bytes were sent but the outcome is unverified: treat the
pedal as still in the bootloader and continue with §5.

## 4. Risks

- **Interrupted transfer.** Unplugging the cable, host sleep, or loss of power
  mid-write leaves the pedal in the bootloader (`0483:5703`). The application
  area may then be blank or partially written and will not boot.
- **The bootloader is recoverable.** It occupies a separate region (hypothesised
  in [`HARDWARE.md`](HARDWARE.md) §3 as 32 KiB at `0x60000000`–`0x60007FFF`,
  pages 0–63) that is never part of the application block, so a failed
  application write does not erase it. The `0xC1` path cannot be used while the
  application does not boot, but the bootloader itself still accepts
  erase/write/exit frames.
- **No integrity fallback.** Neither the container nor the update protocol
  carries a signature or checksum, so a wrong-but-accepted image is not detected
  by anything the update protocol transmits. Keep a stock image (below) and
  verify patches before flashing.

## 5. Recovery

### 5.1 Keep a stock image locally

Extract `FB200.mr` from the official firmware package into your working
directory (for example `fb200-stock.mr`). **Never commit or redistribute it**:
this repository contains no vendor firmware. It is your revert target and the
only guaranteed-good image you have.

### 5.2 Interrupted flash — pedal stuck in the bootloader

The pedal stays at `0483:5703`. It is already in update mode, so skip the jump
and re-flash the stock image:

```bash
fb200 fw flash fb200-stock.mr --yes --no-jump
```

This re-runs erase, write and exit without sending `0xC1`. The operation is
idempotent: it is safe to retry if it is interrupted again.

### 5.3 Application does not boot

If the pedal enumerates as `0483:5703` when connected, use §5.2 — no footswitch
combo is needed. If it does **not** enumerate at all (`34DB:800F` or
`0483:5703`), the bootloader may be waiting on a hardware condition. Try each
power-on footswitch combination while plugging in USB and check for `0483:5703`
enumeration using the commands in §5.4.

| Power-on footswitch combo | Expected if it forces update mode | Result |
|---------------------------|-----------------------------------|--------|
| A + B | `0483:5703` enumerates | TBD — hardware validation pending (Task 20) |
| B + C | `0483:5703` enumerates | TBD — hardware validation pending (Task 20) |
| C + D | `0483:5703` enumerates | TBD — hardware validation pending (Task 20) |
| A + D | `0483:5703` enumerates | TBD — hardware validation pending (Task 20) |
| all four (A + B + C + D) | `0483:5703` enumerates | TBD — hardware validation pending (Task 20) |

The combos are non-destructive: power-cycle the pedal to leave any of these
attempts. Results will be recorded here once verified on hardware.

### 5.4 Host-side enumeration checks

While the pedal is connected, confirm which identity is present:

```bash
# macOS (ioreg prints VID/PID in decimal; 1155 = 0x0483, 13531 = 0x34DB)
ioreg -p IOUSB -l -w 0 | grep -E 'idVendor" = (1155|13531)'
# hidutil list may omit the vendor interface; if present it shows 0x483 / 0x34db
hidutil list | grep -iE '0x483|0x34db'

# Linux
lsusb | grep 0483:5703

# Windows: Device Manager, look for USB\VID_0483&PID_5703
```

If `0483:5703` appears, the pedal is in the bootloader and §5.2 recovers it. If
`34DB:800F` appears, the application already booted and a normal
`fb200 fw flash stock.mr --yes` works.

## 6. Cable and power

- Use a **USB-A to USB-C** cable, per the vendor note. Avoid unpowered hubs,
  docks, and charge-only cables.
- Do not let the host sleep, hibernate or suspend during a flash, and do not
  unplug the pedal until the tool reports the device back online.
- A powered pedal with a stable USB connection is the safest configuration.

## 7. Hardware validation results (2026-09-26)

Performed on a FB200 running application `V1.0.0` / firmware `V1.0.1`, using
this repository's `fw flash` client over USB-A to USB-C.

| Check | Image SHA-256 | Result |
|-------|---------------|--------|
| Stock image round-trip | `dfe409dc…6947` | PASS — erase, 6,810 write frames, exit; device re-enumerated as `FB200 V1.0.1` and answered `fb200 info` |
| Proof patch (`FB200 Audio` → `FB200 Tools`, `V1.0.1` → `V9.9.9`) | `607c790e…a488` | PASS — same 6,810 frames; `fb200 info` then reported `Firmware version: V9.9.9` |
| Revert to stock | `dfe409dc…6947` | PASS — re-flashed the stock image; `fb200 info` reported `V1.0.1` again |
| Hardware smoke tests (`pytest -m hardware`) | — | PASS — 2 tests (versions readable, 9 IR slots) |
| Interrupted-flash recovery (`--no-jump`) | — | PASS — exercised during validation; with the fixed timeout, the retry completed the write without re-sending the jump |
| Bluetooth advertised-name change | — | INCONCLUSIVE — the module advertises a module-side name (BLE advert truncated to `FB200MY FB200`, no GATT name characteristic), so the host could not confirm the `FB200 Tools` change; the version string is the authoritative proof |

Anomalies observed:

- The first proof-patch attempt failed with `erase command not acknowledged (no
  reply)`: the erase acknowledgement measured **~11.5 s** on hardware, beyond the
  original 10 s read timeout. The device stayed safely in the bootloader, the
  `--no-jump` retry was attempted (also timed out at 10 s), and the flash
  completed once the client waited longer. Fixed in `97231ef` (erase now has its
  own 60 s timeout; page writes keep 10 s). The recovery procedure in §5.2
  behaved exactly as documented.
- The update-mode `fn=0x00` reply contains the bootloader's own strings
  (`FB200`, `V1.0.0`), not the application version (see §1).

## References

- Update frames and command set: [`PROTOCOL.md`](PROTOCOL.md) §8
- Container and page math: [`FIRMWARE_FORMAT.md`](FIRMWARE_FORMAT.md) §6–§7
- USB topology and memory-map hypotheses: [`HARDWARE.md`](HARDWARE.md)
- Risk and affiliation notice: [`DISCLAIMER.md`](../DISCLAIMER.md)
- Flash client: `src/fb200/updater.py`, `fb200 fw flash` in `src/fb200/cli.py`
