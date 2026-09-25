import struct
import wave

import pytest

from fb200.wav import wav_to_ir


def write_wav(path, samples, channels=1, rate=44100):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(s * 32767)) for s in samples))


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
    with pytest.raises(ValueError):
        wav_to_ir(path)
