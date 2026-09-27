#!/usr/bin/env python3
"""Pack the two-stage FB200 images (docs/BOOTLOADER.md §4, src/fb200/images.py).

  pack_images.py build/fb200-recovery build/fb200-app -o OUT [--stock FB200.mr]

Always writes the published, vendor-free images:
  fb200-recovery.bin   block 0 [0x0, 0x10000), vendor loader range erased
  fb200-app.slot       block 0 [0x10000, ...) + const tables -> `fb200 update app`
  manifest.json        sizes and CRCs (the web updater reads it)

With --stock (the user's own stock .mr), also the personal images, which
contain vendor data (do not redistribute them):
  fb200-twostage.mr    first install through the vendor updater (A+D)
  fb200-recovery-full.bin  recovery with the vendor loader -> `fb200 update recovery`
  fb200-stock.blob     stock sound data -> `fb200 update stock FB200.mr` writes the same
"""

from __future__ import annotations

import argparse
import json
import sys
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "src"))

from fb200 import images, stockdata
from fb200.errors import FirmwareError


def crc(data: bytes) -> str:
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08x}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("recovery", type=Path, help="build dir prefix, e.g. build/fb200-recovery")
    ap.add_argument("app", type=Path, help="build dir prefix, e.g. build/fb200-app")
    ap.add_argument("-o", "--outdir", type=Path, required=True)
    ap.add_argument("--stock", type=Path, help="your stock .mr: also write the personal images")
    ap.add_argument("--version", default="dev", help="version string for manifest.json")
    args = ap.parse_args()

    def part(prefix: Path, kind: str) -> bytes:
        return Path(f"{prefix}.{kind}.bin").read_bytes()

    try:
        images.require_update_commands("recovery", part(args.recovery, "blob"))
        images.require_update_commands("app", part(args.app, "blob"))
        rec = images.build_recovery(part(args.recovery, "vectors"), part(args.recovery, "blob"),
                                    part(args.recovery, "copier"))
        data_path = Path(f"{args.app}.dtcmdata.bin")
        data = data_path.read_bytes() if data_path.exists() else b""
        slot = images.build_slot(part(args.app, "vectors"), part(args.app, "blob"), data)
        images.check_slot(slot)
        out = args.outdir
        out.mkdir(parents=True, exist_ok=True)
        (out / "fb200-recovery.bin").write_bytes(rec)
        (out / "fb200-app.slot").write_bytes(slot)
        manifest = {"version": args.version,
                    "app": {"file": "fb200-app.slot", "size": len(slot), "crc32": crc(slot)},
                    "recovery": {"file": "fb200-recovery.bin", "size": len(rec),
                                 "crc32": crc(rec)},
                    "stock_data_version": stockdata.VERSION}
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"recovery {len(rec)} B, app slot {len(slot)} B (data {len(data)} B) -> {out}")
        if args.stock:
            stock = args.stock.read_bytes()
            (out / "fb200-twostage.mr").write_bytes(images.twostage_mr(stock, rec, slot))
            full = images.splice_vendor_loader(rec, images.stock_image(stock).blocks[0].data)
            (out / "fb200-recovery-full.bin").write_bytes(full)
            (out / "fb200-stock.blob").write_bytes(stockdata.build(stock))
            print(f"personal images (vendor data, do not share): fb200-twostage.mr, "
                  f"fb200-recovery-full.bin, fb200-stock.blob -> {out}")
    except FirmwareError as exc:
        raise SystemExit(f"pack_images: {exc}") from None
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
