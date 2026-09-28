import re
import shutil
import subprocess
from pathlib import Path

import fwbuild
import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "hello"
VECTORS = FW / "build" / "fb200-hello.vectors.bin"
BLOB = FW / "build" / "fb200-hello.blob.bin"

pytestmark = pytest.mark.skipif(
    any(shutil.which(tool) is None for tool in ("arm-none-eabi-gcc", "make", "curl", "git", "python3")),
    reason="arm-none-eabi toolchain or network tooling not installed",
)


def build() -> tuple[bytes, bytes]:
    fwbuild.build(FW)
    return VECTORS.read_bytes(), BLOB.read_bytes()


def symbols() -> dict[str, int]:
    text = (FW / "build" / "layout.txt").read_text()
    return {
        name: int(addr, 16)
        for addr, name in re.findall(r"^([0-9a-f]{8}) \S+ (\S+)$", text, re.MULTILINE)
    }


def test_vectors_table():
    vectors, _ = build()
    assert len(vectors) == 0x400
    assert int.from_bytes(vectors[0:4], "little") == 0x20058000
    # [1] must keep the vendor flash stub so the stock boot path stays intact
    assert int.from_bytes(vectors[4:8], "little") == 0x600104D9
    # the first handlers (NMI, HardFault, ...) live in the ITCM blob
    for off in range(8, 0x40, 4):
        addr = int.from_bytes(vectors[off:off + 4], "little")
        assert 0x400 <= addr < 0x20000


def test_blob_placement():
    _, blob = build()
    syms = symbols()
    # the vendor loader jumps to ITCM 0x4d6; stage2 sits exactly there
    assert syms["stage2"] == 0x4D6
    assert len(blob) == syms["__blob_end__"] - 0x400
    assert len(blob) > 0xD6
    assert len(blob) <= 0x1E39C - 0x7D4


def test_layout_symbols():
    build()
    syms = symbols()
    assert syms["_estack"] == 0x20058000
    assert syms["app_main"] > 0x4D6
    assert syms["app_main"] < 0x20000
    assert syms["__bss_end__"] <= 0x20000
    out = subprocess.run(
        ["arm-none-eabi-nm", str(FW / "build" / "fb200-hello.elf")],
        check=True, capture_output=True, text=True,
    ).stdout
    # TinyUSB 0.21.0 made tusb_init() a macro around tusb_rhport_init().
    assert "tusb_rhport_init" in out
    undef = subprocess.run(
        ["arm-none-eabi-nm", "-u", str(FW / "build" / "fb200-hello.elf")],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    assert undef == ""


def test_usb_descriptors_present():
    _, blob = build()
    # device descriptor: bLength=0x12, bDescriptorType=1, bcdUSB=0x0200,
    # class/subclass/protocol 0xEF/0x02/0x01, VID 0xCAFE, PID 0x4001
    device_prefix = b"\x12\x01\x00\x02\xef\x02\x01\x40\xfe\xca\x01\x40"
    assert device_prefix in blob
    # configuration descriptor: bLength=9, type=2, wTotalLength=75 (0x4b),
    # 2 interfaces, IAD present
    config_prefix = b"\x09\x02\x4b\x00\x02\x01\x00\x80\x32"
    assert config_prefix in blob
    assert b"FB200 hello - fb200-tools custom firmware\r\n" in blob
