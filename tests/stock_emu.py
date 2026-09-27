"""Run pieces of the STOCK FB200 firmware (V1.0.1) in Unicorn, as a parity oracle.

No vendor code or data lives here: everything is read at run time from the
user's own fb200-stock.mr. The vendor self-loader (block 0 +0x400) is
emulated to build the RAM image (ITCM code, DTCM data), then single stock
functions are called directly. Unicorn's Cortex-M7 has only a single-precision
FPU, so the double-precision instructions the stock code uses are executed by
a small software hook (honouring Thumb IT blocks).

Needs: unicorn, capstone, numpy. Addresses are the RE report's (V1.0.1 only).
"""

from __future__ import annotations

import math
import os
import struct
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from fb200.firmware import MrFile  # noqa: E402

RET = 0x1FFE0                 # unused ITCM word: return address for direct calls
SP = 0x20057F00
BANK = 0x600D0000             # stock drum sample base (the code's contiguous view)


def find_stock_mr() -> Path | None:
    cands = [os.environ.get("FB200_STOCK_MR", ""), ROOT / "fb200-stock.mr",
             Path.home() / "Projects" / "fb200-tools" / "fb200-stock.mr"]
    for c in cands:
        if c and Path(c).is_file():
            return Path(c)
    return None


def have_emulator() -> str | None:
    """None if the oracle can run, else the reason it cannot."""
    for mod in ("unicorn", "capstone"):
        try:
            __import__(mod)
        except ImportError:
            return f"python module '{mod}' not installed"
    if find_stock_mr() is None:
        return "fb200-stock.mr not found (set FB200_STOCK_MR)"
    return None


def _dp_hook_factory():
    import capstone
    from unicorn.arm_const import (UC_ARM_REG_FPSCR, UC_ARM_REG_PC, UC_ARM_REG_S0,
                                   UC_ARM_REG_XPSR)
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
    cache: dict[bytes, tuple[str, str, int]] = {}

    def gd(uc, n):
        lo, hi = uc.reg_read(UC_ARM_REG_S0 + 2 * n), uc.reg_read(UC_ARM_REG_S0 + 2 * n + 1)
        return struct.unpack("<d", struct.pack("<II", lo, hi))[0]

    def sd(uc, n, v):
        lo, hi = struct.unpack("<II", struct.pack("<d", v))
        uc.reg_write(UC_ARM_REG_S0 + 2 * n, lo)
        uc.reg_write(UC_ARM_REG_S0 + 2 * n + 1, hi)

    def gs(uc, n):
        return struct.unpack("<f", struct.pack("<I", uc.reg_read(UC_ARM_REG_S0 + n)))[0]

    def ss(uc, n, v):
        uc.reg_write(UC_ARM_REG_S0 + n, struct.unpack("<I", struct.pack("<f", float(np.float32(v))))[0])

    def cond_ok(xpsr, cond):
        n, z, c, v = (xpsr >> 31) & 1, (xpsr >> 30) & 1, (xpsr >> 29) & 1, (xpsr >> 28) & 1
        base = [z, c, n, v, c and not z, n == v, (not z) and n == v, True][cond >> 1]
        return bool(base) if not (cond & 1) or cond == 15 else not base

    def hook(uc, _user):
        pc = uc.reg_read(UC_ARM_REG_PC)
        raw = bytes(uc.mem_read(pc, 4))
        if raw not in cache:
            ins = next(md.disasm(raw, pc, 1))
            cache[raw] = (ins.mnemonic, ins.op_str, ins.size)
        mn, op, size = cache[raw]
        xpsr = uc.reg_read(UC_ARM_REG_XPSR)
        it = ((xpsr >> 25) & 3) | (((xpsr >> 10) & 0x3F) << 2)
        if it & 0xF:
            ok = cond_ok(xpsr, it >> 4)
            nit = (it & 0xE0) | ((it << 1) & 0x1F) if it & 0x7 else 0
            xpsr = (xpsr & ~((3 << 25) | (0x3F << 10))) | ((nit & 3) << 25) | (((nit >> 2) & 0x3F) << 10)
            uc.reg_write(UC_ARM_REG_XPSR, xpsr)
            if not ok:
                uc.reg_write(UC_ARM_REG_PC, (pc + size) | 1)
                return True
        ops = [o.strip() for o in op.split(",")]

        def R(o):
            return gd(uc, int(o[1:])) if o.startswith("d") else float(o[1:])

        def W(o, v):
            sd(uc, int(o[1:]), v)

        if mn == "vcvt.f64.f32":
            W(ops[0], gs(uc, int(ops[1][1:])))
        elif mn == "vcvt.f32.f64":
            ss(uc, int(ops[0][1:]), R(ops[1]))
        elif mn in ("vcvt.f64.s32", "vcvt.f64.u32"):
            v = uc.reg_read(UC_ARM_REG_S0 + int(ops[1][1:]))
            W(ops[0], v - (1 << 32) if mn.endswith("s32") and v & 0x80000000 else v)
        elif mn in ("vcvt.u32.f64", "vcvt.s32.f64"):
            v = R(ops[1])
            v = 0 if math.isnan(v) else math.trunc(v)
            v = min(max(v, 0), 0xFFFFFFFF) if "u32" in mn else min(max(v, -2**31), 2**31 - 1)
            uc.reg_write(UC_ARM_REG_S0 + int(ops[0][1:]), v & 0xFFFFFFFF)
        elif mn in ("vmul.f64", "vadd.f64", "vsub.f64", "vdiv.f64"):
            a, b = (R(ops[0]), R(ops[1])) if len(ops) == 2 else (R(ops[1]), R(ops[2]))
            if mn == "vmul.f64":
                r = a * b
            elif mn == "vadd.f64":
                r = a + b
            elif mn == "vsub.f64":
                r = a - b
            else:
                r = a / b if b else (math.copysign(math.inf, a) if a else math.nan)
            W(ops[0], r)
        elif mn == "vmla.f64":
            W(ops[0], R(ops[0]) + R(ops[1]) * R(ops[2]))
        elif mn == "vmls.f64":
            W(ops[0], R(ops[0]) - R(ops[1]) * R(ops[2]))
        elif mn == "vnmls.f64":
            W(ops[0], R(ops[1]) * R(ops[2]) - R(ops[0]))
        elif mn == "vmov.f64":
            W(ops[0], R(ops[1]))
        elif mn == "vneg.f64":
            W(ops[0], -R(ops[1]))
        elif mn == "vabs.f64":
            W(ops[0], abs(R(ops[1])))
        elif mn == "vsqrt.f64":
            v = R(ops[1])
            W(ops[0], math.sqrt(v) if v >= 0 else math.nan)
        elif mn in ("vcmpe.f64", "vcmp.f64"):
            a = R(ops[0])
            b = 0.0 if ops[1].startswith("#0") else R(ops[1])
            nzcv = 0b0011 if math.isnan(a) or math.isnan(b) else 0b0110 if a == b else \
                0b1000 if a < b else 0b0010
            fp = uc.reg_read(UC_ARM_REG_FPSCR)
            uc.reg_write(UC_ARM_REG_FPSCR, (fp & 0x0FFFFFFF) | (nzcv << 28))
        else:
            return False
        uc.reg_write(UC_ARM_REG_PC, (pc + size) | 1)
        return True

    return hook


class StockFirmware:
    def __init__(self, mr_path: Path | None = None):
        from unicorn import (UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INSN_INVALID, UC_MODE_MCLASS,
                             UC_MODE_THUMB, Uc)
        from unicorn.arm_const import UC_ARM_REG_C1_C0_2, UC_ARM_REG_SP, UC_CPU_ARM_CORTEX_M7

        self.mr = MrFile.from_path(mr_path or find_stock_mr())
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
        for base, size in [(0, 0x200000), (0x20000000, 0x100000), (0x20200000, 0x100000),
                           (0x40000000, 0x10000000), (0x60000000, 0x800000),
                           (0xE0000000, 0x100000)]:
            uc.mem_map(base, size)
        uc.mem_write(0x60010000, self.mr.blocks[0].data)
        # vendor loader: runs the load table, stops at the ITCM entry 0x4d6
        h = uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: u.emu_stop() if a == 0x4D6 else None,
                        begin=0x4D6, end=0x4D6)
        uc.reg_write(UC_ARM_REG_SP, 0x20008000)
        uc.emu_start(0x600104D9, 0, count=20_000_000)
        uc.hook_del(h)
        uc.reg_write(UC_ARM_REG_C1_C0_2, 0xF << 20)      # FPU on
        uc.hook_add(UC_HOOK_INSN_INVALID, _dp_hook_factory())
        uc.mem_write(RET, b"\x00\xbf\x00\xbf")
        # drum bank in the layout the stock code assumes: entries back to back
        blk = self.mr.blocks[1].data
        n = struct.unpack_from("<I", blk, 0)[0]
        packed = b"".join(blk[p * 512:p * 512 + sz]
                          for p, sz in (struct.unpack_from("<II", blk, 8 + 8 * i) for i in range(n)))
        uc.mem_write(BANK, packed)
        self.uc = uc
        self.bank_raw = blk

    # -- helpers
    def call(self, fn: int, args=(), fargs=(), maxn=50_000_000) -> None:
        from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_S0, UC_ARM_REG_SP
        uc = self.uc
        for i, v in enumerate(args):
            uc.reg_write(UC_ARM_REG_R0 + i, v)
        for i, v in enumerate(fargs):
            uc.reg_write(UC_ARM_REG_S0 + i, struct.unpack("<I", struct.pack("<f", v))[0])
        uc.reg_write(UC_ARM_REG_SP, SP)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        uc.emu_start(fn | 1, RET, count=maxn)

    def rd(self, addr: int, fmt: str):
        return struct.unpack_from("<" + fmt, bytes(self.uc.mem_read(addr, struct.calcsize("<" + fmt))))

    def wr(self, addr: int, fmt: str, *vals) -> None:
        self.uc.mem_write(addr, struct.pack("<" + fmt, *vals))


class StockDrums(StockFirmware):
    """Stock drum block: 0x19d34(out, n) = ticks + sequencer 0xb3ac + mixer 0x2708."""
    SETTINGS, BUF = 0x20007F30, 0x20057800

    def __init__(self, rhythm: int, bpm: int, mr_path: Path | None = None):
        super().__init__(mr_path)
        self.call(0xB334)                       # drum init: offsets table + voice reset
        self.wr(self.SETTINGS, "BBBBH", 1, 0, rhythm, 100, bpm)

    def render(self, n_blocks: int, block: int = 8) -> np.ndarray:
        out = np.empty(n_blocks * block, np.float32)
        for b in range(n_blocks):
            self.call(0x19D34, (self.BUF, block))
            out[b * block:(b + 1) * block] = np.frombuffer(
                bytes(self.uc.mem_read(self.BUF, 4 * block)), "<f4")
        return out


class StockTuner(StockFirmware):
    """Stock tuner: 0x3aa4(s0 = L+R) per sample; main loop 0x17a48(cal) when a buffer is full."""
    FEED_ST, NOTE_ST = 0x20016F54, 0x20016F84

    def __init__(self, a4: int = 440, mr_path: Path | None = None):
        super().__init__(mr_path)
        self.wr(0x20018678 + 0x34, "I", 1)      # skip the licence check in 0x2930
        self.cal = a4 - 430

    def run(self, x: np.ndarray) -> list[dict]:
        res = []
        for i, v in enumerate(x):
            self.call(0x3AA4, (1,), (float(v),))
            a_full, b_full = self.rd(self.FEED_ST, "HH")
            if a_full == 1 or b_full == 1:
                self.call(0x17A48, (self.cal,))
                silent, _, _, note, dev = self.rd(self.NOTE_ST, "BBBBB")
                f = self.rd(self.FEED_ST + 0x2C, "f")[0]
                res.append(dict(sample=i, note=note, deviation=dev, freq=f, silent=silent))
        return res
