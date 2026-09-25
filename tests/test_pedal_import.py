import struct

from conftest import make_report

from fb200.pedal import FB200Device
from fb200.protocol import FrameReader, pack_frame
from fb200.transport import MockTransport


def make_device(frames: int = 9):
    reports = [make_report(pack_frame(0x62, bytes([0, 0, 0, 1]))) for _ in range(frames)]
    transport = MockTransport(reports=reports)
    return FB200Device(transport), transport


def written_frames(transport: MockTransport) -> list[bytes]:
    reader = FrameReader()
    for report in transport.written:
        reader.feed_report(report)
    frames = []
    while (packet := reader.next_packet()) is not None:
        frames.append(packet)
    return frames


def test_import_sends_nine_frames():
    device, transport = make_device()
    assert device.ir_import(2, "My IR", [0.1] * 1024) is True
    frames = written_frames(transport)
    assert len(frames) == 9
    assert all(frame[0] == 0x61 for frame in frames)


def test_import_metadata_frame_layout():
    device, transport = make_device()
    device.ir_import(2, "My IR", [0.0] * 1024)
    first = written_frames(transport)[0]
    expected = (
        bytes([0x61, 1])
        + struct.pack("<H", 2)
        + bytes([9, 0])
        + struct.pack("<H", len(b"My IR"))
        + b"My IR"
    )
    assert first == expected


def test_import_data_frames_are_512_bytes():
    device, transport = make_device()
    device.ir_import(1, "X", [0.5] * 1024)
    frames = written_frames(transport)[1:]
    assert len(frames) == 8
    for frame in frames:
        payload = frame[1:]
        assert struct.unpack_from("<H", payload, 5)[0] == 512
        assert len(payload) == 7 + 512


def test_import_sanitizes_name_length():
    device, transport = make_device()
    device.ir_import(1, "A" * 80, [0.0] * 1024)
    first = written_frames(transport)[0]
    payload = first[1:]
    assert struct.unpack_from("<H", payload, 5)[0] == 50
