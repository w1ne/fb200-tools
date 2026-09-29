"""Fuzzing of the firmware's untrusted-input parsers on the host, under ASan and
UBSan (firmware/audio/tests/fuzz_host_test.c): the app protocol (HID/BLE),
the USB console, presets/settings from flash, the stock data blob.

Default suite: the regression cases (one per bug the fuzzer found) and a
short fixed-seed smoke run per target. `pytest -m fuzz`: long runs with a new
seed each time (FB200_FUZZ_SECONDS, default 60 per target; FB200_FUZZ_SEED to
repeat one), and the same harness built with gcc in `docker run gcc:14` when
docker is there. A failure prints the input that broke it (hex)."""

from __future__ import annotations

import functools
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pytest
from test_dsp_host import DSP, DSP_GROUPS

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"

def _sanitizers_link() -> bool:
    """ASan/UBSan need their runtime libraries (MinGW gcc on Windows has none)."""
    if shutil.which("cc") is None:
        return False
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / "t.c"
        src.write_text("int main(void) { return 0; }\n")
        r = subprocess.run(["cc", *SAN, str(src), "-o", str(Path(d) / "t")],
                           capture_output=True, check=False)
        return r.returncode == 0


SAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
pytestmark = pytest.mark.skipif(not _sanitizers_link(),
                                reason="host C compiler with ASan/UBSan runtime not installed")

TARGETS = ["proto", "console", "preset", "stock"]
FW_SRC = ["proto/proto.c", "ui/ui.c", "ui/lightbar.c", "preset/preset_check.c",
          "dsp/stock_data.c", "crc32.c", "dsp/drums.c", "dsp/eq.c", "dsp/testgen.c",
          "dsp/math.c", "dsp/delay.c", "dsp/amp.c", "dsp/tone.c", "dsp/cab.c", "dsp/conv.c",
          "dsp/conv2.c", "dsp/gate.c", "dsp/detector.c", "dsp/comp.c", "dsp/mod.c",
          "dsp/reverb.c", "dsp/looper.c"]

def _is_clang(cc: str) -> bool:
    out = subprocess.run([cc, "--version"], capture_output=True, text=True, check=False).stdout
    return "clang" in out


def build_cmds(cc: str, out: Path, rel: Path = FW, coverage: bool | None = None) -> list[list[str]]:
    """CMSIS-DSP once without sanitizers (it is not under test, and ASan makes
    it slow), then the harness and the firmware sources with ASan/UBSan and,
    with clang, edge coverage for the fuzz loop."""
    subprocess.run(["make", "-C", str(FW), ".deps/cmsis-dsp/Include/arm_math.h"], check=True,
                   capture_output=True)
    dsp = rel / DSP.relative_to(FW)
    inc = ["-I", str(dsp / "Include"), "-I", str(dsp / "PrivateInclude")]
    defs = ["-D__GNUC_PYTHON__", "-DARM_MATH_LOOPUNROLL"]
    cmds, objs = [], []
    for g in DSP_GROUPS:
        obj = out / f"{g}.o"
        cmds.append([cc, "-O2", "-w", *defs, *inc, "-c", str(dsp / "Source" / g / f"{g}.c"), "-o", str(obj)])
        objs.append(str(obj))
    if coverage is None:
        coverage = _is_clang(cc)
    cov = ["-fsanitize-coverage=trace-pc-guard", "-DFUZZ_COVERAGE"] if coverage else []
    cmds.append([cc, "-g", "-O1", *SAN, *cov, "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                 *defs, "-I", str(rel / "tests" / "host_include"), "-I", str(rel / "src"),
                 "-I", str(rel / "src" / "debug"), *inc,
                 str(rel / "tests" / "fuzz_host_test.c"), *[str(rel / "src" / s) for s in FW_SRC],
                 *objs, "-lm", "-o", str(out / "fuzz_host_test")])
    return cmds


@functools.cache
def build() -> Path:
    out = Path(tempfile.mkdtemp(prefix="fuzz_host_"))
    for cmd in build_cmds("cc", out):
        subprocess.run(cmd, check=True)
    return out / "fuzz_host_test"


def run(exe: Path, *args: str, timeout: float = 600) -> subprocess.CompletedProcess:
    env = dict(os.environ, UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1",
               ASAN_OPTIONS="detect_leaks=0:abort_on_error=0")
    return subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=timeout,
                          env=env, check=False)


# One regression case per bug the fuzzer found (fuzz_host_test.c cases()).
CASES = ["console_peek_hole", "console_crc_span", "console_poke_flash", "console_dumpmem_end",
         "console_gain_range", "console_testgen_range", "console_missing_arg", "preset_erased",
         "preset_reverb_runaway", "proto_edit_load_checked", "settings_erased", "stock_bad_tables"]


def test_regression_cases():
    """Every case runs (one build for all: the harness takes a minute to build
    with the sanitizers, and xdist workers do not share it)."""
    r = run(build(), "cases")
    done = set(r.stdout.splitlines())
    missing = [c for c in CASES if f"ok {c}" not in done]
    assert r.returncode == 0 and not missing, f"failed: {missing}\n" + r.stdout + r.stderr[-4000:]


def test_fuzz_smoke():
    """A short fixed-seed run of each target: the same inputs every time."""
    for target in TARGETS:
        r = run(build(), target, "1", "3", "3000")
        assert r.returncode == 0, target + "\n" + r.stdout + r.stderr[-4000:]
        assert f"fuzz {target}:" in r.stdout


@pytest.mark.fuzz
@pytest.mark.parametrize("target", TARGETS)
def test_fuzz_long(target):
    seconds = os.environ.get("FB200_FUZZ_SECONDS", "60")
    seed = os.environ.get("FB200_FUZZ_SEED", str(int(time.time())))
    r = run(build(), target, seed, seconds, timeout=float(seconds) * 4 + 120)
    print(r.stdout)
    assert r.returncode == 0, f"seed {seed}\n" + r.stdout + r.stderr[-6000:]


@pytest.mark.fuzz
@pytest.mark.skipif(shutil.which("docker") is None, reason="docker not installed")
def test_fuzz_gcc_docker():
    """The same harness built by gcc 14 (its own ASan/UBSan) on Linux."""
    if subprocess.run(["docker", "info"], capture_output=True, check=False).returncode:
        pytest.skip("docker daemon not running")
    seconds = os.environ.get("FB200_FUZZ_SECONDS", "20")
    seed = os.environ.get("FB200_FUZZ_SEED", str(int(time.time())))
    w = Path("/w")
    script = " && ".join(
        [" ".join(c) for c in build_cmds("gcc", Path("/tmp"), rel=w / "firmware" / "audio", coverage=False)]
        + ["/tmp/fuzz_host_test cases"]
        + [f"/tmp/fuzz_host_test {t} {seed} {seconds}" for t in TARGETS])
    r = subprocess.run(["docker", "run", "--rm", "-v", f"{ROOT}:/w:ro", "-w", "/w", "gcc:14",
                        "sh", "-c", script], capture_output=True, text=True, check=False,
                       timeout=float(seconds) * 8 + 900)
    print(r.stdout)
    assert r.returncode == 0, f"seed {seed}\n" + r.stdout + r.stderr[-6000:]


if __name__ == "__main__":   # manual: python tests/test_fuzz_host.py <target> <seed> <seconds>
    sys.exit(run(build(), *sys.argv[1:], timeout=1e9).returncode)
