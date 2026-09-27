#!/usr/bin/env python3
"""Flash the FB200 over the open firmware's USB console (no DFU, no A+D).

Targets (docs/BOOTLOADER.md §4):
  app       FILE.slot  app slot 0x60020000 (`fwbegin`); from the app or recovery
  recovery  FILE.bin   recovery image 0x60010000 (`fwrec`); rarely needed
  block0    FILE.mr    one-time migration from the single-stage firmware,
                       whose `fwbegin` writes block 0 from 0x60010000

The vendor bootloader and the model library are never touched, so A+D at
power-on stays the last-resort recovery path.

Usage:
  usb_update.py app build/out/fb200-app.slot [--port P] [--no-reset]
"""

from __future__ import annotations

import argparse
import glob
import os
import sys
import termios
import time
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "src"))

from fb200.firmware import MrFile  # noqa: E402

LOADER_OFF, LOADER_END = 0x400, 0x784   # vendor stub + loader code, never changes
FLASH_BLOCK0 = 0x60010000


class Console:
    def __init__(self, port: str) -> None:
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = attrs[1] = attrs[3] = 0          # raw: no iflag/oflag/lflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 1
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buf = b""

    def write(self, data: bytes, timeout: float = 10.0) -> None:
        # Non-blocking: a hung pedal stops draining the CDC endpoint, and a
        # blocking write would then wedge this process in the kernel.
        deadline = time.monotonic() + timeout
        view = memoryview(data)
        while view:
            try:
                view = view[os.write(self.fd, view):]
            except BlockingIOError:
                if time.monotonic() > deadline:
                    raise SystemExit("pedal stopped reading USB (firmware hung?)")
                time.sleep(0.01)

    def expect(self, needles: list[bytes], timeout: float) -> bytes:
        """Read until a line contains one of `needles`; return that line."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.strip()
                if text:
                    print("  <", text.decode(errors="replace"))
                if any(n in text for n in needles):
                    return text
            try:
                self.buf += os.read(self.fd, 4096)
            except BlockingIOError:
                time.sleep(0.01)
        raise SystemExit(f"timeout waiting for {needles!r}")

    def command(self, cmd: str, needles: list[bytes], timeout: float = 5.0) -> bytes:
        self.write(cmd.encode() + b"\r")
        return self.expect(needles, timeout)


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/ttyACM*"))
    if not ports:
        raise SystemExit("no USB console found (is the open firmware running?)")
    return ports[0]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("target", choices=["app", "recovery", "block0"])
    ap.add_argument("image", type=Path)
    ap.add_argument("--port")
    ap.add_argument("--no-reset", action="store_true")
    ap.add_argument("--stock", type=Path, default=REPO_ROOT / "fb200-stock.mr",
                    help="stock .mr (gitignored; supplies the vendor loader bytes)")
    ap.add_argument("--force", action="store_true",
                    help="skip the flash-mapping check of the vendor loader")
    args = ap.parse_args()

    if args.target == "block0":
        data = MrFile.from_path(args.image).blocks[0].data
    else:
        data = args.image.read_bytes()
    command = "fwrec" if args.target == "recovery" else "fwbegin"
    stock = MrFile.from_path(args.stock).blocks[0].data
    crc = zlib.crc32(data) & 0xFFFFFFFF
    port = args.port or find_port()
    print(f"{args.target}: {args.image} {len(data)} bytes crc {crc:08x} -> {port}")

    con = Console(port)
    con.command("hb off", [b"heartbeat"])
    con.command("fwinfo", [b"fw:"])

    # Mapping check: the vendor loader code is identical in every image, so
    # flash at 0x60010400 must already hold the stock bytes. A mismatch means
    # block 0 does not start where we think; stop before erasing anything.
    want = zlib.crc32(stock[LOADER_OFF:LOADER_END]) & 0xFFFFFFFF
    if args.target != "app" and data[LOADER_OFF:LOADER_END] != stock[LOADER_OFF:LOADER_END]:
        raise SystemExit("image does not carry the stock vendor loader; refusing")
    line = con.command(f"crc {FLASH_BLOCK0 + LOADER_OFF:#x} {LOADER_END - LOADER_OFF}", [b" = "])
    have = int(line.rsplit(b"=", 1)[1].strip(), 16)
    if have != want and not args.force:
        raise SystemExit(f"mapping check failed: flash {have:08x} != image {want:08x}")

    # Non-destructive probe: the flash must accept write-enable before we erase.
    if b"ok" not in con.command("fwtest", [b"fw test"]):
        raise SystemExit("flash write-enable probe failed; nothing erased")

    ready = con.command(f"{command} {len(data)} {crc:#x}",
                        [b"fw ready", b"FAILED", b"bad length", b"no FCB", b"unsupported",
                         b"write-protected"], timeout=60.0)
    if b"fw ready" not in ready:
        raise SystemExit("fwbegin refused; nothing was streamed")
    t0 = time.monotonic()
    for off in range(0, len(data), 4096):
        con.write(data[off:off + 4096])
    result = con.expect([b"fw done", b"FAILED", b"aborted"], timeout=60.0)
    print(f"streamed in {time.monotonic() - t0:.1f}s")
    if not result.endswith(b"ok"):
        if args.target == "app":
            raise SystemExit("flash FAILED: app slot invalid; recovery keeps the console, retry")
        raise SystemExit("flash FAILED: recovery/block 0 invalid; recover with A+D + fb200 fw flash")

    if not args.no_reset:
        con.write(b"reset\r")
        print("reset sent; the new firmware re-enumerates in a few seconds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
