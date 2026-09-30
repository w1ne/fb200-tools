#!/usr/bin/env python3
# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Build the stock-firmware ELF for the LabWired twin (labwired/stock-boot.yaml).

Reads your own copy of the vendor image, takes block 0 (application) and
block 1 (model library) out of the .mr container and writes them into one
ELF at their flash addresses:

    block 0 -> 0x60010000   (START_PAGE 0x40 x 512 above 0x60008000)
    block 1 -> 0x600D0000   (flash offset 0xD0000, UI_AND_STORAGE.md)

    python3 tools/labwired_stock.py [PATH.mr] [-o build/labwired/stock.elf]

Without PATH it uses $FB200_STOCK_MR, then fb200-stock.mr in the repo root
(or in the main checkout, from a git worktree).
The output is vendor code: it goes to build/ (ignored). Never commit it.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tools"))

from labwired_elf import build_elf

from fb200.errors import FirmwareError
from fb200.firmware import MrFile

BLOCK_ADDRESSES = (0x6001_0000, 0x600D_0000)
DEFAULT_OUT = ROOT / "build" / "labwired" / "stock.elf"


def find_image(arg: str | None) -> Path:
    """PATH, else $FB200_STOCK_MR, else fb200-stock.mr in the repo root or, from
    a git worktree, in the main checkout (same order as tests/stock_emu_fx.py)."""
    cands = [arg, os.environ.get("FB200_STOCK_MR"), ROOT / "fb200-stock.mr"]
    try:
        common = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--git-common-dir"],
                                capture_output=True, text=True, check=True).stdout.strip()
        cands.append((ROOT / common).resolve().parent / "fb200-stock.mr")
    except (OSError, subprocess.CalledProcessError):
        pass
    for cand in cands:
        if cand and Path(cand).is_file():
            return Path(cand)
    raise SystemExit("fb200-stock.mr not found: pass its path or set FB200_STOCK_MR")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image", nargs="?", help="vendor .mr image (default: $FB200_STOCK_MR)")
    ap.add_argument("-o", "--output", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args(argv)

    path = find_image(args.image)
    try:
        mr = MrFile.from_path(path)
    except FirmwareError as exc:
        raise SystemExit(f"{path}: {exc}") from exc
    if len(mr.blocks) < 2:
        raise SystemExit(f"{path}: expected 2 blocks, found {len(mr.blocks)}")
    segments = [(addr, mr.blocks[i].data) for i, addr in enumerate(BLOCK_ADDRESSES)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_elf(segments))
    for i, (addr, data) in enumerate(segments):
        print(f"block {i}: {len(data)} bytes at 0x{addr:08x}")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
