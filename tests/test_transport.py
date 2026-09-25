import pytest

from fb200.errors import DeviceNotFoundError
from fb200.transport import HidapiTransport, MockTransport


def test_mock_transport_records_writes_and_serves_reads():
    mock = MockTransport(reports=[b"\x03abc"])
    mock.write_report(b"\x01x")
    assert mock.written == [b"\x01x"]
    assert mock.read_report() == b"\x03abc"
    assert mock.read_report() is None


def test_hidapi_transport_open_without_device_raises(monkeypatch):
    monkeypatch.setattr(HidapiTransport, "find_path", staticmethod(lambda vid, pid: None))
    with pytest.raises(DeviceNotFoundError):
        HidapiTransport().open()
