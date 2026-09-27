#!/usr/bin/env python3
"""Extract the stock FB200 amp models, cab IRs and tone-stack tables as C data.

The stock block 0 is a self-loading image: the vendor loader decompresses the
DSP tables into DTCM. This tool runs that loader in Unicorn (like
boot_dry_run.py), reads the tables from the RAM image and writes a C source
with const float arrays for src/dsp/amp.c, cab.c and tone.c
(declarations: src/dsp/stock_dsp_data.h).

The output is vendor data: it goes to build/ (gitignored) and is never
committed. Needs `unicorn` (not a project dependency):

    python firmware/tools/extract_stock_dsp.py fb200-stock.mr -o firmware/audio/build/stock_dsp_data.c

Stock RAM layout (44.1 kHz designs, float32):
  0x2000954c  10 amp models x 0x630 B: ws[256], +0x420 pre SOS 10 x [b0 b1 b2 a0 a1 a2],
              +0x510 pre gain, +0x528 post SOS 10 x 6, +0x618 out gain, drive scale,
              drive scale 2, level
  0x20011e3c  10 cabs x 513 floats: 512 taps (natural order) + gain
  0x2000d32c  bass, 0x2000d5ac presence, 0x2000d82c treble: 32 x [b0 b1 b2 a1 a2]
  0x2000dd2c  mid, 5 banks (200/400/800/1600/3000 Hz) x 32 x 5, bank stride 0x280
  ITCM 0x318c amp code literals: 3x-rate anti-alias biquad b0, b1, c1, c2
              (y = b0 x + b1 x1 + b0 x2 + c1 y1 - c2 y2)
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "src"))

from fb200.firmware import MrFile

DEFAULT_OUT = REPO_ROOT / "firmware" / "audio" / "build" / "stock_dsp_data.c"

ITCM, DTCM, OCRAM = 0x0, 0x20000000, 0x20200000
LOADER_STUB, LOADER_DONE = 0x600104D9, 0x4D6    # flash stub -> ITCM entry
RAM_SIZES = {ITCM: 0x20000, DTCM: 0x58000, OCRAM: 0x10000}
MAP = [(0, 0x200000), (DTCM, 0x100000), (OCRAM, 0x100000), (0x40000000, 0x10000000),
       (0x60000000, 0x800000), (0xE0000000, 0x100000)]

AMP_BASE, AMP_STRIDE, AMP_COUNT = 0x2000954C, 0x630, 10
CAB_BASE, CAB_COUNT, CAB_TAPS = 0x20011E3C, 10, 512
TONE_BASS, TONE_PRESENCE, TONE_TREBLE = 0x2000D32C, 0x2000D5AC, 0x2000D82C
TONE_MID, TONE_MID_BANKS, TONE_MID_STRIDE = 0x2000DD2C, 5, 0x280
TONE_STEPS = 32
SOS_ROWS = 10
AMP_AA_LITERALS = 0x318C


def load_stock_ram(mr_path: str | Path) -> dict[int, bytes]:
    """Run the vendor self-loader of block 0 until the ITCM entry; return the
    ITCM, DTCM and OCRAM images keyed by base address."""
    try:
        from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_MCLASS, UC_MODE_THUMB, Uc
        from unicorn.arm_const import UC_ARM_REG_SP
    except ImportError:
        sys.exit("extract_stock_dsp: the 'unicorn' module is missing (pip install unicorn); "
                 "it emulates the vendor loader that unpacks the DSP tables")
    mr = MrFile.from_path(mr_path)
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    for base, size in MAP:
        uc.mem_map(base, size)
    uc.mem_write(0x60010000, mr.blocks[0].data)
    uc.mem_write(0x60041000, mr.blocks[1].data)
    uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: u.emu_stop() if a == LOADER_DONE else None)
    uc.reg_write(UC_ARM_REG_SP, 0x20008000)
    uc.emu_start(LOADER_STUB, 0, count=20_000_000)
    return {base: bytes(uc.mem_read(base, size)) for base, size in RAM_SIZES.items()}


def floats(image: bytes, addr: int, count: int, base: int = DTCM) -> list[float]:
    return list(struct.unpack_from(f"<{count}f", image, addr - base))


def sos_rows(c: list[float], first: int) -> list[list[float]]:
    """rows [b0 b1 b2 a0 a1 a2], y = b0x+b1x1+b2x2-a1y1-a2y2 (a0 unused)
    -> CMSIS DF1 {b0, b1, b2, -a1, -a2}"""
    rows = []
    for r in range(SOS_ROWS):
        b0, b1, b2, _a0, a1, a2 = c[first + 6 * r: first + 6 * r + 6]
        rows.append([b0, b1, b2, -a1, -a2])
    return rows


def extract(ram: dict[int, bytes]) -> dict:
    dtcm = ram[DTCM]
    b0, b1, c1, c2 = floats(ram[ITCM], AMP_AA_LITERALS, 4, ITCM)
    amps = []
    for k in range(AMP_COUNT):
        rec = floats(dtcm, AMP_BASE + k * AMP_STRIDE, AMP_STRIDE // 4)
        c = rec[256:]
        amps.append({"ws": rec[:256], "pre": sos_rows(c, 8), "post": sos_rows(c, 74),
                     "pre_gain": c[0x110 // 4], "out_gain": c[0x218 // 4],
                     "drive_scale": c[0x21C // 4], "drive_scale2": c[0x220 // 4],
                     "level": c[0x224 // 4]})
    cabs = [floats(dtcm, CAB_BASE + i * 513 * 4, 513) for i in range(CAB_COUNT)]

    def table(addr):
        t = floats(dtcm, addr, TONE_STEPS * 5)
        return [t[5 * s: 5 * s + 5] for s in range(TONE_STEPS)]
    tone = {"bass": table(TONE_BASS), "presence": table(TONE_PRESENCE),
            "treble": table(TONE_TREBLE),
            "mid": [table(TONE_MID + j * TONE_MID_STRIDE) for j in range(TONE_MID_BANKS)]}
    data = {"amps": amps, "amp_aa": [b0, b1, b0, c1, -c2],   # CMSIS DF1 order
            "cab_taps": [c[:CAB_TAPS] for c in cabs],
            "cab_gain": [c[CAB_TAPS] for c in cabs], "tone": tone}
    check(data)
    return data


def check(data: dict) -> None:
    """Fail loudly on an image whose tables are not where this tool expects."""
    def bad(msg):
        sys.exit(f"extract_stock_dsp: unexpected stock data ({msg}); not the FB200 stock image?")
    everything = []
    for m in data["amps"]:
        everything += m["ws"] + [v for r in m["pre"] + m["post"] for v in r]
        everything += [m["pre_gain"], m["out_gain"], m["drive_scale"], m["drive_scale2"],
                       m["level"]]
        if not all(abs(v) <= 1.5 for v in m["ws"]) or m["ws"][0] != 0.0:
            bad("amp waveshaper out of range")
    aa = data["amp_aa"]
    everything += aa
    if abs(sum(aa[:3]) / (1 - aa[3] - aa[4]) - 1) > 1e-3 or not 0 < aa[0] < 0.2:
        bad("amp anti-alias literals are not a unity-gain low-pass")
    for taps, gain in zip(data["cab_taps"], data["cab_gain"]):
        everything += taps
        if not 0.9 <= max(abs(v) for v in taps) <= 1.0 or not 0.01 < gain < 1.0:
            bad("cab IR not (near) peak-normalised or gain out of range")
    for t in [data["tone"]["bass"], data["tone"]["presence"], data["tone"]["treble"],
              *data["tone"]["mid"]]:
        for row in t:
            everything += row
            if not 0.2 < row[0] < 3.0:
                bad("tone-stack b0 out of range")
    if not all(math.isfinite(v) for v in everything):
        bad("non-finite coefficient")


def cf(v: float) -> str:
    """Exact float32 literal (9 significant digits round-trip)."""
    s = f"{v:.9g}"
    if "e" not in s and "." not in s and "n" not in s:
        s += ".0"
    return s + "f"


def arr(values, per_line: int = 6, indent: str = "    ") -> str:
    vals = [cf(v) for v in values]
    lines = [", ".join(vals[i:i + per_line]) for i in range(0, len(vals), per_line)]
    return "{\n" + ",\n".join(indent + "  " + ln for ln in lines) + "\n" + indent + "}"


def rows(table, indent: str = "    ") -> str:
    return "{\n" + ",\n".join(indent + "  {" + ", ".join(cf(v) for v in r) + "}"
                              for r in table) + "\n" + indent + "}"


def emit_c(data: dict, source_name: str) -> str:
    out = [
        "/* GENERATED by firmware/tools/extract_stock_dsp.py from " + source_name + ".",
        " * Vendor data: do not edit, do not commit. */",
        '#include "dsp/stock_dsp_data.h"',
        "",
        "const stock_amp_model_t stock_amp_models[STOCK_AMP_MODELS] = {",
    ]
    for m in data["amps"]:
        out.append("  {")
        out.append("    .ws = " + arr(m["ws"], 8) + ",")
        out.append("    .pre = " + rows(m["pre"]) + ",")
        out.append("    .post = " + rows(m["post"]) + ",")
        for key in ("pre_gain", "out_gain", "drive_scale", "drive_scale2", "level"):
            out.append(f"    .{key} = {cf(m[key])},")
        out.append("  },")
    out.append("};")
    out.append("const float stock_amp_aa[5] = {" + ", ".join(cf(v) for v in data["amp_aa"]) + "};")
    out.append("")
    out.append("const float stock_cab_taps[STOCK_CABS][STOCK_CAB_TAPS] = {")
    out += ["  " + arr(t, 8, "  ") + "," for t in data["cab_taps"]]
    out.append("};")
    out.append("const float stock_cab_gain[STOCK_CABS] = {"
               + ", ".join(cf(g) for g in data["cab_gain"]) + "};")
    out.append("")
    for name in ("bass", "presence", "treble"):
        out.append(f"const float stock_tone_{name}[STOCK_TONE_STEPS][5] = "
                   + rows(data["tone"][name], "") + ";")
    out.append("const float stock_tone_mid[STOCK_TONE_MID_BANKS][STOCK_TONE_STEPS][5] = {")
    out += ["  " + rows(t, "  ") + "," for t in data["tone"]["mid"]]
    out.append("};")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("mr", type=Path, help="stock firmware .mr (fb200-stock.mr)")
    ap.add_argument("-o", "--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args(argv)
    ram = load_stock_ram(args.mr)
    text = emit_c(extract(ram), args.mr.name)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text)
    print(f"wrote {args.out} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
