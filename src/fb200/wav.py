# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Convert WAV files into the FB200's 1024-sample float32 IR format.

With no processing options this matches the official editor: decode to
44.1 kHz, take channel 0, truncate or zero-pad to 1024 samples. The open
firmware plays the first 512 of them (``CAB_TAPS``).

`process_ir` is the one entry point (the CLI and other tools wrap it). It adds
optional steps: channel choice, onset trim, low/high cut, blend of a second IR,
minimum phase, truncation with a fade-out and peak normalization. Everything is
pure Python except ``minphase``, which needs numpy (``pip install
'fb200-tools[ir]'``).
"""

from __future__ import annotations

import math
import struct
from fractions import Fraction
from pathlib import Path

from fb200.errors import WavError

IR_LENGTH = 1024
IR_SAMPLE_RATE = 44100
MAX_TAPS = 4096
CHANNELS = ("left", "right", "sum")

# Kaiser windowed-sinc resampler: 48 zero crossings per side, beta 9 (about
# 90 dB stopband), cutoff at 92 % of the lower Nyquist. The stopband starts
# below the new Nyquist, so content above it is attenuated, not folded back.
_SINC_ZEROS = 48
_KAISER_BETA = 9.0
_CUTOFF = 0.92


def _read_wav(source) -> tuple[int, int, int, int, bytes]:
    if isinstance(source, (bytes, bytearray)):
        raw = bytes(source)
    else:
        path = Path(source)
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




def read_wav(source) -> tuple[list[list[float]], int]:
    """Decode a WAV file (path or bytes) into one float list per channel, and its rate."""
    audio_format, channels, rate, bits, data = _read_wav(source)
    decoded = _decode(data, audio_format, bits)
    return [decoded[c::channels] for c in range(channels)], rate


def write_wav(path, frames, sample_rate: int = IR_SAMPLE_RATE) -> None:
    """Write 32-bit float WAV (format 3), readable by any IR loader. ``frames``:
    samples (mono), or one tuple per frame (a channel each)."""
    rows = [f if isinstance(f, (tuple, list)) else (f,) for f in frames]
    channels = len(rows[0]) if rows else 1
    count = len(rows)
    data = struct.pack(f"<{count * channels}f", *(v for row in rows for v in row))
    fmt = struct.pack("<HHIIHHH", 3, channels, sample_rate, sample_rate * 4 * channels,
                      4 * channels, 32, 0)
    body = b"WAVE"
    body += b"fmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"fact" + struct.pack("<II", 4, count)
    body += b"data" + struct.pack("<I", len(data)) + data
    try:
        Path(path).write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
    except OSError as exc:
        raise WavError(f"cannot write {path}: {exc}") from exc


def _select_channel(chans: list[list[float]], channel: str) -> list[float]:
    if channel == "left":
        return chans[0]
    if channel == "right":
        return chans[1] if len(chans) > 1 else chans[0]
    if channel == "sum":
        n = min(len(c) for c in chans)
        k = 1.0 / len(chans)
        return [sum(c[i] for c in chans) * k for i in range(n)]
    raise WavError(f"invalid channel {channel!r} (use {', '.join(CHANNELS)})")


def _bessel_i0(x: float) -> float:
    total = term = 1.0
    k = 0
    while term > 1e-17 * total:
        k += 1
        term *= (x / (2 * k)) ** 2
        total += term
    return total


def resample(samples, src_rate: int, dst_rate: int, max_out: int | None = None) -> list[float]:
    """Kaiser windowed-sinc resampler for any rate pair.

    Output length is ``round(len * dst / src)``, cut at ``max_out`` when given
    (the IR is truncated later, so only the head is computed). Samples outside
    the input are zero.
    """
    if src_rate <= 0:
        raise WavError("invalid source sample rate")
    if dst_rate <= 0:
        raise WavError("invalid destination sample rate")
    samples = list(samples)
    n = len(samples)
    out_len = round(n * dst_rate / src_rate)
    if max_out is not None:
        out_len = min(out_len, max_out)
    if src_rate == dst_rate or not samples:
        return samples[:out_len]
    ratio = Fraction(dst_rate, src_rate)
    up, down = ratio.numerator, ratio.denominator
    fc = _CUTOFF * min(1.0, dst_rate / src_rate)
    half = _SINC_ZEROS / fc
    i0_beta = _bessel_i0(_KAISER_BETA)
    pad = math.ceil(half) + 2
    x = [0.0] * pad + samples + [0.0] * pad
    kernels: dict[int, tuple[int, list[float]]] = {}
    out = []
    for i in range(out_len):
        base, rem = divmod(i * down, up)
        kernel = kernels.get(rem)
        if kernel is None:
            frac = rem / up
            lo = math.ceil(frac - half)
            hi = math.floor(frac + half)
            weights = []
            for j in range(lo, hi + 1):
                u = frac - j
                arg = fc * u
                s = 1.0 if arg == 0 else math.sin(math.pi * arg) / (math.pi * arg)
                r = u / half
                w = _bessel_i0(_KAISER_BETA * math.sqrt(max(0.0, 1.0 - r * r))) / i0_beta
                weights.append(s * w)
            norm = 1.0 / sum(weights)
            kernel = (lo, [w * norm for w in weights])
            kernels[rem] = kernel
        lo, weights = kernel
        start = base + lo + pad
        out.append(math.fsum(map(float.__mul__, weights, x[start:start + len(weights)])))
    return out


def _biquad(x: list[float], b: tuple[float, float, float],
            a: tuple[float, float, float]) -> list[float]:
    b0, b1, b2 = (v / a[0] for v in b)
    a1, a2 = a[1] / a[0], a[2] / a[0]
    x1 = x2 = y1 = y2 = 0.0
    out = []
    for v in x:
        y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, y
        out.append(y)
    return out


def butterworth2(x: list[float], kind: str, freq: float, sample_rate: int) -> list[float]:
    """2nd-order Butterworth low or high pass (bilinear, prewarped; -3 dB at ``freq``)."""
    if not 0 < freq < sample_rate / 2:
        raise WavError(f"cut frequency {freq} Hz must be between 0 and {sample_rate / 2} Hz")
    w0 = 2 * math.pi * freq / sample_rate
    cos_w, alpha = math.cos(w0), math.sin(w0) / math.sqrt(2)
    a = (1 + alpha, -2 * cos_w, 1 - alpha)
    if kind == "low":
        b = ((1 - cos_w) / 2, 1 - cos_w, (1 - cos_w) / 2)
    elif kind == "high":
        b = ((1 + cos_w) / 2, -(1 + cos_w), (1 + cos_w) / 2)
    else:
        raise WavError(f"invalid filter kind {kind!r}")
    return _biquad(x, b, a)


def onset(x: list[float], threshold_db: float = -60.0) -> int:
    """Index of the first sample at or above ``threshold_db`` relative to the peak."""
    peak = max((abs(v) for v in x), default=0.0)
    if peak == 0.0:
        return 0
    limit = peak * 10 ** (threshold_db / 20)
    return next(i for i, v in enumerate(x) if abs(v) >= limit)


def align_lag(a: list[float], b: list[float], search: int = 32, window: int = 256) -> int:
    """Delay (samples) to apply to ``b`` so it lines up with ``a``.

    Starts from the onset difference, then takes the lag of the largest
    cross-correlation in +-``search`` samples over a ``window`` from the onset.
    """
    oa, ob = onset(a), onset(b)
    d0 = oa - ob
    start = max(0, oa - 16)
    seg = a[start:start + window]
    best, best_score = d0, -math.inf
    for d in range(d0 - search, d0 + search + 1):
        score = 0.0
        for m, va in enumerate(seg):
            k = start + m - d
            if 0 <= k < len(b):
                score += va * b[k]
        if score > best_score:
            best, best_score = d, score
    return best


def blend_irs(a: list[float], b: list[float], mix: float) -> list[float]:
    """``(1 - mix) * a + mix * b``, with ``b`` aligned to ``a`` by `align_lag`."""
    if not 0.0 <= mix <= 1.0:
        raise WavError(f"blend mix {mix} must be 0..1")
    d = align_lag(a, b)
    n = max(len(a), len(b) + d)
    out = []
    for i in range(n):
        va = a[i] if i < len(a) else 0.0
        k = i - d
        vb = b[k] if 0 <= k < len(b) else 0.0
        out.append((1.0 - mix) * va + mix * vb)
    return out


def minimum_phase(x: list[float]) -> list[float]:
    """Cepstral minimum-phase version of ``x`` (same magnitude response). Needs numpy."""
    try:
        import numpy as np
    except ImportError as exc:
        raise WavError(
            "minimum phase needs numpy: pip install 'fb200-tools[ir]'"
        ) from exc
    n = len(x)
    if n == 0:
        return []
    # a long FFT keeps cepstral aliasing small (about 1e-6 of the peak magnitude)
    nfft = 1 << max(18, (32 * n - 1).bit_length())
    mag = np.abs(np.fft.fft(np.asarray(x, dtype=float), nfft))
    peak = mag.max()
    if peak == 0.0:
        return list(x)
    cep = np.fft.ifft(np.log(np.maximum(mag, peak * 1e-10))).real
    fold = np.zeros(nfft)
    fold[0] = 1.0
    fold[1:nfft // 2] = 2.0
    fold[nfft // 2] = 1.0
    y = np.fft.ifft(np.exp(np.fft.fft(cep * fold))).real[:n]
    return [float(v) for v in y]


def fade_out(x: list[float], length: int) -> list[float]:
    """Half-Hann fade over the last ``length`` samples; the last sample becomes 0."""
    out = list(x)
    length = min(length, len(out))
    start = len(out) - length
    for j in range(length):
        out[start + j] *= 0.5 * (1.0 + math.cos(math.pi * (j + 1) / length))
    return out


def _load(source, channel: str, sample_rate: int, trim: bool, trim_db: float,
          length: int) -> list[float]:
    chans, rate = read_wav(source)
    x = _select_channel(chans, channel)
    lead = onset(x, trim_db) * sample_rate // rate + 1 if trim else 0
    # the head that can reach the output, with room for filters and minimum phase
    return resample(x, rate, sample_rate, max_out=lead + 2 * length + 64)


def process_ir(source, *, channel: str = "left", taps: int | None = None,
               trim: bool = False, trim_db: float = -60.0, preroll: int = 8,
               lowcut: float | None = None, highcut: float | None = None,
               blend: tuple | None = None, minphase: bool = False,
               normalize: bool = False, sample_rate: int = IR_SAMPLE_RATE) -> list[float]:
    """Load a WAV IR (path or bytes) and turn it into ``sample_rate`` float taps.

    Defaults give the official editor's result: channel 0, resampled, raw
    truncate or zero-pad to `IR_LENGTH` (1024) samples.

    Options, in processing order:
      channel     "left" (channel 0), "right" or "sum" (mean of all channels)
      blend       (source, mix): mix a second IR in, aligned to the first, 0..1
      lowcut      2nd-order Butterworth high pass at this frequency (Hz)
      highcut     2nd-order Butterworth low pass at this frequency (Hz)
      trim        cut silence before the onset (first sample >= ``trim_db`` rel.
                  peak), keeping ``preroll`` samples
      minphase    cepstral minimum phase (needs numpy)
      taps        output length 1..4096; content cut at the end gets a half-Hann
                  fade over the last taps/8 samples
      normalize   scale the peak to 1.0
    """
    if taps is not None and not 1 <= taps <= MAX_TAPS:
        raise WavError(f"taps must be 1..{MAX_TAPS}")
    if preroll < 0:
        raise WavError("preroll must be >= 0")
    length = taps if taps is not None else IR_LENGTH
    y = _load(source, channel, sample_rate, trim, trim_db, length)
    if blend is not None:
        other, mix = blend
        y = blend_irs(y, _load(other, channel, sample_rate, trim, trim_db, length), mix)
    if lowcut is not None:
        y = butterworth2(y, "high", lowcut, sample_rate)
    if highcut is not None:
        y = butterworth2(y, "low", highcut, sample_rate)
    if trim:
        y = y[max(0, onset(y, trim_db) - preroll):]
    if minphase:
        # room for the minimum-phase tail, which may exceed a short input
        y = minimum_phase(y + [0.0] * (length - len(y)))
    if len(y) > length:
        cut_content = any(v != 0.0 for v in y[length:])
        y = y[:length]
        if taps is not None and cut_content:
            y = fade_out(y, max(1, length // 8))
    y = y + [0.0] * (length - len(y))
    if normalize:
        peak = max(abs(v) for v in y)
        if peak > 0.0:
            y = [v / peak for v in y]
    return y


def wav_to_ir(path, length: int = IR_LENGTH, sample_rate: int = IR_SAMPLE_RATE) -> list[float]:
    """Official-editor conversion: channel 0, resampled, truncated or zero-padded."""
    if length <= 0:
        raise WavError("invalid IR length")
    chans, rate = read_wav(path)
    head = resample(chans[0], rate, sample_rate, max_out=length)
    return head + [0.0] * (length - len(head))
