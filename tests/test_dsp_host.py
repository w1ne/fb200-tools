import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
OUT = FW / "build" / "dsp_host_test"
OUT_ENGINE = FW / "build" / "engine_host_test"
OUT_LED = FW / "build" / "led_host_test"

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None, reason="host C compiler not installed"
)

DSP = FW / ".deps" / "cmsis-dsp"
DSP_GROUPS = ["BasicMathFunctions", "ComplexMathFunctions", "FastMathFunctions",
              "FilteringFunctions", "TransformFunctions", "StatisticsFunctions",
              "SupportFunctions", "CommonTables",
              # arm_mfcc_* call the matrix functions; MinGW's linker keeps them
              "MatrixFunctions"]
OUT_BLOCKS = FW / "build" / "dsp_blocks_host_test"
# g_stock and stock_check(): amp, tone, cab and drums read the stock data through them
STOCK_SRC = [FW / "src" / "dsp" / "stock_data.c", FW / "src" / "crc32.c"]


def cmsis_dsp_args() -> list[str]:
    """CMSIS-DSP sources + flags for a host build. The library is fetched by the
    firmware Makefile rule (pinned tag, SHA-256 checked) rather than skipped:
    a skipped DSP test would read as green."""
    subprocess.run(["make", "-C", str(FW), ".deps/cmsis-dsp/Include/arm_math.h"], check=True)
    strip = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    # __GNUC_PYTHON__ is CMSIS-DSP's own switch for host builds without CMSIS-Core.
    return ["-D__GNUC_PYTHON__", "-ffunction-sections", "-fdata-sections", strip,
            "-I", str(DSP / "Include"), "-I", str(DSP / "PrivateInclude"),
            *[str(DSP / "Source" / g / f"{g}.c") for g in DSP_GROUPS]]


def test_dsp_host_suite():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    sources = [str(p) for p in sorted((FW / "src" / "dsp").glob("*.c"))] + [str(FW / "src" / "crc32.c")]
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "dsp_host_test.c"), *sources, *cmsis_dsp_args(), "-lm",
         "-o", str(OUT)],
        check=True,
    )
    result = subprocess.run([str(OUT)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "dsp host tests OK" in result.stdout


def test_dsp_blocks_suite():
    """Convolver vs brute-force FIR, RBJ biquads vs analytic response."""
    OUT_BLOCKS.parent.mkdir(parents=True, exist_ok=True)
    blocks = [FW / "src" / "dsp" / f for f in ("conv.c", "biquad.c", "math.c")]
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "dsp_blocks_host_test.c"), *map(str, blocks), *cmsis_dsp_args(),
         "-lm", "-o", str(OUT_BLOCKS)],
        check=True,
    )
    result = subprocess.run([str(OUT_BLOCKS)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "dsp blocks host tests OK" in result.stdout
    assert "conv 2048 taps" in result.stdout


def test_amp_cab_suite():
    """amp/tone/cab without the stock data (CI): pass-through, cab FIR vs
    brute force, stock user-IR gain formula. Stock parity lives in
    test_stock_dsp_parity.py (needs the vendor .mr)."""
    out = FW / "build" / "amp_cab_host_test"
    out.parent.mkdir(parents=True, exist_ok=True)
    mods = [FW / "src" / "dsp" / f for f in ("amp.c", "tone.c", "cab.c")] + STOCK_SRC
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "amp_cab_host_test.c"), *map(str, mods), *cmsis_dsp_args(),
         "-lm", "-o", str(out)],
        check=True,
    )
    result = subprocess.run([str(out)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "amp cab host tests OK" in result.stdout
    assert "cab fir: max err" in result.stdout


OUT_FX = FW / "build" / "fx_host_test"


def test_fx_suite():
    """Stock-effect ports: behaviour at 44.1 and 48 kHz (parity: test_fx_parity.py)."""
    OUT_FX.parent.mkdir(parents=True, exist_ok=True)
    fx = [FW / "src" / "dsp" / f for f in ("detector.c", "gate.c", "comp.c", "mod.c", "reverb.c", "math.c")]
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(FW / "src"),
         str(FW / "tests" / "fx_host_test.c"), *map(str, fx), *cmsis_dsp_args(),
         "-lm", "-o", str(OUT_FX)],
        check=True,
    )
    result = subprocess.run([str(OUT_FX)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "fx host tests OK" in result.stdout


def test_engine_drift_suite():
    OUT_ENGINE.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "engine_host_test.c"),
         str(FW / "src" / "audio" / "drift.c"), "-o", str(OUT_ENGINE)],
        check=True,
    )
    result = subprocess.run([str(OUT_ENGINE)], capture_output=True, text=True,
                            check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "drift host tests OK" in result.stdout


def test_led_pattern_suite():
    OUT_LED.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "led_host_test.c"), "-o", str(OUT_LED)],
        check=True,
    )
    result = subprocess.run([str(OUT_LED)], capture_output=True, text=True,
                            check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "led host tests OK" in result.stdout
