#!/usr/bin/env python3
"""Instruction counts per audio block for the two-stage convolver (dsp/conv2.c).

Builds firmware/audio/tests/conv2_bench.c + conv2.c + conv.c + CMSIS-DSP for
the Cortex-M7 (the firmware's flags) and runs each bench call in Unicorn,
counting executed instructions. Instructions are not cycles: on the M7 with
code and data in TCM expect about 1..1.3 cycles per instruction here (dual
issue vs FPU/load latency); tail data in OCRAM (D-cache) costs more. The
engine's `prof` console command measures real DWT cycles once integrated.

    CROSS=<toolchain>/bin/arm-none-eabi- python tools/conv2_cycles.py

Needs: unicorn, the Arm GNU toolchain, CMSIS-DSP in firmware/audio/.deps
(`make -C firmware/audio deps`).
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
OUT = FW / "build" / "conv2_bench"
BASE = 0x20000000             # one flat RAM region: timing is not modelled anyway
RET = BASE + 0x1F0000         # return address: stop here
SP = BASE + 0x1E0000
GROUPS = ["BasicMathFunctions", "ComplexMathFunctions", "TransformFunctions",
          "SupportFunctions", "CommonTables"]
LDS = f"""
MEMORY {{ RAM (rwx) : ORIGIN = {BASE:#x}, LENGTH = 0x1D0000 }}
SECTIONS {{
  .text : {{ *(.text*) *(.rodata*) *(.data*) }} > RAM
  .bss (NOLOAD) : {{ *(.bss*) *(COMMON) }} > RAM
  /DISCARD/ : {{ *(.ARM.exidx*) *(.ARM.extab*) }}
}}
"""


def build(cross: str) -> dict[str, int]:
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "bench.ld").write_text(LDS)
    dsp = FW / ".deps" / "cmsis-dsp"
    srcs = [FW / "tests" / "conv2_bench.c", FW / "src" / "dsp" / "conv2.c",
            FW / "src" / "dsp" / "conv.c", FW / "src" / "memfuncs.c",
            *[dsp / "Source" / g / f"{g}.c" for g in GROUPS]]
    elf = OUT / "bench.elf"
    subprocess.run(
        [f"{cross}gcc", "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
         "-O2", "-ffreestanding", "-fno-builtin", "-ffunction-sections", "-fdata-sections",
         "-DARM_MATH_LOOPUNROLL", "-DNDEBUG", "-std=gnu11", "-nostdlib", "-Wl,--gc-sections",
         "-Wl,-e,bench_setup", "-Wl,-u,bench_block", "-Wl,-u,bench_cab_block",
         "-Wl,-u,bench_set_ir", "-Wl,-u,bench_fill", "-Wl,-u,bench_pending", "-Wl,--no-warn-rwx-segments",
         "-I", str(FW / "src"), "-I", str(dsp / "Include"), "-I", str(dsp / "PrivateInclude"),
         "-I", str(FW / ".deps" / "tinyusb" / "lib" / "CMSIS_6" / "CMSIS" / "Core" / "Include"),
         "-T", str(OUT / "bench.ld"), *map(str, srcs), "-lgcc", "-o", str(elf)],
        check=True)
    subprocess.run([f"{cross}objcopy", "-O", "binary", "-j", ".text", str(elf),
                    str(OUT / "bench.bin")], check=True)
    syms = {}
    for line in subprocess.run([f"{cross}nm", str(elf)], capture_output=True, text=True,
                               check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


class Bench:
    def __init__(self, syms: dict[str, int]):
        import unicorn.arm_const as A
        from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_MCLASS, UC_MODE_THUMB, Uc
        self.A, self.syms, self.n = A, syms, 0
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(BASE, 0x200000)
        uc.mem_map(0xE0000000, 0x100000)
        uc.mem_write(0xE000ED88, (0xF << 20).to_bytes(4, "little"))   # CPACR: FPU on
        uc.mem_write(BASE, (OUT / "bench.bin").read_bytes())

        def count(_uc, _addr, _size, _d):
            self.n += 1
        uc.hook_add(UC_HOOK_CODE, count)

    def call(self, name: str, arg: int = 0) -> int:
        """instructions executed by the call"""
        A, uc = self.A, self.uc
        uc.reg_write(A.UC_ARM_REG_SP, SP)
        uc.reg_write(A.UC_ARM_REG_LR, RET | 1)
        uc.reg_write(A.UC_ARM_REG_R0, arg)
        self.n = 0
        uc.emu_start(self.syms[name] | 1, RET, count=50_000_000)
        return self.n

    def ret(self, name: str) -> int:
        """return value (r0) of the call"""
        self.call(name)
        return self.uc.reg_read(self.A.UC_ARM_REG_R0)


def main() -> int:
    cross = os.environ.get("CROSS", "arm-none-eabi-")
    b = Bench(build(cross))
    b.call("bench_setup")
    fill = b.call("bench_fill")
    cab = b.call("bench_cab_block") - fill
    print(f"conv_t 512 taps (the cab before conv2): {cab} instr/block")
    # IR changes in this order: tail off -> on, long -> long, on -> off
    prev = 1
    for taps in (512, 4096, 1024, 2048, 4096, 512):
        n = b.call("bench_set_ir", taps)
        load = []                       # blocks until the new IR plays
        while b.ret("bench_pending"):
            load.append(b.call("bench_block") - fill)
        for _ in range(8):              # a frame to settle
            b.call("bench_block")
        per = [b.call("bench_block") - fill for _ in range(8)]
        worst = max(per)
        print(f"conv2 {prev:4d} -> {taps:4d} taps: set_ir {n} instr", end="")
        if load:
            print(f"; load {len(load)} blocks, worst {max(load)}", end="")
        print("; per block (slice 0..7): " + " ".join(str(p) for p in per)
              + f"; worst {worst} (+{worst - cab} over conv_t), mean {sum(per) / 8:.0f}")
        prev = taps
    return 0


if __name__ == "__main__":
    sys.exit(main())
