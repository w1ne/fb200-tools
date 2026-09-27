"""USB CDC console of the open FB200 firmware (docs/BOOTLOADER.md §4).

One transport for every host tool: `fb200 console`, `fb200 update`, and
firmware/tools/usb_update.py. The port is opened non-blocking: a hung pedal
stops draining its CDC endpoint, and a blocking write would wedge the host
process in the kernel.
"""

from __future__ import annotations

import glob
import os
import sys
import time
import zlib
from dataclasses import dataclass
from typing import Self

from fb200.errors import CommunicationError

LOADER_OFF, LOADER_END = 0x400, 0x784   # vendor stub + loader code in block 0
FLASH_BLOCK0 = 0x60010000


def find_port() -> str:
    """The open firmware's CDC port (serial AUDIO_*), else any usbmodem."""
    ports = sorted(glob.glob("/dev/cu.usbmodemAUDIO*")) or \
        sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/ttyACM*"))
    if not ports:
        raise CommunicationError("no FB200 console on USB (is the open firmware running?)")
    return ports[0]


class Console:
    def __init__(self, port: str | None = None, echo=None) -> None:
        import termios

        self.port = port or find_port()
        self.fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = attrs[1] = attrs[3] = 0          # raw: no iflag/oflag/lflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buf = b""
        self.echo = echo                            # callable(str) per line, or None

    def close(self) -> None:
        os.close(self.fd)

    def __enter__(self) -> Self:
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def write(self, data: bytes, timeout: float = 10.0) -> None:
        deadline = time.monotonic() + timeout
        view = memoryview(data)
        while view:
            try:
                view = view[os.write(self.fd, view):]
            except BlockingIOError:
                if time.monotonic() > deadline:
                    raise CommunicationError("pedal stopped reading USB (firmware hung?)") from None
                time.sleep(0.01)

    def read_some(self) -> bytes:
        try:
            return os.read(self.fd, 65536)
        except (BlockingIOError, OSError):
            return b""

    def drain(self, quiet_s: float = 0.3, max_s: float = 3.0) -> bytes:
        """Read until the device has been quiet for quiet_s."""
        out = b""
        end = time.monotonic() + max_s
        last = time.monotonic()
        while time.monotonic() < end and time.monotonic() - last < quiet_s:
            chunk = self.read_some()
            if chunk:
                out += chunk
                last = time.monotonic()
            else:
                time.sleep(0.01)
        return out

    def run(self, cmd: str, quiet_s: float = 0.3, max_s: float = 5.0) -> str:
        """Send one command; return its output without the echoed command line."""
        self.buf += self.drain(0.05, 0.2)
        self.buf = b""
        self.write(cmd.encode() + b"\r")
        text = self.drain(quiet_s, max_s).decode(errors="replace")
        lines = text.replace("\r", "").split("\n")
        if lines and lines[0].strip() == cmd.strip():
            lines = lines[1:]
        return "\n".join(lines).strip("\n")

    def expect(self, needles: list[bytes], timeout: float) -> bytes:
        """Read until a line contains one of `needles`; return that line."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.strip()
                if text and self.echo:
                    self.echo(text.decode(errors="replace"))
                if any(n in text for n in needles):
                    return text
            chunk = self.read_some()
            if chunk:
                self.buf += chunk
            else:
                time.sleep(0.01)
        raise CommunicationError(f"timeout waiting for {needles!r}")

    def command(self, cmd: str, needles: list[bytes], timeout: float = 5.0) -> bytes:
        self.write(cmd.encode() + b"\r")
        return self.expect(needles, timeout)


@dataclass
class UpdateResult:
    target: str
    size: int
    crc: int
    seconds: float


def update(con: Console, target: str, data: bytes, stock_block0: bytes,
           reset: bool = True, force: bool = False) -> UpdateResult:
    """Write the app slot (`fwbegin`), recovery (`fwrec`) or, for the one-time
    migration from single-stage firmware, all of block 0 (`fwbegin`)."""
    if target not in ("app", "recovery", "block0"):
        raise ValueError(target)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    command = "fwrec" if target == "recovery" else "fwbegin"
    con.command("hb off", [b"heartbeat"])
    con.command("fwinfo", [b"fw:"])

    # Mapping check: flash at 0x60010400 must hold the stock vendor loader.
    want = zlib.crc32(stock_block0[LOADER_OFF:LOADER_END]) & 0xFFFFFFFF
    if target != "app" and data[LOADER_OFF:LOADER_END] != stock_block0[LOADER_OFF:LOADER_END]:
        raise CommunicationError("image does not carry the stock vendor loader; refusing")
    line = con.command(f"crc {FLASH_BLOCK0 + LOADER_OFF:#x} {LOADER_END - LOADER_OFF}", [b" = "])
    have = int(line.rsplit(b"=", 1)[1].strip(), 16)
    if have != want and not force:
        raise CommunicationError(f"mapping check failed: flash {have:08x} != stock {want:08x}")

    if b"ok" not in con.command("fwtest", [b"fw test"]):
        raise CommunicationError("flash write-enable probe failed; nothing erased")
    ready = con.command(f"{command} {len(data)} {crc:#x}",
                        [b"fw ready", b"FAILED", b"bad length", b"no FCB", b"unsupported",
                         b"write-protected", b"unknown command"], timeout=60.0)
    if b"fw ready" not in ready:
        raise CommunicationError(f"{command} refused ({ready.decode(errors='replace')}); "
                                 "nothing was streamed")
    t0 = time.monotonic()
    for off in range(0, len(data), 4096):
        con.write(data[off:off + 4096])
    result = con.expect([b"fw done", b"FAILED", b"aborted"], timeout=60.0)
    seconds = time.monotonic() - t0
    if not result.endswith(b"ok"):
        if target == "app":
            raise CommunicationError("flash FAILED: app slot invalid; recovery keeps the console, retry")
        raise CommunicationError("flash FAILED: recovery/block 0 invalid; recover with A+D")
    if reset:
        con.write(b"reset\r")
    return UpdateResult(target, len(data), crc, seconds)


def interactive(con: Console) -> int:
    """Line-based terminal: stdin lines go to the pedal, output streams back."""
    import select

    print(f"connected to {con.port} (Ctrl-D to quit)", file=sys.stderr)
    con.write(b"\r")
    while True:
        ready, _, _ = select.select([sys.stdin, con.fd], [], [], 0.1)
        if con.fd in ready:
            chunk = con.read_some()
            if chunk:
                sys.stdout.write(chunk.decode(errors="replace").replace("\r", ""))
                sys.stdout.flush()
        if sys.stdin in ready:
            line = sys.stdin.readline()
            if not line:
                return 0
            con.write(line.rstrip("\n").encode() + b"\r")
