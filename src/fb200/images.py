# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Two-stage firmware images (docs/BOOTLOADER.md §4).

Block 0 (flash 0x60010000, 0x31000 bytes) of the open firmware:

  0x00000  recovery vectors
  0x00400  vendor stub + loader, load table at 0x784 (from the stock image)
  0x007d4  recovery blob (loader entry 0 copies it to ITCM 0x400)
  0x0f000  recovery copier (staged to ITCM 0x1F000 at run time)
  0x10000  app slot: header (0x100) + app vectors (0x400) + app blob

The published images contain no vendor bytes: the recovery image leaves
0x400..0x7d4 erased (0xFF), and `splice_vendor_loader` fills it from the
user's own stock .mr for the first install. Load-table entries 1-3
decompress stock data from 0x6002e39c.. into DTCM and OCRAM. That flash is
the app slot now, so they are replaced with copies of entry 4 (the .bss
memset).

Pure Python: the web updater runs this module in Pyodide.
"""

from __future__ import annotations

import struct
import zlib

from fb200.errors import FirmwareError
from fb200.firmware import MrBlock, MrFile

BLOCK0_SIZE = 0x31000
REC_SIZE = 0x10000
BOOT_OFF, TABLE_OFF, BLOB_OFF = 0x400, 0x784, 0x7D4
COPIER_OFF = 0xF000
SLOT_MAGIC = 0x50414246          # "FBAP"
SLOT_HDR = 0x100
APP_ITCM_LIMIT = 0x1F000         # copier lives above
SLOT_DATA_OFF = 0x21000          # slot offset of the const tables (flash 0x60041000)
SLOT_DATA_MAX = 0x20000          # up to the stock data at 0x60061000
STOCK_ENTRY0 = bytes.fromhex("d407016000040000c8db0100a0040160")
BSS_MEMSET = (0x20018B44, 0x358A4)

# Every image must keep its USB update commands, or the next update needs A+D.
# (A console edit once dropped `fwbegin` from both stages; caught on hardware.)
REQUIRED_COMMANDS = (b"fwbegin\0", b"fwrec\0", b"fwstock\0")


def require_update_commands(name: str, blob: bytes) -> None:
    missing = [c.rstrip(b"\0").decode() for c in REQUIRED_COMMANDS if c not in blob]
    if missing:
        raise FirmwareError(f"{name} blob lacks console command(s) {missing}; refusing")


def build_recovery(vectors: bytes, blob: bytes, copier: bytes) -> bytes:
    """The recovery image without vendor bytes (0x400..0x7d4 erased)."""
    if len(vectors) != 0x400:
        raise FirmwareError("recovery vectors must be 0x400 bytes")
    if BLOB_OFF + len(blob) > COPIER_OFF:
        raise FirmwareError(f"recovery blob too large ({len(blob)} > {COPIER_OFF - BLOB_OFF})")
    if len(copier) > REC_SIZE - COPIER_OFF:
        raise FirmwareError("copier too large")
    rec = bytearray(b"\xff" * REC_SIZE)
    rec[0:0x400] = vectors
    rec[BLOB_OFF:BLOB_OFF + len(blob)] = blob
    rec[COPIER_OFF:COPIER_OFF + len(copier)] = copier
    return bytes(rec)


def splice_vendor_loader(recovery: bytes, stock_block0: bytes) -> bytes:
    """Copy the vendor stub + loader from the stock block 0 into a recovery
    image, with load-table entries 1-3 turned into the .bss memset."""
    if len(recovery) != REC_SIZE:
        raise FirmwareError(f"recovery image must be {REC_SIZE:#x} bytes")
    if len(stock_block0) != BLOCK0_SIZE:
        raise FirmwareError(f"unexpected stock block 0 size {len(stock_block0):#x}")
    if stock_block0[TABLE_OFF:TABLE_OFF + 16] != STOCK_ENTRY0:
        raise FirmwareError("stock loader table entry 0 mismatch: not the FB200 stock firmware?")
    entry4 = stock_block0[TABLE_OFF + 64:TABLE_OFF + 80]
    if struct.unpack("<4I", entry4)[1:3] != BSS_MEMSET:
        raise FirmwareError("stock loader table entry 4 is not the .bss memset")
    rec = bytearray(recovery)
    rec[BOOT_OFF:BLOB_OFF] = stock_block0[BOOT_OFF:BLOB_OFF]
    for i in (1, 2, 3):
        rec[TABLE_OFF + 16 * i:TABLE_OFF + 16 * (i + 1)] = entry4
    return bytes(rec)


def build_slot(vectors: bytes, blob: bytes, data: bytes = b"") -> bytes:
    """App slot: header (v2: + data length/CRC), vectors, blob; the const
    tables (linker .dtcmdata) follow at SLOT_DATA_OFF and are copied to DTCM
    by the app's startup."""
    if len(vectors) != 0x400:
        raise FirmwareError("app vectors must be 0x400 bytes")
    if 0x400 + len(blob) > APP_ITCM_LIMIT:
        raise FirmwareError(f"app blob too large for ITCM ({len(blob)} > {APP_ITCM_LIMIT - 0x400})")
    blob = blob + b"\xff" * (-len(blob) % 4)
    body = vectors + blob
    crc = zlib.crc32(body) & 0xFFFFFFFF
    if len(data) > SLOT_DATA_MAX:
        raise FirmwareError(f"app data too large ({len(data)} > {SLOT_DATA_MAX})")
    data = data + b"\xff" * (-len(data) % 4)
    header = struct.pack("<6I", SLOT_MAGIC, len(blob), crc, 2, len(data),
                         zlib.crc32(data) & 0xFFFFFFFF).ljust(SLOT_HDR, b"\xff")
    slot = header + body
    if data:
        if len(slot) > SLOT_DATA_OFF:
            raise FirmwareError("app image overlaps its data area")
        slot = slot.ljust(SLOT_DATA_OFF, b"\xff") + data
    return slot


def check_slot(slot: bytes) -> None:
    """Header, CRCs and size of an app slot image (as recovery checks it)."""
    if len(slot) < SLOT_HDR + 0x400:
        raise FirmwareError("app slot image too short")
    magic, blob_len, crc, version, data_len, data_crc = struct.unpack_from("<6I", slot)
    if magic != SLOT_MAGIC or version != 2:
        raise FirmwareError("not an FB200 app slot image")
    if zlib.crc32(slot[SLOT_HDR:SLOT_HDR + 0x400 + blob_len]) & 0xFFFFFFFF != crc:
        raise FirmwareError("app slot image CRC mismatch")
    data = slot[SLOT_DATA_OFF:SLOT_DATA_OFF + data_len]
    if data_len and (len(data) != data_len or zlib.crc32(data) & 0xFFFFFFFF != data_crc):
        raise FirmwareError("app slot data CRC mismatch")
    if len(slot) > SLOT_DATA_OFF + SLOT_DATA_MAX:
        raise FirmwareError("app slot image too large")


def stock_image(data: bytes) -> MrFile:
    stock = MrFile.from_bytes(data)
    if stock.header.product_tag != "FB200" or len(stock.blocks) < 2:
        raise FirmwareError("not the FB200 stock firmware (.mr with the model block)")
    if len(stock.blocks[0].data) != BLOCK0_SIZE:
        raise FirmwareError(f"unexpected stock block 0 size {len(stock.blocks[0].data):#x}")
    return stock


def twostage_mr(stock_mr: bytes, recovery: bytes, slot: bytes) -> bytes:
    """The first-install image for the vendor updater (A+D), from the user's
    stock .mr and the published recovery and app images.

    The vendor updater only writes block 0 (up to 0x60041000): it cannot
    carry the app's data. Recovery then stays on the USB console (data CRC)
    until the app is written over USB, which writes the whole slot."""
    stock = stock_image(stock_mr)
    rec = splice_vendor_loader(recovery, stock.blocks[0].data)
    block0 = (rec + slot)[:BLOCK0_SIZE].ljust(BLOCK0_SIZE, b"\xff")
    return MrFile(stock.header, [MrBlock(stock.blocks[0].tag, block0),
                                 MrBlock(stock.blocks[1].tag, stock.blocks[1].data)]).to_bytes()
