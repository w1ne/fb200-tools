"""Convert WAV files into the FB200's 1024-sample float32 IR format.

Matches the official editor: decode to 44.1 kHz, take channel 0, truncate or
zero-pad to 1024 samples.
"""

from __future__ import annotations

import struct
from pathlib import Path

from fb200.errors import WavError

IR_LENGTH = 1024
IR_SAMPLE_RATE = 44100


def _read_wav(path: Path) -> tuple[int, int, int, int, bytes]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise WavError(f"cannot read {path}: {exc}") from exc
    if len(raw) < 12 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise WavError("not a RIFF/WAVE file")
    pos = 12
    fmt = None
    data = None
    while pos + 8 <= len(raw):
        chunk_id = raw[pos:pos + 4]
        size = struct.unpack_from("<I", raw, pos + 4)[0]
        body = raw[pos + 8:pos + 8 + size]
        if pos + 8 + size > len(raw):
            raise WavError("truncated RIFF chunk")
        if chunk_id == b"fmt ":
            fmt = body
        elif chunk_id == b"data":
            data = body
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise WavError("missing fmt or data chunk")
    if len(fmt) < 16:
        raise WavError("malformed fmt chunk")
    audio_format, channels, rate, _byte_rate, _block_align, bits = struct.unpack_from(
        "<HHIIHH", fmt, 0
    )
    if audio_format == 0xFFFE and len(fmt) >= 26:
        audio_format = struct.unpack_from("<H", fmt, 24)[0]
    if channels < 1:
        raise WavError("invalid channel count")
    if rate <= 0:
        raise WavError("invalid sample rate")
    if bits <= 0:
        raise WavError("invalid bit depth")
    if not data:
        raise WavError("empty data chunk")
    return audio_format, channels, rate, bits, data


def _decode(data: bytes, audio_format: int, bits: int) -> list[float]:
    if audio_format == 3 and bits == 32:
        count = len(data) // 4
        return list(struct.unpack_from(f"<{count}f", data))
    if audio_format != 1:
        raise WavError(f"unsupported WAV format {audio_format}")
    if bits == 8:
        return [(b - 128) / 128.0 for b in data]
    if bits == 16:
        count = len(data) // 2
        return [v / 32768.0 for v in struct.unpack_from(f"<{count}h", data)]
    if bits == 24:
        out = []
        for i in range(0, len(data) - 2, 3):
            value = int.from_bytes(data[i:i + 3], "little", signed=True)
            out.append(value / 8388608.0)
        return out
    if bits == 32:
        count = len(data) // 4
        return [v / 2147483648.0 for v in struct.unpack_from(f"<{count}i", data)]
    raise WavError(f"unsupported bit depth {bits}")


def _resample_linear(samples: list[float], src_rate: int, dst_rate: int) -> list[float]:
    if src_rate <= 0:
        raise WavError("invalid source sample rate")
    if dst_rate <= 0:
        raise WavError("invalid destination sample rate")
    if src_rate == dst_rate or not samples:
        return samples
    out_len = round(len(samples) * dst_rate / src_rate)
    out = []
    for i in range(out_len):
        pos = i * src_rate / dst_rate
        i0 = int(pos)
        frac = pos - i0
        a = samples[i0] if i0 < len(samples) else 0.0
        b = samples[min(i0 + 1, len(samples) - 1)]
        out.append(a + (b - a) * frac)
    return out


def wav_to_ir(path, length: int = IR_LENGTH, sample_rate: int = IR_SAMPLE_RATE) -> list[float]:
    if length <= 0:
        raise WavError("invalid IR length")
    audio_format, channels, rate, bits, data = _read_wav(Path(path))
    decoded = _decode(data, audio_format, bits)
    channel0 = _resample_linear(decoded[0::channels], rate, sample_rate)
    out = [0.0] * length
    for i in range(min(len(channel0), length)):
        out[i] = channel0[i]
    return out
