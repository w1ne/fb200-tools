#!/usr/bin/env python3
"""Build an ELF of the open FB200 firmware for the LabWired twin.

The twin starts at the flash vector table (FlexSPI 0x60010000). Recovery's
vector points at 0x600104D9, where the vendor loader stub lives on a stock
image. The published recovery image leaves 0x400..0x7D4 erased. This tool
writes a short Thumb loader there. The loader copies the recovery blob to
ITCM 0x400 and branches to stage2 at 0x4D6. Recovery then launches the app
slot, the same hand-over the pedal uses.

Build both images first (docs/LABWIRED.md):

    make -C firmware/audio build VARIANT=recovery
    make -C firmware/audio build VARIANT=app
    python3 tools/labwired_open_fw.py

The ELF is build/labwired/open.elf. It contains no vendor bytes.

The same step reads the `knobs` symbol from fb200-app.elf and writes
build/labwired/open-boot.yaml. labwired/open-boot.yaml names that symbol.
The generated file carries the address for this build. `labwired test`
runs the generated file.
"""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "src"))
sys.path.insert(0, str(REPO_ROOT / "tools"))

from labwired_elf import build_elf

from fb200 import images
from fb200.errors import FirmwareError

# Thumb code linked at 0x600104D8. Copies 0xE82C bytes (0x7D4..0xF000) from
# flash to ITCM 0x400, then bx 0x4D7. Assembled with arm-none-eabi-gcc
# -Ttext=0x600104D8. 36 bytes, so it ends at 0x600104FC, before the blob.
STUB = bytes.fromhex(
    "04480549054a08c808c1043afbd1044b"
    "184700bfd4070160000400002ce80000"
    "d7040000"
)
STUB_OFF = 0x4D8
FLASH_RECOVERY = 0x60010000
FLASH_SLOT = 0x60020000


def install_stub(recovery: bytes) -> bytes:
    """Return recovery with the twin loader written into the erased range."""
    if len(recovery) != images.REC_SIZE:
        raise FirmwareError(f"recovery image must be {images.REC_SIZE:#x} bytes")
    end = STUB_OFF + len(STUB)
    if end > images.BLOB_OFF:
        raise FirmwareError("twin loader overlaps the recovery blob")
    hole = recovery[STUB_OFF:end]
    if any(b != 0xFF for b in hole):
        raise FirmwareError("twin loader range is not erased")
    rec = bytearray(recovery)
    rec[STUB_OFF:end] = STUB
    return bytes(rec)


def open_elf(recovery_vectors: bytes, recovery_blob: bytes, copier: bytes,
             app_vectors: bytes, app_blob: bytes, data: bytes) -> bytes:
    """Recovery at 0x60010000, app slot at 0x60020000, loader stub installed."""
    images.require_update_commands("recovery", recovery_blob)
    images.require_update_commands("app", app_blob + data)
    rec = install_stub(images.build_recovery(recovery_vectors, recovery_blob, copier))
    slot = images.build_slot(app_vectors, app_blob, data)
    images.check_slot(slot)
    return build_elf([(FLASH_RECOVERY, rec), (FLASH_SLOT, slot)])


# `symbol: knobs, offset: N,` in labwired/open-boot.yaml. One slot of the
# file-local uint16_t knobs[16] array.
_KNOB_SYMBOL = re.compile(
    r"symbol:\s*knobs,\s*offset:\s*(?P<offset>0x[0-9a-fA-F]+|\d+),"
)
_KNOB_SLOTS = 16


def elf_symbol_bytes(data: bytes, name: str) -> int | None:
    """Return the address of `name` in an ELF32 little-endian symbol table.

    Local symbols count. `knobs` in the app image is one of those. Returns
    None when the file has no section headers or no symbol of that name.
    Two different addresses for one name is an error.
    """
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise FirmwareError(f"{name} lookup needs an ELF32 little-endian file")
    e_shoff = struct.unpack_from("<I", data, 32)[0]
    e_shentsize, e_shnum, _e_shstrndx = struct.unpack_from("<HHH", data, 46)
    if e_shoff == 0 or e_shnum == 0:
        return None
    if e_shentsize < 40:
        raise FirmwareError("section header is shorter than ELF32")

    def section(index: int) -> tuple[int, ...]:
        off = e_shoff + index * e_shentsize
        if off + 40 > len(data):
            raise FirmwareError("section header runs past the end of the file")
        return struct.unpack_from("<10I", data, off)

    found: list[int] = []
    for index in range(e_shnum):
        (
            _sh_name,
            sh_type,
            _sh_flags,
            _sh_addr,
            sh_offset,
            sh_size,
            sh_link,
            _sh_info,
            _sh_addralign,
            sh_entsize,
        ) = section(index)
        if sh_type != 2:  # SHT_SYMTAB
            continue
        if sh_entsize == 0:
            sh_entsize = 16
        str_off, str_size = section(sh_link)[4], section(sh_link)[5]
        for sym in range(sh_size // sh_entsize):
            sym_off = sh_offset + sym * sh_entsize
            st_name, st_value = struct.unpack_from("<II", data, sym_off)
            if st_name == 0:
                continue
            start = str_off + st_name
            end = data.find(b"\0", start, str_off + str_size)
            if end < 0:
                continue
            if data[start:end].decode("ascii", "replace") != name:
                continue
            found.append(st_value)
    if not found:
        return None
    if any(value != found[0] for value in found):
        raise FirmwareError(f"symbol {name} has more than one address")
    return found[0]


def elf_symbol(path: Path, name: str) -> int | None:
    """Return the address of `name` in the ELF at `path`."""
    return elf_symbol_bytes(path.read_bytes(), name)


def render_gate(text: str, knobs_addr: int) -> str:
    """Replace each `symbol: knobs, offset: N` with the address for this build.

    The open gate names the symbol because the linker moves the array.
    `labwired test` reads a numeric address. Exactly 16 slots are required:
    one raw count per knob.
    """

    def replace(match: re.Match[str]) -> str:
        offset = int(match.group("offset"), 0)
        return f"address: {knobs_addr + offset:#x},"

    rendered, count = _KNOB_SYMBOL.subn(replace, text)
    if count != _KNOB_SLOTS:
        raise FirmwareError(f"open gate must name knobs {_KNOB_SLOTS} times, found {count}")
    if "symbol:" in rendered:
        raise FirmwareError("unresolved symbol in the open gate")
    return rendered


def place_gate(text: str, knobs_addr: int) -> str:
    """Render the gate and point it at the ELF next to the generated file.

    The generated file is build/labwired/open-boot.yaml. The system manifest
    stays in labwired/.
    """
    rendered = render_gate(text, knobs_addr)
    rendered, fw_count = re.subn(
        r'^(\s*firmware:\s*).*$',
        r'\1"open.elf"',
        rendered,
        count=1,
        flags=re.MULTILINE,
    )
    rendered, sys_count = re.subn(
        r'^(\s*system:\s*).*$',
        r'\1"../../labwired/system.yaml"',
        rendered,
        count=1,
        flags=re.MULTILINE,
    )
    if fw_count != 1 or sys_count != 1:
        raise FirmwareError("open gate is missing its firmware or system input")
    return rendered


def _part(prefix: Path, kind: str) -> bytes:
    path = Path(f"{prefix}.{kind}.bin")
    if not path.is_file():
        raise FirmwareError(f"missing {path}; build firmware/audio first")
    return path.read_bytes()


def main() -> int:
    audio = REPO_ROOT / "firmware" / "audio" / "build"
    try:
        elf = open_elf(
            _part(audio / "fb200-recovery", "vectors"),
            _part(audio / "fb200-recovery", "blob"),
            _part(audio / "fb200-recovery", "copier"),
            _part(audio / "fb200-app", "vectors"),
            _part(audio / "fb200-app", "blob"),
            _part(audio / "fb200-app", "dtcmdata"),
        )
    except FirmwareError as exc:
        raise SystemExit(f"labwired_open_fw: {exc}") from None
    out = REPO_ROOT / "build" / "labwired" / "open.elf"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(elf)
    app_elf = audio / "fb200-app.elf"
    knobs = elf_symbol(app_elf, "knobs")
    if knobs is None:
        raise SystemExit(f"labwired_open_fw: no knobs symbol in {app_elf}")
    template = REPO_ROOT / "labwired" / "open-boot.yaml"
    gate = place_gate(template.read_text(), knobs)
    gate_path = out.parent / "open-boot.yaml"
    gate_path.write_text(gate)
    print(f"wrote {out} ({len(elf)} bytes)")
    print(f"wrote {gate_path} (knobs={knobs:#x})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
