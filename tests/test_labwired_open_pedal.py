"""The open pedal gate names the panel checks and 16 raw knob counts.

The counts are the divider formula in labwired/open-boot.yaml. The test
fails when those literals are the mid-scale reading or the turned knob's
start count. It also checks that the image tool resolves the `knobs`
symbol into the file `labwired test` runs.
"""

import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from labwired_open_fw import elf_symbol_bytes, place_gate, render_gate

from fb200.errors import FirmwareError

ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "labwired" / "open-boot.yaml"

# Stock inversion of these same positions is the vendor gate's table.
# 4095 - raw(83 %) = 0x2B8 and 4095 - raw(20 %) = 0xCCC.
MID_SCALE = 2048
TURNED_KNOB = 15
TURN_PERCENT = 20


def raw_count(percent: int) -> int:
    """ADC count for a knob at `percent` of a 3.3 V divider.

    mV = trunc(3300 * P / 100). The i.MX RT model rounds:
    (mV * 4095 + 1650) / 3300. The open firmware stores this count.
    """
    millivolts = (3300 * percent) // 100
    return (millivolts * 4095 + 1650) // 3300


STARTS = [raw_count(8 + 5 * index) for index in range(16)]


def _slots(text: str) -> list[tuple[int, int]]:
    import re

    pattern = re.compile(
        r"symbol:\s*knobs,\s*offset:\s*(0x[0-9a-fA-F]+|\d+),"
        r"\s*size:\s*2,\s*expected_value:\s*(0x[0-9a-fA-F]+|\d+)"
    )
    return [(int(offset, 0), int(expected, 0)) for offset, expected in pattern.findall(text)]


def test_divider_formula_matches_the_known_endpoints():
    assert raw_count(0) == 0
    assert raw_count(50) == MID_SCALE
    assert raw_count(100) == 4095
    assert raw_count(83) == 0xD47
    assert raw_count(TURN_PERCENT) == 0x333
    assert 4095 - raw_count(83) == 0x2B8
    assert 4095 - raw_count(TURN_PERCENT) == 0xCCC
    assert len(set(STARTS)) == 16
    assert MID_SCALE not in STARTS


def test_open_gate_expects_the_sixteen_start_counts_and_the_turn():
    text = GATE.read_text()
    slots = _slots(text)
    assert [offset for offset, _value in slots] == [2 * index for index in range(16)]
    values = [value for _offset, value in slots]
    assert values[:TURNED_KNOB] == STARTS[:TURNED_KNOB]
    assert values[TURNED_KNOB] == raw_count(TURN_PERCENT)
    assert values[TURNED_KNOB] != STARTS[TURNED_KNOB]
    assert all(value != MID_SCALE for value in values)
    assert 'component: "knob_k15_master"' in text
    assert "value: 20" in text
    assert '\\"FB200 Audio\\"' in text
    assert "fidelity_clean: true" in text
    assert 'schema_version: "1.2"' in text
    assert "max_cycles: 250000000" not in text
    assert "max_cycles: 3400000000" not in text
    assert "wall_time_ms: 900000" not in text
    assert "peripheral_tick_interval: 16" in text
    assert "max_cycles: 660000000" in text
    assert "max_steps: 1000000000" in text
    assert "max_cycles: 115000000" not in text
    assert 'component: "footswitch_b"' in text
    assert '\\"0Ut\\"' in text
    assert '\\"P0b\\"' in text
    assert "write 0x001c = 0x0002" in text
    assert "dai slave i2s 16-bit" in text
    assert "adcout driven" in text
    assert "enable dac_l dac_r adc_l adc_r" in text
    assert "AT+TM -> TM+BT201-BLE" in text
    assert "AT+CN00" in text
    assert "AT+B501" in text
    assert "AT+B401" in text
    assert "amp 8000 hz 1000" in text
    assert "peak 256 tail 94" in text
    assert "TS+01 edr connected" in text
    assert "TL+03 ble connected" in text
    assert "phone->mcu aa 55 01 00 00 c8 cf" in text
    assert "56 32 2e 30 2e 30" in text
    assert 'component: "sai1"' in text
    assert 'component: "bt"' in text


def test_render_gate_writes_the_symbol_address_and_keeps_the_counts():
    text = GATE.read_text()
    base = 0x20001000
    rendered = render_gate(text, base)
    assert "symbol:" not in rendered
    for index, count in enumerate(STARTS):
        if index == TURNED_KNOB:
            count = raw_count(TURN_PERCENT)
        address = base + 2 * index
        assert f"address: {address:#x}, size: 2, expected_value: {count:#x}" in rendered
    placed = place_gate(text, base)
    assert 'firmware: "open.elf"' in placed
    assert 'system: "../../labwired/system.yaml"' in placed
    assert "peripheral_tick_interval: 16" in rendered
    assert "peripheral_tick_interval: 16" in placed
    assert '\\"P0b\\"' in rendered
    assert "AT+B401" in rendered
    assert "peak 256 tail 94" in rendered
    assert "56 32 2e 30 2e 30" in rendered
    assert "max_cycles: 660000000" in rendered


def test_render_gate_rejects_a_missing_slot():
    with pytest.raises(FirmwareError, match="16 times"):
        render_gate("symbol: knobs, offset: 0,\n", 0x20000000)


def _elf_with_knobs(address: int) -> bytes:
    """ELF32 LE whose symbol table has one absolute `knobs` object."""
    # Layout: header, two symbols, strtab, shstrtab, four section headers.
    symtab = struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0)
    symtab += struct.pack("<IIIBBH", 1, address, 32, 0x11, 0, 0xFFF1)
    strtab = b"\0knobs\0"
    shstrtab = b"\0.symtab\0.strtab\0.shstrtab\0"
    parts = [symtab, strtab, shstrtab]
    # Filled after we know where the section headers sit.
    body = b"".join(parts)
    shoff = 52 + len(body)
    sym_off, str_off, shstr_off = 52, 52 + len(symtab), 52 + len(symtab) + len(strtab)

    def shdr(name: int, typ: int, off: int, size: int, link: int, info: int, entsize: int) -> bytes:
        return struct.pack("<10I", name, typ, 0, 0, off, size, link, info, 4, entsize)

    headers = b"".join(
        [
            shdr(0, 0, 0, 0, 0, 0, 0),
            shdr(1, 2, sym_off, len(symtab), 2, 1, 16),
            shdr(9, 3, str_off, len(strtab), 0, 0, 0),
            shdr(17, 3, shstr_off, len(shstrtab), 0, 0, 0),
        ]
    )
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    ehdr = struct.pack(
        "<16sHHIIIIIHHHHHH",
        ident,
        2,
        40,
        1,
        0,
        52,
        shoff,
        0,
        52,
        32,
        0,
        40,
        4,
        3,
    )
    return ehdr + body + headers


def test_elf_symbol_reads_a_local_knobs_address():
    address = 0x20038F48
    assert elf_symbol_bytes(_elf_with_knobs(address), "knobs") == address
    assert elf_symbol_bytes(_elf_with_knobs(address), "other") is None
