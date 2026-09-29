#!/usr/bin/env python3
"""Amp (+ tone stack) and cab cost per 32-sample block on the Cortex-M7 build,
per function, plus a bit-exact old/new comparison.

Builds firmware/audio/tests/engine_bench.c with the DSP sources and CMSIS-DSP
for the M7 with the firmware's flags (-O2; -ffp-contract=off for the stock
objects and FilteringFunctions, as the Makefile), loads the stock data blob
(from $FB200_STOCK_MR) and runs blocks in Unicorn.

    engine_cycles.py [--model N] [--cab N]          profile (instructions, cycle estimate)
    engine_cycles.py --compare REV [--blocks N]     old (git REV) vs this tree, bit for bit

The cycle estimate is a small in-order model of the M7 (see M7Model): result
latencies, dual issue, branch prediction. It is not the pedal: its job is to
show where latency (not instruction count) costs, and before/after deltas.
`--ocram-miss N` adds N cycles per 32-byte line of OCRAM data (on the pedal:
s_cab, engine.c, and the 512-point FFT tables, linker.ld) touched in a block:
the cold D-cache case.

Needs unicorn, capstone, the Arm GNU toolchain (CROSS=<...>/bin/arm-none-eabi-)
and CMSIS-DSP in firmware/audio/.deps (`make -C firmware/audio deps`).
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
DSP = FW / ".deps" / "cmsis-dsp"
BASE = 0x20000000             # one flat RAM region; placement is modelled separately
BLOB = BASE + 0x100000        # stock data blob
RET = BASE + 0x1F0000         # return address: stop here
SP = BASE + 0x1E0000
GROUPS = ["BasicMathFunctions", "ComplexMathFunctions", "FastMathFunctions",
          "FilteringFunctions", "TransformFunctions", "SupportFunctions", "CommonTables"]
DSP_SRC = ["amp.c", "tone.c", "cab.c", "conv.c", "conv2.c"]
NO_CONTRACT = {"amp.c", "tone.c", "cab.c", "FilteringFunctions.c"}   # Makefile STOCK_OBJS
FLAGS = ["-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16", "-O2",
         "-ffreestanding", "-fno-builtin", "-ffunction-sections", "-fdata-sections",
         "-DARM_MATH_LOOPUNROLL", "-DNDEBUG", "-std=gnu11"]
LDS = f"""
MEMORY {{ RAM (rwx) : ORIGIN = {BASE:#x}, LENGTH = 0x100000 }}
SECTIONS {{
  .text : {{ *(.text*) }} > RAM
  .rodata : {{ *(.rodata*) *(.data*) }} > RAM
  .bss (NOLOAD) : {{ *(.bss*) *(COMMON) }} > RAM
  /DISCARD/ : {{ *(.ARM.exidx*) *(.ARM.extab*) }}
}}
"""
# what is in OCRAM (D-cache) on the pedal: engine.c s_cab, linker.ld .ocramdata
OCRAM = ("s_cab", "twiddleCoef_rfft_512", "twiddleCoef_256", "armBitRevIndexTable256",
         "arm_cfft_sR_f32_len256")
# (model, gain, bass, mid, midfreq, treble, volume, cab): the stock presets'
# spread (tests/test_stock_dsp_parity.py) and extremes
SETTINGS = [(m, g, b, mi, mf, t, v, c) for m, g, b, mi, mf, t, v, c in (
    (1, 50, 50, 50, 2, 50, 80, 1), (2, 100, 0, 100, 0, 100, 100, 2),
    (3, 0, 100, 0, 4, 0, 30, 3), (4, 75, 60, 40, 1, 70, 90, 4),
    (5, 30, 20, 80, 3, 20, 60, 5), (6, 90, 50, 50, 2, 50, 100, 6),
    (7, 10, 70, 30, 0, 90, 50, 7), (8, 60, 40, 60, 4, 40, 70, 8),
    (9, 100, 100, 100, 2, 100, 100, 9), (10, 45, 55, 65, 1, 35, 85, 10))]


def _obj(cross: str, src: Path, obj: Path, inc: list[str], cache: bool = False) -> None:
    if cache and obj.exists() and obj.stat().st_mtime >= src.stat().st_mtime:
        return
    extra = ["-ffp-contract=off"] if src.name in NO_CONTRACT else []
    subprocess.run([f"{cross}gcc", *FLAGS, *extra, *inc, "-c", str(src), "-o", str(obj)],
                   check=True)


def build(cross: str, src: Path, out: Path) -> tuple[Path, dict[str, int], list]:
    """ELF from the DSP sources under src (a firmware/audio/src), symbols, functions"""
    out.mkdir(parents=True, exist_ok=True)
    (out / "bench.ld").write_text(LDS)
    inc = ["-I", str(src), "-I", str(DSP / "Include"), "-I", str(DSP / "PrivateInclude"),
           "-I", str(FW / ".deps" / "tinyusb" / "lib" / "CMSIS_6" / "CMSIS" / "Core" / "Include")]
    cm = FW / "build" / "engine_bench" / "cmsis"     # shared by every build
    cm.mkdir(parents=True, exist_ok=True)
    objs = []
    for g in GROUPS:
        objs.append(cm / f"{g}.o")
        _obj(cross, DSP / "Source" / g / f"{g}.c", objs[-1], inc, cache=True)
    for f in [*(src / "dsp" / n for n in DSP_SRC), src / "memfuncs.c",
              FW / "tests" / "engine_bench.c"]:
        objs.append(out / f"{f.stem}.o")
        _obj(cross, f, objs[-1], inc)
    elf = out / "bench.elf"
    subprocess.run([f"{cross}gcc", *FLAGS, "-nostdlib", "-Wl,--gc-sections", "-Wl,-e,bench_setup",
                    "-Wl,-u,bench_amp", "-Wl,-u,bench_cab", "-Wl,--no-warn-rwx-segments",
                    "-T", str(out / "bench.ld"), *map(str, objs), "-lgcc", "-o", str(elf)],
                   check=True)
    subprocess.run([f"{cross}objcopy", "-O", "binary", "-j", ".text", "-j", ".rodata", str(elf),
                    str(out / "bench.bin")], check=True)
    syms, funcs = {}, []
    nm = subprocess.run([f"{cross}nm", "-S", str(elf)], capture_output=True, text=True,
                        check=True).stdout
    for line in nm.splitlines():
        p = line.split()
        if len(p) == 4:
            syms[p[3]] = int(p[0], 16)
            funcs.append((int(p[0], 16) & ~1, int(p[1], 16), p[2], p[3]))
        elif len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    return elf, syms, sorted(funcs)


class M7Model:
    """In-order dual-issue estimate of the Cortex-M7 pipeline. Numbers are the
    public ones where known (load-use 1, FP result latency 3 cycles with one FP
    data-processing op per cycle, divide/sqrt 14, branch mispredict ~6);
    TCM memory: no wait states. Tunable: the point is the relative cost."""

    ALU, MUL, LOAD, STORE, FP, FPDIV, BRANCH = range(7)

    def __init__(self, fp_lat: int = 3, mispredict: int = 6):
        from capstone import CS_ARCH_ARM, CS_MODE_MCLASS, CS_MODE_THUMB, Cs
        self.cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        self.cs.detail = True
        self.fp_lat, self.mispredict = fp_lat, mispredict
        self.cache: dict[int, tuple] = {}
        self.reset()

    def reset(self) -> None:
        self.cyc, self.paired, self.prev = 0, True, None
        self.ready: dict[str, int] = {}
        self.pending = None           # (addr, fallthrough) of the last branch
        self.bht: dict[int, int] = {}
        self.fpdiv_free = 0

    @staticmethod
    def _regs(insn, ids) -> list[str]:
        out = []
        for r in ids:
            n = insn.reg_name(r)
            if n[0] == "d" and n[1:].isdigit():        # d<n> = s<2n>, s<2n+1>
                k = int(n[1:])
                out += [f"s{2 * k}", f"s{2 * k + 1}"]
            elif n not in ("pc", "sp", "itstate"):
                out.append(n)
        return out

    def decode(self, uc, addr: int, size: int) -> tuple:
        info = self.cache.get(addr)
        if info:
            return info
        insn = next(self.cs.disasm(bytes(uc.mem_read(addr, size)), addr))
        rd, wr = insn.regs_access()
        m, op = insn.mnemonic.split(".")[0], insn.op_str
        f64 = ".f64" in insn.mnemonic or "d" in op.split(",")[0][:2] and m[0] == "v"
        occ = 1
        if m in ("ldm", "ldmia", "ldmdb", "pop", "vpop", "vldmia", "vldmdb", "vldm", "stm",
                 "stmia", "stmdb", "push", "vpush", "vstmia", "vstmdb", "vstm"):
            n = op.count(",") + 1 if "-" not in op else 8
            occ = max(1, (n + 1) // 2)
        wr_regs, lat, cls = self._regs(insn, wr), 1, self.ALU
        if "pc" in [insn.reg_name(r) for r in wr] or m in ("b", "bl", "bx", "blx", "cbz",
                                                           "cbnz", "tbb", "tbh"):
            cls = self.BRANCH
        elif m.startswith(("ldr", "ldm", "pop", "vldr", "vldm", "vpop")):
            cls, lat = self.LOAD, 2
        elif m.startswith(("str", "stm", "push", "vstr", "vstm", "vpush")):
            cls = self.STORE
        elif m in ("vdiv", "vsqrt"):
            cls, lat = self.FPDIV, 28 if f64 else 14
        elif m in ("vmla", "vmls", "vnmla", "vnmls"):
            cls, lat = self.FP, 2 * self.fp_lat
        elif m in ("vcmp", "vcmpe"):
            cls, lat = self.FP, 2
            wr_regs = ["fpflags"]
        elif m == "vmrs":
            cls, lat = self.ALU, 1
            rd_regs = ["fpflags"]
        elif m in ("vmov", "vabs", "vneg", "vsel", "vselge", "vselgt", "vseleq", "vselvs",
                   "vmaxnm", "vminnm") or m.startswith("vsel"):
            cls, lat = self.FP, 1
        elif m[0] == "v":
            cls, lat = self.FP, self.fp_lat + (1 if f64 and m == "vmul" else 0)
        elif m in ("mul", "mla", "mls", "smull", "umull", "smlal", "umlal", "smulbb"):
            cls, lat = self.MUL, 2
        rd_regs = rd_regs if m == "vmrs" else self._regs(insn, rd)
        info = (cls, tuple(rd_regs), tuple(wr_regs), lat, occ, addr + size)
        self.cache[addr] = info
        return info

    def step(self, uc, addr: int, size: int) -> None:
        if self.pending:                              # resolve the last branch
            baddr, fall = self.pending
            taken = addr != fall
            ctr = self.bht.get(baddr, 2 if addr < baddr else 1)
            if taken != (ctr >= 2):
                self.cyc += self.mispredict
            self.paired = True                        # a taken branch ends the pair
            self.bht[baddr] = min(3, ctr + 1) if taken else max(0, ctr - 1)
            self.pending = None
        cls, rd, wr, lat, occ, fall = self.decode(uc, addr, size)
        t = max((self.ready.get(r, 0) for r in rd), default=0)
        p = self.prev
        pair = not self.paired and p is not None and p[4] == 1 and occ == 1 and not (
            (p[0] in (self.LOAD, self.STORE) and cls in (self.LOAD, self.STORE))
            or (p[0] in (self.FP, self.FPDIV) and cls in (self.FP, self.FPDIV))
            or (p[0] == self.MUL and cls == self.MUL) or p[0] == self.BRANCH)
        issue = max(self.cyc if pair else self.cyc + 1, t)
        if cls == self.FPDIV:
            issue = max(issue, self.fpdiv_free)
            self.fpdiv_free = issue + lat
        self.paired = issue == self.cyc
        self.cyc = issue + occ - 1
        if occ > 1:
            self.paired = True
        for r in wr:
            self.ready[r] = issue + (lat if cls != self.LOAD else lat + occ - 1)
        self.prev = (cls, rd, wr, lat, occ)
        if cls == self.BRANCH:
            self.pending = (addr, fall)


class Bench:
    def __init__(self, bin_path: Path, syms: dict[str, int], funcs: list, blob: bytes,
                 model: M7Model | None = None):
        import unicorn.arm_const as A
        from unicorn import UC_ARCH_ARM, UC_MODE_MCLASS, UC_MODE_THUMB, Uc
        self.A, self.syms, self.funcs, self.model = A, syms, funcs, model
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.ctl_set_cpu_model(A.UC_CPU_ARM_CORTEX_M7)       # double-precision FPU
        uc.mem_map(BASE, 0x200000)
        uc.mem_map(0xE0000000, 0x100000)
        uc.reg_write(A.UC_ARM_REG_C1_C0_2, 0xF << 20)      # CPACR: FPU on
        # Unicorn's M7 lacks the double-precision ops (cab_set_model uses one):
        # the stock emulator's fallback
        sys.path.insert(0, str(ROOT / "tests"))
        from stock_emu import _dp_hook_factory
        from unicorn import UC_HOOK_INSN_INVALID
        uc.hook_add(UC_HOOK_INSN_INVALID, _dp_hook_factory())
        uc.mem_write(BASE, bin_path.read_bytes())
        uc.mem_write(BLOB, blob)
        self.starts = [f[0] for f in funcs if f[2] in "tTW"]
        self.names = [f[3] for f in funcs if f[2] in "tTW"]
        self.hook = None

    def profile(self, on: bool, lines: dict | None = None) -> None:
        """per-function instruction counts (and the M7 model) while on"""
        from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
        if self.hook:
            for h in self.hook:
                self.uc.hook_del(h)
            self.hook = None
        self.uc.ctl_flush_tb()        # translated blocks keep the hooks of their time
        if not on:
            return
        import bisect
        self.counts: dict[str, int] = {}
        self.cycles: dict[str, int] = {}
        if not hasattr(self, "at"):
            self.at: dict[int, int] = {}          # cycles per address, all calls
        cache: dict[int, str] = {}
        model = self.model

        def code(uc, addr, size, _d):
            name = cache.get(addr)
            if name is None:
                name = cache[addr] = self.names[bisect.bisect_right(self.starts, addr) - 1]
            self.counts[name] = self.counts.get(name, 0) + 1
            if model:
                c0 = model.cyc
                model.step(uc, addr, size)
                self.cycles[name] = self.cycles.get(name, 0) + model.cyc - c0
                self.at[addr] = self.at.get(addr, 0) + model.cyc - c0
        self.hook = [self.uc.hook_add(UC_HOOK_CODE, code)]
        if lines is not None:
            data = sorted((f[0], f[0] + f[1], f[3]) for f in self.funcs if f[2] in "bBdDrR")
            starts = [d[0] for d in data]

            def mem(uc, _acc, addr, _size, _v, _d):
                i = bisect.bisect_right(starts, addr) - 1
                if i >= 0 and addr < data[i][1]:
                    lines.setdefault(data[i][2], set()).add(addr >> 5)
            self.hook.append(self.uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, mem))

    def call(self, name: str, arg: int = 0) -> None:
        A, uc = self.A, self.uc
        uc.reg_write(A.UC_ARM_REG_SP, SP)
        uc.reg_write(A.UC_ARM_REG_LR, RET | 1)
        uc.reg_write(A.UC_ARM_REG_R0, arg)
        if self.model:
            self.model.reset()
        uc.emu_start(self.syms[name] | 1, RET, count=100_000_000)

    def setup(self, setting: tuple) -> None:
        self.uc.mem_write(self.syms["bench_args"],
                          b"".join(int(v).to_bytes(4, "little", signed=True) for v in setting))
        self.call("bench_setup", BLOB)

    def put(self, x) -> None:
        self.uc.mem_write(self.syms["bench_buf"], x.astype("<f4").tobytes())

    def get(self) -> bytes:
        return bytes(self.uc.mem_read(self.syms["bench_buf"], 32 * 4))


def signal(rng, blocks: int):
    """guitar-like test input: random level segments of noise and tones"""
    import numpy as np
    n = blocks * 32
    t = np.arange(n) / 44100.0
    x = np.zeros(n, dtype=np.float64)
    pos = 0
    while pos < n:
        seg = int(rng.integers(256, 8192))
        amp = float(10 ** rng.uniform(-4, 0.3))
        kind = rng.integers(0, 3)
        tt = t[pos:pos + seg]
        if kind == 0:
            s = rng.standard_normal(len(tt)) * 0.3
        elif kind == 1:
            s = np.sin(2 * np.pi * rng.uniform(30, 400) * tt) * np.exp(-tt * rng.uniform(0, 8) % 5)
        else:
            s = np.zeros(len(tt))
        x[pos:pos + seg] = amp * s
        pos += seg
    return x.astype(np.float32)


def load_blob() -> bytes:
    sys.path.insert(0, str(ROOT / "src"))
    from fb200 import stockdata
    mr = Path(os.environ.get("FB200_STOCK_MR", ROOT / "fb200-stock.mr"))
    return stockdata.build(mr.read_bytes())


def warm(b: Bench, blocks: int = 1300) -> None:
    import numpy as np
    z = np.zeros(32, dtype=np.float32)
    for _ in range(blocks):                   # drive/volume smoothers settle
        b.put(z)
        b.call("bench_amp")
        b.call("bench_cab")


def annotate(b: Bench, cross: str, elf: Path, func: str, blocks: int) -> None:
    """disassembly of func with the model's cycles per instruction per block"""
    dis = subprocess.run([f"{cross}objdump", "-d", "--no-show-raw-insn", f"--disassemble={func}",
                          str(elf)], capture_output=True, text=True, check=True).stdout
    for line in dis.splitlines():
        head = line.split(":")[0].strip()
        try:
            addr = int(head, 16)
        except ValueError:
            continue
        if "\t" in line:
            print(f"{b.at.get(addr, 0) / blocks:7.0f} {line.strip()}")


def do_profile(args, cross: str) -> int:
    import numpy as np
    elf, syms, funcs = build(cross, FW / "src", FW / "build" / "engine_bench" / "new")
    model = M7Model(fp_lat=args.fp_lat, mispredict=args.mispredict)
    b = Bench(elf.with_name("bench.bin"), syms, funcs, load_blob(), model)
    setting = next(s for s in SETTINGS if s[0] == args.model)
    setting = (*setting[:7], args.cab, args.taps)
    b.setup(setting)
    warm(b)
    rng = np.random.default_rng(1)
    x = signal(rng, args.blocks)
    tot = {}
    for stage in ("bench_amp", "bench_cab"):
        agg_n: dict[str, int] = {}
        agg_c: dict[str, int] = {}
        lines: dict[str, set] = {}
        for k in range(args.blocks):
            b.put(x[32 * k:32 * k + 32])
            if stage == "bench_cab":
                b.call("bench_amp")
            blk: dict = {}
            b.profile(True, blk)
            b.call(stage)
            b.profile(False)
            for f, v in b.counts.items():
                agg_n[f] = agg_n.get(f, 0) + v
            for f, v in b.cycles.items():
                agg_c[f] = agg_c.get(f, 0) + v
            for s, v in blk.items():
                lines.setdefault(s, []).append(len(v))
        nb = args.blocks
        n_all, c_all = sum(agg_n.values()) / nb, sum(agg_c.values()) / nb
        ocram = sum(sum(lines.get(s, [0])) for s in OCRAM) / nb
        tot[stage] = (n_all, c_all, ocram)
        print(f"{stage[6:]}: {n_all:.0f} instr/block, model {c_all:.0f} cycles/block "
              f"(CPI {c_all / n_all:.2f})")
        for f in sorted(agg_n, key=lambda f: -agg_c.get(f, 0)):
            print(f"  {f:40s} {agg_n[f] / nb:8.0f} instr {agg_c.get(f, 0) / nb:8.0f} cycles")
        for s in sorted(lines, key=lambda s: -sum(lines[s])):
            print(f"  data {s:20s} {sum(lines[s]) / nb:6.0f} lines of 32 B touched/block")
        if args.annotate:
            annotate(b, cross, elf, args.annotate, args.blocks)
            b.at = {}
    a, c = tot["bench_amp"], tot["bench_cab"]
    if args.ocram_miss:
        print(f"cab with its OCRAM data cold: +{c[2] * args.ocram_miss:.0f} cycles "
              f"({c[2]:.0f} lines x {args.ocram_miss})")
    print(f"summary: amp {a[0]:.0f} instr / {a[1]:.0f} model cycles; "
          f"cab {c[0]:.0f} instr / {c[1]:.0f} model cycles")
    return 0


def do_compare(args, cross: str) -> int:
    """the DSP sources at git REV vs this tree: every output block bit for bit"""
    import numpy as np
    base = FW / "build" / "engine_bench" / "base-src"
    subprocess.run(["rm", "-rf", str(base)], check=True)
    base.mkdir(parents=True)
    arch = subprocess.run(["git", "-C", str(ROOT), "archive", args.compare, "firmware/audio/src"],
                          capture_output=True, check=True).stdout
    subprocess.run(["tar", "-x", "-C", str(base), "--strip-components=2"], input=arch, check=True)
    blob = load_blob()
    old = build(cross, base / "src", FW / "build" / "engine_bench" / "old")
    new = build(cross, FW / "src", FW / "build" / "engine_bench" / "new")
    bo, bn = (Bench(e.with_name("bench.bin"), s, f, blob) for e, s, f in (old, new))
    rng = np.random.default_rng(args.seed)
    bad = 0
    for setting in [(*s, 0) for s in SETTINGS] + [(*SETTINGS[0], 4096), (*SETTINGS[5], 2000)]:
        for b in (bo, bn):
            b.setup(setting)
        x = signal(rng, args.blocks)
        diff = 0
        for k in range(args.blocks):
            for stage in ("bench_amp", "bench_cab"):
                if stage == "bench_amp":
                    for b in (bo, bn):
                        b.put(x[32 * k:32 * k + 32])
                for b in (bo, bn):
                    b.call(stage)
                if bo.get() != bn.get():
                    diff += 1
                    bn.uc.mem_write(bn.syms["bench_buf"], bo.get())   # keep comparing stages
        print(f"model {setting[0]:2d} cab {setting[7]:2d} taps {setting[8] or 512:4d}: "
              f"{args.blocks} blocks, "
              f"{diff} differing block outputs")
        bad += diff
    print("BIT-EXACT" if bad == 0 else f"DIFFERENT: {bad} block outputs")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--model", type=int, default=1)
    ap.add_argument("--cab", type=int, default=1)
    ap.add_argument("--taps", type=int, default=0, help="long IR taps (0: the stock 512)")
    ap.add_argument("--blocks", type=int, default=0, help="default 20 (profile), 2000 (compare)")
    ap.add_argument("--fp-lat", type=int, default=3)
    ap.add_argument("--mispredict", type=int, default=6)
    ap.add_argument("--ocram-miss", type=int, default=0)
    ap.add_argument("--compare", metavar="REV")
    ap.add_argument("--annotate", metavar="FUNC", help="cycles per instruction of FUNC")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    cross = os.environ.get("CROSS", "arm-none-eabi-")
    if args.compare:
        args.blocks = args.blocks or 2000
        return do_compare(args, cross)
    args.blocks = args.blocks or 20
    return do_profile(args, cross)


if __name__ == "__main__":
    sys.exit(main())
