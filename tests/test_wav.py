import struct
import wave

import pytest

from fb200.errors import WavError
from fb200.wav import _decode, wav_to_ir


def write_wav(path, samples, channels=1, rate=44100):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(s * 32767)) for s in samples))


def build_riff(fmt_chunk: bytes, data_chunk: bytes, data_size: int | None = None) -> bytes:
    body = b"WAVE" + b"fmt " + len(fmt_chunk).to_bytes(4, "little") + fmt_chunk
    size = len(data_chunk) if data_size is None else data_size
    body += b"data" + size.to_bytes(4, "little") + data_chunk
    return b"RIFF" + len(body).to_bytes(4, "little") + body


VALID_FMT = struct.pack("<HHIIHH", 1, 1, 44100, 88200, 2, 16)


def test_pads_and_truncates(tmp_path):
    path = tmp_path / "ir.wav"
    write_wav(path, [0.5] * 10)
    ir = wav_to_ir(path)
    assert len(ir) == 1024
    assert ir[0] == pytest.approx(0.5, abs=1e-3)
    assert ir[9] == pytest.approx(0.5, abs=1e-3)
    assert ir[10] == 0.0
    assert ir[-1] == 0.0


def test_truncates_long_files(tmp_path):
    path = tmp_path / "long.wav"
    write_wav(path, [0.25] * 5000)
    ir = wav_to_ir(path)
    assert len(ir) == 1024
    assert all(v == pytest.approx(0.25, abs=1e-3) for v in ir)


def test_takes_first_channel(tmp_path):
    path = tmp_path / "stereo.wav"
    interleaved = []
    for _ in range(10):
        interleaved.extend([0.5, -0.5])
    write_wav(path, interleaved, channels=2)
    assert wav_to_ir(path)[0] == pytest.approx(0.5, abs=1e-3)


def test_resamples_22050_to_44100(tmp_path):
    path = tmp_path / "low.wav"
    write_wav(path, [0.3] * 100, rate=22050)
    ir = wav_to_ir(path)
    assert len(ir) == 1024
    assert ir[100] == pytest.approx(0.3, abs=1e-3)


def test_rejects_non_wav(tmp_path):
    path = tmp_path / "noise.bin"
    path.write_bytes(b"not a wav at all")
    with pytest.raises(WavError):
        wav_to_ir(path)


def test_decode_24bit_sign_extension():
    data = (
        (-8388608).to_bytes(3, "little", signed=True)
        + (0).to_bytes(3, "little", signed=True)
        + (8388607).to_bytes(3, "little", signed=True)
    )
    out = _decode(data, 1, 24)
    assert out[0] == -1.0
    assert out[1] == 0.0
    assert out[2] == pytest.approx(1.0, abs=1e-6)


def test_decode_float32():
    data = struct.pack("<2f", -0.5, 0.25)
    assert _decode(data, 3, 32) == [-0.5, 0.25]


def test_decode_unsupported_format_raises():
    with pytest.raises(WavError):
        _decode(b"\x00\x00", 7, 16)


def test_short_fmt_chunk_raises(tmp_path):
    path = tmp_path / "short-fmt.wav"
    path.write_bytes(build_riff(b"\x01\x00\x01\x00", b"\x00\x00"))
    with pytest.raises(WavError):
        wav_to_ir(path)


def test_truncated_data_chunk_raises(tmp_path):
    path = tmp_path / "trunc.wav"
    path.write_bytes(build_riff(VALID_FMT, b"\x00\x00", data_size=1000))
    with pytest.raises(WavError):
        wav_to_ir(path)


def test_empty_data_chunk_raises(tmp_path):
    path = tmp_path / "empty.wav"
    path.write_bytes(build_riff(VALID_FMT, b""))
    with pytest.raises(WavError):
        wav_to_ir(path)


def test_missing_file_raises_wav_error(tmp_path):
    with pytest.raises(WavError):
        wav_to_ir(tmp_path / "missing.wav")
