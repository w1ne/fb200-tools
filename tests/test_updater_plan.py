import struct

from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader
from fb200.protocol import pack_frame
from fb200.updater import build_flash_plan


def make_mr(size: int = 1024) -> MrFile:
    header = MrHeader(product_tag="FB200", send_cmd=0x02, rec_cmd=0x03, update_block=1)
    tag = MrBlockTag(send_cmd=0x04, rec_cmd=0x05, start_page=0x40, rom_id=0)
    return MrFile(header, [MrBlock(tag, bytes(range(256)) * (size // 256))])


def test_erase_frame_payload():
    plan = build_flash_plan(make_mr())
    expected = pack_frame(0x02, bytes([0]) + struct.pack("<I", 0x40) + struct.pack("<I", 1024))
    assert plan.erase_frame == expected


def test_write_frame_count_and_pages():
    plan = build_flash_plan(make_mr(1024))
    assert plan.write_count == 2
    first_expected = pack_frame(0x04, struct.pack("<H", 0x40) + make_mr(1024).blocks[0].data[:512])
    second_expected = pack_frame(0x04, struct.pack("<H", 0x41) + make_mr(1024).blocks[0].data[512:])
    assert plan.writes[0] == (0x05, first_expected)
    assert plan.writes[1] == (0x05, second_expected)


def test_total_bytes_and_exit_frame():
    plan = build_flash_plan(make_mr(1024))
    assert plan.total_bytes == 1024
    assert plan.exit_frame == pack_frame(0xFF)


def test_new_port_blocks_use_1024_byte_chunks():
    header = MrHeader(product_tag="FB200", send_cmd=0x02, update_block=1)
    tag = MrBlockTag(send_cmd=0x84, start_page=0)  # bit 7 set = new port
    mr = MrFile(header, [MrBlock(tag, bytes(2048))])
    plan = build_flash_plan(mr)
    assert plan.write_count == 2
