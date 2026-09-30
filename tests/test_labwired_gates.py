# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""The app-protocol bytes in the LabWired gates come from fb200.protocol.

The gates are YAML, so they carry the bytes as literals. These tests make
sure the literals are the frames `fb200.protocol` builds, so a gate cannot
drift from the protocol code.
"""

import re
from pathlib import Path

from fb200.protocol import pack_frame

GATE = Path(__file__).resolve().parents[1] / "labwired" / "stock-first-boot.yaml"


def _hex(data: bytes) -> str:
    return data.hex(" ")


def test_phone_request_is_get_version_frame():
    text = GATE.read_text()
    m = re.search(r'device: "bt"\n\s+bytes: \[([^\]]+)\]', text)
    assert m, "no phone injection in the gate"
    sent = bytes(int(b, 16) for b in m.group(1).split(","))
    assert sent == pack_frame(0x00)
    assert f'"phone->mcu {_hex(sent)}"' in text


def test_expected_answer_is_the_version_reply_header():
    # Reply fn 0x01 with a 55-byte payload (PROTOCOL.md 5.2), product "FB200"
    # NUL padded: the frame starts AA 55, len = 1 + 55, fn, "FB200", 00.
    header = pack_frame(0x01, b"FB200".ljust(55, b"\0"))[:11]
    assert f'"mcu->phone {_hex(header)}"' in GATE.read_text()
