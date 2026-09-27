"""Parity of the web updater (web/js) with the Python protocol code.

Python builds the expected bytes: the HID reports that `updater.py` +
`protocol.py` write for small fake .mr images, the CRC-32 of a few buffers,
and the command dialogue of `console.update()`. The Node tests in web/test
read that fixture and check that the JS port produces the same bytes.
Skips when node is missing.
"""

from __future__ import annotations

import json
import os
import random
import re
import shutil
import subprocess
import zlib
from pathlib import Path

import pytest

from fb200 import console
from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader
from fb200.protocol import pack_frame
from fb200.transport import MockTransport
from fb200.updater import FirmwareUpdater, build_flash_plan

ROOT = Path(__file__).resolve().parents[1]
WEB_TEST = ROOT / "web" / "test"


def reply_report(fn: int) -> bytes:
    frame = pack_frame(fn)
    report = bytes([len(frame)]) + frame
    return report + bytes(64 - len(report))


def fake_mr(version: int, rng: random.Random) -> bytes:
    header = MrHeader(product_tag="FB200", send_cmd=0x02, rec_cmd=0x03, timeout=1000,
                      update_block=2, update_addr=b"\x03\x00\x00\x00", version=version)
    blocks = [
        # 512-byte pages, the last one short
        MrBlock(MrBlockTag(start_addr=0x60010000, send_cmd=0x04, rec_cmd=0x05, start_page=0x7F,
                           rom_id=1), rng.randbytes(1300)),
        # send_cmd >> 7: 1024-byte pages, page numbers up to 0xFFFF (big-endian on the wire)
        MrBlock(MrBlockTag(start_addr=0x60100000, send_cmd=0x86, rec_cmd=0x87,
                           start_page=0xFFFD, rom_id=2), rng.randbytes(2100)),
    ]
    return MrFile(header, blocks).to_bytes()


def vendor_case(mr_bytes: bytes) -> dict:
    mr = MrFile.from_bytes(mr_bytes)
    plan = build_flash_plan(mr)
    replies = [plan.erase_reply] + [reply for reply, _ in plan.writes]
    transport = MockTransport(reports=[reply_report(r) for r in replies])
    result = FirmwareUpdater(transport).flash(mr, dry_run=False)
    return {"mr": mr_bytes.hex(), "replies": replies,
            "reports": [r.hex() for r in transport.written],
            "write_frames": result.write_frames, "bytes_written": result.bytes_written}


class FakeConsole:
    """Answers console.update() like the firmware (and tests/test_console.py FakePedal)."""

    def __init__(self, loader_crc: int) -> None:
        self.loader_crc = loader_crc
        self.log: list[str] = []
        self.written = b""
        self._pending: int | None = None
        self._crc = 0

    def command(self, cmd: str, needles, timeout: float = 5.0) -> bytes:
        self.log.append(cmd)
        argv = cmd.split()
        if argv[0] == "hb":
            answer = "heartbeat off"
        elif argv[0] == "fwinfo":
            answer = "fw: fcb=ok cmd-pads=0 addr=24 status=ok sr=00"
        elif argv[0] == "crc":
            answer = f"crc {argv[1]} {argv[2]} = {self.loader_crc:08x}"
        elif argv[0] == "fwtest":
            answer = "fw test ok (sr=02)"
        elif argv[0] in ("fwbegin", "fwrec", "fwstock"):
            self._pending, self._crc = int(argv[1]), int(argv[2], 16)
            answer = "fw ready"
        else:
            answer = "unknown command (try help)"
        assert answer and any(n.decode() in answer for n in needles)
        return answer.encode()

    def expect(self, needles, timeout: float) -> bytes:
        got = zlib.crc32(self.written) & 0xFFFFFFFF
        return f"fw done crc={got:08x} {'ok' if got == self._crc else 'BAD'}".encode()

    def write(self, data: bytes, timeout: float = 10.0) -> None:
        if data.endswith(b"\r") and self._pending is not None and len(self.written) >= self._pending:
            self.log.append(data.decode().rstrip("\r"))
        else:
            self.written += data


def console_case(steps: list[tuple[str, bytes, bool]], loader_crc: int) -> dict:
    """Run console.update() for each (target, data, reset) on one connection."""
    con = FakeConsole(loader_crc)
    out = []
    for target, data, reset in steps:
        con.written, con._pending = b"", None
        res = console.update(con, target, data, reset=reset,  # type: ignore[arg-type]
                             loader_crc=loader_crc)
        assert con.written == data
        out.append({"target": target, "data": data.hex(), "reset": reset, "crc": res.crc})
    return {"steps": out, "loader_crc": loader_crc, "log": con.log}


def build_fixture() -> dict:
    rng = random.Random(200)
    crc_inputs = [b"", b"123456789", rng.randbytes(1), rng.randbytes(4097)]
    stock = bytes(range(256)) * 8
    loader_crc = zlib.crc32(stock[console.LOADER_OFF:console.LOADER_END]) & 0xFFFFFFFF
    rec = bytearray(rng.randbytes(4096))
    rec[console.LOADER_OFF:console.LOADER_END] = stock[console.LOADER_OFF:console.LOADER_END]
    return {
        "crc32": [{"data": d.hex(), "crc": zlib.crc32(d) & 0xFFFFFFFF} for d in crc_inputs],
        "vendor": [vendor_case(fake_mr(0, rng)), vendor_case(fake_mr(1, rng))],
        "console": [
            console_case([("app", rng.randbytes(9000), True)], loader_crc),
            console_case([("stock", rng.randbytes(100), True)], loader_crc),
            console_case([("recovery", bytes(rec), True)], loader_crc),
            # first install from recovery (web/js/app.js): sound data, then the app, one reset
            console_case([("stock", rng.randbytes(700), False), ("app", rng.randbytes(5000), True)],
                         loader_crc),
        ],
    }


def test_fixture_matches_the_python_protocol():
    fx = build_fixture()
    case = fx["vendor"][0]
    # erase + 3 pages of block 0 + 3 pages of block 1, then the exit frame
    assert case["write_frames"] == 6 and case["bytes_written"] == 3400
    assert fx["console"][0]["log"][:4] == ["hb off", "fwinfo", "crc 0x60010400 900", "fwtest"]
    install = fx["console"][3]["log"]
    assert [c.split()[0] for c in install if c.startswith("fw")] == \
        ["fwinfo", "fwtest", "fwstock", "fwinfo", "fwtest", "fwbegin"]
    assert install[-1] == "reset" and install.count("reset") == 1


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_web_js_matches_python(tmp_path):
    fixture = tmp_path / "parity.json"
    fixture.write_text(json.dumps(build_fixture()))
    tests = sorted(str(p) for p in WEB_TEST.glob("*.test.mjs"))
    assert tests, "no web/test/*.test.mjs found"
    proc = subprocess.run(["node", "--test", *tests], capture_output=True, text=True, check=False,
                          env={**os.environ, "FB200_PARITY_FIXTURE": str(fixture)}, timeout=120)
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0, out
    # A fully skipped suite also exits 0: require real passes and no skips.
    passed = re.search(r"^# pass (\d+)", out, re.MULTILINE)
    skipped = re.search(r"^# skipped (\d+)", out, re.MULTILINE)
    assert passed and int(passed.group(1)) >= 10, out
    assert skipped and int(skipped.group(1)) == 0, out
