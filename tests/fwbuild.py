"""Build each firmware once per test session: clean, deps, then a parallel build."""
import functools
import os
import subprocess
from pathlib import Path


@functools.cache
def build(fw: Path) -> None:
    subprocess.run(["make", "clean", "deps"], cwd=fw, check=True)
    subprocess.run(["make", f"-j{os.cpu_count() or 1}", "build", "layout"], cwd=fw, check=True)
