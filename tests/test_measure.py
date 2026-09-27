"""The measurement suite's analysis code must work on synthetic signals."""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "measure.py"

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None or not (Path(sys.executable).exists()),
    reason="no python",
)


def test_selftest():
    try:
        import numpy  # noqa: F401
        from scipy import signal  # noqa: F401
    except ImportError:
        pytest.skip("numpy/scipy not installed")
    result = subprocess.run([sys.executable, str(TOOL), "selftest"],
                            capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "selftest OK" in result.stdout
