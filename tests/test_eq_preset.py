# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Our bass EQ in the preset (firmware/audio/src/preset/preset.h P_EQ_MARK,
dsp/eq.c eq_load/eq_save, docs/PARITY.md M4).

1. The C host suite: the marker rule (no marker = off + defaults), the
   24-byte record (layout, round trip, grid, clamps), a preset change glides
   (no click; a cleared-state control does click) and ends bit-exact.
2. With the stock image: every factory preset has 0 in the EQ bytes
   (0xc4..0xdd), and the stock DSP ignores them: its output is bit-identical
   with a full EQ record there.

Part 2 skips (with the reason) without the vendor image or unicorn.
"""
import functools
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

import pytest
from test_delay import _factory_presets
from test_dsp_host import cmsis_dsp_args

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="host C compiler not installed")

# preset.h: marker "EQ" at 0xc4, then on, hpf, lpf u16, 5 x (Hz u16, s8 1/8 dB, u8 Q x 50)
EQ_MARK, EQ_DATA, EQ_REC = 0xC4, 0xC6, 24


def eq_record(on=1, hpf=60, lpf=8000, bands=((40, 4.0, 1.0), (100, -6.0, 1.4), (250, 3.0, 0.7),
                                                (800, -2.5, 2.0), (3000, 1.5, 1.0))) -> bytes:
    """The bytes 0xc4..0xdd of a preset with our EQ (the C layout, written again here)."""
    b = struct.pack("<HBBH", 0x5145, on, hpf, lpf)
    for hz, g, q in bands:
        b += struct.pack("<HbB", hz, round(g * 8), round(q * 50))
    assert len(b) == 2 + EQ_REC
    return b


@functools.cache
def build() -> Path:
    """Once per process, in its own dir: parallel workers (pytest -n) do not collide."""
    exe = Path(tempfile.mkdtemp(prefix="eq_preset_host_")) / "eq_preset_host_test"
    subprocess.run(["cc", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
                    "-I", str(FW / "src"), str(FW / "tests" / "eq_preset_host_test.c"),
                    str(FW / "src" / "dsp" / "eq.c"), *cmsis_dsp_args(), "-lm", "-o", str(exe)],
                   check=True)
    return exe


def test_eq_preset_host_suite():
    r = subprocess.run([str(build())], capture_output=True, text=True, check=False)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "eq preset host tests OK" in r.stdout
    assert "preset change, max |d2 y|" in r.stdout


def test_record_layout_matches_c():
    """The Python writer above makes records the C side reads as on (the MCP
    tests and docs use it)."""
    rec = eq_record()
    p = bytearray(0x100)
    p[EQ_MARK:EQ_MARK + len(rec)] = rec
    assert p[0xC4:0xC6] == b"EQ" and p[EQ_DATA] == 1


def test_factory_presets_have_no_eq():
    presets = _factory_presets()
    n = len(presets) // 0x100
    assert n == 21
    r = subprocess.run([str(build()), "--presets"], input=presets, capture_output=True, check=False)
    assert r.returncode == 0, r.stdout.decode() + r.stderr.decode()
    assert f"presets: {n} checked, 0 with EQ bytes, 0 play the EQ".encode() in r.stdout
    # the whole tail after the module order is free in every factory preset
    assert all(presets[i * 0x100 + j] == 0 for i in range(n) for j in range(0xC4, 0x100))


@pytest.mark.stock
def test_stock_dsp_ignores_the_eq_bytes():
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

    rec = eq_record()
    words = {EQ_MARK + i: int.from_bytes(rec[i:i + 2], "little") for i in range(0, len(rec), 2)}
    base = run({})
    ours = run(words)
    control = run({0x30: 90})                  # amp gain: a field the stock reads
    assert np.abs(base[0]).max() > 1e-3, "the stock chain is silent: no test"
    assert not np.array_equal(base[0], control[0]), "the emulation ignores preset edits: no test"
    assert np.array_equal(base[0], ours[0])
    assert np.array_equal(base[1], ours[1])
