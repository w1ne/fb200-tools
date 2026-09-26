import pytest

from fb200.errors import FirmwareError
from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader, pack_app_image
from fb200.updater import build_flash_plan


def make_template(app_size: int = 512) -> MrFile:
    header = MrHeader(
        product_tag="FB200", send_cmd=0x02, rec_cmd=0x03, timeout=10000,
        update_block=2, update_addr=b"\x03\x00\x00\x00", version=0,
    )
    blocks = [
        MrBlock(MrBlockTag(send_cmd=0x04, rec_cmd=0x05, timeout=10000,
                           start_page=0x40, rom_id=0), bytes(app_size)),
        MrBlock(MrBlockTag(send_cmd=0x06, rec_cmd=0x07, start_page=0x00, rom_id=0),
                bytes(64)),
    ]
    return MrFile(header, blocks)


def test_pack_produces_single_block():
    packed = pack_app_image(make_template(), b"\x01\x02\x03")
    assert packed.header.update_block == 1
    assert len(packed.blocks) == 1
    assert packed.blocks[0].data[:3] == b"\x01\x02\x03"


def test_pack_pads_to_template_block_size():
    packed = pack_app_image(make_template(app_size=512), b"abc")
    assert len(packed.blocks[0].data) == 512
    assert packed.blocks[0].data[3:] == b"\xff" * 509


def test_pack_preserves_header_and_tag_fields():
    template = make_template()
    packed = pack_app_image(template, b"abc")
    assert packed.header.product_tag == "FB200"
    assert packed.header.send_cmd == 0x02
    assert packed.header.rec_cmd == 0x03
    assert packed.header.update_addr == b"\x03\x00\x00\x00"
    assert packed.header.version == 0
    assert packed.header.timeout == 10000
    assert packed.blocks[0].tag.send_cmd == 0x04
    assert packed.blocks[0].tag.rec_cmd == 0x05
    assert packed.blocks[0].tag.timeout == 10000
    assert packed.blocks[0].tag.start_page == 0x40
    assert packed.blocks[0].tag.rom_id == 0
    # the stock header's raw bytes (magic, reserved fields) are preserved
    assert packed.to_bytes()[0:9] == b"Mooer_TAG"


def test_pack_round_trips_through_parser():
    packed = pack_app_image(make_template(), b"abc")
    reparsed = MrFile.from_bytes(packed.to_bytes())
    assert reparsed.header.update_block == 1
    assert reparsed.blocks[0].data == packed.blocks[0].data


def test_pack_rejects_wrong_product():
    template = make_template()
    template.header = MrHeader(product_tag="OTHER", send_cmd=0x02, rec_cmd=0x03)
    with pytest.raises(FirmwareError, match="FB200"):
        pack_app_image(template, b"abc")


def test_pack_rejects_empty_app():
    with pytest.raises(FirmwareError, match="empty"):
        pack_app_image(make_template(), b"")


def test_pack_rejects_oversize_app():
    with pytest.raises(FirmwareError, match="exceeds"):
        pack_app_image(make_template(app_size=8), b"123456789")


def test_pack_rejects_template_without_blocks():
    template = MrFile(MrHeader(product_tag="FB200"), [])
    with pytest.raises(FirmwareError, match="block"):
        pack_app_image(template, b"abc")


def test_packed_stock_sized_image_plans_392_writes():
    template = make_template(app_size=200_704)
    packed = pack_app_image(template, b"\x00" * 200_704)
    plan = build_flash_plan(packed)
    assert plan.write_count == 392
    assert plan.total_bytes == 200_704


def test_pack_preserves_raw_reserved_bytes():
    raw = bytearray(128)
    raw[0:9] = b"Mooer_TAG"
    raw[9:14] = b"FB200"
    raw[41] = 0x02
    raw[42] = 0x03
    raw[47] = 2
    raw[48:52] = b"\x03\x00\x00\x00"
    raw[100:104] = b"\xca\xfe\xba\xbe"
    tag_raw = bytearray(512)
    tag_raw[0:9] = b"Mooer_TAG"
    tag_raw[21] = 0x04
    tag_raw[22] = 0x05
    tag_raw[27:31] = (0x40).to_bytes(4, "little")
    tag_raw[500:504] = b"\x12\x34\x56\x78"
    header = MrHeader(
        product_tag="FB200", send_cmd=0x02, rec_cmd=0x03, timeout=10000,
        update_block=2, update_addr=b"\x03\x00\x00\x00", version=0, raw=bytes(raw),
    )
    template = MrFile(header, [
        MrBlock(MrBlockTag(send_cmd=0x04, rec_cmd=0x05, timeout=10000,
                           start_page=0x40, raw=bytes(tag_raw)), bytes(512)),
        MrBlock(MrBlockTag(send_cmd=0x06, rec_cmd=0x07), bytes(64)),
    ])
    packed = pack_app_image(template, b"abc")
    out = packed.to_bytes()
    assert out[100:104] == b"\xca\xfe\xba\xbe"
    assert out[128 + 500:128 + 504] == b"\x12\x34\x56\x78"
    assert packed.blocks[0].tag is not template.blocks[0].tag
