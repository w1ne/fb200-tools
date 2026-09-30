#!/usr/bin/env python3
# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Sound-quality check of the pedal over USB (no cable needed).

What USB reaches: the ADC (idle noise, with the instrument jack as it is),
the digital chain (reamping: the Mac plays into the chain, `usb in`; the
firmware test generator, `tin`), and the capture. The analog output (DAC ->
jack) needs a cable and an interface: not measured here.

  python tools/sound_check.py [--only idle,dry,glitch,eq,amp,gate] [--json out.json]

It edits the edit buffer (effects off, EQ, amp) and re-selects the preset at
the end (the edits are discarded). It never saves. Numbers of v0.9.1 and the
changes: docs/MEASUREMENTS.md.

Requires numpy and sounddevice (the `dev` extra) and the pedal on USB.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from fb200 import audio as A
from fb200.mcp_server import Pedal, PedalTools

FS = A.FS


def _spectrum(x: np.ndarray, n: int = 1 << 15) -> tuple[np.ndarray, np.ndarray]:
    """Welch power spectrum; a sine of amplitude a has power a^2 / 2 (ENBW 1.5 bins)."""
    w = np.hanning(n)
    segs = [x[i:i + n] for i in range(0, len(x) - n + 1, n // 2)]
    p = np.mean([np.abs(np.fft.rfft(s * w)) ** 2 for s in segs], axis=0) * 2 / np.sum(w) ** 2
    return np.fft.rfftfreq(n, 1 / FS), p


def noise(x: np.ndarray) -> dict:
    x = np.asarray(x, dtype=np.float64)
    dc = float(np.mean(x))
    f, p = _spectrum(x - dc)

    def band(lo: float, hi: float) -> float:
        sel = (f >= lo) & (f <= hi)
        return round(10 * np.log10(np.sum(p[sel]) / 1.5 + 1e-30), 1)

    k = np.argsort(p * ((f >= 20) & (f <= 20000)))[-3:][::-1]
    return {"dc_lsb16": round(dc * 32768, 1), "rms_20_20k_dbfs": band(20, 20000),
            "rms_1k_20k_dbfs": band(1000, 20000),
            "tones_hz_dbfs": [[round(float(f[i]), 1), round(10 * np.log10(p[i] + 1e-30), 1)]
                              for i in k]}


def tone(x: np.ndarray, f0: float) -> dict:
    """THD, THD+N and the inharmonic (aliasing) power of a steady sine, dB re
    the fundamental (THD+N: 20 Hz..20 kHz)."""
    x = np.asarray(x[-(1 << 16):], dtype=np.float64)
    x = x - x.mean()
    w = np.blackman(len(x))
    p = np.abs(np.fft.rfft(x * w)) ** 2
    f = np.fft.rfftfreq(len(x), 1 / FS)
    band = (f >= 20) & (f <= 20000)
    fund = np.abs(f - f0) < 8
    harm = np.zeros_like(band)
    for k in range(1, int(FS / 2 / f0) + 1):
        harm |= np.abs(f - k * f0) < 8
    pf = p[fund].sum()

    def db(v: float) -> float:
        return round(10 * np.log10(v / pf + 1e-30), 1)

    return {"level_dbfs": round(A.db(np.sqrt(2 * np.mean(x ** 2))), 1),
            "thd_db": db(p[harm & band & ~fund].sum()), "thdn_db": db(p[band & ~fund].sum()),
            "inharmonic_db": db(p[band & ~harm].sum())}


def steps(x: np.ndarray, f0: float) -> dict:
    """Ticks in a steady sine: 10 ms windows whose sine-fit residual is 10x the median."""
    w = 441
    t = np.arange(w) / FS
    res = []
    for i in range(0, len(x) - w, w):
        tt = t + i / FS
        m = np.stack([np.sin(2 * np.pi * f0 * tt), np.cos(2 * np.pi * f0 * tt), np.ones(w)], 1)
        c, *_ = np.linalg.lstsq(m, x[i:i + w], rcond=None)
        res.append(np.sqrt(np.mean((x[i:i + w] - m @ c) ** 2) / np.mean((m @ c) ** 2)))
    res = np.array(res)
    bad = res > 10 * np.median(res)
    return {"ticks": int(bad.sum()), "seconds": round(len(x) / FS, 1),
            "worst_window_db": round(20 * np.log10(res.max()), 1),
            "median_window_db": round(20 * np.log10(np.median(res)), 1)}


class Check:
    def __init__(self) -> None:
        self.t = PedalTools(Pedal())
        self.c = self.t.console

    def fx_off(self) -> None:
        for m in ("comp", "gate", "amp", "cab", "mod", "reverb"):
            self.t._set(m, enabled=False)
        self.c("delay off")
        self.c("eq off")
        time.sleep(0.3)

    def reamp(self, x: np.ndarray) -> np.ndarray:
        self.c("usb in")
        try:
            return A.play_record(x.astype(np.float32))[:, 0].astype(np.float64)
        finally:
            self.c("usb out")

    def tin(self, freq: float, seconds: float = 2.0) -> np.ndarray:
        self.c(f"tin sine {int(freq)}")
        time.sleep(0.4)
        try:
            return A.record(seconds)[:, 0].astype(np.float64)
        finally:
            self.c("tin off")

    def idle(self) -> dict:
        self.fx_off()
        return {"idle_adc": noise(A.record(4.0)[:, 0])}

    def dry(self) -> dict:
        self.fx_off()
        out = {}
        for f0 in (100, 1000):
            for lv in (-60, -40, -20, -6, 0):
                # 2.5..4.9 s: the playback resampler has settled (`glitch` checks the start)
                r = self.reamp(A.make_signal("sine", 5.0, f0, lv))[int(2.5 * FS):int(4.9 * FS)]
                d = tone(r, f0)
                d["gain_db"] = round(d["level_dbfs"] - lv, 2)
                out[f"dry_{f0}hz_{lv}dbfs"] = d
        return out

    def glitch(self) -> dict:
        self.fx_off()
        r = self.reamp(A.make_signal("sine", 20.0, 1000, -10))
        return {"reamp_ticks_1k": steps(r[int(1 * FS):int(19.5 * FS)], 1000.0),
                "reamp_start_1k": tone(r[int(0.4 * FS):int(1.9 * FS)], 1000.0),
                "reamp_end_1k": tone(r[int(17 * FS):int(19.5 * FS)], 1000.0),
                "stats": self.c("stats").splitlines()[1]}

    def eq(self) -> dict:
        self.fx_off()
        self.c("gain 12")
        try:
            out = {"eq_off": tone(self.tin(1000), 1000)}
            self.t.set_eq(hpf_hz=30, band=1, freq_hz=40, gain_db=6, q=1.0)
            self.t.set_eq(band=2, freq_hz=100, gain_db=-4, q=2.0)
            self.t.set_eq(on=True)
            time.sleep(0.5)
            out["eq_low_bands"] = tone(self.tin(1000), 1000)
        finally:
            self.c("gain 0")
        return out

    def amp(self) -> dict:
        self.fx_off()
        out = {}
        for m in (1, 4, 10):
            self.t.set_amp(enabled=True, model=m, gain=100)
            time.sleep(0.3)
            for f0 in (1000, 3000):
                out[f"amp{m}_gain100_{f0}hz"] = tone(self.tin(f0), f0)
        return out

    def gate(self) -> dict:
        """A 55 Hz note decaying 60 dB in 4 s: gate open/close changes (chatter)."""
        self.fx_off()
        n = int(4 * FS)
        t = np.arange(n) / FS
        rng = np.random.default_rng(1)
        x = 0.5 * np.sin(2 * np.pi * 55 * t) * 10 ** (-3 * t / 4) + 2.2e-4 * rng.standard_normal(n)
        self.t.set_gate(enabled=True, type=1, threshold=60)
        time.sleep(0.3)
        r = self.reamp(x)
        d = A.delay_frames(x, r)
        y = r[d:d + n]
        w = 441
        g = np.array([np.sqrt(np.mean(y[i:i + w] ** 2) / np.mean(x[i:i + w] ** 2))
                      for i in range(0, n - w, w)])
        open_ = 20 * np.log10(g + 1e-9) > -20
        return {"gate_t1_th60_changes": int(np.sum(open_[1:] != open_[:-1]))}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", default="idle,dry,glitch,eq,amp,gate")
    ap.add_argument("--json")
    args = ap.parse_args()
    chk = Check()
    preset = chk.t.preset()["index"]
    result = {}
    try:
        for name in args.only.split(","):
            part = getattr(chk, name.strip())()
            for k, v in part.items():
                print(k, json.dumps(v), flush=True)
            result.update(part)
    finally:
        chk.c(f"preset {preset}")          # discard the edits
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
