# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Host client of the long IR store (fb200.longir, `fb200 ir put/ls/delete`,
the MCP long_ir_* tools) against a fake pedal on a pseudo-terminal whose
irput / irls / irdel run the firmware's store (irstore.c in the C harness of
tests/test_irstore_host.py) on a fake flash."""

from __future__ import annotations

import os
import shutil
import struct
import threading
import zlib

import pytest

pty = pytest.importorskip("pty")
tty = pytest.importorskip("tty")
pytest.importorskip("termios")

from test_irstore_host import Harness, build

from fb200 import cli, console, longir
from fb200.errors import CommunicationError, InvalidArgumentError
from fb200.mcp_server import Pedal, PedalTools
from fb200.wav import write_wav

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="host C compiler not installed")


class StorePedal(threading.Thread):
    """The console of firmware/audio/src/debug/console.c for irput/irls/irdel
    (argument parsing as cmd_irput), backed by the real irstore.c."""

    def __init__(self, fd: int, h: Harness) -> None:
        super().__init__(daemon=True)
        self.fd, self.h = fd, h
        self.log: list[str] = []
        self.received = b""
        self.known = True

    def say(self, text: str) -> None:
        os.write(self.fd, text.encode().replace(b"\n", b"\r\n"))

    def forward(self, out: list[str]) -> None:
        for line in out:
            if not line.startswith(("put ", "active ")):
                self.say(line.rstrip("\r") + "\n")

    def run(self) -> None:
        buf = b""
        while True:
            try:
                buf += os.read(self.fd, 65536)
            except OSError:
                return
            while b"\r" in buf:
                line, buf = buf.split(b"\r", 1)
                cmd = line.decode().strip()
                self.log.append(cmd)
                self.say(cmd + "\n")                        # echo
                argv = cmd.split()
                if not argv:
                    continue
                if not self.known:
                    self.say("unknown command (try help)\n")
                elif argv[0] == "irput":
                    if len(argv) < 5:
                        self.say("ir: usage: irput <slot 20-83> ...\n")
                        continue
                    out = self.h.cmd("put " + " ".join(argv[1:6]))
                    self.forward(out)
                    if out[-1] != "put 0":
                        continue
                    n = int(argv[2]) * 4
                    while len(buf) < n:
                        buf += os.read(self.fd, 65536)
                    self.received, buf = buf[:n], buf[n:]
                    self.h.rx(self.received)
                    self.forward(self.h.cmd("task 20"))
                elif argv[0] == "irls":
                    self.forward(self.h.cmd("irls"))
                elif argv[0] == "irdel":
                    if len(argv) < 2 or not argv[1].isdigit() or not 20 <= int(argv[1]) <= 83:
                        self.say("ir: usage: irdel <slot 20-83>\n")
                    else:
                        self.forward(self.h.cmd(f"irdel {argv[1]}"))
                else:
                    self.say("unknown command (try help)\n")


@pytest.fixture
def rig(tmp_path):
    h = Harness(build(), tmp_path)
    master, slave = pty.openpty()
    tty.setraw(master)
    fake = StorePedal(master, h)
    fake.start()
    port = os.ttyname(slave)
    yield port, fake, h
    os.close(slave)
    h.close()


def taps(n: int) -> list[float]:
    return [struct.unpack("<f", struct.pack("<f", 0.9 * 0.998 ** i * (-1) ** i))[0]
            for i in range(n)]


def test_put_ls_delete(rig):
    port, fake, h = rig
    x = taps(4096)
    with console.Console(port) as con:
        res = longir.put(con, 20, x, "My Cab (SM57)", 48000)
        assert (res.slot, res.taps, res.name, res.gain) == (20, 4096, "My_Cab_(SM57)", 0.75)
        assert fake.received == longir.pack_samples(x)
        assert res.crc == zlib.crc32(fake.received)
        listed = longir.ls(con)
        assert listed["available"] and [(e.slot, e.taps, e.rate, e.ok, e.name)
                                        for e in listed["slots"]] == \
            [(20, 4096, 48000, True, "My_Cab_(SM57)")]
        assert longir.delete(con, 20) is True
        assert longir.delete(con, 20) is False
        assert longir.ls(con)["slots"] == []
    assert h.load(0)[0] == 0


def test_put_refused_and_bad_slot(rig):
    port, _, h = rig
    h.cmd("cap 4194304")                                   # a 4 MB chip
    with console.Console(port) as con:
        with pytest.raises(CommunicationError, match="store not available"):
            longir.put(con, 20, taps(10), "x")
        assert longir.ls(con)["available"] is False
        with pytest.raises(InvalidArgumentError):
            longir.put(con, 19, taps(10), "x")
        with pytest.raises(InvalidArgumentError):
            longir.delete(con, 84)


def test_old_firmware_says_update(rig):
    port, fake, _ = rig
    fake.known = False                    # firmware before the long IR store
    with console.Console(port) as con:
        for call in (lambda: longir.ls(con), lambda: longir.delete(con, 20),
                     lambda: longir.put(con, 20, taps(4), "x")):
            with pytest.raises(CommunicationError, match="update it"):
                call()


def test_cli_put_ls_delete(rig, tmp_path, capsys):
    port, fake, _ = rig
    wav = tmp_path / "Big Cab.wav"
    write_wav(wav, [(v,) for v in taps(3000)] + [(0.0,)] * 500, 48000)
    assert cli.main(["ir", "put", "21", str(wav), "--port", port]) == 0
    err = capsys.readouterr().err
    assert "stored 'Big_Cab' in long slot 21" in err and "select it with cab type 21" in err
    # default 4096 taps, then the trailing zeros dropped: the resampled 3000+500 frames
    got = len(fake.received) // 4
    assert 2600 < got < 4096 and struct.unpack_from("<f", fake.received, 4 * (got - 1))[0] != 0.0
    assert cli.main(["ir", "ls", "--long", "--port", port]) == 0
    out = capsys.readouterr().out
    assert out.startswith(f"21: Big_Cab  {got} taps  gain 0.7500  source 48000 Hz")
    assert cli.main(["ir", "put", "22", str(wav), "--taps", "256", "--name", "short one",
                     "--port", port]) == 0
    # --taps 256 fades out to an exact 0 at the last tap, which is dropped
    assert len(fake.received) == 255 * 4 and "irput 22 255 " in fake.log[-1]
    assert fake.log[-1].endswith(" short_one 48000")
    assert cli.main(["ir", "delete", "21", "--port", port]) == 0
    assert "deleted long slot 21" in capsys.readouterr().out


def test_cli_slot_ranges(capsys):
    with pytest.raises(SystemExit):
        cli.main(["ir", "put", "9", "x.wav"])
    with pytest.raises(SystemExit):
        cli.main(["ir", "delete", "15"])
    assert "20..83" in capsys.readouterr().err


def test_mcp_tools(rig, tmp_path):
    port, _, _ = rig
    wav = tmp_path / "cab.wav"
    write_wav(wav, [(v,) for v in taps(5000)], 44100)
    tools = PedalTools(Pedal(port))
    try:
        res = tools.long_ir_import(83, str(wav), name="mcp cab")
        # 4096 with the fade-out, whose last tap is an exact 0: dropped
        assert res == {"slot": 83, "name": "mcp_cab", "taps": 4095, "gain": 0.75, "rate": 44100}
        listed = tools.long_ir_list()
        assert listed["available"] and listed["slots"] == [
            {"slot": 83, "name": "mcp_cab", "taps": 4095, "gain": 0.75, "rate": 44100, "ok": True}]
        assert tools.long_ir_delete(83) == {"slot": 83, "deleted": True}
        assert tools.long_ir_list()["slots"] == []
        with pytest.raises(InvalidArgumentError):
            tools.long_ir_import(19, str(wav))
        with pytest.raises(InvalidArgumentError, match="long_ir_import"):
            tools.console("irput 20 1 0x0 x")
    finally:
        tools.pedal.close()
