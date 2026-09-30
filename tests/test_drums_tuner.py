# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Drum machine + tuner ports (firmware/audio/src/dsp/{drums,tuner}.c).

The parity tests run the STOCK code in Unicorn (tests/stock_emu.py) from the
user's own fb200-stock.mr and compare. They SKIP with a reason when the .mr or
unicorn/capstone are missing and must pass when both are present.
"""

import functools
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
sys.path.insert(0, str(Path(__file__).parent))

import stock_emu

from fb200 import stockdata

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="host C compiler not installed")

FS = 44100
SRC = [FW / "tests" / "drums_tuner_host_test.c", FW / "src" / "dsp" / "drums.c",
       FW / "src" / "dsp" / "tuner.c", FW / "src" / "dsp" / "stock_data.c", FW / "src" / "crc32.c"]
CFLAGS = ["-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off", "-I", str(FW / "src")]


@functools.cache
def build() -> Path:
    """Once per process, in its own dir: parallel workers (pytest -n) do not collide."""
    exe = Path(tempfile.mkdtemp(prefix="drums_tuner_")) / "drums_tuner_host_test"
    subprocess.run(["cc", *CFLAGS, *map(str, SRC), "-lm", "-o", str(exe)], check=True)
    return exe


def mr_or_skip() -> Path:
    mr = stock_emu.find_stock_mr()
    if mr is None:
        pytest.skip("fb200-stock.mr not found (set FB200_STOCK_MR); stock data is not in the repo")
    return mr


def emu_or_skip() -> Path:
    why = stock_emu.have_emulator()
    if why:
        pytest.skip(f"stock emulation unavailable: {why}")
    return stock_emu.find_stock_mr()


@pytest.fixture(scope="module")
def stock_blob(tmp_path_factory) -> Path:
    """The stock data blob (vendor data: only in the test's tmp dir)."""
    out = tmp_path_factory.mktemp("stock") / "stock.blob"
    out.write_bytes(stockdata.build(mr_or_skip().read_bytes()))
    return out


def tone(f: float, secs: float, amp: float, pre: int) -> np.ndarray:
    t = np.arange(int(FS * secs)) / FS
    x = amp * (np.sin(2 * np.pi * f * t) + 0.5 * np.sin(4 * np.pi * f * t + 0.3)
               + 0.25 * np.sin(6 * np.pi * f * t + 1.1)) * np.exp(-t / 3)
    return np.r_[np.zeros(pre), x].astype(np.float32)


def run_tuner(exe: Path, x: np.ndarray, a4: int = 440) -> list[dict]:
    path = exe.parent / "tuner_in.f32"
    x.astype("<f4").tofile(path)
    out = subprocess.run([str(exe), "tuner", str(path), str(a4)], capture_output=True, text=True,
                         check=True).stdout
    keys = ("sample", "note", "deviation", "octave", "freq", "cents", "confidence", "silent", "valid")
    return [dict(zip(keys, (float(v) if "." in v else int(v) for v in line.split())))
            for line in out.splitlines()]


def test_selftest_without_stock_data():
    exe = build()
    r = subprocess.run([str(exe), "selftest"], capture_output=True, text=True, check=False)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "drums tuner host tests OK" in r.stdout


def test_blob_reads_the_stock_image(stock_blob):
    blob = stock_blob.read_bytes()
    stockdata.verify(blob)
    # sizeof(stock_data_t) + 21 factory presets (version 2): test_stockdata.py checks the C side
    assert len(blob) == 59276 + 21 * 256


@pytest.mark.stock
@pytest.mark.parametrize("rhythm,bpm,secs,block", [(0, 110, 3.0, 32), (7, 180, 2.0, 8),
                                                   (39, 60, 3.0, 13), (21, 260, 1.5, 32)])
def test_drums_match_stock(stock_blob, rhythm, bpm, secs, block):
    emu_or_skip()
    exe = build()
    st = stock_emu.StockDrums(rhythm, bpm)
    n_blocks = int(FS * secs) // 8
    ref = st.render(n_blocks)
    bank = exe.parent / "drum_bank.bin"
    bank.write_bytes(st.bank_raw)                   # block 1 as flashed: header layout
    out = exe.parent / "drums_out.f32"
    subprocess.run([str(exe), "drums", str(bank), str(rhythm), str(bpm), str(n_blocks * 8),
                    str(out), str(block)], check=True,
                   env={**os.environ, "FB200_STOCK_BLOB": str(stock_blob)})
    ours = np.fromfile(out, "<f4")
    assert np.abs(ref).max() > 0.1, "stock render is silent"
    diff = float(np.abs(ours - ref).max())
    print(f"rhythm {rhythm} @ {bpm} BPM, block {block}: max |ours - stock| = {diff:g}")
    assert diff <= 1e-6


@pytest.mark.stock
@pytest.mark.parametrize("name,freq,note", [("B0", 30.8677, 2), ("E1", 41.2034, 7),
                                            ("A2", 110.0, 12)])
def test_tuner_matches_stock(name, freq, note):
    emu_or_skip()
    exe = build()
    x = tone(freq, 1.2, 0.2, 8820)
    ref = stock_emu.StockTuner(440).run(x)
    ours = run_tuner(exe, x)
    assert len(ours) == len(ref) and len(ref) >= 10
    for s, o in zip(ref, ours):
        assert (s["sample"], s["note"], s["deviation"]) == (o["sample"], o["note"], o["deviation"])
        assert abs(s["freq"] - o["freq"]) <= 1e-3 * max(1.0, s["freq"] / 100)
    locked = [o for o in ours if o["valid"] and o["sample"] > 8820 + 0.4 * FS]
    assert locked and all(o["note"] == note for o in locked)
    err = max(abs(1200 * np.log2(o["freq"] / freq)) for o in locked)
    print(f"{name}: {len(locked)} readings, max error {err:.3f} cents (stock-identical)")


@pytest.mark.parametrize("name,freq,note", [("B0", 30.8677, 2), ("E1", 41.2034, 7)])
def test_tuner_low_notes_accuracy_and_lock(name, freq, note):
    """No emulator needed: accuracy and lock time of the (stock-identical) port."""
    exe = build()
    locks, errs = [], []
    for pre in range(8820, 8820 + 4096, 512):
        res = run_tuner(exe, tone(freq, 1.5, 0.2, pre))
        lock = next((r["sample"] - pre for r in res
                     if r["sample"] > pre and r["valid"] and r["note"] == note), None)
        assert lock is not None
        locks.append(lock / FS * 1000)
        errs += [abs(1200 * np.log2(r["freq"] / freq)) for r in res
                 if r["valid"] and r["sample"] > pre + 0.4 * FS]
    print(f"{name}: lock {min(locks):.0f}..{max(locks):.0f} ms, max error {max(errs):.2f} cents")
    assert max(locks) < 200 and max(errs) < 2.0
