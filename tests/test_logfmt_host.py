import functools
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None, reason="host C compiler not installed"
)


@functools.cache
def build() -> Path:
    """Once per process, in its own dir: parallel workers (pytest -n) do not collide."""
    exe = Path(tempfile.mkdtemp(prefix="logfmt_host_")) / "logfmt_host_test"
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "logfmt_host_test.c"),
         str(FW / "src" / "debug" / "log_fmt.c"), "-o", str(exe)],
        check=True,
    )
    return exe


def test_logfmt_host_suite():
    result = subprocess.run([str(build())], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "log_fmt host tests OK" in result.stdout
