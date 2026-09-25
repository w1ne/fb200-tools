"""High-level FB200 device operations."""

from __future__ import annotations

import struct
from dataclasses import dataclass

from fb200 import protocol
from fb200.errors import CommunicationError
from fb200.protocol import FrameReader, Transport, pack_frame, write_frame

IR_SLOT_COUNT = 9
IR_SAMPLE_COUNT = 1024


@dataclass(frozen=True)
class DeviceInfo:
    product: str
    app_version: str
    firmware_version: str
    bluetooth_version: str
    hardware_rev: str
    raw: bytes


@dataclass(frozen=True)
class IrSlot:
    index: int
    name: str | None

    @property
    def empty(self) -> bool:
        return self.name is None


def _cstr(data: bytes, start: int, end: int) -> str:
    return data[start:end].split(b"\x00")[0].decode(errors="replace")


def sanitize_ir_name(name: str) -> str:
    cleaned = "".join(ch if 0x20 <= ord(ch) <= 0x7E else " " for ch in name)
    cleaned = " ".join(cleaned.split())
    return (cleaned or "untitled")[:50]


def ir_index_bytes(index: int) -> bytes:
    return struct.pack("<H", index)


class FB200Device:
    def __init__(self, transport: Transport) -> None:
        self.transport = transport
        self.reader = FrameReader()

    def request(self, fn: int, data: bytes = b"", expect: int | None = None,
                timeout_ms: int = 1500) -> bytes:
        write_frame(self.transport, pack_frame(fn, data))
        while True:
            packet = self.reader.read_packet(self.transport, timeout_ms)
            if packet is None:
                raise CommunicationError(f"no reply to command 0x{fn:02x}")
            if expect is None or packet[0] == expect:
                return packet

    def info(self) -> DeviceInfo:
        packet = self.request(protocol.CMD_GET_VERSION, expect=protocol.REPLY_VERSION)
        data = packet[1:]
        return DeviceInfo(
            product=_cstr(data, 0, 32),
            app_version=_cstr(data, 32, 39),
            firmware_version=_cstr(data, 39, 45),
            bluetooth_version=_cstr(data, 46, 53),
            hardware_rev=_cstr(data, 53, 55),
            raw=data,
        )

    def enter_bootloader(self) -> None:
        write_frame(self.transport, pack_frame(protocol.CMD_JUMP_BOOTLOADER))
