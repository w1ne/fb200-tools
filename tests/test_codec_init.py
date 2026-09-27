"""Host test for the codec init sequence generator (no hardware needed)."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
OUT = FW / "build" / "codec_host_test"

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="no host compiler")


def test_codec_init_sequence():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-DCODEC_HOST_TEST", "-I", str(FW / "src"),
         str(FW / "tests" / "codec_host_test.c"),
         str(FW / "src" / "audio" / "codec.c"), "-o", str(OUT)],
        check=True,
    )
    result = subprocess.run([str(OUT)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "codec init sequence OK" in result.stdout
