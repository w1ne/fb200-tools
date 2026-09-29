import pytest
from conftest import make_report, version_payload

from fb200.errors import CommunicationError
from fb200.pedal import FB200Device, _cstr, sanitize_ir_name
from fb200.protocol import pack_frame
from fb200.transport import MockTransport


def test_info_parses_version_reply(version_transport):
    info = FB200Device(version_transport).info()
    assert info.product == "FB200"
    assert info.app_version == "V1.0.0"
    assert info.firmware_version == "V1.0.1"
    assert info.bluetooth_version == "V1.0.0"
    assert info.hardware_rev == "A"


def test_info_sends_get_version_frame(version_transport):
    FB200Device(version_transport).info()
    expected = pack_frame(0x00)
    assert version_transport.written[0] == bytes([len(expected)]) + expected + bytes(64 - 1 - len(expected))


def test_request_skips_non_matching_packets():
    reports = [make_report(pack_frame(0x63, bytes(16))), make_report(pack_frame(0x01, version_payload()))]
    assert FB200Device(MockTransport(reports=reports)).info().product == "FB200"


def test_request_timeout_raises():
    with pytest.raises(CommunicationError, match="no reply"):
        FB200Device(MockTransport()).request(0x00, expect=0x01, timeout_ms=50)


def test_request_ignores_stale_buffered_packets():
    stale = pack_frame(0x64, b"\x00" * 4)
    fresh = pack_frame(0x64, b"\x00" * 16)
    combined = make_report(pack_frame(0x01, b"\x00" * 30) + stale)
    transport = MockTransport(reports=[combined, make_report(fresh)])
    device = FB200Device(transport)
    device.request(0x00, expect=0x01)  # buffers both packets, returns 0x01
    packet = device.request(0x63, expect=0x64)
    assert packet[0] == 0x64
    assert len(packet) == 17  # the fresh reply, not the stale 5-byte one


def test_enter_bootloader_sends_c1_frame():
    transport = MockTransport()
    FB200Device(transport).enter_bootloader()
    expected = pack_frame(0xC1)
    assert transport.written[0] == bytes([len(expected)]) + expected + bytes(64 - 1 - len(expected))


def test_cstr_handles_short_and_nul():
    assert _cstr(b"", 0, 10) == ""
    assert _cstr(b"ab\x00zz", 0, 6) == "ab"


def test_sanitize_ir_name_edges():
    assert sanitize_ir_name("") == "untitled"
    assert sanitize_ir_name("\x01\x02") == "untitled"
    assert sanitize_ir_name("A" * 80) == "A" * 50
    assert sanitize_ir_name("café!") == "caf !"


class _DropFirst(MockTransport):
    """Loses the first request (the pedal was busy), answers the second."""

    def __init__(self) -> None:
        super().__init__()
        self.requests = 0

    def write_report(self, report: bytes) -> None:
        super().write_report(report)
        self.requests += 1
        if self.requests == 2:
            self.queue(make_report(pack_frame(0x01, version_payload())))


def test_queries_retry_once_after_no_reply(monkeypatch):
    from fb200 import pedal

    monkeypatch.setattr(pedal.FB200Device, "_request_once", _fast(pedal.FB200Device._request_once))
    t = _DropFirst()
    assert FB200Device(t).info().product == "FB200"
    assert t.requests == 2


def test_no_retry_on_a_dead_device():
    import time

    class Dead(MockTransport):
        def write_report(self, report: bytes) -> None:
            raise CommunicationError("HID write failed (the pedal reset or was unplugged?)")

    t0 = time.monotonic()
    with pytest.raises(CommunicationError, match="unplugged"):
        FB200Device(Dead()).info()
    assert time.monotonic() - t0 < 0.5


def _fast(fn):
    def call(self, fn_, data, expect, timeout_ms):
        return fn(self, fn_, data, expect, min(timeout_ms, 50))
    return call
