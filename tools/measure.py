#!/usr/bin/env python3
"""Measurement suite for the FB200 audio path.

Measures the pedal (or any audio device) with the mixer loopback rig:

  fr        exponential sweep 20 Hz..20 kHz -> frequency response (dB)
  thd       1 kHz sine at -6 dBFS -> THD+N (% and dB)
  noise     2 s silence -> RMS, A-weighted
  latency   impulse -> cross-correlation -> round-trip latency (ms)
  run-all   all of the above, appends a section to docs/MEASUREMENTS.md

The analysis functions are pure and covered by `--selftest` (synthetic
signals, no hardware needed), which tests/test_measure.py runs in CI.

Requires numpy, scipy and sounddevice (the `dev` extra).
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
FS = 48000
SWEEP_F1, SWEEP_F2, SWEEP_DUR = 20.0, 20000.0, 5.0
THD_FREQ, THD_AMP = 1000.0, 0.5  # -6 dBFS
CAPTURE_PAD = 0.5  # extra capture time after playback


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


def a_weighting(fs: int):
    """A-weighting filter coefficients (standard analog prototype, bilinear)."""
    from scipy import signal

    f1, f2, f3, f4 = 20.598997, 107.65265, 737.86223, 12194.217
    a1000 = 1.9997
    num = [(2 * np.pi * f4) ** 2 * (10 ** (a1000 / 20)), 0, 0, 0, 0]
    den = np.polymul(
        [1, 4 * np.pi * f4, (2 * np.pi * f4) ** 2],
        [1, 4 * np.pi * f1, (2 * np.pi * f1) ** 2],
    )
    den = np.polymul(den, [1, 2 * np.pi * f3])
    den = np.polymul(den, [1, 2 * np.pi * f2])
    return signal.bilinear(num, den, fs)


def noise_metrics(rec: np.ndarray, fs: int) -> dict:
    from scipy import signal

    rms = float(np.sqrt(np.mean(rec**2) + 1e-30))
    b, a = a_weighting(fs)
    weighted = signal.lfilter(b, a, rec)
    rms_a = float(np.sqrt(np.mean(weighted**2) + 1e-30))
    return {
        "rms_dbfs": 20.0 * np.log10(rms + 1e-12),
        "rms_a_dbfs": 20.0 * np.log10(rms_a + 1e-12),
    }


def latency_ms(play: np.ndarray, rec: np.ndarray, fs: int) -> float:
    """Round-trip latency via cross-correlation of an impulse."""
    corr = np.correlate(rec, play, mode="full")
    lag = int(np.argmax(np.abs(corr))) - (len(play) - 1)
    return 1000.0 * lag / fs


# ---------------------------------------------------------------- device io


def _open_stream(device: str | None, fs: int):
    import sounddevice as sd

    kwargs = {"samplerate": fs, "channels": 2, "dtype": "float32"}
    if device:
        matches = [d for d in sd.query_devices()
                   if device.lower() in d["name"].lower()]
        if not matches:
            raise SystemExit(f"no audio device matching {device!r}")
        kwargs["device"] = matches[0]["name"]
    return sd.Stream(**kwargs), sd


def _duplex(device: str | None, play: np.ndarray, fs: int) -> np.ndarray:
    stream, sd = _open_stream(device, fs)
    pad = np.zeros(int(fs * CAPTURE_PAD), dtype=np.float32)
    stereo = np.stack([play, play], axis=1).astype(np.float32)
    rec = np.zeros((len(play) + len(pad), 2), dtype=np.float32)
    with stream:
        stream.write(np.zeros((int(0.2 * fs), 2), dtype=np.float32))
        stream.write(stereo)
        stream.read(rec.shape[0], blocking=True, out=rec)
    sd.wait()
    return rec[:, 0]


def measure_fr(device: str | None, fs: int) -> dict:
    x = exp_sweep(fs, SWEEP_F1, SWEEP_F2, SWEEP_DUR, 0.5)
    rec = _duplex(device, x, fs)
    freqs, mag = sweep_response(rec, fs, SWEEP_F1, SWEEP_F2, SWEEP_DUR, 0.5)
    points = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
    out = {}
    for f in points:
        idx = int(np.argmin(np.abs(freqs - f)))
        out[f"fr_{f}hz_db"] = round(float(mag[idx]), 2)
    return out


def measure_thd(device: str | None, fs: int) -> dict:
    t = np.arange(int(fs * 2.0)) / fs
    x = THD_AMP * np.sin(2 * np.pi * THD_FREQ * t)
    rec = _duplex(device, x, fs)
    return {k: round(v, 3) for k, v in thd_plus_n(rec, fs, THD_FREQ).items()}


def measure_noise(device: str | None, fs: int) -> dict:
    rec = _duplex(device, np.zeros(int(fs * 2.0)), fs)
    return {k: round(v, 2) for k, v in noise_metrics(rec, fs).items()}


def measure_latency(device: str | None, fs: int) -> dict:
    x = np.zeros(int(fs * 0.5))
    x[100] = 0.5
    rec = _duplex(device, x, fs)
    return {"latency_ms": round(latency_ms(x, rec, fs), 2)}


# ---------------------------------------------------------------- reporting


def firmware_banner() -> str:
    """Read the CDC banner (best effort) for the firmware identity."""
    try:
        import glob
        import serial

        for tty in sorted(glob.glob("/dev/tty.usbmodem*")):
            if "AUDIO_0001" not in tty:
                continue
            with serial.Serial(tty, 115200, timeout=0.3) as s:
                return s.read(256).decode(errors="replace").strip().splitlines()[0]
    except Exception:
        pass
    return "unknown"


def write_section(results: dict, device: str | None, fs: int, note: str) -> Path:
    out = ROOT / "docs" / "MEASUREMENTS.md"
    stamp = _dt.datetime.now().strftime("%Y-%m-%d %H:%M")
    lines = [
        f"\n## {stamp} - {device or 'default device'} @ {fs} Hz\n",
        f"firmware: `{firmware_banner()}`  ",
        f"note: {note}\n",
        "| metric | value |",
        "| --- | --- |",
    ]
    for k, v in sorted(results.items()):
        lines.append(f"| {k} | {v} |")
    lines.append("")
    with out.open("a") as fh:
        fh.write("\n".join(lines))
    return out


def selftest() -> int:
    """Run the analysis on synthetic signals (no hardware)."""
    fs = FS
    # Frequency response of an ideal loopback should be ~flat.
    x = exp_sweep(fs, SWEEP_F1, SWEEP_F2, SWEEP_DUR, 0.5)
    freqs, mag = sweep_response(x, fs, SWEEP_F1, SWEEP_F2, SWEEP_DUR, 0.5)
    band = (freqs > 100) & (freqs < 10000)
    assert np.max(np.abs(mag[band])) < 6.0, f"loopback FR not flat: {mag[band].max()}"

    # THD of a clean sine should be tiny.
    t = np.arange(int(fs * 2.0)) / fs
    sine = 0.5 * np.sin(2 * np.pi * THD_FREQ * t)
    thd = thd_plus_n(sine, fs, THD_FREQ)
    assert thd["thd_plus_n_pct"] < 1.0, thd
    # 0.5 amplitude = -6 dBFS peak (small scalloping loss is expected).
    assert -7.0 < thd["fundamental_dbfs"] < -5.5, thd

    # Noise: silence should be extremely quiet.
    quiet = noise_metrics(np.zeros(fs), fs)
    assert quiet["rms_dbfs"] < -100.0, quiet

    # Latency: an impulse delayed by 480 samples = 10 ms at 48 kHz.
    imp = np.zeros(fs // 2)
    imp[100] = 1.0
    delayed = np.zeros_like(imp)
    delayed[100 + 480] = 1.0
    assert abs(latency_ms(imp, delayed, fs) - 10.0) < 0.1

    print("measure.py selftest OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("command", choices=["fr", "thd", "noise", "latency",
                                        "run-all", "selftest"])
    ap.add_argument("--device", help="audio device name substring")
    ap.add_argument("--fs", type=int, default=FS)
    ap.add_argument("--note", default="", help="note for the report section")
    ap.add_argument("--json", help="write raw results to this file")
    args = ap.parse_args(argv)

    if args.command == "selftest":
        return selftest()

    funcs = {"fr": measure_fr, "thd": measure_thd, "noise": measure_noise,
             "latency": measure_latency}
    if args.command == "run-all":
        results = {}
        for name, fn in funcs.items():
            print(f"== {name} ==", flush=True)
            results.update(fn(args.device, args.fs))
    else:
        results = funcs[args.command](args.device, args.fs)

    for k, v in sorted(results.items()):
        print(f"{k}: {v}")
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2) + "\n")
    if args.command == "run-all":
        out = write_section(results, args.device, args.fs, args.note)
        print(f"appended to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
