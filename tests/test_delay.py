"""Our bass delay (firmware/audio/src/dsp/delay.c, docs/PARITY.md M4).

1. The C host suite: echo timing, feedback decay, low cut on the repeats,
   silence (no NaN / subnormals), time glide, and the rule that decides when
   a preset plays the delay (src/preset/preset.h preset_delay_on).
2. With the stock image: every factory preset keeps the delay off under
   that rule (they all have the stock delay block "on"), and the stock DSP
   ignores our marker words (0x96..0x9b): its output is bit-identical.

Part 2 skips (with the reason) without the vendor image or unicorn.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
OUT = FW / "build" / "delay_host_test"
sys.path.insert(0, str(ROOT / "src"))

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="host C compiler not installed")


def build() -> Path:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(FW / "src"),
                    str(FW / "tests" / "delay_host_test.c"),
                    str(FW / "src" / "dsp" / "delay.c"), str(FW / "src" / "dsp" / "math.c"),
                    "-lm", "-o", str(OUT)], check=True)
    return OUT


def test_delay_host_suite():
    r = subprocess.run([str(build())], capture_output=True, text=True, check=False)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "delay host tests OK" in r.stdout


def _stock_mr() -> Path | None:
    try:
        import stock_emu_fx  # also finds it from a git worktree
        return stock_emu_fx.MR_PATH
    except ImportError:                         # no unicorn/capstone: the plain places
        c = [os.environ.get("FB200_STOCK_MR"), ROOT / "fb200-stock.mr"]
        return next((Path(p) for p in c if p and Path(p).is_file()), None)


def _factory_presets() -> bytes:
    mr = _stock_mr()
    if mr is None:
        pytest.skip("fb200-stock.mr not found: factory presets unavailable")
    from fb200 import stockdata
    from fb200.firmware import MrFile
    d = stockdata.extract(stockdata.unpack_stock_ram(MrFile.from_path(mr).blocks[0].data))
    return d.factory_presets


def test_factory_presets_never_play_the_delay():
    presets = _factory_presets()
    n = len(presets) // 0x100
    assert n == 21
    # the premise: every named factory preset has the stock delay block on
    assert all(presets[i * 0x100 + 0x8C] == 1 for i in range(20))
    r = subprocess.run([str(build()), "--presets"], input=presets, capture_output=True,
                       check=False)
    assert r.returncode == 0, r.stdout.decode() + r.stderr.decode()
    assert f"presets: {n} checked, 0 play the delay".encode() in r.stdout


@pytest.mark.stock
def test_stock_dsp_ignores_the_marker():
    np = pytest.importorskip("numpy", reason="needs numpy")
    pytest.importorskip("unicorn", reason="needs unicorn (stock DSP emulation)")
    pytest.importorskip("capstone", reason="needs capstone (stock DSP emulation)")
    import stock_emu_fx as stock_emu
    if stock_emu.MR_PATH is None:
        pytest.skip("fb200-stock.mr not found: stock reference unavailable")
    from test_fx_parity import signal
    ram = stock_emu.load_ram(stock_emu.MR_PATH)
    x = signal(6000)

    def run(fields):
        # preset 13 "Space Signal" (comp, gate, amp, cab, mod) + its reverb on
        s = stock_emu.StockDSP(ram)
        s.load_preset(13)
        for off, v in {0xA4: 1, **fields}.items():
            s.set_field(off, v)
        s.settle()
        return s.process(x), s.chain_out.copy()

    base = run({})
    ours = run({0x96: 0x4C44, 0x98: 100, 0x9A: 0, 0x8C: 1, 0x90: 100, 0x92: 100, 0x94: 40})
    control = run({0x30: 90})                  # amp gain: a field the stock reads
    assert np.abs(base[0]).max() > 1e-3, "the stock chain is silent: no test"
    assert not np.array_equal(base[0], control[0]), "the emulation ignores preset edits: no test"
    assert np.array_equal(base[0], ours[0])
    assert np.array_equal(base[1], ours[1])
