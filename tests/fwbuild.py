# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Build each firmware once per process: deps, then a parallel build into its
own tmp dir (make BUILD=...), so parallel pytest workers (pytest -n) never
share one build/ tree."""
import functools
import os
import subprocess
import tempfile
from pathlib import Path


@functools.cache
def build(fw: Path, variant: str = "app") -> Path:
    """Returns the build directory (BUILD=) this process built into."""
    out = Path(tempfile.mkdtemp(prefix="fwbuild_"))
    subprocess.run(["make", f"BUILD={out}", "deps"], cwd=fw, check=True)
    subprocess.run(["make", f"BUILD={out}", f"VARIANT={variant}", f"-j{os.cpu_count() or 1}",
                    "build", "layout"], cwd=fw, check=True)
    return out
