#!/usr/bin/env python3
"""Hot-path check for the FB200 app: nothing the audio path can reach runs
from OCRAM (docs/FIRMWARE_BRINGUP.md, "Hot and cold code").

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
and none may load the address of cold const data (.ocramtext).
Function pointers kept in data tables (console commands, usbd's driver
table) are not followed: those tables are cold, except usbd's audio entries
(the audiod_* root above).

Exit status 1 and a list of the offending call chains if a hot function is
in OCRAM.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ITCM = range(0x0, 0x20000)
OCRAM = range(0x20200000, 0x20280000)
HOT_ENTRY = ("engine_task", "usb_audio_task")
HOT_PATTERN = re.compile(r"^audiod_")

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
        self.cold = range(syms.get("__ocramtext_start__", 0), syms.get("__ocramtext_end__", 0))

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


def reachable(img: Image, roots: dict[int, str]) -> dict[int, int | None]:
    """function -> the caller it was first reached from (None for roots)."""
    parent: dict[int, int | None] = {r: None for r in roots}
    todo = list(roots)
    while todo:
        f = todo.pop()
        for g in img.calls.get(f, ()):
            if g not in parent:
                parent[g] = f
                todo.append(g)
    return parent


def check(elf: Path, cross: str = "arm-none-eabi-") -> list[str]:
    """Returns one line per hot function outside ITCM (empty = pass)."""
    img = Image(elf, cross)
    roots = hot_roots(img)
    for n in HOT_ENTRY:
        if n not in img.funcs.values():
            return [f"hot entry {n} not found in {elf}"]
    parent = reachable(img, roots)
    bad = []
    for f in sorted(parent):
        if f in ITCM or "veneer" in img.name(f):
            continue
        chain, g, root = [], f, f
        while g is not None:
            chain.append(img.name(g))
            root, g = g, parent[g]
        bad.append(f"{img.name(f)} @ {f:#010x}: " + " <- ".join(chain) + f" ({roots[root]})")
    # Hot code must not read cold const data either (strings, tables of a
    # COLD_SRC file): a literal-pool address into .ocramtext that is not a
    # function (those are callbacks, checked above).
    for f in sorted(parent):
        for w in sorted(img.words.get(f, ())):
            if w in img.cold and (w & ~1) not in img.funcs:
                bad.append(f"{img.name(f)} reads cold data at {w:#010x}")
    return bad


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("elf", type=Path)
    ap.add_argument("--cross", default="arm-none-eabi-")
    ap.add_argument("--list", action="store_true", help="print the hot set")
    args = ap.parse_args()
    if args.list:
        img = Image(args.elf, args.cross)
        for f in sorted(reachable(img, hot_roots(img))):
            print(f"{f:#010x} {img.name(f)}")
        return 0
    bad = check(args.elf, args.cross)
    for line in bad:
        print(line)
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
