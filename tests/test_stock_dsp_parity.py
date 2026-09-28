"""Parity of our amp + tone stack + cab (firmware/audio/src/dsp) with the stock
FB200 DSP, emulated in Unicorn (firmware/tools/stock_render.py).

Needs the vendor image fb200-stock.mr (repo root, or FB200_STOCK_MR) and a
Python with unicorn, capstone and numpy (PY_UNICORN, default: this Python).
Without them the tests SKIP with the reason; with them they assert the
parity numbers. Nothing extracted from the .mr is written outside the test's
tmp dir.
"""

import json
import math
import os
import shutil
import subprocess
import sys
from array import array
from functools import partial
from pathlib import Path

import pytest
from test_dsp_host import FW, STOCK_SRC, cmsis_dsp_args

from fb200 import stockdata

ROOT = Path(__file__).resolve().parents[1]
MR = Path(os.environ.get("FB200_STOCK_MR", ROOT / "fb200-stock.mr"))
PY = os.environ.get("PY_UNICORN", sys.executable)
PRESETS = [0, 2, 5, 8, 16]      # Fat Bass, Clean Pick, Vortex OD, Crunch, Wide Hall
CAB_LATENCY = 16                # stock cab runs 2 x 8-sample ping-pong blocks behind
# The target was -60 dB; the gates sit just above what is measured, so a
# regression shows. amp + tone is bit-exact (error 0: the stock's float32 op
# order, literals and smoother stalls are reproduced); the cab adds ~-110 dB
# of FIR rounding. Last-bit changes in the amp alone cost up to -42 dB: its
# filter chains are ill-conditioned.
AMP_MAX_ERR_DB = -120.0
CHAIN_MAX_ERR_DB = -100.0

pytestmark = pytest.mark.stock


@pytest.fixture(scope="module")
def stock(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("host C compiler not installed")
    if not MR.is_file():
        pytest.skip(f"stock firmware {MR} not found (vendor file; set FB200_STOCK_MR)")
    probe = subprocess.run([PY, "-c", "import unicorn, capstone, numpy"], capture_output=True,
                           check=False)
    if probe.returncode != 0:
        pytest.skip(f"{PY} lacks unicorn/capstone/numpy (set PY_UNICORN to a Python with them)")
    work = tmp_path_factory.mktemp("stock_dsp")
    tools = ROOT / "firmware" / "tools"
    blob = work / "stock.blob"
    blob.write_bytes(stockdata.build(MR.read_bytes()))
    subprocess.run([PY, str(tools / "stock_render.py"), str(MR), str(work),
                    "--presets", ",".join(map(str, PRESETS)), "--ir-gains", "8"], check=True)
    exe = work / "stock_parity_host_test"
    mods = [FW / "src" / "dsp" / f for f in ("amp.c", "tone.c", "cab.c")] + STOCK_SRC
    # -ffp-contract=off: the stock uses separate multiply and add (vmla), no FMA
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-ffp-contract=off",
         "-I", str(FW / "src"), str(FW / "tests" / "stock_parity_host_test.c"),
         *map(str, mods), *cmsis_dsp_args(), "-lm", "-o", str(exe)],
        check=True,
    )
    run = partial(subprocess.run, env={**os.environ, "FB200_STOCK_BLOB": str(blob)}, check=True)
    return work, run, exe, json.loads((work / "manifest.json").read_text())


def f32(path: Path) -> array:
    a = array("f")
    a.frombytes(path.read_bytes())
    return a


def rms(v) -> float:
    return math.sqrt(sum(x * x for x in v) / len(v))


def err_db(ours, ref) -> tuple[float, float]:
    """(rms error, peak error) relative to the reference, in dB"""
    assert rms(ref) > 1e-3, "stock render is silent: the comparison would be vacuous"
    err = [a - b for a, b in zip(ours, ref)]
    return (20 * math.log10(max(rms(err), 1e-30) / rms(ref)),
            20 * math.log10(max(max(map(abs, err)), 1e-30) / max(map(abs, ref))))


@pytest.mark.parametrize("preset", PRESETS)
def test_amp_tone_cab_matches_stock(stock, preset):
    """amp + tone stack against the stock amp's own output tap, and amp + tone
    + cab against the stock output (which runs the cab 16 samples later)."""
    work, run, exe, manifest = stock
    p = next(r for r in manifest["presets"] if r["preset"] == preset)
    assert p["amp_on"] == 1 and p["cab_on"] == 1, p
    out = work / f"p{preset}.ours"
    run([str(exe), "render", str(work / f"p{preset}.in.f32"), str(out),
         *(str(p[k]) for k in ("model", "gain", "bass", "mid", "midfreq", "treble", "volume",
                               "cab"))])
    n = p["samples"]
    amp_ref, amp_ours = f32(work / f"p{preset}.amp.f32"), f32(Path(f"{out}.amp.f32"))
    ref = f32(work / f"p{preset}.stock.f32")[CAB_LATENCY:]
    ours = f32(Path(f"{out}.f32"))[:n - CAB_LATENCY]
    assert len(amp_ref) == len(amp_ours) == n and len(ref) == len(ours) == n - CAB_LATENCY
    assert max(map(abs, ref)) < 0.95, "stock output clipped: the comparison would be unfair"
    amp_rms, amp_peak = err_db(amp_ours, amp_ref)
    all_rms, all_peak = err_db(ours, ref)
    amp_txt = "bit-exact" if amp_peak < -300 else f"{amp_rms:.1f} dB rms / {amp_peak:.1f} dB peak"
    print(f"preset {preset} {p['name']}: amp {p['model']} cab {p['cab']}: amp+tone {amp_txt}, "
          f"amp+tone+cab {all_rms:.1f} dB rms / {all_peak:.1f} dB peak")
    assert amp_rms <= AMP_MAX_ERR_DB, f"{p['name']}: amp+tone {amp_rms:.1f} dB rms error"
    assert all_rms <= CHAIN_MAX_ERR_DB, f"{p['name']}: amp+tone+cab {all_rms:.1f} dB rms error"


def test_user_ir_gain_matches_stock(stock):
    work, run, exe, manifest = stock
    run([str(exe), "irgain", str(work / "irs.f32"), str(work / "irs.txt")])
    ours = [float(v) for v in (work / "irs.txt").read_text().split()]
    ref = manifest["ir_gains"]
    assert len(ours) == len(ref) == 9
    worst = max(abs(a - b) / b for a, b in zip(ours, ref))
    print(f"user IR gain: max rel err {worst:.2e} over {len(ref)} IRs")
    assert ref[-1] == 1.0 and ours[-1] == 1.0     # empty IR
    assert worst < 1e-5
