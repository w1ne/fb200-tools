#!/usr/bin/env python3
"""Worst-case stack depth of the FB200 firmware against its stack reserve
(linker.ld: 8 kB below _estack, painted at boot and measured by `stack`).

    stack_usage.py <build dir> [--variant app|recovery] [--cross arm-none-eabi-] [--top 10]

Frame sizes come from gcc's -fstack-usage (.su files next to the objects,
Makefile CFLAGS), the call graph from the disassembly (hot_path.Image:
direct calls and tail calls, through veneers). The bound is conservative:

  main   deepest chain from the entry, stage2_main (the main loop and all it
         calls; tud_task reaches the TinyUSB class drivers through usbd's table)
  + isr  the two deepest interrupt handlers or driver callbacks (the USB
         interrupt runs at a lower priority than the SAI/eDMA/LPUART ones, so
         one can preempt the other), each with an exception frame of 104 bytes
         (FPU context) + 4 (alignment)

Functions without a .su entry (assembly: stage2.S) count 0 and are listed;
a dynamic frame (alloca/VLA) or recursion makes the bound unknown: listed
and a failure. Exit status 1 if the bound exceeds the reserve.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import hot_path

RESERVE = 0x2000
# Known re-entry: log_flush_ms (only on the way to a reset: console_reboot,
# recovery_request) runs tud_task, which can run the protocol again (HID ->
# 0xC1 -> recovery_request). recovery_request goes straight to the reset when
# re-entered (recovery.c), so this nests once: counted as 2 x the callee.
REENTRY = {("log_flush_ms", "tud_task_ext")}
EXC_FRAME = 104 + 4
SU_RE = re.compile(r"^(.*):(\d+):(\d+):(\S+)\t(\d+)\t(\S+)")


@dataclass
class Report:
    main: int
    main_chain: list[str]
    isrs: list[tuple[int, list[str]]]
    total: int
    unknown: list[str] = field(default_factory=list)   # dynamic frames, recursion
    no_su: list[str] = field(default_factory=list)

    def lines(self, top: int = 0) -> list[str]:
        out = [f"stack bound {self.total} of {RESERVE} bytes (headroom {RESERVE - self.total})",
               f"  main   {self.main:5d}  " + " -> ".join(self.main_chain)]
        for depth, chain in self.isrs[:max(top, 2)]:
            out.append(f"  isr    {depth:5d}  " + " -> ".join(chain))
        if self.unknown:
            out.append("  UNKNOWN: " + ", ".join(self.unknown))
        return out


def frames(build: Path) -> tuple[dict[str, int], set[str]]:
    """function name -> largest frame of that name; names with dynamic frames."""
    size: dict[str, int] = {}
    dynamic: set[str] = set()
    for su in build.rglob("*.su"):
        for line in su.read_text(errors="replace").splitlines():
            m = SU_RE.match(line)
            if not m:
                continue
            name, n, kind = m.group(4), int(m.group(5)), m.group(6)
            size[name] = max(size.get(name, 0), n)
            if kind.startswith("dynamic") and not kind.endswith("bounded"):
                dynamic.add(name)
    return size, dynamic


def analyse(elf: Path, build: Path, cross: str = "arm-none-eabi-") -> Report:
    img = hot_path.Image(elf, cross)
    size, dynamic = frames(build)
    if not size:
        raise SystemExit(f"no .su files under {build}: build with -fstack-usage")
    memo: dict[int, tuple[int, list[int]]] = {}
    unknown: set[str] = set()
    no_su: set[str] = set()
    onstack: set[int] = set()

    def frame(f: int) -> int:
        name = img.name(f)
        if "veneer" in name:
            return 0
        if name in dynamic:
            unknown.add(f"{name} (dynamic frame)")
        if name not in size:
            no_su.add(name)
        return size.get(name, 0)

    # Calls through pointers that the disassembly cannot see: usbd's class
    # driver table, from tud_task (main loop) and from the USB interrupt
    # (audiod_xfer_isr, dcd_event_handler).
    drivers = {a for a, n in img.funcs.items() if hot_path.FLASH_WRITE_PATTERN.match(n)}
    extra = {img.addr[n]: drivers for n in ("tud_task_ext",) if n in img.addr}
    if "dcd_event_handler" in img.addr:   # the interrupt runs only the audio driver's hook
        extra[img.addr["dcd_event_handler"]] = {a for a, n in img.funcs.items() if n == "audiod_xfer_isr"}
    cut = {(img.addr[a], img.addr[b]) for a, b in REENTRY if a in img.addr and b in img.addr}

    def callees(f: int) -> set[int]:
        if f in img.veneer:
            return {img.veneer[f]}
        return {g for g in set(img.calls.get(f, ())) | extra.get(f, set())
                if (f, img.veneer.get(g, g)) not in cut}

    def worst(f: int) -> tuple[int, list[int]]:
        if f in memo:
            return memo[f]
        if f in onstack:
            unknown.add(f"{img.name(f)} (recursion)")
            return 0, []
        onstack.add(f)
        best, chain = 0, []
        for g in callees(f):
            d, c = worst(g)
            if d > best:
                best, chain = d, c
        onstack.discard(f)
        memo[f] = (frame(f) + best, [f, *chain])
        return memo[f]

    def names(chain: list[int]) -> list[str]:
        return [img.name(f) for f in chain if "veneer" not in img.name(f)]

    reset = img.addr.get("stage2_main")   # the entry: src/stage2.S b.w stage2_main -> app_main
    if reset is None:
        raise SystemExit(f"no stage2_main in {elf}")
    main, main_chain = worst(reset)
    for _, b in cut:
        d, c = worst(b)
        if 2 * d > main:
            main, main_chain = 2 * d, [*c, *c]
    # Driver callbacks (a code address loaded into a register, hot_path
    # "callback"): the SDK drivers call them from their interrupt handlers,
    # so each counts as an interrupt of its own.
    isrs = sorted((worst(v) for v in img.vectors | img.taken if v != reset), key=lambda t: -t[0])
    isr_total = sum(d + EXC_FRAME for d, _ in isrs[:2])
    return Report(main=main, main_chain=names(main_chain), isrs=[(d, names(c)) for d, c in isrs],
                  total=main + isr_total, unknown=sorted(unknown), no_su=sorted(no_su))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("build", type=Path, help="make BUILD= directory")
    ap.add_argument("--variant", default="app")
    ap.add_argument("--cross", default="arm-none-eabi-")
    ap.add_argument("--top", type=int, default=4)
    args = ap.parse_args()
    r = analyse(args.build / f"fb200-{args.variant}.elf", args.build, args.cross)
    print("\n".join(r.lines(args.top)))
    return 1 if r.unknown or r.total > RESERVE else 0


if __name__ == "__main__":
    raise SystemExit(main())
