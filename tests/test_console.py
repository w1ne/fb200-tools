"""Host tests for the USB console transport and update protocol, against a
fake pedal on a pseudo-terminal."""

from __future__ import annotations

import os
import pty
import threading
import tty
import zlib

import pytest

from fb200 import console
from fb200.errors import CommunicationError

STOCK = bytes(range(256)) * 8                      # stands in for block 0
LOADER_CRC = zlib.crc32(STOCK[console.LOADER_OFF:console.LOADER_END]) & 0xFFFFFFFF


class FakePedal(threading.Thread):
    """Answers the console protocol like firmware/audio/src/debug/console.c."""

    def __init__(self, fd: int, corrupt: bool = False, known=("fwbegin", "fwrec")) -> None:
        super().__init__(daemon=True)
        self.fd, self.corrupt, self.known = fd, corrupt, known
        self.written = b""
        self.log: list[str] = []

    def say(self, text: str) -> None:
        os.write(self.fd, text.encode().replace(b"\n", b"\r\n"))

    def run(self) -> None:
        buf = b""
        while True:
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:
                return
            buf += chunk
            while b"\r" in buf:
                line, buf = buf.split(b"\r", 1)
                cmd = line.decode().strip()
                self.log.append(cmd)
                self.say(cmd + "\n")                       # echo, as the firmware does
                argv = cmd.split()
                if not argv:
                    continue
                if argv[0] == "hb":
                    self.say("heartbeat off\n")
                elif argv[0] == "fwinfo":
                    self.say("fw: fcb=ok cmd-pads=0 addr=24 status=ok sr=00\n")
                elif argv[0] == "crc":
                    self.say(f"crc {argv[1]} {argv[2]} = {LOADER_CRC:08x}\n")
                elif argv[0] == "fwtest":
                    self.say("fw test ok (sr=02)\n")
                elif argv[0] in ("fwbegin", "fwrec"):
                    if argv[0] not in self.known:
                        self.say("unknown command (try help)\n")
                        continue
                    n, crc = int(argv[1]), int(argv[2], 16)
                    self.say("fw: erasing\nfw ready\n")
                    data = buf
                    while len(data) < n:
                        data += os.read(self.fd, 65536)
                    self.written, buf = data[:n], data[n:]
                    got = zlib.crc32(self.written) & 0xFFFFFFFF
                    if self.corrupt:
                        got ^= 1
                    self.say(f"fw done crc={got:08x} {'ok' if got == crc else 'BAD'}\n")
                elif argv[0] == "stats":
                    self.say("bss_writable=1 heartbeat=0 line_len=5\n")


@pytest.fixture
def pedal(request):
    master, slave = pty.openpty()
    tty.setraw(master)
    fake = FakePedal(master, **getattr(request, "param", {}))
    fake.start()
    con = console.Console(os.ttyname(slave))
    yield con, fake
    con.close()
    os.close(slave)


def test_run_strips_echo(pedal):
    con, _ = pedal
    assert con.run("stats") == "bss_writable=1 heartbeat=0 line_len=5"


def test_update_app_streams_and_resets(pedal):
    con, fake = pedal
    data = os.urandom(47_000)
    res = console.update(con, "app", data, STOCK)
    assert fake.written == data
    assert res.crc == zlib.crc32(data) & 0xFFFFFFFF
    assert "fwtest" in fake.log and f"fwbegin {len(data)} {res.crc:#x}" in fake.log


def test_update_recovery_uses_fwrec(pedal):
    con, fake = pedal
    data = bytearray(os.urandom(4096))
    data[console.LOADER_OFF:console.LOADER_END] = STOCK[console.LOADER_OFF:console.LOADER_END]
    console.update(con, "recovery", bytes(data), STOCK, reset=False)
    assert any(c.startswith("fwrec ") for c in fake.log)
    assert fake.written == bytes(data)


def test_update_recovery_refuses_foreign_loader(pedal):
    con, fake = pedal
    with pytest.raises(CommunicationError, match="vendor loader"):
        console.update(con, "recovery", os.urandom(4096), STOCK)
    assert not any(c.startswith("fwrec") for c in fake.log)


@pytest.mark.parametrize("pedal", [{"known": ("fwrec",)}], indirect=True)
def test_update_fails_fast_on_unknown_command(pedal):
    con, fake = pedal
    with pytest.raises(CommunicationError, match="refused"):
        console.update(con, "app", b"x" * 100, STOCK)
    assert fake.written == b""


@pytest.mark.parametrize("pedal", [{"corrupt": True}], indirect=True)
def test_update_reports_crc_mismatch(pedal):
    con, _ = pedal
    with pytest.raises(CommunicationError, match="app slot invalid"):
        console.update(con, "app", b"y" * 1000, STOCK)
