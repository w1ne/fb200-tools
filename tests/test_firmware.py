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
