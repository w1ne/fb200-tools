"""Host test for the codec init sequence generator (no hardware needed)."""
import functools
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="no host compiler")


@functools.cache
def build() -> Path:
    """Once per process, in its own dir: parallel workers (pytest -n) do not collide."""
    exe = Path(tempfile.mkdtemp(prefix="codec_init_")) / "codec_host_test"
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-DCODEC_HOST_TEST", "-I", str(FW / "src"),
         str(FW / "tests" / "codec_host_test.c"),
         str(FW / "src" / "audio" / "codec.c"), "-o", str(exe)],
        check=True,
    )
    return exe


def test_codec_init_sequence():
    result = subprocess.run([str(build())], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "codec init sequence OK" in result.stdout
