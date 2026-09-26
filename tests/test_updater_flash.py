import pytest
from conftest import make_report

from fb200.errors import CommunicationError
from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader
from fb200.protocol import pack_frame
from fb200.transport import MockTransport
from fb200.updater import FirmwareUpdater


def make_mr(size: int = 1024) -> MrFile:
    header = MrHeader(product_tag="FB200", send_cmd=0x02, rec_cmd=0x03, update_block=1)
    tag = MrBlockTag(send_cmd=0x04, rec_cmd=0x05, start_page=0x40)
    return MrFile(header, [MrBlock(tag, bytes(range(256)) * (size // 256))])


def test_flash_dry_run_writes_nothing():
    transport = MockTransport()
    result = FirmwareUpdater(transport).flash(make_mr(), dry_run=True)
    assert result.dry_run is True
    assert transport.written == []
    assert result.write_frames == 0


def test_flash_full_sequence_against_mock():
    reports = [
        make_report(pack_frame(0x03, b"\x01")),
        make_report(pack_frame(0x05, b"\x01")),
        make_report(pack_frame(0x05, b"\x01")),
    ]
    transport = MockTransport(reports=reports)
    result = FirmwareUpdater(transport).flash(make_mr(), dry_run=False)
    assert result.write_frames == 2
    assert result.bytes_written == 1024
    assert make_report(pack_frame(0xFF)) in transport.written


def test_flash_raises_on_unexpected_reply():
    reports = [make_report(pack_frame(0x03, b"\x01")), make_report(pack_frame(0x99, b"\x01"))]
    transport = MockTransport(reports=reports)
    with pytest.raises(CommunicationError):
        FirmwareUpdater(transport).flash(make_mr(), dry_run=False)


def test_flash_reports_progress():
    reports = [
        make_report(pack_frame(0x03, b"\x01")),
        make_report(pack_frame(0x05, b"\x01")),
        make_report(pack_frame(0x05, b"\x01")),
    ]
    calls = []
    FirmwareUpdater(MockTransport(reports=reports)).flash(
        make_mr(), dry_run=False, progress=lambda done, total: calls.append((done, total))
    )
    assert calls == [(1, 2), (2, 2)]


def test_flash_raises_on_wrong_erase_ack():
    transport = MockTransport(reports=[make_report(pack_frame(0x99, b"\x01"))])
    with pytest.raises(CommunicationError, match="erase"):
        FirmwareUpdater(transport).flash(
            make_mr(), dry_run=False, timeout_ms=50, erase_timeout_ms=50
        )


def test_flash_raises_on_missing_erase_ack():
    with pytest.raises(CommunicationError, match="erase"):
        FirmwareUpdater(MockTransport()).flash(
            make_mr(), dry_run=False, timeout_ms=50, erase_timeout_ms=50
        )


def test_flash_does_not_send_exit_after_write_failure():
    reports = [make_report(pack_frame(0x03, b"\x01")), make_report(pack_frame(0x99, b"\x01"))]
    transport = MockTransport(reports=reports)
    with pytest.raises(CommunicationError):
        FirmwareUpdater(transport).flash(make_mr(), dry_run=False, timeout_ms=50)
    assert make_report(pack_frame(0xFF)) not in transport.written


def test_flash_uses_longer_erase_timeout():
    class SlowEraseTransport(MockTransport):
        slow = True

        def read_report(self, timeout_ms=500):
            if self.slow:
                if timeout_ms >= 200:
                    self.slow = False
                    return super().read_report(timeout_ms)
                return None
            return super().read_report(timeout_ms)

    reports = [
        make_report(pack_frame(0x03, b"\x01")),
        make_report(pack_frame(0x05, b"\x01")),
        make_report(pack_frame(0x05, b"\x01")),
    ]
    result = FirmwareUpdater(SlowEraseTransport(reports=reports)).flash(
        make_mr(), dry_run=False, timeout_ms=50, erase_timeout_ms=300
    )
    assert result.write_frames == 2


def test_flash_routes_timeouts(monkeypatch):
    from fb200.protocol import FrameReader

    seen = []
    original = FrameReader.read_packet

    def spy(self, transport, timeout_ms=1500):
        seen.append(timeout_ms)
        return original(self, transport, timeout_ms)

    monkeypatch.setattr(FrameReader, "read_packet", spy)
    reports = [
        make_report(pack_frame(0x03, b"\x01")),
        make_report(pack_frame(0x05, b"\x01")),
        make_report(pack_frame(0x05, b"\x01")),
    ]
    FirmwareUpdater(MockTransport(reports=reports)).flash(
        make_mr(), dry_run=False, timeout_ms=111, erase_timeout_ms=222
    )
    assert seen == [222, 111, 111]


def test_wait_for_device_returns_none_after_timeout(monkeypatch):
    from fb200 import updater as updater_mod
    from fb200.transport import HidapiTransport

    monkeypatch.setattr(HidapiTransport, "find_path", staticmethod(lambda vid, pid: None))
    assert updater_mod.wait_for_device(1, 2, timeout_s=0.05, poll_s=0.001) is None


def test_wait_for_device_returns_path_when_found(monkeypatch):
    from fb200 import updater as updater_mod
    from fb200.transport import HidapiTransport

    monkeypatch.setattr(HidapiTransport, "find_path", staticmethod(lambda vid, pid: b"dev"))
    assert updater_mod.wait_for_device(1, 2, timeout_s=0.05) == b"dev"
