#!/usr/bin/env python3
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
app_main.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "src"))

from fb200.firmware import MrFile

ITCM_BASE, ITCM_SIZE = 0x0, 0x200000
DTCM_BASE, DTCM_SIZE = 0x20000000, 0x100000
OCRAM_BASE, OCRAM_SIZE = 0x20200000, 0x100000
PERIPH_BASE, PERIPH_SIZE = 0x40000000, 0x10000000
FLASH_BASE, FLASH_SIZE = 0x60000000, 0x800000
SCB_BASE, SCB_SIZE = 0xE0000000, 0x100000

MODELS_BASE = 0x60041000
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
    parser.add_argument("--max-instructions", type=int, default=4_000_000)
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

    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB + UC_MODE_MCLASS)
    for base, size in [
        (ITCM_BASE, ITCM_SIZE), (DTCM_BASE, DTCM_SIZE), (OCRAM_BASE, OCRAM_SIZE),
        (PERIPH_BASE, PERIPH_SIZE), (FLASH_BASE, FLASH_SIZE), (SCB_BASE, SCB_SIZE),
    ]:
        uc.mem_map(base, size)
    uc.mem_write(0x60010000, mr.blocks[0].data)
    if len(mr.blocks) > 1:
        uc.mem_write(MODELS_BASE, mr.blocks[1].data)

    hits: set[str] = set()
    count = [0]
    reached_app = [False]

    def hook(uc_, address, size, user):
        count[0] += 1
        if address == stage2:
            hits.add("stage2")
        elif address == stage2_main:
            hits.add("stage2_main")
        elif address == app_main:
            hits.add("app_main")
            reached_app[0] = True
            uc_.emu_stop()
        if count[0] > args.max_instructions:
            uc_.emu_stop()

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.reg_write(UC_ARM_REG_SP, 0x20008000)
    error = ""
    try:
        uc.emu_start(STUB, 0, count=args.max_instructions)
    except Exception as exc:  # noqa: BLE001
        error = str(exc)

    pc = uc.reg_read(UC_ARM_REG_PC)
    print(f"instructions: {count[0]}, final PC: {pc:#010x}")
    for name in ("stage2", "stage2_main", "app_main"):
        print(f"  reached {name}: {name in hits}")
    if error:
        print(f"emulation stopped with: {error}")
    ok = reached_app[0] and not error
    print("DRY RUN OK" if ok else "DRY RUN FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
