"""Run the STOCK FB200 DSP in Unicorn: a bit-exact reference for parity tests.

Everything is rebuilt at run time from the vendor image (fb200-stock.mr, never
committed): the vendor self-loader is emulated to get the RAM image, then the
stock per-sample audio callback (ITCM 0x17d8c) or single DSP functions are
called directly. Unicorn's Cortex-M7 model has no double-precision FPU, so the
f64 instructions the stock code uses are emulated in Python (`_dp_hook`).

    s = StockDSP.from_mr(path)          # ~1 s: loader + DSP init
    s.load_preset(15); s.set_field(0xa4, 1); s.settle()
    y = s.process(x)                    # (N, 2) float, L/R

Needs numpy, unicorn and capstone (tests skip without them).
"""
from __future__ import annotations

import math
import struct
from pathlib import Path

import capstone
import numpy as np
from unicorn import UC_ARCH_ARM, UC_HOOK_INSN_INVALID, UC_MODE_MCLASS, UC_MODE_THUMB, Uc
from unicorn import arm_const as A

ROOT = Path(__file__).resolve().parents[1]


def find_stock_mr() -> Path | None:
    """$FB200_STOCK_MR, else fb200-stock.mr in the repo root or, from a git
    worktree, in the main checkout."""
    import os
    import subprocess
    cands = [os.environ.get("FB200_STOCK_MR"), ROOT / "fb200-stock.mr"]
    try:
        common = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--git-common-dir"],
                                capture_output=True, text=True, check=True).stdout.strip()
        cands.append((ROOT / common).resolve().parent / "fb200-stock.mr")
    except (OSError, subprocess.CalledProcessError):
        pass
    return next((Path(c) for c in cands if c and Path(c).is_file()), None)


MR_PATH = find_stock_mr()

# stock RAM map (from the disassembly, see docs/PARITY.md "Stock DSP")
PRESET_CUR = 0x2001DC40   # live preset, u16 knob fields
PRESET_TAB = 0x20004E40   # 20 factory presets x 0x100
GLOBAL_CUR = 0x2001DD40
GLOBAL_DEF = 0x20004E00
FADE = 0x200088A4         # fade/commit block (0x174a4 re-inits the chain)
ST = 0x200076CC           # callback state: parameter smoothers
P = 0x200264A0            # committed DSP parameters (floats + enable bytes)
IN_GAIN_TAB = 0x20007770   # input gain per global[0x1a] (dB steps)
CHAIN_IO = 0x20007768     # chain in/out L/R floats: in = (L + R) * input gain
DETECTOR = 0x200088EC     # 0x7a08 state: hold counter, held, window peak, ...
IN = 0x20057000
OUT = 0x20057040
RET = 0x1FFE0             # magic return address

CALLBACK = 0x17D8C
CAB_JOB = 0x26C4
DSP_INIT = 0x90C4
MID_EQ_SELECT = 0x90E4
COMMIT = 0x174A4

_MAP = [(0, 0x200000), (0x20000000, 0x100000), (0x20200000, 0x100000),
        (0x40000000, 0x10000000), (0x60000000, 0x800000), (0xE0000000, 0x100000)]


def _new_uc() -> Uc:
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    for base, size in _MAP:
        uc.mem_map(base, size)
    return uc


def load_ram(mr_path: Path = MR_PATH) -> dict[str, bytes]:
    """Emulate the vendor loader (block 0 0x400..0x7d4) up to ITCM 0x4d6."""
    import sys
    sys.path.insert(0, str(ROOT / "src"))
    from fb200.firmware import MrFile

    mr = MrFile.from_path(mr_path)
    uc = _new_uc()
    uc.mem_write(0x60010000, mr.blocks[0].data)
    uc.mem_write(0x60041000, mr.blocks[1].data)
    uc.reg_write(A.UC_ARM_REG_SP, 0x20008000)
    uc.emu_start(0x600104D9, 0x4D6, count=20_000_000)
    return {"itcm": bytes(uc.mem_read(0, 0x20000)),
            "dtcm": bytes(uc.mem_read(0x20000000, 0x58000)),
            "ocram": bytes(uc.mem_read(0x20200000, 0x10000)),
            "block0": mr.blocks[0].data, "block1": mr.blocks[1].data}


# ---- software double-precision FPU (Unicorn's M7 model is single-precision only)
_md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
_cache: dict[bytes, tuple[str, str, int]] = {}


def _gd(uc, n):
    lo = uc.reg_read(A.UC_ARM_REG_S0 + 2 * n)
    hi = uc.reg_read(A.UC_ARM_REG_S0 + 2 * n + 1)
    return struct.unpack("<d", struct.pack("<II", lo, hi))[0]


def _sd(uc, n, v):
    lo, hi = struct.unpack("<II", struct.pack("<d", v))
    uc.reg_write(A.UC_ARM_REG_S0 + 2 * n, lo)
    uc.reg_write(A.UC_ARM_REG_S0 + 2 * n + 1, hi)


def _gs(uc, n):
    return struct.unpack("<f", struct.pack("<I", uc.reg_read(A.UC_ARM_REG_S0 + n)))[0]


def _ss(uc, n, v):
    uc.reg_write(A.UC_ARM_REG_S0 + n, struct.unpack("<I", struct.pack("<f", float(np.float32(v))))[0])


def _it_skip(uc, size, pc) -> bool:
    """QEMU faults before evaluating an IT condition: honour ITSTATE here."""
    xpsr = uc.reg_read(A.UC_ARM_REG_XPSR)
    it = ((xpsr >> 25) & 3) | (((xpsr >> 10) & 0x3F) << 2)
    if not it & 0xF:
        return False
    cond = it >> 4
    n, z, c, v = (xpsr >> 31) & 1, (xpsr >> 30) & 1, (xpsr >> 29) & 1, (xpsr >> 28) & 1
    base = {0: z, 1: z, 2: c, 3: c, 4: n, 5: n, 6: v, 7: v, 8: c and not z, 9: c and not z,
            10: n == v, 11: n == v, 12: (not z) and n == v, 13: (not z) and n == v,
            14: 1, 15: 1}[cond]
    ok = (not bool(base)) if cond in (1, 3, 5, 7, 9, 11, 13) else bool(base)
    nit = (it & 0xE0) | ((it << 1) & 0x1F) if (it & 0x7) != 0 else 0
    xpsr = (xpsr & ~((3 << 25) | (0x3F << 10))) | ((nit & 3) << 25) | (((nit >> 2) & 0x3F) << 10)
    uc.reg_write(A.UC_ARM_REG_XPSR, xpsr)
    if not ok:
        uc.reg_write(A.UC_ARM_REG_PC, (pc + size) | 1)
    return not ok


def _dp_hook(uc, _user):
    pc = uc.reg_read(A.UC_ARM_REG_PC)
    raw = bytes(uc.mem_read(pc, 4))
    if raw not in _cache:
        ins = next(_md.disasm(raw, pc, 1))
        _cache[raw] = (ins.mnemonic, ins.op_str, ins.size)
    mn, op, size = _cache[raw]
    if _it_skip(uc, size, pc):
        return True
    ops = [o.strip() for o in op.split(",")]

    def R(o):
        if o.startswith("d"):
            return _gd(uc, int(o[1:]))
        if o.startswith("#"):
            return float(o[1:])
        raise ValueError(o)

    def W(o, val):
        _sd(uc, int(o[1:]), val)

    if mn == "vcvt.f64.f32":
        W(ops[0], _gs(uc, int(ops[1][1:])))
    elif mn == "vcvt.f32.f64":
        _ss(uc, int(ops[0][1:]), R(ops[1]))
    elif mn in ("vcvt.f64.s32", "vcvt.f64.u32"):
        val = uc.reg_read(A.UC_ARM_REG_S0 + int(ops[1][1:]))
        if mn == "vcvt.f64.s32" and val & 0x80000000:
            val -= 1 << 32
        W(ops[0], val)
    elif mn in ("vcvt.u32.f64", "vcvt.s32.f64"):
        val = R(ops[1])
        val = 0 if math.isnan(val) else math.trunc(val)
        lo, hi = (0, 0xFFFFFFFF) if mn == "vcvt.u32.f64" else (-2**31, 2**31 - 1)
        uc.reg_write(A.UC_ARM_REG_S0 + int(ops[0][1:]), min(max(val, lo), hi) & 0xFFFFFFFF)
    elif mn in ("vmul.f64", "vadd.f64", "vsub.f64", "vdiv.f64"):
        a, b = (R(ops[0]), R(ops[1])) if len(ops) == 2 else (R(ops[1]), R(ops[2]))
        if mn == "vmul.f64":
            r = a * b
        elif mn == "vadd.f64":
            r = a + b
        elif mn == "vsub.f64":
            r = a - b
        else:
            r = a / b if b != 0 else (math.copysign(math.inf, a) if a != 0 else math.nan)
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
        val = R(ops[1])
        W(ops[0], math.sqrt(val) if val >= 0 else math.nan)
    elif mn in ("vcmpe.f64", "vcmp.f64"):
        a = R(ops[0])
        b = 0.0 if ops[1].startswith("#0") else R(ops[1])
        if math.isnan(a) or math.isnan(b):
            nzcv = 0b0011
        elif a == b:
            nzcv = 0b0110
        elif a < b:
            nzcv = 0b1000
        else:
            nzcv = 0b0010
        fp = uc.reg_read(A.UC_ARM_REG_FPSCR)
        uc.reg_write(A.UC_ARM_REG_FPSCR, (fp & 0x0FFFFFFF) | (nzcv << 28))
    else:
        return False
    uc.reg_write(A.UC_ARM_REG_PC, (pc + size) | 1)
    return True


class Emu:
    """Stock RAM image + FPU fixups; call(fn, args, fargs) runs one function."""

    def __init__(self, ram: dict[str, bytes]):
        uc = _new_uc()
        uc.mem_write(0, ram["itcm"])
        uc.mem_write(0x20000000, ram["dtcm"])
        uc.mem_write(0x20200000, ram["ocram"])
        uc.mem_write(0x60010000, ram["block0"])
        m = ram["block1"]          # drum bank, contiguous at 0x600d0000 as the code expects
        n = struct.unpack_from("<I", m, 0)[0]
        blob = b"".join(m[p * 512:p * 512 + sz]
                        for p, sz in (struct.unpack_from("<II", m, 8 + 8 * i) for i in range(n)))
        uc.mem_write(0x600D0000, blob)
        uc.mem_write(RET, b"\x00\xbf\x00\xbf")
        uc.mem_write(0xE000ED88, struct.pack("<I", 0xF << 20))   # CPACR: FPU on
        uc.hook_add(UC_HOOK_INSN_INVALID, _dp_hook)
        self.uc = uc

    def call(self, fn: int, args=(), fargs=(), sp=0x20057F00, maxn=5_000_000) -> float:
        uc = self.uc
        for r, v in zip((A.UC_ARM_REG_R0, A.UC_ARM_REG_R1, A.UC_ARM_REG_R2, A.UC_ARM_REG_R3), args):
            uc.reg_write(r, v)
        for i, v in enumerate(fargs):
            _ss(uc, i, v)
        uc.reg_write(A.UC_ARM_REG_SP, sp)
        uc.reg_write(A.UC_ARM_REG_LR, RET | 1)
        uc.emu_start(fn | 1, RET, count=maxn)
        return _gs(uc, 0)

    def rf(self, addr: int, n: int = 1) -> np.ndarray:
        return np.frombuffer(bytes(self.uc.mem_read(addr, 4 * n)), "<f4").copy()

    def wf(self, addr: int, v) -> None:
        self.uc.mem_write(addr, np.asarray(v, "<f4").tobytes())

    def ru32(self, addr: int) -> int:
        return struct.unpack("<I", self.uc.mem_read(addr, 4))[0]

    def w32(self, addr: int, v: int) -> None:
        self.uc.mem_write(addr, struct.pack("<I", v & 0xFFFFFFFF))


def quantize_input(x) -> np.ndarray:
    """The float the stock callback sees for input x: int32 codec word * 2^-31 in f32."""
    q = np.clip(np.asarray(x, float), -1, 1 - 2**-31) * 2**31
    return (q.astype(np.int64).astype(np.float32) * np.float32(2**-31)).astype(np.float32)


class StockDSP:
    """The stock per-sample callback: mono in (L=R), stereo out (codec words / 2^31)."""

    def __init__(self, ram: dict[str, bytes]):
        self.e = Emu(ram)
        uc = self.e.uc
        uc.mem_write(0x20016E84, struct.pack("<I", 1))           # skip the licence check
        uc.mem_write(GLOBAL_CUR, bytes(uc.mem_read(GLOBAL_DEF, 0x31)))
        self.gset(0x18, 100)                                     # master volume 100
        self.gset(0x2E, 0)                                       # un-mute
        self.e.call(DSP_INIT)
        self.since_commit = 0                                    # samples since last re-init

    @classmethod
    def from_mr(cls, mr_path: Path = MR_PATH) -> StockDSP:
        return cls(load_ram(mr_path))

    def gset(self, off: int, v: int) -> None:
        self.e.uc.mem_write(GLOBAL_CUR + off, bytes([v]))

    def set_field(self, off: int, v: int) -> None:
        self.e.uc.mem_write(PRESET_CUR + off, struct.pack("<H", v))

    def field(self, off: int) -> int:
        return struct.unpack("<H", self.e.uc.mem_read(PRESET_CUR + off, 2))[0]

    def load_preset(self, i: int) -> None:
        uc = self.e.uc
        uc.mem_write(PRESET_CUR, bytes(uc.mem_read(PRESET_TAB + i * 0x100, 0x100)))

    def only(self, fields: dict[int, int], base_preset: int = 15) -> None:
        """Load a factory preset, switch every module off, then apply `fields`."""
        self.load_preset(base_preset)
        for en in (0x14, 0x2C, 0x44, 0x5C, 0x74, 0x8C, 0xA4):   # comp amp cab gate mod delay rev
            self.set_field(en, 0)
        for off, v in fields.items():
            self.set_field(off, v)

    def settle(self, n: int = 4096) -> None:
        """Commit the preset like the main loop does, then let smoothers/fade settle."""
        self.process(np.zeros(64))
        self.e.call(MID_EQ_SELECT)
        self.e.uc.mem_write(FADE + 0xF, b"\x01")
        self.e.uc.mem_write(FADE + 0xD, b"\x01")
        self.e.call(COMMIT, (FADE,))
        self.since_commit = 0
        self.snap_smoothers()
        # the detector's 800-step hold window free-runs since boot: start it
        # where a freshly initialised port starts (after silence all else is 0)
        self.e.w32(DETECTOR, 0)
        self.e.wf(DETECTOR + 8, [0.0])
        self.process(np.zeros(n))

    def snap_smoothers(self) -> None:
        """Jump every knob smoother to its exact target (knob * 0.01f).

        The stock smooths knobs every 3rd sample, y = t*a + y*b in float, which
        stalls on a float that depends on where it came from (0.999997 when
        rising to 1.0). Our ports start from the exact target (dsp_knob_t), so
        the stock must too; both then run the same map during the settle. The
        slow ones (input gain, comp threshold/level) would also need ~20k
        samples to settle otherwise."""
        f32 = np.float32
        k = lambda off: f32(self.field(off)) * f32(0.01)
        g = self.e.rf(IN_GAIN_TAB + 4 * self.e.uc.mem_read(GLOBAL_CUR + 0x1A, 1)[0])[0]
        master = f32(self.e.uc.mem_read(GLOBAL_CUR + 0x18, 1)[0]) / f32(100)
        ratio = np.sqrt(k(0x1C))
        attack = k(0x18)
        snaps = [
            ((ST + 0x40, ST + 0x44), g),                          # input gain
            ((ST + 0x0C, ST + 0x60), master),                     # master volume
            ((ST + 0x18, ST + 0x1C, P + 0x88), k(0x60)),          # gate threshold
            ((ST + 0x58, P + 0x2C), k(0xAA)),                     # reverb level
            ((ST + 0x54, P + 0x30), k(0x90)),                     # delay mix
            ((ST + 0x50, P + 0x44), k(0x7A)),                     # mod mix
            ((ST + 0x20, ST + 0x24), ratio),                      # comp ratio
            ((P + 0x80,), ratio if ratio > f32(1e-4) else f32(0)),
            ((ST + 0x5C,), attack),                               # comp attack
            ((P + 0x78,), attack if attack > f32(1e-4) else f32(0)),
            ((ST + 0x68, P + 0x7C), k(0x1A)),                     # comp threshold
            ((ST + 0x64, P + 0x84), k(0x1E)),                     # comp level
        ]
        for addrs, v in snaps:
            for a in addrs:
                self.e.wf(a, [v])

    def chain_input(self, x) -> np.ndarray:
        """f32 input of the effect chain (0x7b60) for mono input x, once the
        input gain has settled: (L + R) * gain, both as the callback rounds."""
        q = quantize_input(x)
        return ((q + q) * self.e.rf(ST + 0x40)[0]).astype(np.float32)

    def process(self, x, xr=None) -> np.ndarray:
        """Run the callback per sample. Returns the codec output (N, 2); the
        chain output before master volume/clip is kept in self.chain_out."""
        e = self.e
        uc = e.uc
        x = np.asarray(x, float)
        xr = x if xr is None else np.asarray(xr, float)
        li = (np.clip(x, -1, 1 - 2**-31) * 2**31).astype(np.int64)
        ri = (np.clip(xr, -1, 1 - 2**-31) * 2**31).astype(np.int64)
        out = np.zeros((len(x), 2))
        self.chain_out = np.zeros((len(x), 2), np.float32)
        for n in range(len(x)):
            uc.mem_write(IN, struct.pack("<iiiii", li[n], ri[n], 0, 0, 0))
            e.call(CALLBACK, (IN, OUT))
            e.call(CAB_JOB)                      # the SAI ISR runs both
            self.chain_out[n] = e.rf(CHAIN_IO, 2)
            self.since_commit += 1
            if uc.mem_read(FADE + 0xF, 1)[0] == 1:
                e.call(COMMIT, (FADE,))
                self.since_commit = 0
            a, b = struct.unpack("<ii", uc.mem_read(OUT, 8))
            out[n] = (a / 2**31, b / 2**31)
        return out
