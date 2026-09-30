# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""tools/soak.py dry run against the fake console and HID pedal of
test_mcp_server.py (no hardware): a clean run passes, a counter jump, a crumbs
change or a low stack fails, and the original preset comes back."""

from __future__ import annotations

import importlib.util
import struct
from pathlib import Path

import pytest
from test_mcp_server import FakeAudio, FakeConsole, FakeHid

from fb200.mcp_server import Pedal, PedalTools
from fb200.pedal import PRESET_SIZE, FB200Device

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("soak", ROOT / "tools" / "soak.py")
soak = importlib.util.module_from_spec(spec)
spec.loader.exec_module(soak)


class SoakConsole(FakeConsole):
    """FakeConsole plus the counters soak.py reads (console.c formats)."""

    def __init__(self) -> None:
        super().__init__()
        self.sai_ovf = 0
        self.crumbs = "00000000 00000000 00000000 00000000"
        self.stack_free = 7000
        self.has_stack = True

    def run(self, cmd: str, max_s: float = 5.0) -> str:
        w = cmd.split()
        if w[0] == "stats":
            self.sent.append(cmd)
            return ("bss_writable=1 heartbeat=0 line_len=0\n"
                    "engine: gain=0 dB mute=0 drops=3 inserts=2 dma_errs=0 skips=0 resets=0\n"
                    "meters: peak L=0 R=0 (x1000)")
        if w[0] == "sai":
            self.sent.append(cmd)
            return (f"sai: rx_fill=8 tx_fill=8 rx_blocks=100 tx_blocks=100 "
                    f"ovf={self.sai_ovf} unf=0")
        if w[0] == "crumbs":
            self.sent.append(cmd)
            return f"crumbs {self.crumbs} srsr 00000001"
        if w[0] == "stack" and self.has_stack:
            self.sent.append(cmd)
            return f"stack: used {8192 - self.stack_free} of 8192 bytes, free {self.stack_free}"
        return super().run(cmd, max_s)


class SoakHid(FakeHid):
    """FakeHid plus 0x98 select, 0x97 [FF] edit-buffer load and 0x65 IR list."""

    def __init__(self) -> None:
        super().__init__()
        self.index = 3
        self.selected: list[int] = []

    def write_report(self, report: bytes) -> None:
        before = len(self.frames)
        super().write_report(report)
        for packet in self.frames[before:]:
            fn, data = packet[0], packet[1:]
            if fn == 0x98:
                self.index = data[0]
                self.selected.append(data[0])
                self.edit[:] = bytes(PRESET_SIZE)       # "reloaded from flash"
            elif fn == 0x97 and data[0] == 0xFF:
                self.edit[:] = data[1:1 + PRESET_SIZE]
            elif fn == 0x65:
                self.reply(0x66, bytes(9 * 59))

    def reply(self, fn: int, data: bytes) -> None:
        if fn == 0xA1:
            data = bytes([self.index]) + data[1:]
        super().reply(fn, data)


def make(con=None, hid=None):
    con = con or SoakConsole()
    hid = hid or SoakHid()
    tools = PedalTools(Pedal(console_factory=lambda: con,
                             device_factory=lambda: FB200Device(hid)))
    tools._audio = FakeAudio()
    tools.settle_s = 0
    return tools, con, hid


def args(tmp_path, *extra):
    return soak.parse_args(["--cycles", "4", "--interval", "0", "--audio-every", "2",
                            "--csv", str(tmp_path / "soak.csv"), *extra])


def test_clean_run_passes_and_restores(tmp_path):
    pytest.importorskip("numpy")
    tools, con, hid = make()
    struct.pack_into("<H", hid.edit, 0x30, 77)          # an unsaved edit to bring back
    original = bytes(hid.edit)
    logs: list[str] = []
    assert soak.run(tools, args(tmp_path), logs.append, audio=True) == 0, logs
    rows = (tmp_path / "soak.csv").read_text().splitlines()
    assert rows[0].startswith("time_s,cycle,preset") and len(rows) == 5
    assert all(r.endswith(",") for r in rows[1:])        # empty faults column
    assert "stack" in con.sent and "tin sine 1000" in con.sent
    assert hid.selected[-1] == 3 and bytes(hid.edit) == original
    assert any(f[0] in range(0x80, 0x87) for f in hid.frames)
    assert "PASS" in logs[-1]


def test_sai_overrun_fails(tmp_path):
    tools, con, _ = make()
    runs = {"n": 0}
    base = con.run

    def run(cmd, max_s=5.0):
        if cmd == "sai":
            runs["n"] += 1
            con.sai_ovf = 0 if runs["n"] < 3 else 5
        return base(cmd, max_s)

    con.run = run
    logs: list[str] = []
    assert soak.run(tools, args(tmp_path, "--no-audio"), logs.append, audio=False) == 1
    assert any("sai_ovf +5" in line for line in logs)


def test_crumbs_change_and_low_stack_fail(tmp_path):
    tools, con, _ = make()
    base = con.run

    def run(cmd, max_s=5.0):
        if cmd == "crumbs" and con.sent.count("crumbs") >= 2:
            con.crumbs = "fa000003 00000400 40000000 0000a1b2"
        return base(cmd, max_s)

    con.run = run
    con.stack_free = 500
    logs: list[str] = []
    assert soak.run(tools, args(tmp_path, "--no-audio", "--no-hid"), logs.append, audio=False) == 1
    text = "\n".join(logs)
    assert "crumbs changed" in text and "stack free 500 < 1024" in text


def test_old_firmware_without_stack_is_not_a_fault(tmp_path):
    tools, con, _ = make()
    con.has_stack = False
    logs: list[str] = []
    assert soak.run(tools, args(tmp_path, "--no-audio"), logs.append, audio=False) == 0, logs
    assert con.sent.count("stack") == 1                  # asked once, then skipped


def test_silent_console_is_a_fault_not_a_hang(tmp_path):
    tools, con, _ = make()
    base = con.run
    con.run = lambda cmd, max_s=5.0: "" if cmd == "cpu" else base(cmd, max_s)
    logs: list[str] = []
    assert soak.run(tools, args(tmp_path, "--no-audio", "--no-hid"), logs.append, audio=False) == 1
    assert any("did not answer `cpu`" in line for line in logs)
