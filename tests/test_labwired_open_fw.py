# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""The open-firmware twin image carries our loader and no vendor bytes."""

import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from labwired_elf import build_elf
from labwired_open_fw import FLASH_RECOVERY, FLASH_SLOT, STUB, STUB_OFF, install_stub, open_elf

from fb200.images import BLOB_OFF, REC_SIZE, build_recovery, build_slot


def _vectors(reset: int = 0x600104D9) -> bytes:
    words = [0x20058000, reset] + [0x20000001] * 254
    return struct.pack("<256I", *words)


def test_stub_fits_in_the_erased_loader_range():
    assert STUB_OFF == 0x4D8
    assert STUB_OFF + len(STUB) <= BLOB_OFF
    # The last literal is the Thumb entry of stage2.
    assert struct.unpack_from("<I", STUB, len(STUB) - 4)[0] == 0x4D7


def test_install_stub_writes_only_the_erased_range():
    rec = build_recovery(_vectors(), b"\x11" * 16, b"\x22" * 8)
    assert rec[STUB_OFF:STUB_OFF + len(STUB)] == b"\xff" * len(STUB)
    out = install_stub(rec)
    assert out[STUB_OFF:STUB_OFF + len(STUB)] == STUB
    assert out[BLOB_OFF:BLOB_OFF + 16] == b"\x11" * 16
    assert out[:0x400] == rec[:0x400]


def test_install_stub_refuses_a_range_that_already_has_bytes():
    rec = bytearray(build_recovery(_vectors(), b"\x11" * 16, b"\x22" * 8))
    rec[STUB_OFF] = 0x00
    with pytest.raises(Exception, match="not erased"):
        install_stub(bytes(rec))


def test_open_elf_places_recovery_and_the_slot():
    blob = b"fwbegin\0fwrec\0fwstock\0" + b"\x00" * 8
    data = b"\x33" * 4
    elf = open_elf(_vectors(), blob, b"\x22" * 4, _vectors(), blob, data)
    placed = build_elf([
        (FLASH_RECOVERY, install_stub(build_recovery(_vectors(), blob, b"\x22" * 4))),
        (FLASH_SLOT, build_slot(_vectors(), blob, data)),
    ])
    assert elf == placed
    assert len(elf) > REC_SIZE
