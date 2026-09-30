# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

import pytest

from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader


def make_mr() -> MrFile:
    header = MrHeader(product_tag="FB200", send_cmd=0x02, rec_cmd=0x03, update_block=1)
    blocks = [MrBlock(MrBlockTag(send_cmd=0x04, rec_cmd=0x05, start_page=0x40),
                      b"AT+BDFB200 Audio" + bytes(16) + b"FB200")]
    return MrFile(header, blocks)


def test_patch_string_replaces_only_block_data():
    mr = make_mr()
    count = mr.patch_string("FB200", "FB2X0")
    # "FB200" occurs twice inside block data: inside "AT+BDFB200 Audio" and as the trailing run
    assert count == 2
    assert b"AT+BDFB200 Audio" not in mr.blocks[0].data
    assert mr.blocks[0].data.endswith(b"FB2X0")
    # the header field must NOT be patched
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
    trailing = [h for h in mr.find_strings() if h.text == "FB200"]
    assert trailing and trailing[0].offset == 32
