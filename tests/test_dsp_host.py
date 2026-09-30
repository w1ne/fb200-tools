# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

import functools
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None, reason="host C compiler not installed"
)

DSP = FW / ".deps" / "cmsis-dsp"
DSP_GROUPS = ["BasicMathFunctions", "ComplexMathFunctions", "FastMathFunctions",
              "FilteringFunctions", "TransformFunctions", "StatisticsFunctions",
              "SupportFunctions", "CommonTables",
              # arm_mfcc_* call the matrix functions; MinGW's linker keeps them
              "MatrixFunctions"]
# g_stock and stock_check(): amp, tone, cab and drums read the stock data through them
STOCK_SRC = [FW / "src" / "dsp" / "stock_data.c", FW / "src" / "crc32.c"]


@functools.cache
def _build_dir() -> Path:
    """Per process: parallel workers (pytest -n) do not overwrite each other's binaries."""
    return Path(tempfile.mkdtemp(prefix="dsp_host_"))


def cmsis_dsp_args() -> list[str]:
    """CMSIS-DSP sources + flags for a host build. The library is fetched by the
    firmware Makefile rule (pinned tag, SHA-256 checked) rather than skipped:
    a skipped DSP test would read as green."""
    subprocess.run(["make", "-C", str(FW), ".deps/cmsis-dsp/Include/arm_math.h"], check=True)
    strip = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    # __GNUC_PYTHON__ is CMSIS-DSP's own switch for host builds without CMSIS-Core.
    # ARM_MATH_LOOPUNROLL as the firmware (Makefile): the tests check the real build
    return ["-D__GNUC_PYTHON__", "-DARM_MATH_LOOPUNROLL", "-ffunction-sections", "-fdata-sections", strip,
            "-I", str(DSP / "Include"), "-I", str(DSP / "PrivateInclude"),
            *[str(DSP / "Source" / g / f"{g}.c") for g in DSP_GROUPS]]


def test_dsp_host_suite():
    out = _build_dir() / "dsp_host_test"
    # looper.c calls the flash store. MinGW keeps every function in a listed
    # file, so this link would need loopstore. The looper host test covers it.
    sources = [str(p) for p in sorted((FW / "src" / "dsp").glob("*.c")) if p.name != "looper.c"]
    sources.append(str(FW / "src" / "crc32.c"))
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "dsp_host_test.c"), *sources, *cmsis_dsp_args(), "-lm",
         "-o", str(out)],
        check=True,
    )
    result = subprocess.run([str(out)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "dsp host tests OK" in result.stdout


def test_dsp_blocks_suite():
    """Convolver vs brute-force FIR, RBJ biquads vs analytic response."""
    out = _build_dir() / "dsp_blocks_host_test"
    blocks = [FW / "src" / "dsp" / f for f in ("conv.c", "biquad.c", "math.c")]
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "dsp_blocks_host_test.c"), *map(str, blocks), *cmsis_dsp_args(),
         "-lm", "-o", str(out)],
        check=True,
    )
    result = subprocess.run([str(out)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "dsp blocks host tests OK" in result.stdout
    assert "conv 2048 taps" in result.stdout


def test_conv2_suite():
    """Two-stage convolver (M5, up to 4096 taps) vs a double-precision FIR,
    bit-identity with conv_t for IRs <= 512 taps, IR swaps, zero latency."""
    out = _build_dir() / "conv2_host_test"
    mods = [FW / "src" / "dsp" / f for f in ("conv2.c", "conv.c")]
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(FW / "src"),
         str(FW / "tests" / "conv2_host_test.c"), *map(str, mods), *cmsis_dsp_args(),
         "-lm", "-o", str(out)],
        check=True,
    )
    result = subprocess.run([str(out)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "conv2 host tests OK" in result.stdout
    assert "conv2 4096 taps, odd blocks" in result.stdout
    assert "conv2 4096 taps,  8-sample block then" in result.stdout


def test_amp_cab_suite():
    """amp/tone/cab without the stock data (CI): pass-through, cab FIR vs
    brute force, long IRs (gain swaps with the IR), stock user-IR gain formula. Stock parity lives in
    test_stock_dsp_parity.py (needs the vendor .mr)."""
    out = _build_dir() / "amp_cab_host_test"
    mods = [FW / "src" / "dsp" / f for f in ("amp.c", "tone.c", "cab.c", "conv2.c", "conv.c")] + STOCK_SRC
    # -ffp-contract=off as the firmware's amp/tone and FilteringFunctions: the
    # tone_df1 check is bit for bit against CMSIS df1
    subprocess.run(
        ["cc", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "amp_cab_host_test.c"), *map(str, mods), *cmsis_dsp_args(),
         "-lm", "-o", str(out)],
        check=True,
    )
    result = subprocess.run([str(out)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "amp cab host tests OK" in result.stdout
    assert "cab fir: max err" in result.stdout
    assert "cab long IR: swaps at" in result.stdout
    assert "tone_df1 bit-identical to CMSIS df1" in result.stdout


def test_whole_blocks_suite():
    """SAI rings (audio/sai_ring.h): the engine gets whole 32-frame blocks
    only, through odd input chunks, overruns and main-loop stalls; the DAC
    stays on the ADC block grid, the latency comes back after a stall, and
    the cab runs one FFT step per block (no re-phase)."""
    out = _build_dir() / "blocks_host_test"
    mods = [FW / "src" / "dsp" / f for f in ("cab.c", "conv2.c", "conv.c")] + STOCK_SRC
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(FW / "src"),
         str(FW / "tests" / "blocks_host_test.c"), *map(str, mods), *cmsis_dsp_args(),
         "-lm", "-o", str(out)],
        check=True,
    )
    result = subprocess.run([str(out)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout)
    assert "blocks host tests OK" in result.stdout
    for name in ("edma blocks", "odd chunks", "tiny chunks"):
        assert f"{name}: " in result.stdout


def test_eq_suite():
    """Our bass EQ (dsp/eq.c): response vs the RBJ cookbook, flat = bit-exact,
    no clicks on a change, stable at the extremes."""
    out = _build_dir() / "eq_host_test"
    subprocess.run(
        # -ffp-contract=off: the same numbers on every host (clang on arm64
        # fuses); the EQ runs in double, FMA or not is far below its tolerances
        ["cc", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror", "-I", str(FW / "src"),
         str(FW / "tests" / "eq_host_test.c"), str(FW / "src" / "dsp" / "eq.c"),
         *cmsis_dsp_args(), "-lm", "-o", str(out)],
        check=True,
    )
    result = subprocess.run([str(out)], capture_output=True, text=True, check=False)
    print(result.stdout)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "eq host tests OK" in result.stdout
    assert "no clicks:" in result.stdout and "all 7 stages:" in result.stdout


def test_fx_suite():
    """Stock-effect ports: behaviour at 44.1 and 48 kHz (parity: test_fx_parity.py)."""
    out = _build_dir() / "fx_host_test"
    fx = [FW / "src" / "dsp" / f for f in ("detector.c", "gate.c", "comp.c", "mod.c", "reverb.c", "math.c")]
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(FW / "src"),
         str(FW / "tests" / "fx_host_test.c"), *map(str, fx), *cmsis_dsp_args(),
         "-lm", "-o", str(out)],
        check=True,
    )
    result = subprocess.run([str(out)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "fx host tests OK" in result.stdout


def test_engine_drift_suite():
    """Host playback resampler (audio/drift.c drift_rs): no repeated or dropped
    frame at +-500 ppm, no step in a sine, fallbacks; the `usb in|mix` input."""
    out = _build_dir() / "engine_host_test"
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(FW / "src"),
         str(FW / "tests" / "engine_host_test.c"),
         str(FW / "src" / "audio" / "drift.c"), "-lm", "-o", str(out)],
        check=True,
    )
    result = subprocess.run([str(out)], capture_output=True, text=True,
                            check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout)
    assert "drift host tests OK" in result.stdout
    assert "drift_rs  +500 ppm: inserts 0 drops 0" in result.stdout


def test_led_pattern_suite():
    out = _build_dir() / "led_host_test"
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "led_host_test.c"), "-o", str(out)],
        check=True,
    )
    result = subprocess.run([str(out)], capture_output=True, text=True,
                            check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "led host tests OK" in result.stdout


def test_memfuncs_suite():
    """The firmware's memcpy/memset/memmove (word fast paths) vs byte-wise
    references: all alignments, lengths 0..80, overlaps."""
    out = _build_dir() / "memfuncs_host_test"
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-fno-builtin", "-I", str(FW / "src"),
         str(FW / "tests" / "memfuncs_host_test.c"), "-o", str(out)],
        check=True,
    )
    result = subprocess.run([str(out)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "memfuncs host tests OK" in result.stdout


def test_outq_suite():
    """16-bit output stage (dsp/outq.h): rounding beats the v0.9.1 truncation on
    a low-level sine, dither moves the harmonics into noise, 0 stays 0."""
    out = _build_dir() / "outq_host_test"
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(FW / "src"),
         str(FW / "tests" / "outq_host_test.c"), "-lm", "-o", str(out)],
        check=True,
    )
    result = subprocess.run([str(out)], capture_output=True, text=True, check=False)
    print(result.stdout)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "outq host tests OK" in result.stdout
