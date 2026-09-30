#!/usr/bin/env python3
"""Hot-path check for the FB200 app: nothing the audio path can reach, and
nothing that runs during a flash write, runs from flash (XIP)
(docs/FIRMWARE_BRINGUP.md, "Hot and cold code").

    hot_path.py build/fb200-app.elf [--cross arm-none-eabi-]

Hot roots:
  - every vector-table handler (all ISRs);
  - engine_task and usb_audio_task (the per-block audio path);
  - the TinyUSB audio class driver (audiod_*, called through usbd's driver
    table from the USB ISR and tud_task);
  - every function whose address is loaded by code (a literal-pool word):
    these are the callbacks handed to drivers (SAI/eDMA/LPUART/FlexIO), and
    drivers call them from their ISRs.
From the roots, direct calls and tail calls (bl/b to another function) are
followed, through linker veneers. Every reachable function must be in ITCM,
and none may load the address of cold const data (.xiptext).
Function pointers kept in data tables (console commands, usbd's driver
table) are not followed: those tables are cold, except usbd's audio entries
(the audiod_* root above).

Flash-write roots (second check): the code that runs while the flash is
busy, or after an app update has erased the app's own cold code (it lives
in the app slot's data area): fw_begin, fw_rx_task, fw_session and
flash_store (src/debug/selfupdate.c), the looper's flash operations
(src/loopstore/lsio.c: its cold caller runs only while the flash is idle
or its erase suspended; these return only then), and the USB class drivers that
tud_task dispatches through usbd's driver table (cdcd_*, hidd_*, audiod_*).
Everything they reach must be in RAM (not in flash), and none of it may
load the address of cold const data. A call guarded by fw_xip_gone() (the
caller returns first once an app update started) is not followed: GATED.

Flash-busy roots (third check): flash_pump, the audio work that runs while
the flash erases or programs (src/debug/flash_rmw.c flash_wait_idle). All
it reaches must be in ITCM, like the hot set, and none of it may load the
address of anything in flash (FLASH_DATA): a flash read while the flash is
busy returns garbage and can leave stale cache lines. Flash reads through
pointers kept in RAM (the drum samples) are not visible here: the engine
gates them at run time (engine_pump, drums no_flash). The pump must also
be wired: each PUMP_CALLERS function reaches flash_pump, and flash_pump
reaches each PUMP_MUST_REACH function.

Exit status 1 and a list of the offending call chains if a hot function is
in flash.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

ITCM = range(0x20000)
FLASH = range(0x60000000, 0x70000000)
HOT_ENTRY = ("engine_task", "usb_audio_task")
HOT_PATTERN = re.compile(r"^audiod_")
FLASH_WRITE_ENTRY = ("fw_begin", "fw_rx_task", "fw_session", "flash_store",
                     "lsio_read", "lsio_program", "lsio_erase_begin", "lsio_erase_run",
                     "lsio_quiesce")
FLASH_WRITE_PATTERN = re.compile(r"^(cdcd_|hidd_|audiod_)")
FLASH_BUSY_ENTRY = ("flash_pump",)
PUMP_CALLERS = ("flash_rmw", "fw_begin", "fw_rx_task", "lsio_program", "lsio_erase_run",
                "lsio_quiesce")
PUMP_MUST_REACH = ("engine_task", "usb_audio_task", "wdog_feed")
FLASH_DATA = range(0x60000000, 0x61000000)   # the 16 MB of the flash window in use
# caller -> callee edges not followed by the flash-write check, and the gate
# the caller must also call (it returns before the callee once set)
GATED = {("tud_hid_set_report_cb", "proto_feed"): "fw_xip_gone"}

FUNC_RE = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
INSN_RE = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
BRANCH_RE = re.compile(r"^(bl|blx|b|b\w\w)(\.w|\.n)?$")
TARGET_RE = re.compile(r"^([0-9a-f]+) <")


class Image:
    def __init__(self, elf: Path, cross: str):
        nm = subprocess.run([f"{cross}nm", "-S", "--defined-only", str(elf)],
                            check=True, capture_output=True, text=True).stdout
        self.funcs: dict[int, str] = {}      # start -> name (first seen)
        self.size: dict[int, int] = {}
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[2] in "tTwW":
                addr, size = int(parts[0], 16), int(parts[1], 16)
                self.funcs.setdefault(addr, parts[3])
                self.size[addr] = max(self.size.get(addr, 0), size)
        dis = subprocess.run([f"{cross}objdump", "-d", "--no-show-raw-insn", str(elf)],
                             check=True, capture_output=True, text=True).stdout
        self.calls: dict[int, set[int]] = {}
        self.taken: set[int] = set()
        self.words: dict[int, set[int]] = {}   # function -> its literal-pool words
        self.veneer: dict[int, int] = {}
        cur = None
        for line in dis.splitlines():
            if line.startswith("Disassembly of section"):
                cur = None   # a new section: code before its first known function is no one's
                continue
            m = FUNC_RE.match(line)
            if m:
                addr = int(m.group(1), 16)
                cur = addr if addr in self.funcs or "veneer" in m.group(2) else cur
                if "veneer" in m.group(2):
                    self.funcs.setdefault(addr, m.group(2))
                continue
            m = INSN_RE.match(line)
            if not m or cur is None:
                continue
            op, args = m.group(2), m.group(3)
            if op == ".word":
                val = int(args.split()[0], 0)
                if "veneer" in self.funcs.get(cur, ""):
                    self.veneer[cur] = val & ~1
                else:
                    self.words.setdefault(cur, set()).add(val)
                    if val & 1 and (val & ~1) in self.funcs:
                        self.taken.add(val & ~1)
                continue
            if BRANCH_RE.match(op):
                t = TARGET_RE.match(args)
                if t:
                    tgt = self.owner(int(t.group(1), 16))
                    if tgt is not None and tgt != cur:
                        self.calls.setdefault(cur, set()).add(tgt)
        # ARM->Thumb long veneers keep the target as their last word; Thumb
        # veneers ("ldr.w pc, [pc]") too. Link veneers into the graph.
        for v, tgt in self.veneer.items():
            self.calls.setdefault(v, set()).add(tgt)
        self.vectors = self._vectors(elf, cross)
        syms = {parts[-1]: int(parts[0], 16) for parts in map(str.split, nm.splitlines())}
        self.cold = range(syms.get("__xiptext_start__", 0), syms.get("__xiptext_end__", 0))
        self.addr = {n: a for a, n in self.funcs.items()}

    def owner(self, addr: int) -> int | None:
        if addr in self.funcs:
            return addr
        best = None
        for start, size in self.size.items():
            if start <= addr < start + max(size, 1) and (best is None or start > best):
                best = start
        return best

    def _vectors(self, elf: Path, cross: str) -> set[int]:
        out = subprocess.run([f"{cross}objdump", "-s", "-j", ".vectors", str(elf)],
                             check=True, capture_output=True, text=True).stdout
        words = []
        for line in out.splitlines():
            m = re.match(r"^\s*[0-9a-f]{4,8} ((?:[0-9a-f]{8} ?){1,4})", line)
            if m:
                words += [int.from_bytes(bytes.fromhex(w), "little") for w in m.group(1).split()]
        return {w & ~1 for w in words[2:] if w & 1 and (w & ~1) in self.funcs}

    def name(self, addr: int) -> str:
        return self.funcs.get(addr, f"{addr:#x}")


def hot_roots(img: Image) -> dict[int, str]:
    roots = {a: "vector" for a in img.vectors}
    for a, n in img.funcs.items():
        if n in HOT_ENTRY or HOT_PATTERN.match(n):
            roots[a] = "entry"
    for a in img.taken:
        roots.setdefault(a, "callback")
    return roots


def flash_write_roots(img: Image) -> dict[int, str]:
    roots = {}
    for a, n in img.funcs.items():
        if n in FLASH_WRITE_ENTRY or FLASH_WRITE_PATTERN.match(n):
            roots[a] = "flash write"
    return roots


def gated_edges(img: Image) -> tuple[set[tuple[int, int]], list[str]]:
    """GATED as addresses; a gate its caller does not call is an error."""
    edges, bad = set(), []
    for (caller, callee), gate in GATED.items():
        a, b, g = img.addr.get(caller), img.addr.get(callee), img.addr.get(gate)
        if a is None or b is None:
            continue
        if g is None or g not in {img.veneer.get(c, c) for c in img.calls.get(a, ())}:
            bad.append(f"{caller} -> {callee}: gate {gate}() not called by {caller}")
            continue
        edges.add((a, b))
    return edges, bad


def reachable(img: Image, roots: dict[int, str],
              skip: set[tuple[int, int]] = frozenset()) -> dict[int, int | None]:
    """function -> the caller it was first reached from (None for roots)."""
    parent: dict[int, int | None] = {r: None for r in roots}
    todo = list(roots)
    while todo:
        f = todo.pop()
        for g in img.calls.get(f, ()):
            if (f, img.veneer.get(g, g)) in skip:   # a gated call, direct or through a veneer
                continue
            if g not in parent:
                parent[g] = f
                todo.append(g)
    return parent


def _chains(img: Image, parent: dict[int, int | None], roots: dict[int, str],
            bad_place, what: str, data: range | None = None) -> list[str]:
    bad = []
    for f in sorted(parent):
        if not bad_place(f) or "veneer" in img.name(f):
            continue
        chain, g, root = [], f, f
        while g is not None:
            chain.append(img.name(g))
            root, g = g, parent[g]
        bad.append(f"{what}{img.name(f)} @ {f:#010x}: " + " <- ".join(chain) + f" ({roots[root]})")
    # Nor may it read cold const data (strings, tables of a COLD_SRC file):
    # a literal-pool address into .xiptext that is not a function (those are
    # callbacks, checked above). The flash-busy set: no flash address at all.
    for f in sorted(parent):
        for w in sorted(img.words.get(f, ())):
            if w in (img.cold if data is None else data) and (w & ~1) not in img.funcs:
                kind = "cold data" if data is None else "flash data"
                bad.append(f"{what}{img.name(f)} reads {kind} at {w:#010x}")
    return bad


def check(elf: Path, cross: str = "arm-none-eabi-") -> list[str]:
    """Returns one line per violation (empty = pass): a hot function outside
    ITCM, or a flash-write function in flash."""
    img = Image(elf, cross)
    roots = hot_roots(img)
    for n in HOT_ENTRY:
        if n not in img.funcs.values():
            return [f"hot entry {n} not found in {elf}"]
    bad = _chains(img, reachable(img, roots), roots, lambda f: f not in ITCM, "")
    wroots = flash_write_roots(img)
    for n in FLASH_WRITE_ENTRY:
        if n not in img.addr:
            return bad + [f"flash-write entry {n} not found in {elf}"]
    skip, gate_bad = gated_edges(img)
    bad += gate_bad
    bad += _chains(img, reachable(img, wroots, skip), wroots, lambda f: f in FLASH,
                   "flash write: ")
    return bad + check_flash_busy(img)


def flash_busy_roots(img: Image) -> dict[int, str]:
    return {a: "flash busy" for a, n in img.funcs.items() if n in FLASH_BUSY_ENTRY}


def check_flash_busy(img: Image) -> list[str]:
    """The audio pump: RAM only, no flash address, and wired both ways."""
    roots = flash_busy_roots(img)
    if len(roots) != len(FLASH_BUSY_ENTRY):
        return [f"flash-busy entry {n} not found" for n in FLASH_BUSY_ENTRY
                if n not in img.addr]
    found = reachable(img, roots)
    bad = _chains(img, found, roots, lambda f: f not in ITCM, "flash busy: ", FLASH_DATA)
    names = {img.name(f) for f in found}
    bad += [f"flash busy: {', '.join(FLASH_BUSY_ENTRY)} does not reach {n}"
            for n in PUMP_MUST_REACH if n not in names]
    for caller in PUMP_CALLERS:
        a = img.addr.get(caller)
        reach = {img.name(f) for f in reachable(img, {a: "caller"})} if a is not None else set()
        if not set(FLASH_BUSY_ENTRY) & reach:
            bad.append(f"flash busy: {caller} does not reach {', '.join(FLASH_BUSY_ENTRY)} "
                       "(the flash wait no longer runs the audio)")
    return bad


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("elf", type=Path)
    ap.add_argument("--cross", default="arm-none-eabi-")
    ap.add_argument("--list", action="store_true", help="print the hot set")
    ap.add_argument("--list-flash-write", action="store_true",
                    help="print the flash-write set")
    ap.add_argument("--list-flash-busy", action="store_true",
                    help="print the flash-busy set (the audio pump)")
    args = ap.parse_args()
    if args.list or args.list_flash_write or args.list_flash_busy:
        img = Image(args.elf, args.cross)
        if args.list:
            found = reachable(img, hot_roots(img))
        elif args.list_flash_busy:
            found = reachable(img, flash_busy_roots(img))
        else:
            found = reachable(img, flash_write_roots(img), gated_edges(img)[0])
        for f in sorted(found):
            print(f"{f:#010x} {img.name(f)}")
        return 0
    bad = check(args.elf, args.cross)
    for line in bad:
        print(line)
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
