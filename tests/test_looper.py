"""The flash looper: the frame codec (firmware/audio/src/dsp/loopcodec.c),
the audio side (src/dsp/looper.c) and the flash side (src/loopstore/) on a
simulated W25Q64 (firmware/audio/tests/loopflash_sim.c): the C host suite.
Codec SNR (100 Hz..5 kHz), the recorded sine's SNR/THD through the whole
path, sample-accurate loop points, no click at the wrap or at the fades, the
state machine (rec, play, dub, undo/redo, stop, clear), streaming with
typical, worst-case and no-suspend flash timing (no audio skip, undo bit
exact), a record that outruns a slow erase (closes cleanly), the whole
area, and the boot (nothing read before written)."""
import functools
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest
from test_dsp_host import FW

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="host C compiler not installed")

SRC = [FW / "src" / p for p in ("dsp/looper.c", "dsp/loopcodec.c", "loopstore/loopstore.c",
                                "loopstore/lsio.c", "debug/flash_rmw.c", "crc32.c")]
TESTS = [FW / "tests" / "looper_host_test.c", FW / "tests" / "loopflash_sim.c"]


@functools.cache
def build() -> Path:
    exe = Path(tempfile.mkdtemp(prefix="looper_host_")) / "looper_host_test"
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
                    "-I", str(FW / "src"), "-I", str(FW / "tests"), *map(str, TESTS),
                    *map(str, SRC), "-lm", "-o", str(exe)], check=True)
    return exe


def test_looper_host_suite():
    r = subprocess.run([str(build())], capture_output=True, text=True, check=False)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    out = r.stdout
    assert "looper host tests OK" in out
    for line in ("codec: round trip OK", "loop points: len 30018 samples", "wrap: ",
                 "states: rec play dub undo redo stop clear OK", "typical flash: 20.0 s loop, undo exact",
                 "worst-case flash: 20.0 s loop, undo exact", "no erase suspend: 10.0 s loop",
                 "slow erase: the record closed by itself", "area: a dub over the full loop",
                 "boot: nothing read before written"):
        assert line in out, line
    # the numbers the docs quote (docs/PARITY.md M8)
    snr = float(re.search(r"looper: SNR >= ([\d.]+) dB", out).group(1))
    assert snr >= 55.0
    assert "skips 0" in out and "sim errors 0" in out
    assert not re.search(r"skips [1-9]|sim errors [1-9]", out)
