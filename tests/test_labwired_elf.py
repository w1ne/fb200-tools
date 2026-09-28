"""tools/labwired_elf.py: raw blobs to one ARM ELF for `labwired test`."""
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from labwired_elf import ALIGN, EHDR, EM_ARM, PHDR, PT_LOAD, build_elf, main


def vector_table(sp: int, reset: int, size: int = 64) -> bytes:
    return struct.pack("<II", sp, reset) + bytes(size - 8)


def parse(elf: bytes):
    ehdr = EHDR.unpack_from(elf, 0)
    ident, e_type, machine, _ver, entry, phoff = ehdr[:6]
    phnum = ehdr[10]
    phdrs = [PHDR.unpack_from(elf, phoff + i * PHDR.size) for i in range(phnum)]
    return ident, e_type, machine, entry, phdrs


def test_header_and_entry_from_reset_vector():
    elf = build_elf([(0x6001_0000, vector_table(0x2002_0000, 0x6001_04D9))])
    ident, e_type, machine, entry, phdrs = parse(elf)
    assert ident[:4] == b"\x7fELF" and ident[4] == 1 and ident[5] == 1  # ELF32, LSB
    assert e_type == 2 and machine == EM_ARM
    assert entry == 0x6001_04D9  # vector [1], Thumb bit kept
    assert len(phdrs) == 1


def test_segments_addresses_offsets_and_bytes():
    a = vector_table(0x2002_0000, 0x6001_0101, 0x200)
    b = bytes(range(256)) * 3
    elf = build_elf([(0x6001_0000, a), (0x600D_0000, b)])
    _, _, _, _, phdrs = parse(elf)
    assert [(p[0], p[2], p[3], p[4], p[5]) for p in phdrs] == [
        (PT_LOAD, 0x6001_0000, 0x6001_0000, len(a), len(a)),
        (PT_LOAD, 0x600D_0000, 0x600D_0000, len(b), len(b)),
    ]
    for p, data in zip(phdrs, (a, b)):
        offset, vaddr, align = p[1], p[2], p[7]
        assert align == ALIGN
        assert offset % ALIGN == vaddr % ALIGN  # loaders need this congruence
        assert elf[offset:offset + len(data)] == data
    assert phdrs[1][1] >= phdrs[0][1] + len(a)  # file ranges do not overlap


def test_unaligned_address_keeps_congruence():
    elf = build_elf([(0x2000_0123, vector_table(0, 0x2000_0201))])
    _, _, _, _, (p,) = parse(elf)
    assert p[1] % ALIGN == 0x123


def test_overlap_rejected():
    with pytest.raises(ValueError, match="overlap"):
        build_elf([(0x6001_0000, bytes(0x100)), (0x6001_00FF, bytes(4))])


def test_overlap_rejected_in_any_order():
    with pytest.raises(ValueError, match="overlap"):
        build_elf([(0x6001_0080, bytes(4)), (0x6001_0000, bytes(0x100))])


def test_adjacent_segments_accepted():
    elf = build_elf([(0x6001_0000, bytes(0x100)), (0x6001_0100, bytes(4))])
    assert len(parse(elf)[4]) == 2


def test_empty_rejected():
    with pytest.raises(ValueError):
        build_elf([])


def test_cli(tmp_path, capsys):
    blob = tmp_path / "b0.bin"
    blob.write_bytes(vector_table(0x2002_0000, 0x6001_0401))
    out = tmp_path / "out" / "x.elf"
    assert main(["-o", str(out), "--segment", f"{blob}@0x60010000"]) == 0
    assert parse(out.read_bytes())[3] == 0x6001_0401
    assert "1 segments" in capsys.readouterr().out


def test_cli_bad_segment(tmp_path):
    with pytest.raises(SystemExit):
        main(["-o", str(tmp_path / "x.elf"), "--segment", "no-address"])
