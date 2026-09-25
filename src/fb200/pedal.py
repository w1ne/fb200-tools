"""High-level FB200 device operations."""

from __future__ import annotations

import struct
import time
from dataclasses import dataclass

from fb200 import protocol
from fb200.errors import CommunicationError, InvalidArgumentError, ProtocolError
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
    if not 1 <= index <= IR_SLOT_COUNT:
        raise InvalidArgumentError(f"IR slot index must be 1..{IR_SLOT_COUNT}, got {index}")
    return struct.pack("<H", index)


class FB200Device:
    def __init__(self, transport: Transport) -> None:
        self.transport = transport
        self.reader = FrameReader()

    def request(self, fn: int, data: bytes = b"", expect: int | None = None,
                timeout_ms: int = 1500) -> bytes:
        while self.reader.next_packet() is not None:
            pass  # drop stale replies buffered by earlier requests
        write_frame(self.transport, pack_frame(fn, data))
        deadline = time.monotonic() + timeout_ms / 1000
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CommunicationError(f"no reply to command 0x{fn:02x}")
            packet = self.reader.read_packet(self.transport, int(remaining * 1000))
            if packet is None:
                raise CommunicationError(f"no reply to command 0x{fn:02x}")
            if not packet:
                raise ProtocolError(f"empty reply packet for command 0x{fn:02x}")
            if expect is None or packet[0] == expect:
                return packet

    def info(self) -> DeviceInfo:
        packet = self.request(protocol.CMD_GET_VERSION, expect=protocol.REPLY_VERSION)
        data = packet[1:]
        return DeviceInfo(
            product=_cstr(data, 0, 32),
            app_version=_cstr(data, 32, 39),
            firmware_version=_cstr(data, 39, 46),
            bluetooth_version=_cstr(data, 46, 53),
            hardware_rev=_cstr(data, 53, 55),
            raw=data,
        )

    def ir_list(self) -> list[IrSlot]:
        slots = []
        for index in range(1, IR_SLOT_COUNT + 1):
            payload = bytes([1]) + ir_index_bytes(index) + bytes([0])
            packet = self.request(protocol.CMD_QUERY_IR, payload, expect=protocol.REPLY_QUERY_IR)
            data = packet[1:]
            if len(data) > 3 and data[3] == 0:
                slots.append(IrSlot(index, None))
            else:
                if len(data) < 8:
                    raise ProtocolError(f"malformed IR query reply: {data.hex()}")
                name_len = struct.unpack_from("<H", data, 6)[0]
                if 8 + name_len > len(data):
                    raise ProtocolError(f"IR name length {name_len} exceeds reply size")
                name = _cstr(data, 8, 8 + name_len)
                slots.append(IrSlot(index, name))
        return slots

    def ir_delete(self, index: int) -> bool:
        payload = bytes([1]) + ir_index_bytes(index) + bytes([1])
        packet = self.request(protocol.CMD_DELETE_IR, payload, expect=protocol.REPLY_DELETE_IR)
        data = packet[1:]
        return len(data) > 3 and data[3] != 0

    def enter_bootloader(self) -> None:
        write_frame(self.transport, pack_frame(protocol.CMD_JUMP_BOOTLOADER))
