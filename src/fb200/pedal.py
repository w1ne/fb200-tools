"""High-level FB200 device operations."""

from __future__ import annotations

import struct
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from itertools import islice

from fb200 import protocol
from fb200.errors import CommunicationError, InvalidArgumentError, ProtocolError
from fb200.protocol import FrameReader, Transport, pack_frame, write_frame

IR_SLOT_COUNT = 9
IR_SAMPLE_COUNT = 1024
PRESET_SIZE = 256
PRESET_COUNT = 40
PRESET_NAME_SIZE = 20
SETTINGS_SIZE = 13
# Named bytes of the 0xB0 settings block (docs/PROTOCOL.md 5.5); the others
# are written back unchanged.
SETTINGS_FIELDS = {"cab_global": 1, "input_gain": 2, "tuner": 8, "bt_audio": 10,
                   "ring_color": 11, "ring_level": 12}

# Effect module blocks of the edit buffer (docs/PROTOCOL.md 5.3): fn 0x80 + index
# writes the u16 fields in this order. Field names follow
# firmware/audio/src/preset/preset.h; pN = meaning not yet mapped.
MODULES: dict[str, tuple[int, int, tuple[str, ...]]] = {
    "comp": (0, 0x14, ("enabled", "type", "attack", "threshold", "ratio", "level")),
    "gate": (1, 0x5C, ("enabled", "type", "threshold")),
    "amp": (2, 0x2C, ("enabled", "model", "gain", "bass", "mid", "midfreq", "treble", "volume")),
    "cab": (3, 0x44, ("enabled", "type", "p1", "p2", "p3", "p4")),
    "mod": (4, 0x74, ("enabled", "type", "p1", "p2", "p3", "p4", "p5")),
    "delay": (5, 0x8C, ("enabled", "type", "mix", "feedback", "time_ms")),
    "reverb": (6, 0xA4, ("enabled", "type", "p1", "level", "decay", "p4")),
}


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


def decode_module(edit: bytes, name: str) -> dict[str, int]:
    """One module block of a 256-byte edit buffer, as {field: value}."""
    _index, offset, fields = MODULES[name]
    values = struct.unpack_from(f"<{len(fields)}H", edit, offset)
    return dict(zip(fields, values))


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

    def ir_import(self, index: int, name: str, samples: Iterable[float],
                  progress: Callable[[int, int], None] | None = None) -> bool:
        name_bytes = sanitize_ir_name(name).encode("ascii")
        data = bytearray()
        for sample in islice(samples, IR_SAMPLE_COUNT):
            data += struct.pack("<f", sample)
        data += bytes(IR_SAMPLE_COUNT * 4 - len(data))
        frame_count = (len(data) + 511) // 512 + 1
        for frame_index in range(frame_count):
            if frame_index == 0:
                chunk = name_bytes
            else:
                chunk = bytes(data[(frame_index - 1) * 512:frame_index * 512])
            payload = (
                bytes([1])
                + ir_index_bytes(index)
                + bytes([frame_count, frame_index])
                + struct.pack("<H", len(chunk))
                + chunk
            )
            self.request(protocol.CMD_UPLOAD_IR, payload, expect=protocol.REPLY_UPLOAD_IR)
            if progress is not None:
                progress(frame_index + 1, frame_count)
        return True

    def import_wav(self, index: int, path, name: str | None = None,
                   progress: Callable[[int, int], None] | None = None, **options) -> str:
        """Convert a WAV to the IR format and upload it; return the slot name.
        ``options`` go to `fb200.wav.process_ir` (none: the stock conversion)."""
        from pathlib import Path

        from fb200.wav import IR_LENGTH, process_ir

        if (options.get("taps") or 0) > IR_LENGTH:
            raise InvalidArgumentError(f"a pedal slot holds {IR_LENGTH} taps")
        samples = process_ir(path, **options)
        name = sanitize_ir_name(name or Path(path).stem)
        self.ir_import(index, name, samples, progress=progress)
        return name

    def send(self, fn: int, data: bytes = b"") -> None:
        """Send one frame that has no reply."""
        write_frame(self.transport, pack_frame(fn, data))

    def edit_buffer(self) -> tuple[int, bytes]:
        """The current preset index and its live 256-byte edit buffer."""
        packet = self.request(protocol.CMD_CONNECT, expect=protocol.REPLY_EDIT)
        data = packet[1:]
        if len(data) < 1 + PRESET_SIZE:
            raise ProtocolError(f"short edit buffer reply ({len(data)} bytes)")
        return data[0], bytes(data[1:1 + PRESET_SIZE])

    def read_preset(self, index: int) -> bytes:
        """The stored 256-byte preset `index` (0..39), from flash."""
        if not 0 <= index < PRESET_COUNT:
            raise InvalidArgumentError(f"preset index must be 0..{PRESET_COUNT - 1}")
        while True:
            packet = self.request(protocol.CMD_READ_PRESET, bytes([index]),
                                  expect=protocol.REPLY_PRESET)
            data = packet[1:]
            if len(data) < 1 + PRESET_SIZE:
                raise ProtocolError(f"short preset reply ({len(data)} bytes)")
            if data[0] == index:
                return bytes(data[1:1 + PRESET_SIZE])

    def preset_names(self) -> list[str]:
        """The names of the 40 stored presets."""
        return [_cstr(self.read_preset(i), 0, PRESET_NAME_SIZE) for i in range(PRESET_COUNT)]

    def rename_preset(self, index: int, name: str) -> str:
        """Select preset `index`, set its name and store it (flash). The edit
        buffer is stored with the new name. Returns the name read back."""
        if not 0 <= index < PRESET_COUNT:
            raise InvalidArgumentError(f"preset index must be 0..{PRESET_COUNT - 1}")
        raw = sanitize_ir_name(name)[:PRESET_NAME_SIZE].encode("ascii")
        self.send(protocol.CMD_RENAME_PRESET,
                  bytes([index]) + raw + bytes(PRESET_NAME_SIZE - len(raw)))
        return _cstr(self.read_preset(index), 0, PRESET_NAME_SIZE)

    def settings(self) -> bytes:
        """The 13-byte global settings block (from the connect dump)."""
        packet = self.request(protocol.CMD_CONNECT, expect=protocol.REPLY_SETTINGS)
        data = packet[1:]
        if len(data) < SETTINGS_SIZE:
            raise ProtocolError(f"short settings reply ({len(data)} bytes)")
        return bytes(data[:SETTINGS_SIZE])

    def set_settings(self, changes: dict[str, int]) -> dict[str, int]:
        """Change named settings bytes (SETTINGS_FIELDS); return them read back."""
        unknown = set(changes) - set(SETTINGS_FIELDS)
        if unknown:
            raise InvalidArgumentError(f"unknown setting {', '.join(sorted(unknown))} "
                                       f"(have {', '.join(SETTINGS_FIELDS)})")
        block = bytearray(self.settings())
        for key, value in changes.items():
            if not 0 <= int(value) <= 0xFF:
                raise InvalidArgumentError(f"{key} = {value}: must be 0..255")
            block[SETTINGS_FIELDS[key]] = int(value)
        if changes:
            self.send(protocol.CMD_SETTINGS, bytes(block))
        block = self.settings()
        return {k: block[i] for k, i in SETTINGS_FIELDS.items()}

    def modules(self) -> dict[str, dict[str, int]]:
        _index, edit = self.edit_buffer()
        return {name: decode_module(edit, name) for name in MODULES}

    def set_module(self, name: str, changes: dict[str, int]) -> dict[str, int]:
        """Change some fields of one effect module; return the block read back.

        The pedal clamps the values, and a new model/type loads that model's
        default parameters. So a type change goes first, and the other
        changes go on top of the new defaults."""
        if name not in MODULES:
            raise InvalidArgumentError(f"unknown module {name!r} (have {', '.join(MODULES)})")
        index, _offset, fields = MODULES[name]
        unknown = set(changes) - set(fields)
        if unknown:
            raise InvalidArgumentError(f"{name} has no field {', '.join(sorted(unknown))} "
                                       f"(fields: {', '.join(fields)})")
        for key, value in changes.items():
            if not 0 <= int(value) <= 0xFFFF:
                raise InvalidArgumentError(f"{name}.{key} = {value}: must be 0..65535")
        type_field = fields[1]
        current = decode_module(self.edit_buffer()[1], name)
        pending = {k: int(v) for k, v in changes.items()}
        if type_field in pending and pending[type_field] != current[type_field]:
            current[type_field] = pending.pop(type_field)
            self._write_module(index, fields, current)
            current = decode_module(self.edit_buffer()[1], name)
        if pending or not changes:
            current.update(pending)
            self._write_module(index, fields, current)
        return decode_module(self.edit_buffer()[1], name)

    def _write_module(self, index: int, fields: tuple[str, ...], values: dict[str, int]) -> None:
        payload = struct.pack(f"<{len(fields)}H", *(values[f] for f in fields))
        self.send(protocol.CMD_MODULE + index, payload)

    def enter_bootloader(self) -> None:
        self.send(protocol.CMD_JUMP_BOOTLOADER)
