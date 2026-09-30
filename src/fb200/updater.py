# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Mooer bootloader flash client for the FB200."""

from __future__ import annotations

import struct
import time
from collections.abc import Callable
from dataclasses import dataclass

from fb200 import protocol
from fb200.errors import CommunicationError, FirmwareError
from fb200.firmware import MrFile
from fb200.protocol import FrameReader, Transport, pack_frame, write_frame

PAGE_SIZE = 512
NEW_PORT_PAGE_SIZE = 1024


@dataclass
class FlashPlan:
    erase_frame: bytes
    erase_reply: int
    writes: list[tuple[int, bytes]]
    exit_frame: bytes
    total_bytes: int

    @property
    def write_count(self) -> int:
        return len(self.writes)


def build_flash_plan(mr: MrFile) -> FlashPlan:
    header_cmd = mr.header.send_cmd
    if not 0 <= header_cmd <= 0xFF:
        raise FirmwareError(f"invalid erase command 0x{header_cmd:x}")
    if mr.header.version == 0:
        erase_data = bytes(mr.header.update_addr)
    else:
        erase_data = bytearray()
        for block in mr.blocks:
            erase_data.append(block.tag.rom_id)
            erase_data += struct.pack("<I", block.tag.start_page)
            erase_data += struct.pack("<I", len(block.data))
        erase_data = bytes(erase_data)
    writes: list[tuple[int, bytes]] = []
    total = 0
    for block in mr.blocks:
        if not block.data:
            raise FirmwareError("empty firmware block")
        if not 0 <= block.tag.send_cmd <= 0xFE:
            raise FirmwareError(f"invalid block send command 0x{block.tag.send_cmd:x}")
        chunk_size = NEW_PORT_PAGE_SIZE if (block.tag.send_cmd >> 7) else PAGE_SIZE
        page_count = (len(block.data) + chunk_size - 1) // chunk_size
        if block.tag.start_page + page_count > 0x10000:
            raise FirmwareError("page range exceeds 16-bit page address space")
        for page, offset in enumerate(range(0, len(block.data), chunk_size)):
            chunk = block.data[offset:offset + chunk_size]
            page_number = (block.tag.start_page + page) & 0xFFFF
            writes.append(
                (block.tag.send_cmd + 1,
                 pack_frame(block.tag.send_cmd, struct.pack(">H", page_number) + chunk))
            )
            total += len(chunk)
    return FlashPlan(
        erase_frame=pack_frame(header_cmd, erase_data),
        erase_reply=mr.header.rec_cmd,
        writes=writes,
        exit_frame=pack_frame(protocol.CMD_EXIT_BOOTLOADER),
        total_bytes=total,
    )


@dataclass
class FlashResult:
    dry_run: bool
    write_frames: int
    bytes_written: int


class FirmwareUpdater:
    def __init__(self, transport: Transport) -> None:
        self.transport = transport
        self.reader = FrameReader()

    def flash(self, mr: MrFile, dry_run: bool = True,
              progress: Callable[[int, int], None] | None = None,
              timeout_ms: int = 10000, erase_timeout_ms: int = 60000) -> FlashResult:
        """Erase, write and exit the bootloader.

        The erase acknowledgement can take well over ten seconds on hardware
        (measured ~11.5 s), so it uses its own, longer timeout. On failure the
        device is left in the bootloader; the call is safely re-runnable (use
        `--no-jump` to skip entering the bootloader again).
        """
        plan = build_flash_plan(mr)
        if dry_run:
            return FlashResult(dry_run=True, write_frames=0, bytes_written=0)
        while self.reader.next_packet() is not None:
            pass
        write_frame(self.transport, plan.erase_frame)
        packet = self.reader.read_packet(self.transport, erase_timeout_ms)
        if not packet:
            raise CommunicationError("erase command not acknowledged (no reply)")
        if packet[0] != plan.erase_reply:
            raise CommunicationError(
                f"erase command not acknowledged (got {packet[0]:#04x})"
            )
        sent = 0
        for expected_reply, frame in plan.writes:
            write_frame(self.transport, frame)
            packet = self.reader.read_packet(self.transport, timeout_ms)
            if not packet or packet[0] != expected_reply:
                raise CommunicationError(f"unexpected reply while writing page {sent + 1}")
            sent += 1
            if progress is not None:
                progress(sent, len(plan.writes))
        write_frame(self.transport, plan.exit_frame)
        return FlashResult(dry_run=False, write_frames=sent, bytes_written=plan.total_bytes)


def wait_for_device(vid: int, pid: int, timeout_s: float = 15.0,
                    poll_s: float = 0.5) -> bytes | None:
    from fb200.transport import HidapiTransport

    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        path = HidapiTransport.find_path(vid, pid)
        if path:
            return path
        time.sleep(poll_s)
    return None
