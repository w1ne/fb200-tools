import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
OUT = FW / "build" / "dsp_host_test"
OUT_ENGINE = FW / "build" / "engine_host_test"

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None, reason="host C compiler not installed"
)


def test_dsp_host_suite():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    sources = [str(p) for p in sorted((FW / "src" / "dsp").glob("*.c"))]
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "dsp_host_test.c"), *sources, "-lm", "-o", str(OUT)],
        check=True,
    )
    result = subprocess.run([str(OUT)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "dsp host tests OK" in result.stdout


def test_engine_drift_suite():
    OUT_ENGINE.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "engine_host_test.c"),
         str(FW / "src" / "audio" / "drift.c"), "-o", str(OUT_ENGINE)],
        check=True,
    )
    result = subprocess.run([str(OUT_ENGINE)], capture_output=True, text=True,
                            check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "drift host tests OK" in result.stdout
