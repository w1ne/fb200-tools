"""Two-stage image assembly (src/fb200/images.py), without vendor data."""

import os
import struct
from pathlib import Path

import pytest

from fb200 import images, stockdata
from fb200.errors import FirmwareError
from fb200.firmware import MrFile

MR = Path(os.environ.get("FB200_STOCK_MR", Path(__file__).parents[1] / "fb200-stock.mr"))


def fake_block0() -> bytes:
    b = bytearray(os.urandom(images.BLOCK0_SIZE))
    b[images.TABLE_OFF:images.TABLE_OFF + 16] = images.STOCK_ENTRY0
    b[images.TABLE_OFF + 64:images.TABLE_OFF + 80] = struct.pack("<4I", 0, *images.BSS_MEMSET, 0)
    return bytes(b)


def recovery() -> bytes:
    return images.build_recovery(b"\x01" * 0x400, b"fwbegin\0fwrec\0fwstock\0" * 10, b"\x02" * 64)


def test_published_recovery_has_no_vendor_bytes():
    rec = recovery()
    assert rec[images.BOOT_OFF:images.BLOB_OFF] == b"\xff" * (images.BLOB_OFF - images.BOOT_OFF)


def test_splice_copies_the_loader_and_neuters_entries_1_to_3():
    b0 = fake_block0()
    rec = images.splice_vendor_loader(recovery(), b0)
    assert rec[images.BOOT_OFF:images.TABLE_OFF] == b0[images.BOOT_OFF:images.TABLE_OFF]
    entry4 = b0[images.TABLE_OFF + 64:images.TABLE_OFF + 80]
    for i in (1, 2, 3, 4):
        assert rec[images.TABLE_OFF + 16 * i:images.TABLE_OFF + 16 * (i + 1)] == entry4
    assert rec[:0x400] == b"\x01" * 0x400 and rec[images.BLOB_OFF:images.BLOB_OFF + 7] == b"fwbegin"


def test_splice_refuses_a_foreign_image():
    with pytest.raises(FirmwareError, match="entry 0"):
        images.splice_vendor_loader(recovery(), os.urandom(images.BLOCK0_SIZE))


def test_slot_roundtrip_and_damage():
    slot = images.build_slot(b"\x03" * 0x400, b"fwbegin\0" * 100, b"\x04" * 5000)
    images.check_slot(slot)
    bad = bytearray(slot)
    bad[images.SLOT_HDR + 0x500] ^= 1
    with pytest.raises(FirmwareError, match="CRC"):
        images.check_slot(bytes(bad))
    bad = bytearray(slot)
    bad[-1] ^= 1
    with pytest.raises(FirmwareError, match="data CRC"):
        images.check_slot(bytes(bad))
    with pytest.raises(FirmwareError, match="too large"):
        images.build_slot(b"\x03" * 0x400, b"x", b"\x04" * (images.SLOT_DATA_MAX + 4))


def test_missing_update_command_is_refused():
    with pytest.raises(FirmwareError, match="fwstock"):
        images.require_update_commands("app", b"fwbegin\0fwrec\0")


def test_twostage_from_the_real_stock_image():
    """Local only (vendor file): the first-install .mr keeps the stock header and
    model block, and carries the loader, recovery and app."""
    if not MR.is_file():
        pytest.skip("fb200-stock.mr not found (vendor file; set FB200_STOCK_MR)")
    stock_bytes = MR.read_bytes()
    slot = images.build_slot(b"\x03" * 0x400, b"fwbegin\0" * 100, b"\x04" * 5000)
    mr = MrFile.from_bytes(images.twostage_mr(stock_bytes, recovery(), slot))
    stock = MrFile.from_bytes(stock_bytes)
    assert mr.header.product_tag == "FB200" and mr.blocks[1].data == stock.blocks[1].data
    b0 = mr.blocks[0].data
    assert b0[images.BOOT_OFF:images.TABLE_OFF] == stock.blocks[0].data[images.BOOT_OFF:images.TABLE_OFF]
    assert b0[images.REC_SIZE:images.REC_SIZE + len(slot)][:images.BLOCK0_SIZE - images.REC_SIZE] \
        == slot[:images.BLOCK0_SIZE - images.REC_SIZE]
    stockdata.verify(stockdata.build(stock_bytes))
