#!/usr/bin/env python3
"""Render reference audio through the STOCK FB200 DSP, emulated in Unicorn.

The stock per-sample callback (ITCM 0x17d8c) and the cab FIR block job
(0x26c4) run on the RAM image that the vendor loader unpacks from the .mr
(load_stock_ram). Unicorn's Cortex-M7 model has no
double-precision FPU, so the few f64 instructions the stock uses are
emulated in software (capstone decodes them). This is the reference for the
parity tests of src/dsp/amp.c, tone.c and cab.c (tests/test_stock_dsp_parity.py).

Needs unicorn, capstone and numpy (not project dependencies):

    python firmware/tools/stock_render.py fb200-stock.mr OUTDIR --presets 0,2,5,8,16

For each factory preset P it writes, into OUTDIR:
  pP.in.f32     float32 amp input, tapped at the stock amp's entry (~2 x ADC)
  pP.amp.f32    float32 amp + tone stack output, tapped inside the stock amp
  pP.stock.f32  float32 stock output (left channel, after the cab)
and manifest.json with the preset's amp/cab parameters. Only the amp, tone
stack and cab run: the other effect blocks (preset +0x14, +0x5c, +0x74,
+0xa4) are switched off. --ir-gains N adds N random user IRs (irs.f32) and
the stock gain the cab code computes for each (0x5918).
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "src"))

import numpy as np

from fb200.firmware import MrFile

ITCM, DTCM, OCRAM = 0x0, 0x20000000, 0x20200000
LOADER_STUB, LOADER_DONE = 0x600104D9, 0x4D6    # flash stub -> ITCM entry
# the pedal's RAM (ITCM 128 kB, DTCM 352 kB, OCRAM 32 kB: docs/FIRMWARE_BRINGUP.md)
RAM_SIZES = {ITCM: 0x20000, DTCM: 0x58000, OCRAM: 0x8000}
MAP = [(ITCM, 0x20000), (DTCM, 0x58000), (OCRAM, 0x8000), (0x40000000, 0x10000000),
       (0x60000000, 0x800000), (0xE0000000, 0x100000)]


def load_stock_ram(mr_path: str | Path) -> dict[int, bytes]:
    """Run the vendor self-loader of block 0 until the ITCM entry; return the
    ITCM, DTCM and OCRAM images keyed by base address."""
    from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_MCLASS, UC_MODE_THUMB, Uc
    from unicorn.arm_const import UC_ARM_REG_SP

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

FS = 44100
RET = 0x1FFE0                        # magic return address (ITCM)
PRESET_CUR, PRESET_TAB = 0x2001DC40, 0x20004E40
GLOBAL_CUR, GLOBAL_DEF = 0x2001DD40, 0x20004E00
FADE, IN, OUT = 0x200088A4, 0x20057000, 0x20057040
LICENCE_OK = 0x20016E84
FN_DSP_INIT, FN_MID_BANK, FN_COMMIT = 0x90C4, 0x90E4, 0x174A4
FN_CALLBACK, FN_CAB_JOB, FN_IR_GAIN = 0x17D8C, 0x26C4, 0x5918
AMP_ENTRY, AMP_STORE = 0x2D78, 0x36F4     # amp(in r0 -> [r0]); final vstr s0 of its output
IR_BUF, IR_GAIN_OUT = 0x2001B420, 0x20010E34
EFFECT_ENABLES = (0x14, 0x5C, 0x74, 0xA4)
F_MODEL, F_GAIN, F_BASS, F_MID, F_MIDFREQ, F_TREBLE, F_VOL, F_CAB = (
    0x2E, 0x30, 0x32, 0x34, 0x36, 0x38, 0x3A, 0x46)


class SoftF64:
    """Software f64 for the VFP double instructions Unicorn's M7 lacks."""

    def __init__(self, uc):
        import capstone
        from unicorn import UC_HOOK_INSN_INVALID
        self.md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
        self.cache = {}
        uc.hook_add(UC_HOOK_INSN_INVALID, self.hook)

    @staticmethod
    def _gd(uc, n):
        from unicorn.arm_const import UC_ARM_REG_S0
        lo, hi = uc.reg_read(UC_ARM_REG_S0 + 2 * n), uc.reg_read(UC_ARM_REG_S0 + 2 * n + 1)
        return struct.unpack("<d", struct.pack("<II", lo, hi))[0]

    @staticmethod
    def _sd(uc, n, v):
        from unicorn.arm_const import UC_ARM_REG_S0
        lo, hi = struct.unpack("<II", struct.pack("<d", v))
        uc.reg_write(UC_ARM_REG_S0 + 2 * n, lo)
        uc.reg_write(UC_ARM_REG_S0 + 2 * n + 1, hi)

    def hook(self, uc, _user):
        from unicorn.arm_const import (
            UC_ARM_REG_FPSCR,
            UC_ARM_REG_PC,
            UC_ARM_REG_S0,
            UC_ARM_REG_XPSR,
        )
        pc = uc.reg_read(UC_ARM_REG_PC)
        raw = bytes(uc.mem_read(pc, 4))
        if raw not in self.cache:
            ins = next(self.md.disasm(raw, pc, 1))
            self.cache[raw] = (ins.mnemonic, [o.strip() for o in ins.op_str.split(",")], ins.size)
        mn, ops, size = self.cache[raw]
        # IT blocks: QEMU faults before the condition check, so honour ITSTATE here
        xpsr = uc.reg_read(UC_ARM_REG_XPSR)
        it = ((xpsr >> 25) & 3) | (((xpsr >> 10) & 0x3F) << 2)
        if it & 0xF:
            cond = it >> 4
            n, z, c, v = (xpsr >> 31) & 1, (xpsr >> 30) & 1, (xpsr >> 29) & 1, (xpsr >> 28) & 1
            base = {0: z, 2: c, 4: n, 6: v, 8: c and not z, 10: n == v, 12: (not z) and n == v,
                    14: 1}[cond & ~1]
            ok = bool(base) if cond % 2 == 0 or cond == 15 else not bool(base)
            nit = (it & 0xE0) | ((it << 1) & 0x1F) if (it & 0x7) != 0 else 0
            xpsr = (xpsr & ~((3 << 25) | (0x3F << 10))) | ((nit & 3) << 25) | (((nit >> 2) & 0x3F) << 10)
            uc.reg_write(UC_ARM_REG_XPSR, xpsr)
            if not ok:
                uc.reg_write(UC_ARM_REG_PC, (pc + size) | 1)
                return True

        def rd(o):
            return float(o[1:]) if o.startswith("#") else self._gd(uc, int(o[1:]))

        def wd(o, val):
            self._sd(uc, int(o[1:]), val)

        def sreg(o):
            return UC_ARM_REG_S0 + int(o[1:])
        if mn == "vcvt.f64.f32":
            wd(ops[0], struct.unpack("<f", struct.pack("<I", uc.reg_read(sreg(ops[1]))))[0])
        elif mn == "vcvt.f32.f64":
            f = struct.unpack("<I", struct.pack("<f", float(np.float32(rd(ops[1])))))[0]
            uc.reg_write(sreg(ops[0]), f)
        elif mn in ("vcvt.f64.s32", "vcvt.f64.u32"):
            iv = uc.reg_read(sreg(ops[1]))
            wd(ops[0], iv - (1 << 32) if mn.endswith("s32") and iv & 0x80000000 else iv)
        elif mn in ("vcvt.u32.f64", "vcvt.s32.f64"):
            val = rd(ops[1])
            val = 0 if math.isnan(val) else math.trunc(val)
            val = min(max(val, 0), 0xFFFFFFFF) if mn.startswith("vcvt.u32") else \
                min(max(val, -2**31), 2**31 - 1)
            uc.reg_write(sreg(ops[0]), val & 0xFFFFFFFF)
        elif mn in ("vmul.f64", "vadd.f64", "vsub.f64", "vdiv.f64"):
            a, b = (rd(ops[0]), rd(ops[1])) if len(ops) == 2 else (rd(ops[1]), rd(ops[2]))
            if mn == "vdiv.f64":
                res = a / b if b != 0 else (math.copysign(math.inf, a) if a != 0 else math.nan)
            else:
                res = {"vmul.f64": a * b, "vadd.f64": a + b, "vsub.f64": a - b}[mn]
            wd(ops[0], res)
        elif mn == "vmla.f64":
            wd(ops[0], rd(ops[0]) + rd(ops[1]) * rd(ops[2]))
        elif mn == "vmls.f64":
            wd(ops[0], rd(ops[0]) - rd(ops[1]) * rd(ops[2]))
        elif mn == "vnmls.f64":
            wd(ops[0], rd(ops[1]) * rd(ops[2]) - rd(ops[0]))
        elif mn == "vmov.f64":
            wd(ops[0], rd(ops[1]))
        elif mn == "vneg.f64":
            wd(ops[0], -rd(ops[1]))
        elif mn == "vabs.f64":
            wd(ops[0], abs(rd(ops[1])))
        elif mn == "vsqrt.f64":
            val = rd(ops[1])
            wd(ops[0], math.sqrt(val) if val >= 0 else math.nan)
        elif mn in ("vcmpe.f64", "vcmp.f64"):
            a = rd(ops[0])
            b = 0.0 if ops[1].startswith("#0") else rd(ops[1])
            nzcv = 0b0011 if math.isnan(a) or math.isnan(b) else \
                0b0110 if a == b else 0b1000 if a < b else 0b0010
            fp = uc.reg_read(UC_ARM_REG_FPSCR)
            uc.reg_write(UC_ARM_REG_FPSCR, (fp & 0x0FFFFFFF) | (nzcv << 28))
        else:
            return False
        uc.reg_write(UC_ARM_REG_PC, (pc + size) | 1)
        return True


class StockDSP:
    """The stock per-sample DSP callback on the unpacked RAM image."""

    def __init__(self, mr_path: str | Path):
        from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_MCLASS, UC_MODE_THUMB, Uc

        from fb200.firmware import MrFile
        ram = load_stock_ram(mr_path)
        mr = MrFile.from_path(mr_path)
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        for base, size in MAP:
            uc.mem_map(base, size)
        for base in (ITCM, DTCM, OCRAM):
            uc.mem_write(base, ram[base])
        uc.mem_write(0x60010000, mr.blocks[0].data)
        bank, blob = mr.blocks[1].data, b""       # drum bank, contiguous at 0x600d0000
        for i in range(struct.unpack_from("<I", bank, 0)[0]):
            page, size = struct.unpack_from("<II", bank, 8 + 8 * i)
            blob += bank[page * 512: page * 512 + size]
        uc.mem_write(0x600D0000, blob)
        uc.mem_write(RET, b"\x00\xbf\x00\xbf")
        uc.mem_write(0xE000ED88, struct.pack("<I", 0xF << 20))   # CPACR: FPU on
        self.uc = uc
        self.f64 = SoftF64(uc)
        uc.mem_write(LICENCE_OK, struct.pack("<I", 1))             # skip licence check
        uc.mem_write(GLOBAL_CUR, bytes(uc.mem_read(GLOBAL_DEF, 0x31)))
        uc.mem_write(GLOBAL_CUR + 0x18, bytes([100]))              # master 100
        uc.mem_write(GLOBAL_CUR + 0x2E, bytes([0]))                # un-mute
        # taps on the amp's own input and output, so parity can be checked on
        # exactly what the amp sees (hooks must exist before code is translated)
        self.amp_in, self.amp_out = [], []
        uc.hook_add(UC_HOOK_CODE, self._amp_tap, begin=AMP_ENTRY, end=AMP_ENTRY)
        uc.hook_add(UC_HOOK_CODE, self._amp_tap, begin=AMP_STORE, end=AMP_STORE)
        self.call(FN_DSP_INIT)

    def _amp_tap(self, uc, addr, _size, _user):
        from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_S0
        if addr == AMP_ENTRY:
            self.amp_in.append(struct.unpack("<f", uc.mem_read(uc.reg_read(UC_ARM_REG_R0), 4))[0])
        else:
            self.amp_out.append(struct.unpack("<f", struct.pack("<I", uc.reg_read(UC_ARM_REG_S0)))[0])

    def call(self, fn, args=()):
        from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_S0, UC_ARM_REG_SP
        uc = self.uc
        for i, v in enumerate(args):
            uc.reg_write(UC_ARM_REG_R0 + i, v)
        uc.reg_write(UC_ARM_REG_SP, 0x20057F00)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        uc.emu_start(fn | 1, RET, count=5_000_000)
        return struct.unpack("<f", struct.pack("<I", uc.reg_read(UC_ARM_REG_S0)))[0]

    def field(self, off):
        return struct.unpack("<H", self.uc.mem_read(PRESET_CUR + off, 2))[0]

    def set_field(self, off, v):
        self.uc.mem_write(PRESET_CUR + off, struct.pack("<H", v))

    def load_preset(self, index, amp_cab_only=True):
        uc = self.uc
        uc.mem_write(PRESET_CUR, bytes(uc.mem_read(PRESET_TAB + index * 0x100, 0x100)))
        if amp_cab_only:
            for off in EFFECT_ENABLES:
                self.set_field(off, 0)
        self.process(np.zeros(64))                  # the callback copies knobs in
        self.call(FN_MID_BANK)                      # mid-EQ bank from +0x36 (main loop)
        uc.mem_write(FADE + 0xF, b"\x01")
        uc.mem_write(FADE + 0xD, b"\x01")
        self.call(FN_COMMIT, (FADE,))               # re-init + commit, like the main loop
        self.process(np.zeros(4096))                # fade-in

    def process(self, x):
        """x: ADC samples (fed to L and R). Returns the left output channel;
        self.amp_in / self.amp_out hold what the amp saw and produced."""
        uc = self.uc
        self.amp_in, self.amp_out = [], []
        out = np.zeros(len(x))
        for n, v in enumerate(np.asarray(x, float)):
            q = int(np.clip(v, -1, 1 - 2**-31) * 2**31)
            uc.mem_write(IN, struct.pack("<iiiii", q, q, 0, 0, 0))
            self.call(FN_CALLBACK, (IN, OUT))
            self.call(FN_CAB_JOB)
            if uc.mem_read(FADE + 0xF, 1)[0] == 1:
                self.call(FN_COMMIT, (FADE,))
            out[n] = struct.unpack("<i", uc.mem_read(OUT, 4))[0] / 2**31
        return out

    def ir_gain(self, ir):
        """Stock gain for a user IR slot (0x5918 reads the slot buffer)."""
        self.uc.mem_write(IR_BUF, np.asarray(ir, "<f4").tobytes())
        self.call(FN_IR_GAIN, (10,))
        return struct.unpack("<f", self.uc.mem_read(IR_GAIN_OUT, 4))[0]


def test_signal() -> np.ndarray:
    """0.5 s log sweep 20 Hz..20 kHz at -30 dBFS, 0.2 s 1 kHz at -20 dBFS,
    0.3 s decaying 55 Hz note at -14 dBFS, 0.1 s silence (1.1 s). The note
    stays below the stock output clip (0.95) on the reference presets."""
    t = np.arange(int(0.5 * FS)) / FS
    k = math.log(20000 / 20)
    sweep = 10 ** (-30 / 20) * np.sin(2 * np.pi * 20 * 0.5 / k * (np.exp(t / 0.5 * k) - 1))
    t = np.arange(int(0.2 * FS)) / FS
    tone = 10 ** (-20 / 20) * np.sin(2 * np.pi * 1000 * t)
    t = np.arange(int(0.3 * FS)) / FS
    note = 10 ** (-14 / 20) * np.sin(2 * np.pi * 55 * t) * np.exp(-t / 0.1)
    return np.concatenate([sweep, tone, note, np.zeros(int(0.1 * FS))])


def render_preset(args) -> dict:
    mr, out, preset = args
    dsp = StockDSP(mr)
    dsp.load_preset(preset)
    dsp.process(np.zeros(20000))                  # settle the drive/volume smoothers
    x = test_signal()
    y = dsp.process(x)
    if not len(dsp.amp_in) == len(dsp.amp_out) == len(x):
        raise RuntimeError(f"amp taps: {len(dsp.amp_in)} in, {len(dsp.amp_out)} out, "
                           f"{len(x)} samples")
    (out / f"p{preset}.in.f32").write_bytes(np.array(dsp.amp_in, "<f4").tobytes())
    (out / f"p{preset}.amp.f32").write_bytes(np.array(dsp.amp_out, "<f4").tobytes())
    (out / f"p{preset}.stock.f32").write_bytes(y.astype("<f4").tobytes())
    name = bytes(dsp.uc.mem_read(PRESET_CUR, 0x18)).split(b"\0")[0].decode("ascii", "replace")
    fields = {"model": F_MODEL, "gain": F_GAIN, "bass": F_BASS, "mid": F_MID,
              "midfreq": F_MIDFREQ, "treble": F_TREBLE, "volume": F_VOL, "cab": F_CAB,
              "amp_on": 0x2C, "cab_on": 0x44}
    return {"preset": preset, "name": name, "samples": len(x),
            **{k: dsp.field(off) for k, off in fields.items()}}


def render_ir_gains(mr, out: Path, count: int) -> list[float]:
    dsp = StockDSP(mr)
    rng = np.random.default_rng(200)
    irs = []
    for i in range(count):
        n = np.arange(512)
        ir = rng.standard_normal(512) * np.exp(-n / rng.uniform(20, 300))
        irs.append((ir / np.abs(ir).max() * rng.uniform(0.05, 1.0)).astype("<f4"))
    irs.append(np.zeros(512, "<f4"))              # empty slot: gain 1
    (out / "irs.f32").write_bytes(b"".join(ir.tobytes() for ir in irs))
    return [dsp.ir_gain(ir) for ir in irs]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("mr", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--presets", default="0,2,5,8,16")
    ap.add_argument("--ir-gains", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=5)
    a = ap.parse_args(argv)
    a.out.mkdir(parents=True, exist_ok=True)
    presets = [int(p) for p in a.presets.split(",") if p]
    with ProcessPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        results = list(pool.map(render_preset, [(a.mr, a.out, p) for p in presets]))
    manifest = {"fs": FS, "presets": results}
    if a.ir_gains:
        manifest["ir_gains"] = render_ir_gains(a.mr, a.out, a.ir_gains)
    (a.out / "manifest.json").write_text(json.dumps(manifest, indent=1))
    for r in results:
        print(r)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
