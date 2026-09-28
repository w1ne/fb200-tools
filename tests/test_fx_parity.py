"""Parity of our effect ports (firmware/audio/src/dsp/{gate,comp,mod,reverb}.c)
with the stock FB200 DSP.

The stock per-sample callback runs in emulation (tests/stock_emu_fx.py) with a
preset that enables only the module under test; its effect-chain output (before
master volume and the +-0.95 output clip) is compared with our C module fed the
same chain input. Error = RMS(ours - stock) / RMS(stock) in dB; target <= -60.

Skips (with the reason) when the vendor image or unicorn/numpy/capstone are
absent; asserts when they are present. `pytest -s` prints the table.
"""
import shutil
import subprocess
from pathlib import Path

import pytest

np = pytest.importorskip("numpy", reason="parity tests need numpy")
pytest.importorskip("unicorn", reason="parity tests need unicorn (stock DSP emulation)")
pytest.importorskip("capstone", reason="parity tests need capstone (stock DSP emulation)")

import stock_emu_fx as stock_emu

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
BUILD = FW / "build" / "fx_parity"
FS = 44100
TARGET_DB = -60.0

pytestmark = [
    pytest.mark.stock,
    pytest.mark.skipif(shutil.which("cc") is None, reason="host C compiler not installed"),
    pytest.mark.skipif(stock_emu.MR_PATH is None,
                       reason="fb200-stock.mr not found (repo root, main checkout or "
                              "$FB200_STOCK_MR): stock reference unavailable"),
]

# module -> (C sources, preset enable field)
MODULES = {
    "gate": (["detector.c", "gate.c"], 0x5C),
    "comp": (["comp.c"], 0x14),
    "mod": (["mod.c"], 0x74),
    "reverb": (["reverb.c"], 0xA4),
}


def signal(n: int, seed: int = 1) -> "np.ndarray":
    """Bass-like test input: plucked notes (harmonics, exponential decay) at
    levels from -40 to -10 dBFS, a silent gap, and a -80 dBFS noise floor."""
    rng = np.random.default_rng(seed)
    t = np.arange(n) / FS
    x = 1e-4 * rng.standard_normal(n)
    step = FS // 5
    for k, start in enumerate(range(0, n, step)):
        if k % 4 == 3:
            continue                                  # a silent gap every 4th slot
        f0 = (41.2, 55.0, 73.4, 98.0, 146.8)[k % 5]
        amp = (0.3, 0.03, 0.1, 0.01, 0.2)[k % 5]
        tt = t[start:start + step] - t[start]
        env = amp * np.exp(-tt * 6) * np.minimum(1, tt * 2000)
        note = sum(np.sin(2 * np.pi * f0 * h * tt + h) / h for h in (1, 2, 3, 5))
        x[start:start + step] += env * note / 1.5
    return x


def err_db(ours, ref) -> float:
    ours, ref = np.asarray(ours, float), np.asarray(ref, float)
    e = np.sqrt(np.mean((ours - ref) ** 2)) / max(np.sqrt(np.mean(ref ** 2)), 1e-30)
    return 20 * np.log10(max(e, 1e-12))


_bins: dict[str, Path] = {}


def build(module: str) -> Path:
    if module in _bins:
        return _bins[module]
    from test_dsp_host import cmsis_dsp_args
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / f"fx_render_{module}"
    srcs = [str(FW / "src" / "dsp" / s) for s in MODULES[module][0] + ["math.c"]]
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
                    f"-DFX_{module.upper()}", "-I", str(FW / "src"),
                    str(FW / "tests" / "fx_render.c"), *srcs, *cmsis_dsp_args(), "-lm",
                    "-o", str(exe)], check=True)
    _bins[module] = exe
    return exe


def render_c(module: str, warmup: int, params, x) -> "np.ndarray":
    out = subprocess.run([str(build(module)), str(warmup), *map(str, params)],
                         input=np.asarray(x, "<f4").tobytes(), capture_output=True, check=True)
    y = np.frombuffer(out.stdout, "<f4")
    return y.reshape(-1, 2) if module == "reverb" else y


@pytest.fixture(scope="module")
def ram():
    return stock_emu.load_ram(stock_emu.MR_PATH)


# (id, module, preset fields, C params, samples)
GATE_CASES = [
    (f"gate-thr{t}", "gate", {0x60: t}, (t,), 3 * FS // 2) for t in (20, 60, 100)
]
COMP_CASES = [
    # fields: 0x16 type, 0x18 attack, 0x1a threshold, 0x1c ratio, 0x1e level
    (f"comp-a{a}-t{t}-r{r}-l{lv}", "comp",
     {0x16: 0, 0x18: a, 0x1A: t, 0x1C: r, 0x1E: lv}, (0, a, t, r, lv), FS)
    for a, t, r, lv in ((50, 50, 50, 50), (0, 20, 100, 80), (100, 80, 10, 30))
]
MOD_NAMES = ["phaser", "stepphaser", "flanger", "jetflanger", "tremolo", "stutter", "vibrato",
             "rotary", "achorus", "mchorus", "ringmod", "filter"]
MOD_CASES = [
    (f"mod-{name}", "mod", {0x76: k, 0x78: 60, 0x7A: 70, 0x7C: 40, 0x7E: 50},
     (k, 60, 70, 40, 50), FS // 2)
    for k, name in enumerate(MOD_NAMES)
] + [
    # knob extremes: fastest LFO, full p3/p4, low mix
    (f"mod-{name}-max", "mod", {0x76: k, 0x78: 100, 0x7A: 30, 0x7C: 100, 0x7E: 100},
     (k, 100, 30, 100, 100), FS // 4)
    for k, name in enumerate(MOD_NAMES)
]
REV_NAMES = ["room", "hall", "plate", "spring", "mod"]
REV_CASES = [
    (f"reverb-{name}", "reverb", {0xA6: k, 0xAA: 70, 0xAC: 50, 0xAE: 50, 0xA8: 50},
     (k, 70, 50, 50, 50), FS // 2)
    for k, name in enumerate(REV_NAMES)
] + [
    # knob extremes: level (0xaa), decay (0xac), tone (0xae)
    (f"reverb-{REV_NAMES[k]}-l{lv}-d{d}-t{t}", "reverb",
     {0xA6: k, 0xAA: lv, 0xAC: d, 0xAE: t, 0xA8: 0}, (k, lv, d, t, 0), FS // 4)
    for k, lv, d, t in ((0, 100, 0, 0), (1, 100, 100, 100), (3, 40, 100, 0))
]
CASES = GATE_CASES + COMP_CASES + MOD_CASES + REV_CASES
RESULTS: dict[str, float] = {}


@pytest.mark.parametrize("cid,module,fields,params,n", CASES, ids=[c[0] for c in CASES])
def test_parity(ram, cid, module, fields, params, n):
    s = stock_emu.StockDSP(ram)
    s.only({MODULES[module][1]: 1, **fields})
    s.settle()
    x = signal(n)
    s.process(x)
    ref = s.chain_out.astype(float)
    assert s.since_commit >= n, "the stock re-initialised the chain mid-signal"
    ours = render_c(module, s.since_commit - n, params, s.chain_input(x))
    if module == "reverb":
        e = max(err_db(ours[:, 0], ref[:, 0]), err_db(ours[:, 1], ref[:, 1]))
    else:
        assert np.array_equal(ref[:, 0], ref[:, 1]), "stock chain is mono here"
        e = err_db(ours, ref[:, 0])
    RESULTS[cid] = e
    print(f"\n{cid:28s} error {e:8.1f} dB  (stock rms {np.sqrt(np.mean(ref ** 2)):.4f})")
    assert e <= TARGET_DB, f"{cid}: {e:.1f} dB > {TARGET_DB} dB"
