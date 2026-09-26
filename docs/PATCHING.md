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

The visible proof patch is the Bluetooth friendly name:

| In the image | Patched to |
|--------------|------------|
| `FB200 Audio` | `FB200 Tools` |

Both strings are exactly **11 bytes** of printable ASCII.
[`FIRMWARE_ANALYSIS.md`](FIRMWARE_ANALYSIS.md) §1.3 documents `FB200 Audio` as
the Bluetooth friendly name embedded in block 0 (used with the
`AT+BD%-15.15s` name-setting template), but does not state an occurrence count.
Do not assume one: the count is whatever `fw inspect --strings` / `fw
patch-string` reports for your image.

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

Before flashing, confirm with `fb200 fw inspect patched.mr` that the product tag
is `FB200` and the block count and write-frame total match the stock image; the
`fw flash` dry run prints the plan (blocks, data bytes, write frames) but not the
product tag.

## 4. Patch table

| Block | Offset | Before | After | Length | Why safe |
|-------|--------|--------|-------|--------|----------|
| 0 (application payload) | printed by `fw inspect --strings`; file offset = payload offset + 640 | `FB200 Audio` | `FB200 Tools` | 11 bytes | Same length; printable ASCII to printable ASCII; inside an opaque string payload, not a code pointer, header field or tag field |

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
