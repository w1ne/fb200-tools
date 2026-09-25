import pytest

from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader


def make_mr() -> MrFile:
    header = MrHeader(product_tag="FB200", send_cmd=0x02, rec_cmd=0x03, update_block=1)
    blocks = [MrBlock(MrBlockTag(send_cmd=0x04, rec_cmd=0x05, start_page=0x40),
                      b"AT+BDFB200 Audio" + bytes(16) + b"FB200")]
    return MrFile(header, blocks)


def test_patch_string_replaces_only_block_data():
    mr = make_mr()
    count = mr.patch_string("FB200 Audio", "FB200 Tools")
    assert count == 1
    assert mr.blocks[0].data.startswith(b"AT+BDFB200 Tools")
    assert mr.header.product_tag == "FB200"


def test_patch_string_requires_equal_length():
    mr = make_mr()
    with pytest.raises(ValueError, match="same length"):
        mr.patch_string("FB200", "FB2000")


def test_patch_string_missing_raises():
    mr = make_mr()
    with pytest.raises(ValueError, match="not found"):
        mr.patch_string("NOPE!", "NOPE?")


def test_find_strings_reports_locations():
    mr = make_mr()
    hits = [h for h in mr.find_strings() if "FB200 Audio" in h.text]
    assert hits
    assert hits[0].block == 0
    hit = hits[0]
    assert mr.blocks[0].data[hit.offset:hit.offset + len(hit.text)] == hit.text.encode()
