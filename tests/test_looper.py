"""The looper (firmware/audio/src/dsp/looper.c) and its borrowed memory
(src/dsp/loop_mem.c): the C host suite. ADPCM round trip, the recorded sine's
SNR/THD (22.05 kHz and hq), sample-accurate loop points, no click at the wrap
or at the fades, the state machine (rec, play, dub, undo/redo, stop, clear),
a loop over the whole memory, and the handover delay/long IR -> looper ->
back with clean state."""
import functools
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

from test_dsp_host import FW, STOCK_SRC, cmsis_dsp_args

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="host C compiler not installed")

MODS = ["looper.c", "loop_mem.c", "delay.c", "math.c", "cab.c", "conv2.c", "conv.c"]


@functools.cache
def build() -> Path:
    exe = Path(tempfile.mkdtemp(prefix="looper_host_")) / "looper_host_test"
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
                    str(FW / "tests" / "looper_host_test.c"),
                    *[str(FW / "src" / "dsp" / m) for m in MODS], *map(str, STOCK_SRC),
                    *cmsis_dsp_args(), "-lm", "-o", str(exe)], check=True)
    return exe


def test_looper_host_suite():
    r = subprocess.run([str(build())], capture_output=True, text=True, check=False)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    out = r.stdout
    assert "looper host tests OK" in out
    for line in ("adpcm: round trip OK", "loop points 22.05k", "loop points hq", "wrap 22.05k",
                 "punch in/out hq", "states: rec play dub undo redo stop clear OK",
                 "memory hq:", "handover: delay/long IR -> looper -> back, clean"):
        assert line in out, line
