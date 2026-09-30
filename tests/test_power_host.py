# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Battery gauge and idle standby logic (firmware/audio/src/ui/power_logic.c):
stock level thresholds, filter and hysteresis, the Li-ion % estimate,
low/critical battery events and the idle timer (docs/POWER.md)."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="host C compiler not installed")


def test_power_logic_host_suite():
    exe = Path(tempfile.mkdtemp(prefix="power_host_")) / "power_host_test"
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(FW / "src"),
                    str(FW / "tests" / "power_host_test.c"),
                    str(FW / "src" / "ui" / "power_logic.c"), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
    assert out.returncode == 0 and "power host tests OK" in out.stdout, out.stdout
