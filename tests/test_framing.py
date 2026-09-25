from fb200.protocol import FrameReader, iter_reports, pack_frame


def test_pack_frame_get_version():
    assert pack_frame(0x00) == bytes.fromhex("aa55010000c8cf")


def test_pack_frame_length_field():
    frame = pack_frame(0x63, bytes([1, 1, 0, 0]))
    assert frame[:2] == b"\xaa\x55"
    assert frame[2:4] == (5).to_bytes(2, "little")


def test_iter_reports_single_report_padding():
    reports = list(iter_reports(pack_frame(0x00)))
    assert len(reports) == 1
    assert reports[0][0] == 7
    assert reports[0][1:8] == pack_frame(0x00)
    assert len(reports[0]) == 64
    assert reports[0][8:] == bytes(56)


def test_iter_reports_chunks_long_frames():
    frame = b"\x01" * 70
    reports = list(iter_reports(frame))
    assert [r[0] for r in reports] == [63, 7]
    assert reports[0][1:64] == frame[:63]
    assert reports[1][1:8] == frame[63:]


def test_frame_reader_reassembles_split_frame():
    frame = pack_frame(0x01, b"hello world, bye")
    r1 = bytes([20]) + frame[:20] + bytes(64 - 1 - 20)
    r2 = bytes([len(frame) - 20]) + frame[20:] + bytes(64 - 1 - (len(frame) - 20))
    reader = FrameReader()
    reader.feed_report(r1)
    assert reader.next_packet() is None
    reader.feed_report(r2)
    assert reader.next_packet() == bytes([0x01]) + b"hello world, bye"


def test_frame_reader_skips_garbage():
    frame = pack_frame(0x01, b"ok")
    report = bytes([len(frame) + 3]) + b"\x00\x11\x22" + frame + bytes(64 - 1 - len(frame) - 3)
    reader = FrameReader()
    reader.feed_report(report)
    assert reader.next_packet() == bytes([0x01]) + b"ok"


def test_frame_reader_rejects_bad_crc():
    frame = bytearray(pack_frame(0x01, b"ok"))
    frame[-1] ^= 0xFF
    report = bytes([len(frame)]) + bytes(frame) + bytes(64 - 1 - len(frame))
    reader = FrameReader()
    reader.feed_report(report)
    assert reader.next_packet() is None


def test_frame_reader_handles_marker_split_across_reports():
    frame = pack_frame(0x01, b"split")
    # first report ends exactly after the leading 0xAA of the marker
    cut = 1  # frame[0:1] is b"\xaa"
    r1 = bytes([cut]) + frame[:cut] + bytes(64 - 1 - cut)
    r2 = bytes([len(frame) - cut]) + frame[cut:] + bytes(64 - 1 - (len(frame) - cut))
    reader = FrameReader()
    reader.feed_report(r1)
    assert reader.next_packet() is None
    reader.feed_report(r2)
    assert reader.next_packet() == bytes([0x01]) + b"split"


def test_frame_reader_recovers_after_bad_crc():
    good = pack_frame(0x01, b"ok")
    bad = bytearray(good)
    bad[-1] ^= 0xFF
    payload = bytes(bad) + good
    report = bytes([len(payload)]) + payload + bytes(64 - 1 - len(payload))
    reader = FrameReader()
    reader.feed_report(report)
    assert reader.next_packet() == bytes([0x01]) + b"ok"


def test_frame_reader_emits_multiple_frames_from_one_report():
    first = pack_frame(0x01, b"one")
    second = pack_frame(0x02, b"two")
    payload = first + second
    report = bytes([len(payload)]) + payload + bytes(64 - 1 - len(payload))
    reader = FrameReader()
    reader.feed_report(report)
    assert reader.next_packet() == bytes([0x01]) + b"one"
    assert reader.next_packet() == bytes([0x02]) + b"two"
    assert reader.next_packet() is None
