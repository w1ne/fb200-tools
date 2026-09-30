#!/usr/bin/env python3
# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Dry-run a packed .mr through the vendor boot loader with Unicorn.

The stock block 0 is a self-loading image (see docs/FIRMWARE_BRINGUP.md): the
flash stub at 0x4d8 runs the loader, which walks the load table and jumps to
the fixed ITCM 0x4d6 entry. This tool maps the image the way the pedal does
and verifies the chain reaches the expected entry points before any hardware
is touched.

Requires `unicorn` (pip install unicorn); it is not a project dependency, so
run it with a Python that has it:

    python firmware/tools/boot_dry_run.py image.mr --elf firmware/audio/build/fb200-audio.elf

Symbols read from the ELF (via arm-none-eabi-nm): stage2, stage2_main,
app_main. With --app-elf (two-stage image, docs/BOOTLOADER.md §4) the ELF is
the recovery build and the run continues through recovery's boot decision and
copier until the app's own app_main, then (with the app slot) into the app's
cold code, run in place from flash (linker.ld .xiptext), and back.

RAM is mapped as the pedal has it (IOMUXC_GPR17 = 0xFFAAAAA9, measured):
ITCM 128 kB, DTCM 352 kB, OCRAM 32 kB. An access anywhere else stops the
run.
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "src"))

from fb200.firmware import MrFile

ITCM_BASE, ITCM_SIZE = 0x0, 0x20000             # 4 FlexRAM banks
DTCM_BASE, DTCM_SIZE = 0x20000000, 0x58000      # 11 banks
OCRAM_BASE, OCRAM_SIZE = 0x20200000, 0x8000     # 1 bank
PERIPH_BASE, PERIPH_SIZE = 0x40000000, 0x10000000
FLASH_BASE, FLASH_SIZE = 0x60000000, 0x800000
SCB_BASE, SCB_SIZE = 0xE0000000, 0x100000

MODELS_BASE = 0x60041000

# Peripherals the boot path may touch before the firmware's own board_init
# has enabled clocks: a read of a clock-gated peripheral stalls the bus on
# hardware, which the emulator (plain RAM) cannot show. The recovery image's
# first hardware run hung on a USB1 read exactly like that.
SAFE_EARLY_MMIO = {
    0x400AC000: "IOMUXC_GPR", 0x400B8000: "WDOG1", 0x400BC000: "WDOG3",
    0x400D0000: "WDOG2", 0x400F8000: "SRC", 0x400FC000: "CCM",
}
STUB = 0x600104D9


def elf_symbols(elf: Path) -> dict[str, int]:
    out = subprocess.run(
        ["arm-none-eabi-nm", str(elf)], check=True, capture_output=True, text=True
    ).stdout
    return {
        name: int(addr, 16)
        for addr, name in re.findall(r"^([0-9a-f]{8}) \S+ (\S+)$", out, re.MULTILINE)
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="packed .mr to dry-run")
    parser.add_argument("--elf", type=Path, required=True, help="firmware ELF for symbols")
    parser.add_argument("--app-elf", type=Path,
                        help="two-stage image: follow recovery into this app build")
    parser.add_argument("--app-slot", type=Path,
                        help="write this app slot image (with its data) at 0x60020000, "
                             "as the USB update does; the DFU .mr cannot carry the data")
    parser.add_argument("--max-instructions", type=int, default=4_000_000)
    parser.add_argument("--mmio", action="store_true",
                        help="list peripheral accesses (first PC per 4 KiB block) on the path")
    args = parser.parse_args()

    from unicorn import (
        UC_ARCH_ARM,
        UC_HOOK_CODE,
        UC_MODE_MCLASS,
        UC_MODE_THUMB,
        Uc,
    )
    from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_SP

    mr = MrFile.from_path(args.image)
    syms = elf_symbols(args.elf)
    stage2, stage2_main, app_main = syms["stage2"], syms["stage2_main"], syms["app_main"]
    copier = syms.get("copier")
    app_syms = elf_symbols(args.app_elf) if args.app_elf else {}
    app_app_main = app_syms.get("app_main")
    # The app's cold code (linker.ld .xiptext) runs in place from the slot
    # data area. The run goes on until app_main has called into it and come
    # back to ITCM through the veneers.
    cold = range(app_syms.get("__xiptext_start__", 0), app_syms.get("__xiptext_end__", 0))
    if args.app_elf and copier is None:
        raise SystemExit("--app-elf needs a recovery ELF (no copier symbol)")

    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB + UC_MODE_MCLASS)
    for base, size in [
        (ITCM_BASE, ITCM_SIZE), (DTCM_BASE, DTCM_SIZE), (OCRAM_BASE, OCRAM_SIZE),
        (PERIPH_BASE, PERIPH_SIZE), (FLASH_BASE, FLASH_SIZE), (SCB_BASE, SCB_SIZE),
    ]:
        uc.mem_map(base, size)
    uc.mem_write(0x60010000, mr.blocks[0].data)
    if len(mr.blocks) > 1:
        uc.mem_write(MODELS_BASE, mr.blocks[1].data)
    if args.app_slot:
        uc.mem_write(0x60020000, args.app_slot.read_bytes())

    hits: set[str] = set()
    count = [0]
    reached_app = [False]

    def hook(uc_, address, size, user):
        count[0] += 1
        if address == stage2:
            hits.add("stage2")
        elif address == stage2_main:
            hits.add("stage2_main")
        elif address == copier:
            hits.add("copier")
        elif "copier" in hits and address == app_app_main:
            hits.add("app app_main")
            reached_app[0] = True
            if not cold:
                uc_.emu_stop()
        elif "app app_main" in hits and address in cold:
            hits.add("app XIP code")
        elif "app XIP code" in hits and address < ITCM_SIZE:
            hits.add("back in ITCM")
            uc_.emu_stop()
        elif address == app_main and "app_main" not in hits:
            hits.add("app_main")
            if app_app_main is None:
                reached_app[0] = True
                uc_.emu_stop()
        if count[0] > args.max_instructions:
            uc_.emu_stop()

    uc.hook_add(UC_HOOK_CODE, hook)
    mmio: dict[int, tuple[int, int, str]] = {}
    if True:
        from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE

        def mem_hook(uc_, access, address, size, value, user):
            block = address & ~0xFFF
            if block not in mmio:
                # Before board_init/app_main of the image that owns the clocks.
                stage = "late" if ("app_main" in hits and app_app_main is None) or \
                    "app app_main" in hits else "early"
                mmio[block] = (address, uc_.reg_read(UC_ARM_REG_PC), stage)

        uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, mem_hook,
                    begin=PERIPH_BASE, end=PERIPH_BASE + PERIPH_SIZE - 1)

        def reset_hook(uc_, access, address, size, value, user):
            if value & 0xFFFF0004 == 0x05FA0004:   # AIRCR: VECTKEY + SYSRESETREQ
                hits.add("reset")
                uc_.emu_stop()

        uc.hook_add(UC_HOOK_MEM_WRITE, reset_hook, begin=0xE000ED0C, end=0xE000ED0F)
    uc.reg_write(UC_ARM_REG_SP, 0x20008000)
    error = ""
    try:
        uc.emu_start(STUB, 0, count=args.max_instructions)
    except Exception as exc:  # noqa: BLE001
        error = str(exc)

    pc = uc.reg_read(UC_ARM_REG_PC)
    print(f"instructions: {count[0]}, final PC: {pc:#010x}")
    names = ["stage2", "stage2_main", "app_main"]
    if app_app_main is not None:
        names += ["copier", "app app_main"]
    if cold:
        names += ["app XIP code", "back in ITCM"]
    for name in names:
        print(f"  reached {name}: {name in hits}")
    if "reset" in hits:
        print("  software reset requested")
    crumbs = struct.unpack("<4I", bytes(uc.mem_read(0x400F8028, 16)))   # SRC_GPR3..6
    print("  crumbs: " + " ".join(f"{c:08x}" for c in crumbs))
    copied = True
    if cold and args.app_slot and "app app_main" in hits:
        slot = args.app_slot.read_bytes()
        for name in ("ocramdata", "dtcmdata"):
            start, end = app_syms[f"__{name}_start__"], app_syms[f"__{name}_end__"]
            load = app_syms[f"__{name}_load__"] - 0x60020000
            want = slot[load:load + end - start]
            ok = bytes(uc.mem_read(start, end - start)) == want and len(want) == end - start
            print(f"  app .{name} copied: {ok}")
            copied = copied and ok
    if error:
        print(f"emulation stopped with: {error}")
    unsafe = []
    for block, (addr, at, stage) in sorted(mmio.items()):
        name = SAFE_EARLY_MMIO.get(block, "")
        if args.mmio:
            print(f"  mmio {addr:#010x} first at pc {at:#010x} ({stage}) {name}")
        if stage == "early" and not name:
            unsafe.append(f"{addr:#010x} at pc {at:#010x}")
    for u in unsafe:
        print(f"  UNSAFE early peripheral access: {u}")
    ok = reached_app[0] and not error and not unsafe and copied and all(n in hits for n in names)
    print("DRY RUN OK" if ok else "DRY RUN FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
