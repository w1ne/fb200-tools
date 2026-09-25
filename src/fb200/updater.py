"""Mooer bootloader flash client for the FB200."""

from __future__ import annotations

import struct
from dataclasses import dataclass

from fb200 import protocol
from fb200.errors import FirmwareError
from fb200.firmware import MrFile
from fb200.protocol import pack_frame

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
