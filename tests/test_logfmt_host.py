import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
OUT = FW / "build" / "logfmt_host_test"

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None, reason="host C compiler not installed"
)


def test_logfmt_host_suite():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "logfmt_host_test.c"),
         str(FW / "src" / "debug" / "log_fmt.c"), "-o", str(OUT)],
        check=True,
    )
    result = subprocess.run([str(OUT)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "log_fmt host tests OK" in result.stdout
