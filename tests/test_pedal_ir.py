# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

import struct

import pytest
from conftest import make_report

from fb200.errors import InvalidArgumentError, ProtocolError
from fb200.pedal import FB200Device, IrSlot
from fb200.protocol import pack_frame
from fb200.transport import MockTransport


def query_reply(index: int, name: str | None) -> bytes:
    payload = bytearray(16)
    payload[3] = 0 if name is None else 1
    if name is not None:
        encoded = name.encode()
        struct.pack_into("<H", payload, 6, len(encoded))
        payload[8:8 + len(encoded)] = encoded
    return pack_frame(0x64, bytes(payload))


def delete_reply(ok: bool) -> bytes:
    payload = bytearray(4)
    payload[3] = 1 if ok else 0
    return pack_frame(0x68, bytes(payload))


def make_device(names, delete_ok: bool = True):
    reports = [make_report(query_reply(i + 1, n)) for i, n in enumerate(names)]
    reports.append(make_report(delete_reply(delete_ok)))
    transport = MockTransport(reports=reports)
    return FB200Device(transport), transport


def test_ir_list_parses_names_and_empty_slots():
    device, _ = make_device(["My IR", None] + [None] * 7)
    slots = device.ir_list()
    assert len(slots) == 9
    assert slots[0] == IrSlot(1, "My IR")
    assert slots[1] == IrSlot(2, None)
    assert slots[1].empty


def test_ir_list_sends_1_based_index():
    device, transport = make_device([None] * 9)
    device.ir_list()
    frame = pack_frame(0x63, bytes([1, 1, 0, 0]))
    assert transport.written[0] == bytes([len(frame)]) + frame + bytes(64 - 1 - len(frame))


def test_ir_delete_returns_true_on_success():
    device, _ = make_device([None] * 9, delete_ok=True)
    assert device.ir_delete(3) is True


def test_ir_delete_returns_false_on_failure():
    device, _ = make_device([None] * 9, delete_ok=False)
    assert device.ir_delete(3) is False


def test_ir_delete_wire_payload():
    device, transport = make_device([None] * 9)
    device.ir_delete(7)
    expected = pack_frame(0x67, bytes([1, 7, 0, 1]))
    assert transport.written[0] == bytes([len(expected)]) + expected + bytes(64 - 1 - len(expected))


def test_ir_list_malformed_short_named_reply_raises():
    reports = [make_report(pack_frame(0x64, bytes([0, 0, 0, 1])))]
    device = FB200Device(MockTransport(reports=reports))
    with pytest.raises(ProtocolError):
        device.ir_list()


def test_ir_list_name_longer_than_reply_raises():
    payload = bytearray(10)
    payload[3] = 1
    struct.pack_into("<H", payload, 6, 200)
    reports = [make_report(pack_frame(0x64, bytes(payload)))]
    device = FB200Device(MockTransport(reports=reports))
    with pytest.raises(ProtocolError):
        device.ir_list()


def test_ir_index_out_of_range_raises():
    device = FB200Device(MockTransport(reports=[]))
    for bad in (0, -1, 10, 70000):
        with pytest.raises(InvalidArgumentError):
            device.ir_delete(bad)
