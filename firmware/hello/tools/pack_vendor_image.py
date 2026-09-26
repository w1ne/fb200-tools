#!/usr/bin/env python3
"""Assemble a flashable FB200 .mr from a stock template and the hello build.

The stock block 0 is a self-loading image (see docs/FIRMWARE_BRINGUP.md): the
boot region at offsets 0x400..0x7d4 contains the vendor stub and loader, the
table at 0x784 tells the loader what to copy where, and the loader ends by
jumping to the fixed address ITCM 0x4d6 (blob offset 0xd6). This script keeps
that region, the table and the stock model block byte-identical, and replaces:

  * block 0 offsets 0x000..0x400 with our vector table (vectors.bin)
  * block 0 offsets 0x7d4..0x1e39c with our ITCM payload (blob.bin, padded
    with 0xff)

The table still points entry 0 at flash 0x600107d4 -> ITCM 0x400, so the
loader copies our blob exactly where the linker expects it.

Usage:
  pack_vendor_image.py STOCK.mr VECTORS.bin BLOB.bin -o hello.mr
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import replace
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
if not (REPO_ROOT / "src" / "fb200").is_dir():
    raise SystemExit("cannot locate fb200 package next to this script")
sys.path.insert(0, str(REPO_ROOT / "src"))

from fb200.firmware import MrBlock, MrFile

VECTORS_OFF = 0x000
VECTORS_SIZE = 0x400
BOOT_OFF = 0x400          # vendor boot region, kept verbatim
BLOB_OFF = 0x7D4          # entry 0 source: 0x600107d4 -> ITCM 0x400
BLOB_LIMIT = 0x1E39C - BLOB_OFF   # 0x1dbc8, entry 0 length


def pack(template: MrFile, vectors: bytes, blob: bytes, app_only: bool = False) -> MrFile:
    if template.header.product_tag != "FB200":
        raise SystemExit(f"template is for {template.header.product_tag!r}")
    if len(template.blocks) < 2:
        raise SystemExit("template needs both the app and model blocks")
    if len(vectors) != VECTORS_SIZE:
        raise SystemExit(f"vectors must be {VECTORS_SIZE} bytes, got {len(vectors)}")
    if len(blob) > BLOB_LIMIT:
        raise SystemExit(f"blob exceeds the loader's payload region ({len(blob)} > {BLOB_LIMIT})")

    block0 = bytearray(template.blocks[0].data)
    if len(block0) != 0x31000:
        raise SystemExit(f"unexpected template block 0 size {len(block0):#x}")
    # Sanity-check the vendor boot region before trusting the template.
    if block0[0x4D8:0x4DA] != b"\x72\xb6":
        raise SystemExit("template does not look like a stock FB200 image (stub missing)")
    if bytes(block0[0x434:0x43C]) != bytes.fromhex("50030000a0030000"):
        raise SystemExit("template loader table header mismatch")
    if bytes(block0[0x784:0x794]) != bytes.fromhex("d407016000040000c8db0100a0040160"):
        raise SystemExit("template loader table entry 0 mismatch")

    block0[VECTORS_OFF:VECTORS_OFF + VECTORS_SIZE] = vectors
    block0[BLOB_OFF:BLOB_OFF + BLOB_LIMIT] = blob.ljust(BLOB_LIMIT, b"\xff")

    blocks = [MrBlock(template.blocks[0].tag, bytes(block0))]
    header = template.header
    if app_only:
        header = replace(header, update_block=1)
    else:
        blocks.append(MrBlock(template.blocks[1].tag, template.blocks[1].data))
    return MrFile(header, blocks)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("template", type=Path, help="stock .mr image to use as the base")
    parser.add_argument("vectors", type=Path, help="build/fb200-hello.vectors.bin")
    parser.add_argument("blob", type=Path, help="build/fb200-hello.blob.bin")
    parser.add_argument("-o", "--output", type=Path, required=True, help="output .mr")
    parser.add_argument(
        "--app-only",
        action="store_true",
        help="drop the model block (write the application pages only)",
    )
    args = parser.parse_args()

    template = MrFile.from_path(args.template)
    vectors = args.vectors.read_bytes()
    blob = args.blob.read_bytes()
    out = pack(template, vectors, blob, app_only=args.app_only)
    args.output.write_bytes(out.to_bytes())
    print(
        f"wrote {args.output}: vectors {len(vectors)} B at block0 {VECTORS_OFF:#x}, "
        f"blob {len(blob)} B at block0 {BLOB_OFF:#x} (padded to {BLOB_LIMIT} B), "
        f"{'app-only' if args.app_only else 'app + model blocks'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
