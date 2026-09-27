"""Parse the built firmware's USB configuration descriptor and check the
composite layout (CDC + UAC2 audio). Catches descriptor-length/entity bugs
that are painful to debug on hardware."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
ELF = FW / "build" / "fb200-app.elf"

pytestmark = pytest.mark.skipif(
    any(shutil.which(t) is None for t in ("arm-none-eabi-gcc", "make")),
    reason="toolchain not installed",
)

CONFIG, INTERFACE, ENDPOINT, IAD = 0x02, 0x04, 0x05, 0x0B
CS_INTERFACE = 0x24
CS_AC_HEADER, CS_AC_INPUT_TERM, CS_AC_OUTPUT_TERM = 0x01, 0x02, 0x03
CS_AC_FEATURE_UNIT, CS_AC_CLOCK_SOURCE = 0x06, 0x0A


def descriptor() -> bytes:
    subprocess.run(["make", "clean", "build"], cwd=FW, check=True,
                   capture_output=True)
    nm = subprocess.run(["arm-none-eabi-nm", str(ELF)], check=True,
                        capture_output=True, text=True).stdout
    addr = next(int(line.split()[0], 16) for line in nm.splitlines()
                if line.endswith(" desc_configuration"))
    sections = subprocess.run(["arm-none-eabi-readelf", "-S", "-W", str(ELF)],
                              check=True, capture_output=True, text=True).stdout
    # Find the allocatable section containing the descriptor (the linker merges
    # .rodata into .blob).
    sec_re = re.compile(r"\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)"
                        r"\s+([0-9a-f]+)\s+\S+\s+(\S*)")
    off = None
    for line in sections.splitlines():
        m = sec_re.search(line)
        if not m:
            continue
        name, _, vma, file_off, size, flags = m.groups()
        if "A" not in flags or name.startswith(".debug"):
            continue
        if int(vma, 16) <= addr < int(vma, 16) + int(size, 16):
            off = int(file_off, 16) + (addr - int(vma, 16))
            break
    assert off is not None, "no section contains desc_configuration"
    data = ELF.read_bytes()
    total = int.from_bytes(data[off + 2:off + 4], "little")
    return data[off:off + total]


def walk(desc: bytes) -> list[tuple[int, bytes, int, int]]:
    """Yield (type, entry, interface_class, interface_subclass)."""
    entries = []
    i = 0
    cls = sub = 0
    while i < len(desc):
        length = desc[i]
        assert length >= 2, f"zero-length descriptor at offset {i}"
        t = desc[i + 1]
        e = desc[i:i + length]
        if t == INTERFACE:
            cls, sub = e[5], e[6]
        entries.append((t, e, cls, sub))
        i += length
    return entries


def test_config_descriptor_layout():
    desc = descriptor()
    entries = walk(desc)
    assert entries[0][0] == CONFIG

    # Interfaces: CDC 0/1, audio AC 2, playback AS 3 (alt 0+1), capture AS 4
    # (alt 0+1).
    ifaces = [e for t, e, _, _ in entries if t == INTERFACE]
    assert sorted(e[2] for e in ifaces) == [0, 1, 2, 3, 3, 4, 4]
    assert sorted(e[3] for e in ifaces) == [0, 0, 0, 0, 0, 1, 1]

    # One audio IAD (class 1) covering the three audio interfaces; TinyUSB's
    # CDC macro emits its own IAD (class 2).
    audio_iads = [e for t, e, _, _ in entries if t == IAD and e[4] == 0x01]
    assert len(audio_iads) == 1
    assert audio_iads[0][2] == 2 and audio_iads[0][3] == 3

    # Endpoints: CDC notif/out/in + audio out/in/int.
    eps = sorted(e[2] for t, e, _, _ in entries if t == ENDPOINT)
    assert eps == [0x02, 0x03, 0x81, 0x82, 0x83, 0x84]

    # The CS_AC header's wTotalLength must equal the header plus every AC
    # entity (clock source, terminals, feature unit).
    ac = [(t, e) for t, e, cls, sub in entries if t == CS_INTERFACE and sub == 1]
    cs_ac = [e for t, e in ac if e[2] == CS_AC_HEADER]
    assert len(cs_ac) == 1
    total = int.from_bytes(cs_ac[0][6:8], "little")
    entities = sum(len(e) for t, e in ac
                   if e[2] in (CS_AC_INPUT_TERM, CS_AC_OUTPUT_TERM,
                               CS_AC_FEATURE_UNIT, CS_AC_CLOCK_SOURCE))
    assert total == len(cs_ac[0]) + entities

    # Each streaming interface's alt-1 setting carries one isochronous EP.
    alt1 = [e for t, e, _, _ in entries if t == INTERFACE and e[3] == 1]
    assert [e[4] for e in alt1] == [1, 1]
