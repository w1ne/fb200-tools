# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""FB200 USB HID wire protocol: framing, CRC16, report chunking."""

from __future__ import annotations

from collections.abc import Iterator
from typing import Protocol

VID = 0x34DB
PID_APP = 0x800F
UPDATE_VID = 0x0483
UPDATE_PID = 0x5703

REPORT_SIZE = 64
CHUNK_SIZE = 63

CMD_GET_VERSION = 0x00
CMD_UPLOAD_IR = 0x61
CMD_QUERY_IR = 0x63
CMD_DELETE_IR = 0x67
CMD_MODULE = 0x80          # + module index 0..6, no reply
CMD_CONNECT = 0x94         # state dump; the first reply is REPLY_EDIT
CMD_READ_PRESET = 0x96     # [index] -> REPLY_PRESET
CMD_RENAME_PRESET = 0x99   # [index][name 20]: select, rename, store
CMD_SETTINGS = 0xB0        # [13 B] global settings (also in the connect dump)
CMD_JUMP_BOOTLOADER = 0xC1
CMD_EXIT_BOOTLOADER = 0xFF

REPLY_VERSION = 0x01
REPLY_UPLOAD_IR = 0x62
REPLY_QUERY_IR = 0x64
REPLY_DELETE_IR = 0x68
REPLY_EDIT = 0xA1          # [preset index][256-byte edit buffer]
REPLY_PRESET = 0x97        # [index][256-byte preset from flash]
REPLY_SETTINGS = 0xB0

CRC_TABLE = [
    0, 4129, 8258, 12387, 16516, 20645, 24774, 28903, 33032, 37161, 41290, 45419, 49548, 53677,
    57806, 61935, 4657, 528, 12915, 8786, 21173, 17044, 29431, 25302, 37689, 33560, 45947, 41818,
    54205, 50076, 62463, 58334, 9314, 13379, 1056, 5121, 25830, 29895, 17572, 21637, 42346, 46411,
    34088, 38153, 58862, 62927, 50604, 54669, 13907, 9842, 5649, 1584, 30423, 26358, 22165, 18100,
    46939, 42874, 38681, 34616, 63455, 59390, 55197, 51132, 18628, 22757, 26758, 30887, 2112,
    6241, 10242, 14371, 51660, 55789, 59790, 63919, 35144, 39273, 43274, 47403, 23285, 19156,
    31415, 27286, 6769, 2640, 14899, 10770, 56317, 52188, 64447, 60318, 39801, 35672, 47931,
    43802, 27814, 31879, 19684, 23749, 11298, 15363, 3168, 7233, 60846, 64911, 52716, 56781, 44330,
    48395, 36200, 40265, 32407, 28342, 24277, 20212, 15891, 11826, 7761, 3696, 65439, 61374, 57309,
    53244, 48923, 44858, 40793, 36728, 37256, 33193, 45514, 41451, 53516, 49453, 61774, 57711,
    4224, 161, 12482, 8419, 20484, 16421, 28742, 24679, 33721, 37784, 41979, 46042, 49981, 54044,
    58239, 62302, 689, 4752, 8947, 13010, 16949, 21012, 25207, 29270, 46570, 42443, 38312, 34185,
    62830, 58703, 54572, 50445, 13538, 9411, 5280, 1153, 29798, 25671, 21540, 17413, 42971, 47098,
    34713, 38840, 59231, 63358, 50973, 55100, 9939, 14066, 1681, 5808, 26199, 30326, 17941, 22068,
    55628, 51565, 63758, 59695, 39368, 35305, 47498, 43435, 22596, 18533, 30726, 26663, 6336,
    2273, 14466, 10403, 52093, 56156, 60223, 64286, 35833, 39896, 43963, 48026, 19061, 23124,
    27191, 31254, 2801, 6864, 10931, 14994, 64814, 60687, 56684, 52557, 48554, 44427, 40424,
    36297, 31782, 27655, 23652, 19525, 15522, 11395, 7392, 3265, 61215, 65342, 53085, 57212, 44955,
    49082, 36825, 40952, 28183, 32310, 20053, 24180, 11923, 16050, 3793, 7920,
]


def crc16(data: bytes) -> bytes:
    """Table-driven CCITT-style CRC16 with XOR-0xFFFF final, as used by the FB200."""
    crc = 0
    for b in data:
        crc = (CRC_TABLE[((crc >> 8) ^ b) & 0xFF] ^ ((crc << 8) & 0xFFFF)) & 0xFFFF
    crc ^= 0xFFFF
    return bytes([(crc >> 8) & 0xFF, crc & 0xFF])


class Transport(Protocol):
    """Minimal transport interface: 64-byte HID reports."""

    def write_report(self, report: bytes) -> None: ...

    def read_report(self, timeout_ms: int = 500) -> bytes | None: ...

    def close(self) -> None: ...


def pack_frame(fn: int, data: bytes = b"") -> bytes:
    """Pack a command frame: AA 55 | len(u16 LE) | fn | data | CRC16."""
    body = bytes([fn]) + bytes(data)
    framed = len(body).to_bytes(2, "little") + body
    return b"\xaa\x55" + framed + crc16(framed)


def iter_reports(frame: bytes, report_size: int = REPORT_SIZE) -> Iterator[bytes]:
    """Split a frame into HID reports: [length][payload][padding]."""
    for off in range(0, max(len(frame), 1), CHUNK_SIZE):
        chunk = frame[off:off + CHUNK_SIZE]
        report = bytes([len(chunk)]) + chunk
        yield report + bytes(report_size - len(report))


def write_frame(transport: Transport, frame: bytes) -> None:
    for report in iter_reports(frame):
        transport.write_report(report)


class FrameReader:
    """Incremental reassembly of AA 55 frames from HID reports."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self._packets: list[bytes] = []

    def feed_report(self, report: bytes) -> None:
        valid = report[0]
        self._buf.extend(report[1:1 + valid])
        self._parse()

    def _parse(self) -> None:
        while True:
            start = self._buf.find(b"\xaa\x55")
            if start < 0:
                if len(self._buf) > 1:
                    del self._buf[:-1]
                return
            if start > 0:
                del self._buf[:start]
            if len(self._buf) < 4:
                return
            length = int.from_bytes(self._buf[2:4], "little")
            total = 6 + length
            if len(self._buf) < total:
                return
            framed = bytes(self._buf[2:4 + length])
            got = bytes(self._buf[4 + length:6 + length])
            if crc16(framed) != got:
                del self._buf[:2]
                continue
            self._packets.append(bytes(self._buf[4:4 + length]))
            del self._buf[:total]

    def next_packet(self) -> bytes | None:
        return self._packets.pop(0) if self._packets else None

    def read_packet(self, transport: Transport, timeout_ms: int = 1500) -> bytes | None:
        import time

        deadline = time.monotonic() + timeout_ms / 1000
        while True:
            packet = self.next_packet()
            if packet is not None:
                return packet
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            report = transport.read_report(int(remaining * 1000))
            if report:
                self.feed_report(report)
