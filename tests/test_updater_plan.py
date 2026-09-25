import struct

import pytest

from fb200.errors import FirmwareError
from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader
from fb200.protocol import pack_frame
from fb200.updater import build_flash_plan


def make_mr(size: int = 1024, version: int = 1,
            update_addr: bytes = b"\x03\x00\x00\x00") -> MrFile:
    header = MrHeader(product_tag="FB200", send_cmd=0x02, rec_cmd=0x03,
                      update_block=1, version=version, update_addr=update_addr)
    tag = MrBlockTag(send_cmd=0x04, rec_cmd=0x05, start_page=0x40, rom_id=0)
    return MrFile(header, [MrBlock(tag, bytes(range(256)) * (size // 256))])


def test_erase_frame_new_format():
    plan = build_flash_plan(make_mr(version=1))
    expected = pack_frame(0x02, bytes([0]) + struct.pack("<I", 0x40) + struct.pack("<I", 1024))
    assert plan.erase_frame == expected
    assert plan.erase_reply == 0x03


def test_erase_frame_version_zero_uses_update_addr():
    plan = build_flash_plan(make_mr(version=0))
    assert plan.erase_frame == pack_frame(0x02, b"\x03\x00\x00\x00")


def test_write_frame_pages_are_big_endian():
    plan = build_flash_plan(make_mr(1024, version=1))
    data = make_mr(1024, version=1).blocks[0].data
    assert plan.write_count == 2
    assert plan.writes[0] == (0x05, pack_frame(0x04, struct.pack(">H", 0x40) + data[:512]))
    assert plan.writes[1] == (0x05, pack_frame(0x04, struct.pack(">H", 0x41) + data[512:]))


def test_total_bytes_and_exit_frame():
    plan = build_flash_plan(make_mr(1024, version=1))
    assert plan.total_bytes == 1024
    assert plan.exit_frame == pack_frame(0xFF)


def test_new_port_blocks_use_1024_byte_chunks():
    tag = MrBlockTag(send_cmd=0x86, start_page=0)
    mr = MrFile(MrHeader(product_tag="FB200", send_cmd=0x02, update_block=1),
                [MrBlock(tag, bytes(2048))])
    plan = build_flash_plan(mr)
    assert plan.write_count == 2
    first = plan.writes[0][1]
    assert plan.writes[0][0] == 0x87
    assert first[4] == 0x86
    assert first[5:7] == struct.pack(">H", 0)
    assert first[7:7 + 1024] == bytes(1024)
    assert len(first) == 7 + 2 + 1024
    second = plan.writes[1][1]
    assert second[5:7] == struct.pack(">H", 1)
    assert len(second) == 7 + 2 + 1024


def test_partial_final_chunk():
    tag = MrBlockTag(send_cmd=0x04, start_page=0)
    mr = MrFile(MrHeader(product_tag="FB200", send_cmd=0x02, update_block=1),
                [MrBlock(tag, bytes(600))])
    plan = build_flash_plan(mr)
    assert plan.write_count == 2
    assert len(plan.writes[0][1]) == 7 + 2 + 512
    assert len(plan.writes[1][1]) == 7 + 2 + 88
    assert plan.writes[1][1][5:7] == struct.pack(">H", 1)
    assert plan.total_bytes == 600


def test_page_range_overflow_raises():
    tag = MrBlockTag(send_cmd=0x04, start_page=0xFFFF)
    mr = MrFile(MrHeader(product_tag="FB200", send_cmd=0x02, update_block=1),
                [MrBlock(tag, bytes(1024))])
    with pytest.raises(FirmwareError):
        build_flash_plan(mr)


def test_invalid_send_cmd_raises():
    tag = MrBlockTag(send_cmd=0xFF, start_page=0)
    mr = MrFile(MrHeader(product_tag="FB200", send_cmd=0x02, update_block=1),
                [MrBlock(tag, bytes(512))])
    with pytest.raises(FirmwareError):
        build_flash_plan(mr)


def test_empty_block_raises():
    mr = MrFile(MrHeader(product_tag="FB200", send_cmd=0x02, update_block=1),
                [MrBlock(MrBlockTag(send_cmd=0x04), b"")])
    with pytest.raises(FirmwareError):
        build_flash_plan(mr)
