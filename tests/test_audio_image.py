import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
VECTORS = FW / "build" / "fb200-app.vectors.bin"
BLOB = FW / "build" / "fb200-app.blob.bin"

pytestmark = pytest.mark.skipif(
    any(shutil.which(t) is None for t in ("arm-none-eabi-gcc", "make", "curl", "git", "python3")),
    reason="toolchain not installed",
)


def build() -> tuple[bytes, bytes]:
    subprocess.run(["make", "clean", "build", "layout"], cwd=FW, check=True)
    return VECTORS.read_bytes(), BLOB.read_bytes()


def symbols() -> dict[str, int]:
    text = (FW / "build" / "layout.txt").read_text()
    return {n: int(a, 16) for a, n in re.findall(r"^([0-9a-f]{8}) \S+ (\S+)$", text, re.MULTILINE)}


def test_vectors_and_entry():
    vectors, blob = build()
    assert len(vectors) == 0x400
    assert int.from_bytes(vectors[0:4], "little") == 0x20058000
    assert int.from_bytes(vectors[4:8], "little") == 0x600104D9
    syms = symbols()
    assert syms["stage2"] == 0x4D6
    assert len(blob) == syms["__blob_end__"] - 0x400
    assert len(blob) <= 0x1DBC8          # vendor entry 0 payload limit


def test_bss_uses_the_stock_memset_region():
    build()
    syms = symbols()
    assert 0x20018B44 <= syms["__bss_start__"] <= 0x2004E3E8   # .bss may be aligned up
    assert syms["__bss_end__"] <= 0x2004E3E8
    assert syms["_estack"] == 0x20058000


def test_no_undefined_symbols():
    build()
    undef = subprocess.run(
        ["arm-none-eabi-nm", "-u", str(FW / "build" / "fb200-app.elf")],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    assert undef == ""
