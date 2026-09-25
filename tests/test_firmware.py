import os
from pathlib import Path

import pytest

from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader


def make_mr() -> MrFile:
    header = MrHeader(
        product_tag="FB200", send_cmd=0x02, rec_cmd=0x03, timeout=10000,
        update_block=2, update_addr=b"\x03\x00\x00\x00", version=0,
    )
    blocks = [
        MrBlock(MrBlockTag(send_cmd=0x04, rec_cmd=0x05, start_page=0x40),
                b"FB200 Audio" + bytes(16)),
        MrBlock(MrBlockTag(send_cmd=0x06, rec_cmd=0x07, start_page=0x00),
                bytes(range(256)) * 4),
    ]
    return MrFile(header, blocks)


def test_round_trip():
    mr = make_mr()
    parsed = MrFile.from_bytes(mr.to_bytes())
    assert parsed.header.product_tag == "FB200"
    assert parsed.header.update_block == 2
    assert len(parsed.blocks) == 2
    assert parsed.blocks[0].data == mr.blocks[0].data
    assert parsed.blocks[1].tag.send_cmd == 0x06
    assert parsed.blocks[1].tag.start_page == 0


def test_to_bytes_is_stable():
    mr = make_mr()
    assert MrFile.from_bytes(mr.to_bytes()).to_bytes() == mr.to_bytes()


def test_total_data_size():
    mr = make_mr()
    assert mr.total_data_size == sum(len(b.data) for b in mr.blocks)


def test_rejects_trailing_bytes():
    raw = make_mr().to_bytes() + b"junk"
    with pytest.raises(ValueError, match="trailing"):
        MrFile.from_bytes(raw)


def test_rejects_truncated_block():
    raw = make_mr().to_bytes()[:-10]
    with pytest.raises(ValueError, match="truncated"):
        MrFile.from_bytes(raw)


def build_raw_header(**overrides) -> bytearray:
    raw = bytearray(128)
    raw[0:9] = b"Mooer_TAG"
    raw[9:14] = b"FB200"
    raw[41] = 0x02
    raw[42] = 0x03
    raw[47] = 0  # block count, set per test
    raw[52] = 0
    for offset, value in overrides.items():
        raw[int(offset)] = value
    return raw


def test_nonzero_padding_bytes_are_preserved():
    raw = build_raw_header()
    raw[60] = 0xAA
    raw[127] = 0xBB
    raw[47] = 1
    payload = bytes(range(64))
    tag = bytearray(512)
    tag[0:9] = b"Mooer_TAG"
    tag[17:21] = len(payload).to_bytes(4, "little")
    tag[32] = 0xCC
    tag[511] = 0xDD
    data = bytes(raw) + bytes(tag) + payload
    mr = MrFile.from_bytes(data)
    assert mr.to_bytes() == data


def test_rejects_wrong_magic():
    raw = bytearray(build_raw_header())
    raw[0:9] = b"NotMooer!"
    with pytest.raises(ValueError, match="header tag"):
        MrFile.from_bytes(bytes(raw))


def test_rejects_wrong_block_magic():
    data = bytearray(make_mr().to_bytes())
    data[128:137] = b"NotMooer!"
    with pytest.raises(ValueError, match="block tag"):
        MrFile.from_bytes(bytes(data))


def test_rejects_update_block_mismatch_on_serialize():
    header = MrHeader(product_tag="FB200", update_block=3)
    block = MrBlock(MrBlockTag(), b"x")
    with pytest.raises(ValueError, match="update_block"):
        MrFile(header, [block]).to_bytes()


def test_rejects_oversized_fixed_fields():
    with pytest.raises(ValueError):
        MrFile(MrHeader(tag=b"short"), []).to_bytes()
    with pytest.raises(ValueError):
        MrFile(MrHeader(update_addr=b"\x01"), []).to_bytes()


def test_rejects_empty_find():
    with pytest.raises(ValueError, match="empty"):
        make_mr().patch_string("", "")


def test_find_strings_rejects_min_len_below_one():
    with pytest.raises(ValueError, match="min_len"):
        make_mr().find_strings(0)


@pytest.mark.skipif(not os.environ.get("FB200_MR"), reason="FB200_MR not set")
def test_real_image_round_trip():
    raw = Path(os.environ["FB200_MR"]).read_bytes()
    mr = MrFile.from_bytes(raw)
    assert mr.to_bytes() == raw
    assert len(mr.blocks) == 2
    assert [len(b.data) for b in mr.blocks] == [200704, 3286016]
