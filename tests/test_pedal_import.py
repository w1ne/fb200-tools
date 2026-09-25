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


def test_import_data_frame_bytes_are_float32_le():
    device, transport = make_device()
    device.ir_import(1, "X", [0.5] * 1024)
    frames = written_frames(transport)
    assert frames[0][0] == 0x61
    first_data = frames[1]
    assert first_data[0] == 0x61
    assert first_data[8:12] == struct.pack("<f", 0.5)


def test_import_short_sample_list_is_padded_to_nine_frames():
    device, transport = make_device()
    device.ir_import(1, "X", [0.25] * 10)
    frames = written_frames(transport)
    assert len(frames) == 9
    data = b"".join(bytes(frame[8:]) for frame in frames[1:])
    assert data[:40] == struct.pack("<10f", *([0.25] * 10))
    assert data[40:] == bytes(len(data) - 40)


def test_import_long_sample_list_is_truncated_to_1024():
    device, transport = make_device()
    assert device.ir_import(1, "X", [0.5] * 2000) is True
    frames = written_frames(transport)
    assert len(frames) == 9
    data = b"".join(bytes(frame[8:]) for frame in frames[1:])
    assert len(data) == 4096
    assert data == struct.pack("<1024f", *([0.5] * 1024))


def test_import_reports_progress_per_frame():
    device, _ = make_device()
    calls = []
    device.ir_import(1, "X", [0.0] * 1024, progress=lambda done, total: calls.append((done, total)))
    assert calls == [(i, 9) for i in range(1, 10)]
