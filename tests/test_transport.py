# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

import sys

import pytest

from fb200.errors import CommunicationError, DeviceNotFoundError
from fb200.transport import HidapiTransport, MockTransport


def test_mock_transport_records_writes_and_serves_reads():
    mock = MockTransport(reports=[b"\x03abc"])
    mock.write_report(b"\x01x")
    assert mock.written == [b"\x01x"]
    assert mock.read_report() == b"\x03abc"
    assert mock.read_report() is None


class FakeHidDevice:
    def __init__(self):
        self.opened_path = None
        self.nonblocking = None
        self.writes = []
        self.reads = []
        self.closed = False
        self.fail_open = False

    def open_path(self, path):
        if self.fail_open:
            raise OSError("permission denied")
        self.opened_path = path

    def set_nonblocking(self, value):
        self.nonblocking = value

    def write(self, data):
        self.writes.append(list(data))
        return len(data)

    def read(self, size, timeout_ms):
        return self.reads.pop(0) if self.reads else []

    def close(self):
        self.closed = True


def install_fake_hid(monkeypatch, paths=(b"dev-1",), fail_open=False):
    created = []

    class FakeDeviceFactory:
        @staticmethod
        def device():
            dev = FakeHidDevice()
            dev.fail_open = fail_open
            created.append(dev)
            return dev

    class FakeHidModule:
        @staticmethod
        def enumerate(vid, pid):
            return [{"path": p} for p in paths]

        device = FakeDeviceFactory.device

    monkeypatch.setitem(sys.modules, "hid", FakeHidModule)
    return created


def test_find_path_returns_first_path(monkeypatch):
    install_fake_hid(monkeypatch, paths=(b"dev-1", b"dev-2"))
    assert HidapiTransport.find_path() == b"dev-1"


def test_open_path_raises_when_no_device(monkeypatch):
    install_fake_hid(monkeypatch, paths=())
    with pytest.raises(DeviceNotFoundError, match="FB200 not found"):
        HidapiTransport().open()


def test_open_opens_device_and_io_works(monkeypatch):
    created = install_fake_hid(monkeypatch)
    transport = HidapiTransport().open()
    dev = created[0]
    assert dev.opened_path == b"dev-1"
    assert dev.nonblocking == 0
    transport.write_report(b"\x01abc")
    assert dev.writes == [[1, 97, 98, 99]]
    dev.reads.append(b"\x01xyz")
    assert transport.read_report() == b"\x01xyz"
    assert transport.read_report() is None
    transport.close()
    transport.close()
    assert dev.closed is True


def test_open_failure_is_wrapped_and_state_is_clean(monkeypatch):
    install_fake_hid(monkeypatch, fail_open=True)
    transport = HidapiTransport()
    with pytest.raises(CommunicationError, match="failed to open"):
        transport.open()
    with pytest.raises(CommunicationError, match="not open"):
        transport.write_report(b"\x00")


def test_io_before_open_raises():
    transport = HidapiTransport()
    with pytest.raises(CommunicationError, match="not open"):
        transport.write_report(b"\x00")
    with pytest.raises(CommunicationError, match="not open"):
        transport.read_report()


def test_device_gone_mid_command_is_a_clear_error(monkeypatch):
    """A pedal reset: hidapi raises OSError("read error"/"write error")."""
    created = install_fake_hid(monkeypatch)
    transport = HidapiTransport().open()
    dev = created[0]

    def gone(*_args):
        raise OSError("read error")

    dev.read = gone
    with pytest.raises(CommunicationError, match="reset or was unplugged"):
        transport.read_report()
    dev.write = gone
    with pytest.raises(CommunicationError, match="reset or was unplugged"):
        transport.write_report(b"\x00")


def test_read_timeout_is_never_zero(monkeypatch):
    """hidapi read(n, 0) on a blocking device waits forever: never pass 0."""
    created = install_fake_hid(monkeypatch)
    transport = HidapiTransport().open()
    seen = []
    created[0].read = lambda size, timeout_ms: seen.append(timeout_ms) or []
    transport.read_report(0)
    transport.read_report(-5)
    assert seen == [1, 1]
