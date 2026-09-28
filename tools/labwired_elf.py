#!/usr/bin/env python3
"""Wrap raw binary blobs into one ARM ELF so `labwired test` can load them.

`labwired test --script` loads `inputs.firmware` as an ELF (its PT_LOAD
segments). The FB200 stock image is two raw payloads that live at fixed flash
addresses (block 0 at 0x60010000, block 1 at 0x600D0000), so this tool places
each blob in its own PT_LOAD segment at its address:

    labwired_elf.py -o build/labwired/stock.elf \
        --segment build/labwired/block0.bin@0x60010000 \
        --segment build/labwired/block1.bin@0x600D0000

The entry point is the reset vector of the first segment ([1] of its vector
table); the LabWired machine takes SP/PC from the vector table anyway.
The output carries only the bytes it was given: run it on your own copy of
the vendor image and keep the result out of git (build/ is ignored).
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

EHDR = struct.Struct("<16sHHIIIIIHHHHHH")
PHDR = struct.Struct("<IIIIIIII")
EM_ARM = 40
ET_EXEC = 2
PT_LOAD = 1
PF_R, PF_X = 4, 1
EF_ARM_EABI_VER5 = 0x05000000
ALIGN = 0x1000


def build_elf(segments: list[tuple[int, bytes]]) -> bytes:
    """Return an ELF32 little-endian ARM image with one PT_LOAD per segment."""
    if not segments:
        raise ValueError("at least one segment is required")
    ordered = sorted(segments)
    for (a0, d0), (a1, _) in zip(ordered, ordered[1:]):
        if a0 + len(d0) > a1:
            raise ValueError(f"segments overlap: 0x{a0:08x}+{len(d0):#x} > 0x{a1:08x}")
    first_addr, first = segments[0]
    entry = struct.unpack_from("<I", first, 4)[0] if len(first) >= 8 else first_addr

    offset = EHDR.size + PHDR.size * len(segments)
    body = bytearray()
    phdrs = bytearray()
    for addr, data in segments:
        # Keep p_offset congruent to p_vaddr modulo the alignment.
        pad = (addr - (offset + len(body))) % ALIGN
        body += bytes(pad)
        phdrs += PHDR.pack(PT_LOAD, offset + len(body), addr, addr, len(data), len(data),
                           PF_R | PF_X, ALIGN)
        body += data

    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)  # ELFCLASS32, LSB, v1, SYSV
    ehdr = EHDR.pack(ident, ET_EXEC, EM_ARM, 1, entry, EHDR.size, 0, EF_ARM_EABI_VER5,
                     EHDR.size, PHDR.size, len(segments), 40, 0, 0)
    return ehdr + bytes(phdrs) + bytes(body)


def parse_segment(spec: str) -> tuple[int, bytes]:
    path, sep, addr = spec.rpartition("@")
    if not sep or not path:
        raise argparse.ArgumentTypeError(f"expected PATH@ADDRESS, got {spec!r}")
    try:
        address = int(addr, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"bad address in {spec!r}") from exc
    try:
        data = Path(path).read_bytes()
    except OSError as exc:
        raise argparse.ArgumentTypeError(f"cannot read {path}: {exc}") from exc
    return address, data


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-o", "--output", required=True, type=Path)
    ap.add_argument("--segment", action="append", required=True, type=parse_segment,
                    metavar="PATH@ADDRESS", help="raw blob and its load address (repeatable)")
    args = ap.parse_args(argv)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_elf(args.segment))
    total = sum(len(d) for _, d in args.segment)
    print(f"wrote {args.output} ({len(args.segment)} segments, {total} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
