"""Mooer `.mr` firmware container: parse, serialize, patch."""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"Mooer_TAG"
HEADER_SIZE = 128
TAG_SIZE = 512


@dataclass
class MrHeader:
    tag: bytes = MAGIC
    product_tag: str = ""
    send_cmd: int = 0
    rec_cmd: int = 0
    timeout: int = 0
    update_block: int = 0
    update_addr: bytes = b"\x00\x00\x00\x00"
    version: int = 0


@dataclass
class MrBlockTag:
    tag: bytes = MAGIC
    start_addr: int = 0
    stop_addr: int = 0
    block_size: int = 0
    send_cmd: int = 0
    rec_cmd: int = 0
    timeout: int = 0
    start_page: int = 0
    rom_id: int = 0


@dataclass
class MrBlock:
    tag: MrBlockTag
    data: bytes


@dataclass(frozen=True)
class StringHit:
    block: int
    offset: int
    text: str


class MrFile:
    def __init__(self, header: MrHeader, blocks: list[MrBlock]) -> None:
        self.header = header
        self.blocks = blocks

    @property
    def total_data_size(self) -> int:
        return sum(len(b.data) for b in self.blocks)

    @classmethod
    def from_path(cls, path) -> MrFile:
        return cls.from_bytes(Path(path).read_bytes())

    @classmethod
    def from_bytes(cls, data: bytes) -> MrFile:
        if len(data) < HEADER_SIZE:
            raise ValueError("file too small for .mr header")
        header = _parse_header(data[:HEADER_SIZE])
        blocks = []
        offset = HEADER_SIZE
        for _ in range(header.update_block):
            if offset + TAG_SIZE > len(data):
                raise ValueError("truncated block tag")
            tag = _parse_tag(data[offset:offset + TAG_SIZE])
            offset += TAG_SIZE
            if offset + tag.block_size > len(data):
                raise ValueError("truncated block data")
            blocks.append(MrBlock(tag, data[offset:offset + tag.block_size]))
            offset += tag.block_size
        if offset != len(data):
            raise ValueError(f"{len(data) - offset} trailing bytes after blocks")
        return cls(header, blocks)

    def to_bytes(self) -> bytes:
        out = bytearray(_serialize_header(self.header))
        for block in self.blocks:
            out += _serialize_tag(block.tag, len(block.data))
            out += block.data
        return bytes(out)

    def find_strings(self, min_len: int = 4):
        pattern = rb"[\x20-\x7e]{%d,}" % min_len
        for block_index, block in enumerate(self.blocks):
            for match in re.finditer(pattern, block.data):
                yield StringHit(block_index, match.start(), match.group().decode())

    def patch_string(self, find: str, replace: str) -> int:
        find_bytes = find.encode()
        replace_bytes = replace.encode()
        if len(find_bytes) != len(replace_bytes):
            raise ValueError("replacement must be the same length in bytes")
        count = 0
        for block in self.blocks:
            occurrences = block.data.count(find_bytes)
            if occurrences:
                block.data = block.data.replace(find_bytes, replace_bytes)
                count += occurrences
        if count == 0:
            raise ValueError(f"string {find!r} not found in firmware")
        return count


def _parse_header(raw: bytes) -> MrHeader:
    return MrHeader(
        tag=raw[0:9],
        product_tag=raw[9:41].split(b"\x00")[0].decode(errors="replace"),
        send_cmd=raw[41],
        rec_cmd=raw[42],
        timeout=struct.unpack_from("<I", raw, 43)[0],
        update_block=raw[47],
        update_addr=raw[48:52],
        version=raw[52],
    )


def _parse_tag(raw: bytes) -> MrBlockTag:
    return MrBlockTag(
        tag=raw[0:9],
        start_addr=struct.unpack_from("<I", raw, 9)[0],
        stop_addr=struct.unpack_from("<I", raw, 13)[0],
        block_size=struct.unpack_from("<I", raw, 17)[0],
        send_cmd=raw[21],
        rec_cmd=raw[22],
        timeout=struct.unpack_from("<I", raw, 23)[0],
        start_page=struct.unpack_from("<I", raw, 27)[0],
        rom_id=raw[31],
    )


def _serialize_header(header: MrHeader) -> bytes:
    raw = bytearray(HEADER_SIZE)
    raw[0:9] = header.tag
    raw[9:41] = header.product_tag.encode().ljust(32, b"\x00")[:32]
    raw[41] = header.send_cmd
    raw[42] = header.rec_cmd
    struct.pack_into("<I", raw, 43, header.timeout)
    raw[47] = header.update_block
    raw[48:52] = header.update_addr
    raw[52] = header.version
    return bytes(raw)


def _serialize_tag(tag: MrBlockTag, data_size: int) -> bytes:
    raw = bytearray(TAG_SIZE)
    raw[0:9] = tag.tag
    struct.pack_into("<I", raw, 9, tag.start_addr)
    struct.pack_into("<I", raw, 13, tag.stop_addr)
    struct.pack_into("<I", raw, 17, data_size)
    raw[21] = tag.send_cmd
    raw[22] = tag.rec_cmd
    struct.pack_into("<I", raw, 23, tag.timeout)
    struct.pack_into("<I", raw, 27, tag.start_page)
    raw[31] = tag.rom_id
    return bytes(raw)
