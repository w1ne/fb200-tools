import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "hello"
BIN = FW / "build" / "fb200-hello.bin"

pytestmark = pytest.mark.skipif(
    shutil.which("arm-none-eabi-gcc") is None or shutil.which("make") is None,
    reason="arm-none-eabi toolchain not installed",
)


def build():
    subprocess.run(["make", "clean", "build", "layout"], cwd=FW, check=True)
    return BIN.read_bytes()


def test_image_boot_header_and_size():
    data = build()
    assert len(data) <= 200_704
    sp, reset = int.from_bytes(data[0:4], "little"), int.from_bytes(data[4:8], "little")
    assert sp == 0x20058000
    assert reset & 1
    assert 0x60010008 <= reset < 0x60010000 + len(data)


def test_layout_symbols():
    subprocess.run(["make", "layout"], cwd=FW, check=True, capture_output=True)
    text = (FW / "build" / "layout.txt").read_text()
    syms = {
        name: addr
        for addr, name in re.findall(r"^([0-9a-f]{8}) \S+ (\S+)$", text, re.MULTILINE)
    }
    assert int(syms["_estack"], 16) == 0x20058000
    assert int(syms["__itcm_start__"], 16) == 0
    assert 0x60010008 <= int(syms["__itcm_lma__"], 16) < 0x60040000
    assert int(syms["__itcm_end__"], 16) <= 0x20000
    assert int(syms["__data_end__"], 16) <= int(syms["__bss_start__"], 16)
    assert int(syms["__bss_end__"], 16) <= int(syms["_estack"], 16)
    assert int(syms["app_main"], 16) < 0x20000
    reset = int(syms["reset_stub"], 16)
    assert 0x60010000 <= reset < 0x60010000 + len(BIN.read_bytes())
    out = subprocess.run(
        ["arm-none-eabi-nm", str(FW / "build" / "fb200-hello.elf")],
        check=True, capture_output=True, text=True,
    ).stdout
    # TinyUSB 0.21.0 made tusb_init() a macro around tusb_rhport_init().
    assert "tusb_rhport_init" in out
