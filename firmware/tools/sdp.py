#!/usr/bin/env python3
"""Minimal i.MX RT1062 ROM serial downloader (SDP) client over USB HID.

The open firmware enters the ROM downloader with the console command `rom`
(and on any fault). The downloader lives in mask ROM, so this path works
whatever is in flash and needs neither the vendor bootloader nor A+D.

Usage:
  sdp.py status          HAB mode and error status
  sdp.py reset           system reset -> normal boot (vendor bootloader -> app)
"""

from __future__ import annotations

import struct
import sys
import time

SDP_VID, SDP_PID = 0x1FC9, 0x0135

CMD_WRITE_REGISTER = 0x0202
CMD_ERROR_STATUS = 0x0505
HAB_OPEN, HAB_CLOSED = 0x56787856, 0x12343412
WRITE_COMPLETE = 0x128A8A12
AIRCR, AIRCR_SYSRESETREQ = 0xE000ED0C, 0x05FA0004


def open_sdp(timeout_s: float = 10.0):
    import hid

    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if hid.enumerate(SDP_VID, SDP_PID):
            dev = hid.device()
            dev.open(SDP_VID, SDP_PID)
            return dev
        time.sleep(0.2)
    raise SystemExit(f"no ROM serial downloader ({SDP_VID:04x}:{SDP_PID:04x}) on USB")


def command(dev, cmd: int, addr: int = 0, fmt: int = 0, count: int = 0, data: int = 0) -> None:
    report = bytes([1]) + struct.pack(">HIBIIB", cmd, addr, fmt, count, data, 0)
    dev.write(report)


def read_u32(dev, report_id: int, timeout_ms: int = 1000) -> int | None:
    pkt = dev.read(65, timeout_ms)
    if not pkt:
        return None
    if pkt[0] != report_id:
        raise SystemExit(f"unexpected HID report {pkt[0]} (wanted {report_id}): {bytes(pkt[:8]).hex()}")
    return struct.unpack("<I", bytes(pkt[1:5]))[0]


def hab_name(v: int | None) -> str:
    return {HAB_OPEN: "open", HAB_CLOSED: "closed"}.get(v, f"{v!r:}" if v is None else f"{v:08x}")


def main() -> int:
    op = sys.argv[1] if len(sys.argv) > 1 else "status"
    dev = open_sdp()
    if op == "status":
        command(dev, CMD_ERROR_STATUS)
        hab = read_u32(dev, 3)
        status = read_u32(dev, 4)
        print(f"ROM serial downloader: HAB {hab_name(hab)}, status {status:#010x}")
    elif op == "reset":
        command(dev, CMD_WRITE_REGISTER, AIRCR, 0x20, 4, AIRCR_SYSRESETREQ)
        hab = read_u32(dev, 3)
        print(f"reset requested (HAB {hab_name(hab)}); the pedal boots normally")
    else:
        raise SystemExit(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main())
