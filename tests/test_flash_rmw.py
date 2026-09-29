"""Flash writes keep the audio running (firmware/audio/src/debug/flash_rmw.c).

The C harness (firmware/audio/tests/flash_rmw_host_test.c) runs the sector
read-modify-write on a fake NOR flash that stays busy for a number of status
polls after each erase and page program: one audio pump per busy poll, no
flash read or command while busy or from the pump, range checks, timeout and
read-back errors."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="host C compiler not installed")


def test_flash_rmw_pumps_audio_while_busy():
    exe = Path(tempfile.mkdtemp(prefix="flash_rmw_")) / "flash_rmw_host_test"
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(FW / "src"),
                    str(FW / "tests" / "flash_rmw_host_test.c"),
                    str(FW / "src" / "debug" / "flash_rmw.c"), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
    assert out.returncode == 0 and "flash_rmw host tests OK" in out.stdout, out.stdout
