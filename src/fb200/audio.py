"""Audio tests over the pedal's USB audio (UAC2, 2 in / 2 out, 44.1 kHz).

Pure analysis on numpy arrays (tools/measure.py uses it too) and device I/O
through sounddevice. Needs numpy; the device I/O also needs sounddevice (both
in the `dev` extra).
"""

from __future__ import annotations

import numpy as np

from fb200.errors import DeviceNotFoundError, InvalidArgumentError

FS = 44100
DEVICE_NAME = "FB200"
SWEEP_F1, SWEEP_F2 = 20.0, 20000.0
BANDS = (63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000)   # octave centres, Hz
SILENT_DBFS = -80.0


def db(x: float) -> float:
    return float(20.0 * np.log10(max(float(x), 1e-12)))


# ---------------------------------------------------------------- signals


def exp_sweep(fs: int, f1: float, f2: float, dur: float, amp: float) -> np.ndarray:
    """Farina exponential sweep with short fades."""
    n = int(fs * dur)
    t = np.arange(n) / fs
    k = np.log(f2 / f1)
    x = amp * np.sin(2.0 * np.pi * f1 * dur / k * (np.exp(t / dur * k) - 1.0))
    fade = min(int(0.01 * fs), n // 8)
    if fade:
        ramp = np.linspace(0.0, 1.0, fade)
        x[:fade] *= ramp
        x[-fade:] *= ramp[::-1]
    return x


def make_signal(kind: str, seconds: float, freq: float = 1000.0, level_dbfs: float = -20.0,
                wav_path: str | None = None, fs: int = FS) -> np.ndarray:
    """A mono test signal: sine / sweep (peak level_dbfs), noise (RMS
    level_dbfs), or channel 0 of a WAV file (as is)."""
    amp = 10.0 ** (level_dbfs / 20.0)
    n = int(fs * seconds)
    if kind == "sine":
        return amp * np.sin(2.0 * np.pi * freq * np.arange(n) / fs)
    if kind == "sweep":
        return exp_sweep(fs, SWEEP_F1, SWEEP_F2, seconds, amp)
    if kind == "noise":
        return amp * np.random.default_rng(0).standard_normal(n)
    if kind == "wav":
        if not wav_path:
            raise InvalidArgumentError("signal 'wav' needs wav_path")
        from fb200.wav import read_wav

        return np.asarray(read_wav(wav_path, fs), dtype=np.float64)
    raise InvalidArgumentError(f"unknown signal {kind!r} (sine, sweep, noise, wav)")


# ---------------------------------------------------------------- analysis


def levels(x: np.ndarray) -> dict:
    return {"rms_dbfs": round(db(np.sqrt(np.mean(x ** 2))), 2),
            "peak_dbfs": round(db(np.max(np.abs(x))), 2)}


def thd(rec: np.ndarray, fs: int, freq: float, harmonics: int = 10) -> dict:
    """Fundamental and THD (harmonics 2..N below Nyquist) of a steady sine."""
    n = min(len(rec), 1 << 16)
    seg = rec[-n:]
    win = np.hanning(n)
    spec = np.abs(np.fft.rfft(seg * win)) ** 2
    binhz = fs / n
    lo, hi = int(freq * 0.9 / binhz), int(freq * 1.1 / binhz) + 1
    k0 = lo + int(np.argmax(spec[lo:hi]))
    # parabolic peak interpolation on the log spectrum
    a, b, c = (np.log(spec[k0 + d] + 1e-30) for d in (-1, 0, 1))
    delta = 0.5 * (a - c) / (a - 2 * b + c) if a - 2 * b + c else 0.0
    f0 = (k0 + delta) * binhz

    def power(f: float) -> float:
        k = round(f / binhz)
        return float(np.sum(spec[max(0, k - 4):k + 5]))

    p1 = power(f0)
    ph = sum(power(h * f0) for h in range(2, harmonics + 1) if h * f0 < fs / 2 - 5 * binhz)
    ratio = np.sqrt(ph / p1) if p1 > 0 else float("inf")
    amp = 2.0 * np.sqrt(np.max(spec[k0 - 1:k0 + 2])) / np.sum(win)
    return {"fundamental_hz": round(f0, 2), "fundamental_dbfs": round(db(amp), 2),
            "thd_pct": round(100.0 * ratio, 4), "thd_db": round(db(ratio), 2)}


def thd_plus_n(rec: np.ndarray, fs: int, freq: float) -> dict:
    """THD+N and fundamental level for a steady sine capture."""
    n = 1 << 16
    seg = rec[-n:] if len(rec) >= n else rec
    w = seg * np.hanning(len(seg))
    spec = np.abs(np.fft.rfft(w)) ** 2
    freqs = np.fft.rfftfreq(len(seg), 1.0 / fs)
    fund = int(np.argmin(np.abs(freqs - freq)))
    half = 3
    fund_power = float(np.sum(spec[max(0, fund - half):fund + half + 1]))
    total = float(np.sum(spec))
    rest = max(total - fund_power, 0.0)
    thdn = np.sqrt(rest / fund_power) if fund_power > 0 else float("inf")
    # One-sided amplitude: 2*peak/coherent gain (the Hann lobe spreads the
    # power over several bins, so use the peak bin, not the summed power).
    peak = float(np.max(np.sqrt(spec[max(0, fund - 3):fund + 4])))
    ref = 2.0 * peak / np.sum(np.hanning(len(seg)))
    return {
        "fundamental_dbfs": 20.0 * np.log10(ref + 1e-12),
        "thd_plus_n_pct": 100.0 * thdn,
        "thd_plus_n_db": 20.0 * np.log10(thdn + 1e-12),
    }


def sweep_response(rec: np.ndarray, fs: int, f1: float, f2: float, dur: float,
                   amp: float) -> tuple[np.ndarray, np.ndarray]:
    """Frequency response via regularized frequency-domain deconvolution.

    The excitation is known exactly, so dividing its spectrum out recovers the
    system impulse response without inverse-filter approximations. The
    regularization keeps the silent band edges from blowing up.
    """
    x = exp_sweep(fs, f1, f2, dur, amp)
    n = 1 << int(np.ceil(np.log2(len(x) + len(rec))))
    X = np.fft.rfft(x, n)
    R = np.fft.rfft(rec, n)
    H = R * np.conj(X) / (np.abs(X) ** 2 + 1e-3)
    ir = np.fft.irfft(H, n)
    start = len(x) - 1
    win = ir[start:start + int(1.0 * fs)]
    n_fft = 1 << 15
    spec = np.fft.rfft(win * np.hanning(len(win)), n=n_fft)
    freqs = np.fft.rfftfreq(n_fft, 1.0 / fs)
    mag = 20.0 * np.log10(np.abs(spec) + 1e-12)
    band = (freqs > 500) & (freqs < 4000)
    mag -= np.mean(mag[band])
    return freqs, mag


def band_levels(freqs: np.ndarray, power: np.ndarray, fs: int) -> dict:
    """Mean power per octave band in dB, relative to the 1 kHz band."""
    out = {}
    for fc in BANDS:
        if fc * np.sqrt(2) > fs / 2:
            continue
        sel = (freqs >= fc / np.sqrt(2)) & (freqs < fc * np.sqrt(2))
        out[fc] = 10.0 * np.log10(np.mean(power[sel]) + 1e-30)
    ref = out.get(1000, 0.0)
    return {f"{fc}": round(v - ref, 1) for fc, v in out.items()}


def sweep_bands(rec: np.ndarray, fs: int, dur: float, amp: float) -> dict:
    """Octave-band response (dB re 1 kHz) of a capture of make_signal('sweep')."""
    freqs, mag = sweep_response(rec, fs, SWEEP_F1, SWEEP_F2, dur, amp)
    return band_levels(freqs, 10.0 ** (mag / 10.0), fs)


def noise_bands(rec: np.ndarray, fs: int) -> dict:
    """Octave-band response (dB re 1 kHz) of a capture of white noise."""
    n = 4096
    segs = [rec[i:i + n] for i in range(0, len(rec) - n + 1, n // 2)]
    if not segs:
        raise InvalidArgumentError("capture too short for a spectrum")
    win = np.hanning(n)
    power = np.mean([np.abs(np.fft.rfft(s * win)) ** 2 for s in segs], axis=0)
    return band_levels(np.fft.rfftfreq(n, 1.0 / fs), power, fs)


def latency_ms(play: np.ndarray, rec: np.ndarray, fs: int) -> float:
    """Round-trip latency via cross-correlation of an impulse."""
    corr = np.correlate(rec, play, mode="full")
    lag = int(np.argmax(np.abs(corr))) - (len(play) - 1)
    return 1000.0 * lag / fs


def analyze(rec: np.ndarray, kind: str, fs: int = FS, freq: float = 1000.0,
            seconds: float = 0.0, level_dbfs: float = -20.0) -> dict:
    """Levels per channel, plus THD (sine) or an octave-band response (sweep,
    noise) of the left channel. rec: (frames, channels)."""
    rec = np.atleast_2d(np.asarray(rec, dtype=np.float64).T).T
    out = {"left": levels(rec[:, 0])}
    if rec.shape[1] > 1:
        out["right"] = levels(rec[:, 1])
    x = rec[:, 0]
    if out["left"]["peak_dbfs"] < SILENT_DBFS:
        out["warning"] = "the capture is silent"
        return out
    if kind == "sine":
        out["thd"] = thd(x, fs, freq)
    elif kind == "sweep":
        out["response_db"] = sweep_bands(x, fs, seconds, 10.0 ** (level_dbfs / 20.0))
    elif kind in ("noise", "white"):
        out["response_db"] = noise_bands(x, fs)
    return out


# ---------------------------------------------------------------- device I/O


def _sd():
    try:
        import sounddevice
    except (ImportError, OSError) as exc:
        raise DeviceNotFoundError("audio needs sounddevice: pip install sounddevice") from exc
    return sounddevice


def find_device(name: str = DEVICE_NAME, output: bool = False, sd=None) -> int:
    """Index of the first audio device whose name contains `name` and that
    has 2 input (or output) channels."""
    sd = sd or _sd()
    key = "max_output_channels" if output else "max_input_channels"
    devices = list(sd.query_devices())
    for i, dev in enumerate(devices):
        if name.lower() in dev["name"].lower() and dev[key] >= 2:
            return i
    have = ", ".join(sorted({d["name"] for d in devices})) or "none"
    raise DeviceNotFoundError(f"no {'output' if output else 'input'} audio device named "
                              f"{name!r} (is the pedal on USB?); devices: {have}")


def record(seconds: float, name: str = DEVICE_NAME, sd=None) -> np.ndarray:
    """Capture (frames, 2) float32 from the pedal."""
    sd = sd or _sd()
    dev = find_device(name, sd=sd)
    rec = sd.rec(int(seconds * FS), samplerate=FS, channels=2, dtype="float32", device=dev)
    sd.wait()
    return np.asarray(rec)


def play_record(x: np.ndarray, pad_s: float = 0.5, name: str = DEVICE_NAME,
                sd=None) -> np.ndarray:
    """Play mono x on both channels to the pedal and capture at the same time."""
    sd = sd or _sd()
    dev_in, dev_out = find_device(name, sd=sd), find_device(name, output=True, sd=sd)
    x = np.concatenate([x, np.zeros(int(pad_s * FS))]).astype(np.float32)
    rec = sd.playrec(np.stack([x, x], axis=1), samplerate=FS, channels=2, dtype="float32",
                     device=(dev_in, dev_out))
    sd.wait()
    return np.asarray(rec)
