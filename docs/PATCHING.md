# Safe `.mr` Patches

How to modify an FB200 `.mr` firmware image without changing its structure or
size. The container format is specified in [`FIRMWARE_FORMAT.md`](FIRMWARE_FORMAT.md);
`src/fb200/firmware.py` implements parsing, serialization and same-length
patching. Flashing a patched image is still flashing — read
[`DISCLAIMER.md`](../DISCLAIMER.md) and keep a stock image to revert.

## 1. Rules for safe patches

| Rule | Why |
|------|-----|
| **Same-length byte replacement only** | `patch_string` refuses replacements whose byte length differs; block sizes, page counts and the file layout stay identical. |
| **Never change block sizes, page counts, header fields, or the 512-byte tag layout** | The updater derives erase ranges and the 512-byte page plan from those fields; changing them can erase or write the wrong flash region. |
| **Prefer ASCII string swaps inside block payloads** | Replacing printable ASCII with the same number of printable ASCII bytes does not move code, shift offsets, or change any pointer. |
| **Keep a stock image to revert** | The only guaranteed-good image is the original `FB200.mr`; never modify it in place (the CLI writes to a new output file by default). |

`MrFile` preserves uninterpreted bytes verbatim (`MrHeader.raw`,
`MrBlockTag.raw`), so a same-length patch round-trips byte-for-byte apart from
the replaced bytes (see [`FIRMWARE_FORMAT.md`](FIRMWARE_FORMAT.md) §8).

## 2. The proof patch

The proof patch makes two same-length replacements in block 0:

| In the image | Patched to | Bytes changed | Hardware-verified by |
|--------------|------------|---------------|----------------------|
| `FB200 Audio` | `FB200 Tools` | 5 | Bluetooth name: **inconclusive** (see below) |
| `V1.0.1` | `V9.9.9` | 3 | `fb200 info` reporting `Firmware version: V9.9.9` |

All strings are printable ASCII of identical length, and `V1.0.1` occurs exactly
once in the stock FB200 v1.0.1 image, so the version replacement is deterministic
and readable over USB without any Bluetooth tooling. The pair was flashed and
observed on hardware on 2026-09-26: `fb200 info` reported `V9.9.9` after the
flash, and re-flashing the stock image restored `V1.0.1` (see
[`UPDATE_AND_RECOVERY.md`](UPDATE_AND_RECOVERY.md) §7).

**Bluetooth name caveat.** `FB200 Audio` is embedded in block 0 as the module
command `AT+BDFB200 Audio` (`AT+BD%-15.15s` is the formatting template). On the
tested unit the advertised name is composed module-side — the BLE advertisement
is truncated (`FB200MY FB200`) and the module exposes no GATT name
characteristic — so a host could not confirm the `FB200 Tools` change. Patch it
if you like, but verify a patched image with the version string instead.

## 3. CLI walkthrough

Inspect the stock image first:

```bash
fb200 fw inspect stock.mr
fb200 fw inspect stock.mr --strings --filter "FB200 Audio"
```

Make the patch (the CLI requires equal byte lengths and writes to `--output`, or
to `<file>.patched.mr` when `--output` is omitted):

```bash
fb200 fw patch-string stock.mr \
  --find "FB200 Audio" --replace "FB200 Tools" \
  --output patched.mr
fb200 fw patch-string patched.mr \
  --find "V1.0.1" --replace "V9.9.9" \
  --output patched.mr
```

The command prints the number of occurrences it replaced. Verify the result:

```bash
fb200 fw inspect patched.mr
fb200 fw inspect patched.mr --strings --filter "FB200 Tools"
cmp -l stock.mr patched.mr | head          # expect only the patched byte offsets
sha256sum stock.mr patched.mr              # hashes must differ
fb200 fw flash patched.mr                  # dry run: no --yes, nothing written
```

`cmp -l` lists every differing byte position (1-based, in decimal) with the
byte values in octal. Because the patch is a same-length ASCII replacement, the
only differences must fall inside the replaced 11-byte region; any difference
at another offset means the file was not otherwise preserved. On macOS without
coreutils, use `shasum -a 256` instead of `sha256sum`.

Optionally confirm the string lives in the application block:

```bash
fb200 fw extract-block stock.mr 0 app.bin
fb200 fw extract-block patched.mr 0 patched-app.bin
cmp -l app.bin patched-app.bin | head
```

After flashing the patched image, verify on hardware: `fb200 info` must report
`Firmware version: V9.9.9`. Re-flash the stock image to revert and confirm
`V1.0.1` returns.

Before flashing, confirm with `fb200 fw inspect patched.mr` that the product tag
is `FB200` and the block count and write-frame total match the stock image; the
`fw flash` dry run prints the plan (blocks, data bytes, write frames) but not the
product tag.

## 4. Patch table

| Block | Offset | Before | After | Length | Why safe |
|-------|--------|--------|-------|--------|----------|
| 0 (application payload) | printed by `fw inspect --strings`; file offset = payload offset + 640 | `FB200 Audio` | `FB200 Tools` | 11 bytes | Same length; printable ASCII to printable ASCII; inside an opaque string payload, not a code pointer, header field or tag field |
| 0 (application payload) | printed by `fw inspect --strings`; file offset = payload offset + 640 | `V1.0.1` | `V9.9.9` | 6 bytes | Same length; unique in the stock v1.0.1 image; makes the patch verifiable via `fb200 info` after flashing |

## 5. Revert

Two options:

1. **Flash the stock image back** — the intended revert path. Follow
   [`UPDATE_AND_RECOVERY.md`](UPDATE_AND_RECOVERY.md) and run:

   ```bash
   fb200 fw flash stock.mr --yes
   ```

2. **Reverse-patch** — only if you no longer have the stock image and the
   patched image still boots. Run the patch in the opposite direction:

   ```bash
   fb200 fw patch-string patched.mr \
     --find "FB200 Tools" --replace "FB200 Audio" \
     --output reverted.mr
   ```

Reverting does not repair a failed flash: if the pedal is stuck in the
bootloader, use the `--no-jump` recovery flow in
[`UPDATE_AND_RECOVERY.md`](UPDATE_AND_RECOVERY.md) §5.2.

## References

- Container layout, page math and round-trip guarantee: [`FIRMWARE_FORMAT.md`](FIRMWARE_FORMAT.md)
- Where the patch strings live: [`FIRMWARE_ANALYSIS.md`](FIRMWARE_ANALYSIS.md) §1.3
- Update and recovery flow: [`UPDATE_AND_RECOVERY.md`](UPDATE_AND_RECOVERY.md)
- Risk notice: [`DISCLAIMER.md`](../DISCLAIMER.md)
- Implementation: `src/fb200/firmware.py` (`find_strings`, `patch_string`)
