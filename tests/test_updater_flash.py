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
    assert result.frames_sent == 0


def test_flash_full_sequence_against_mock():
    reports = [
        make_report(pack_frame(0x03, b"\x01")),
        make_report(pack_frame(0x05, b"\x01")),
        make_report(pack_frame(0x05, b"\x01")),
    ]
    transport = MockTransport(reports=reports)
    result = FirmwareUpdater(transport).flash(make_mr(), dry_run=False)
    assert result.frames_sent == 2
    assert result.bytes_written == 1024
    exit_frame = pack_frame(0xFF)
    exit_report = bytes([len(exit_frame)]) + exit_frame + bytes(64 - 1 - len(exit_frame))
    assert exit_report in transport.written


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
