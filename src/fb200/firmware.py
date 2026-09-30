# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Mooer `.mr` firmware container: parse, serialize, patch."""

from __future__ import annotations

import re
import struct
from collections.abc import Iterator
from dataclasses import dataclass, field, replace
from pathlib import Path

from fb200.errors import FirmwareError

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
    raw: bytes | None = field(default=None, repr=False, compare=False)


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
    raw: bytes | None = field(default=None, repr=False, compare=False)


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
    def from_path(cls, path: str | Path) -> MrFile:
        try:
            data = Path(path).read_bytes()
        except OSError as exc:
            raise FirmwareError(f"cannot read {path}: {exc}") from exc
        return cls.from_bytes(data)

    @classmethod
    def from_bytes(cls, data: bytes) -> MrFile:
        if len(data) < HEADER_SIZE:
            raise FirmwareError("file too small for .mr header")
        header = _parse_header(data[:HEADER_SIZE])
        if header.tag != MAGIC:
            raise FirmwareError("not a Mooer .mr file (bad header tag)")
        blocks = []
        offset = HEADER_SIZE
        for _ in range(header.update_block):
            if offset + TAG_SIZE > len(data):
                raise FirmwareError("truncated block tag")
            tag = _parse_tag(data[offset:offset + TAG_SIZE])
            if tag.tag != MAGIC:
                raise FirmwareError("not a Mooer .mr file (bad block tag)")
            offset += TAG_SIZE
            if offset + tag.block_size > len(data):
                raise FirmwareError("truncated block data")
            blocks.append(MrBlock(tag, data[offset:offset + tag.block_size]))
            offset += tag.block_size
        if offset != len(data):
            raise FirmwareError(f"{len(data) - offset} trailing bytes after blocks")
        return cls(header, blocks)

    def to_bytes(self) -> bytes:
        if self.header.update_block != len(self.blocks):
            raise FirmwareError(
                f"header update_block {self.header.update_block} does not match "
                f"{len(self.blocks)} blocks"
            )
        out = bytearray(_serialize_header(self.header))
        for block in self.blocks:
            out += _serialize_tag(block.tag, len(block.data))
            out += block.data
        return bytes(out)

    def find_strings(self, min_len: int = 4) -> Iterator[StringHit]:
        if min_len < 1:
            raise FirmwareError("min_len must be at least 1")
        pattern = re.compile(rb"[\x20-\x7e]{%d,}" % min_len)
        return (
            StringHit(block_index, match.start(), match.group().decode())
            for block_index, block in enumerate(self.blocks)
            for match in pattern.finditer(block.data)
        )

    def patch_string(self, find: str, replace: str) -> int:
        if not find:
            raise FirmwareError("find string must not be empty")
        find_bytes = find.encode()
        replace_bytes = replace.encode()
        if len(find_bytes) != len(replace_bytes):
            raise FirmwareError("replacement must be the same length in bytes")
        count = 0
        for block in self.blocks:
            occurrences = block.data.count(find_bytes)
            if occurrences:
                block.data = block.data.replace(find_bytes, replace_bytes)
                count += occurrences
        if count == 0:
            raise FirmwareError(f"string {find!r} not found in firmware")
        return count


def pack_app_image(template: MrFile, app: bytes) -> MrFile:
    """Build a single-block (application-only) ``.mr`` image.

    The template supplies the header and block-0 tag; the bootloader's erase
    and write commands come from those fields. The payload is padded with
    ``0xFF`` (erased-flash value) to the template's block-0 size so the flash
    plan covers exactly the stock application pages.
    """
    if template.header.product_tag != "FB200":
        raise FirmwareError(
            f"template is for {template.header.product_tag!r} (expected 'FB200')"
        )
    if not template.blocks:
        raise FirmwareError("template has no blocks")
    if not app:
        raise FirmwareError("application binary is empty")
    limit = len(template.blocks[0].data)
    if len(app) > limit:
        raise FirmwareError(
            f"application binary exceeds template block 0 ({len(app)} > {limit} bytes)"
        )
    header = replace(template.header, update_block=1)
    block = MrBlock(replace(template.blocks[0].tag), app.ljust(limit, b"\xff"))
    return MrFile(header, [block])


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
        raw=raw,
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
        raw=raw,
    )


def _serialize_header(header: MrHeader) -> bytes:
    if len(header.tag) != 9:
        raise FirmwareError("header tag must be exactly 9 bytes")
    if len(header.update_addr) != 4:
        raise FirmwareError("update_addr must be exactly 4 bytes")
    raw = bytearray(header.raw) if header.raw is not None else bytearray(HEADER_SIZE)
    if len(raw) != HEADER_SIZE:
        raise FirmwareError(f"raw header must be exactly {HEADER_SIZE} bytes")
    raw[0:9] = header.tag
    if header.raw is None or header.product_tag != header.raw[9:41].split(b"\x00")[0].decode(
        errors="replace"
    ):
        encoded = header.product_tag.encode()
        if len(encoded) > 32:
            raise FirmwareError("product_tag must encode to at most 32 bytes")
        raw[9:41] = encoded.ljust(32, b"\x00")
    raw[41] = header.send_cmd
    raw[42] = header.rec_cmd
    struct.pack_into("<I", raw, 43, header.timeout)
    raw[47] = header.update_block
    raw[48:52] = header.update_addr
    raw[52] = header.version
    return bytes(raw)


def _serialize_tag(tag: MrBlockTag, data_size: int) -> bytes:
    if len(tag.tag) != 9:
        raise FirmwareError("block tag must be exactly 9 bytes")
    raw = bytearray(tag.raw) if tag.raw is not None else bytearray(TAG_SIZE)
    if len(raw) != TAG_SIZE:
        raise FirmwareError(f"raw block tag must be exactly {TAG_SIZE} bytes")
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
