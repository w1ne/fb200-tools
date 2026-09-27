#!/usr/bin/env python3
"""Pack the two-stage FB200 images (docs/BOOTLOADER.md §4).

Block 0 (flash 0x60010000, 0x31000 bytes) becomes:

  0x00000  recovery vectors
  0x00400  vendor stub + loader (kept verbatim); load table at 0x784 edited
  0x007d4  recovery blob (loader entry 0 copies it to ITCM 0x400)
  0x0f000  recovery copier (staged to ITCM 0x1F000 at run time)
  0x10000  app slot: header (0x100) + app vectors (0x400) + app blob

Load-table entries 1-3 decompress stock data from 0x6002e39c.. into DTCM and
OCRAM. That flash is the app slot now, and our firmware uses none of that
data, so they are replaced with copies of entry 4 (the .bss memset).

Outputs (in OUTDIR):
  fb200-recovery.bin   block 0 [0x0, 0x10000)  -> usb_update.py recovery
  fb200-app.slot       block 0 [0x10000, ...)  -> usb_update.py app
  fb200-twostage.mr    full image for the vendor DFU (A+D)
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "src"))

from fb200.firmware import MrBlock, MrFile

BLOCK0_SIZE = 0x31000
REC_SIZE = 0x10000
BOOT_OFF, TABLE_OFF, BLOB_OFF = 0x400, 0x784, 0x7D4
COPIER_OFF = 0xF000
SLOT_OFF = 0x10000
SLOT_MAGIC = 0x50414246          # "FBAP"
SLOT_HDR = 0x100
APP_ITCM_LIMIT = 0x1F000         # copier lives above
STOCK_ENTRY0 = bytes.fromhex("d407016000040000c8db0100a0040160")


# Every image must keep its USB update commands, or the next update needs A+D.
# (A console edit once dropped `fwbegin` from both stages; caught on hardware.)
REQUIRED_COMMANDS = (b"fwbegin\0", b"fwrec\0")


def require_update_commands(name: str, blob: bytes) -> None:
    missing = [c.rstrip(b"\0").decode() for c in REQUIRED_COMMANDS if c not in blob]
    if missing:
        raise SystemExit(f"{name} blob lacks console command(s) {missing}; refusing")


def build_recovery(stock_block0: bytes, vectors: bytes, blob: bytes, copier: bytes) -> bytes:
    if len(vectors) != 0x400:
        raise SystemExit("recovery vectors must be 0x400 bytes")
    if BLOB_OFF + len(blob) > COPIER_OFF:
        raise SystemExit(f"recovery blob too large ({len(blob)} > {COPIER_OFF - BLOB_OFF})")
    if len(copier) > REC_SIZE - COPIER_OFF:
        raise SystemExit("copier too large")
    if stock_block0[TABLE_OFF:TABLE_OFF + 16] != STOCK_ENTRY0:
        raise SystemExit("template loader table entry 0 mismatch")
    rec = bytearray(b"\xff" * REC_SIZE)
    rec[BOOT_OFF:BLOB_OFF] = stock_block0[BOOT_OFF:BLOB_OFF]
    entry4 = stock_block0[TABLE_OFF + 64:TABLE_OFF + 80]
    if struct.unpack("<4I", entry4)[1:3] != (0x20018B44, 0x358A4):
        raise SystemExit("template loader table entry 4 is not the .bss memset")
    for i in (1, 2, 3):
        rec[TABLE_OFF + 16 * i:TABLE_OFF + 16 * (i + 1)] = entry4
    rec[0:0x400] = vectors
    rec[BLOB_OFF:BLOB_OFF + len(blob)] = blob
    rec[COPIER_OFF:COPIER_OFF + len(copier)] = copier
    return bytes(rec)


SLOT_DATA_OFF = 0x21000         # slot offset of the const tables (flash 0x60041000)
SLOT_DATA_MAX = 0x30000         # up to the presets at 0x60071000


def build_slot(vectors: bytes, blob: bytes, data: bytes = b"") -> bytes:
    """App slot: header (v2: + data length/CRC), vectors, blob; the const
    tables (linker .dtcmdata) follow at SLOT_DATA_OFF and are copied to DTCM
    by the app's startup."""
    if len(vectors) != 0x400:
        raise SystemExit("app vectors must be 0x400 bytes")
    if 0x400 + len(blob) > APP_ITCM_LIMIT:
        raise SystemExit(f"app blob too large for ITCM ({len(blob)} > {APP_ITCM_LIMIT - 0x400})")
    blob = blob + b"\xff" * (-len(blob) % 4)
    body = vectors + blob
    crc = zlib.crc32(body) & 0xFFFFFFFF
    if len(data) > SLOT_DATA_MAX:
        raise SystemExit(f"app data too large ({len(data)} > {SLOT_DATA_MAX})")
    data = data + b"\xff" * (-len(data) % 4)
    header = struct.pack("<6I", SLOT_MAGIC, len(blob), crc, 2, len(data),
                         zlib.crc32(data) & 0xFFFFFFFF).ljust(SLOT_HDR, b"\xff")
    slot = header + body
    if data:
        if len(slot) > SLOT_DATA_OFF:
            raise SystemExit("app image overlaps its data area")
        slot = slot.ljust(SLOT_DATA_OFF, b"\xff") + data
    return slot


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("stock", type=Path, help="stock .mr (vendor loader + model block)")
    ap.add_argument("recovery", type=Path, help="build dir prefix, e.g. build/fb200-recovery")
    ap.add_argument("app", type=Path, help="build dir prefix, e.g. build/fb200-app")
    ap.add_argument("-o", "--outdir", type=Path, required=True)
    args = ap.parse_args()

    stock = MrFile.from_path(args.stock)
    if stock.header.product_tag != "FB200" or len(stock.blocks) < 2:
        raise SystemExit("stock image must be an FB200 .mr with the model block")
    b0 = stock.blocks[0].data
    if len(b0) != BLOCK0_SIZE:
        raise SystemExit(f"unexpected stock block 0 size {len(b0):#x}")

    def part(prefix: Path, kind: str) -> bytes:
        return Path(f"{prefix}.{kind}.bin").read_bytes()

    require_update_commands("recovery", part(args.recovery, "blob"))
    require_update_commands("app", part(args.app, "blob"))
    rec = build_recovery(b0, part(args.recovery, "vectors"), part(args.recovery, "blob"),
                         part(args.recovery, "copier"))
    data_path = Path(f"{args.app}.dtcmdata.bin")
    data = data_path.read_bytes() if data_path.exists() else b""
    slot = build_slot(part(args.app, "vectors"), part(args.app, "blob"), data)
    # The DFU image only covers block 0 (up to 0x60041000): it cannot carry the
    # app data. Recovery then stays on the console (data CRC) until the app is
    # updated over USB, which writes the whole slot including the data.
    block0 = (rec + slot)[:BLOCK0_SIZE].ljust(BLOCK0_SIZE, b"\xff")
    mr = MrFile(stock.header, [MrBlock(stock.blocks[0].tag, block0),
                               MrBlock(stock.blocks[1].tag, stock.blocks[1].data)])

    args.outdir.mkdir(parents=True, exist_ok=True)
    (args.outdir / "fb200-recovery.bin").write_bytes(rec)
    (args.outdir / "fb200-app.slot").write_bytes(slot)
    (args.outdir / "fb200-twostage.mr").write_bytes(mr.to_bytes())
    print(f"recovery {len(rec)} B, app slot {len(slot)} B (blob {len(part(args.app, 'blob'))} B, "
          f"data {len(data)} B), .mr written to {args.outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
