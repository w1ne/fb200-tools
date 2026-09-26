#!/usr/bin/env python3
"""Generate a vendor-free FB200 .mr template with stock-shaped fields.

Used by CI and for local pack tests. The payload is all zeros; this file must
never be flashed to a pedal.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
if not (REPO_ROOT / "src" / "fb200").is_dir():
    raise SystemExit("cannot locate fb200 package next to this script")
sys.path.insert(0, str(REPO_ROOT / "src"))

from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader

APP_SIZE = 200_704
MODELS_SIZE = 3_286_016


def build() -> MrFile:
    header = MrHeader(
        product_tag="FB200",
        send_cmd=0x02,
        rec_cmd=0x03,
        timeout=10000,
        update_block=2,
        update_addr=b"\x03\x00\x00\x00",
        version=0,
    )
    blocks = [
        MrBlock(
            MrBlockTag(
                start_addr=641,
                stop_addr=641 + APP_SIZE - 1,
                send_cmd=0x04,
                rec_cmd=0x05,
                timeout=50000,
                start_page=0x40,
                rom_id=0,
            ),
            bytes(APP_SIZE),
        ),
        MrBlock(
            MrBlockTag(
                start_addr=641 + APP_SIZE + 512,
                stop_addr=641 + APP_SIZE + 512 + MODELS_SIZE - 1,
                send_cmd=0x06,
                rec_cmd=0x07,
                timeout=50000,
                start_page=0x00,
                rom_id=0,
            ),
            bytes(MODELS_SIZE),
        ),
    ]
    return MrFile(header, blocks)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", required=True)
    args = parser.parse_args(argv)
    mr = build()
    Path(args.output).write_bytes(mr.to_bytes())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
