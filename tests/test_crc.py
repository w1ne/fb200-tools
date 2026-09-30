# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

from fb200.protocol import crc16


def test_crc16_matches_captured_frame():
    # Verified against a live FB200: frame AA 55 01 00 00 ends with C8 CF
    assert crc16(bytes.fromhex("010000")) == bytes.fromhex("c8cf")


def test_crc16_empty_input_xors_to_ffff():
    assert crc16(b"") == bytes.fromhex("ffff")
