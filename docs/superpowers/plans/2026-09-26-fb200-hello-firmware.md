# fb200-hello Custom Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an app-only `.mr` packer (`fb200 fw pack`) and a buildable TinyUSB-based custom firmware (`fb200-hello`) that boots on the FB200 and enumerates as a custom USB CDC device, verified on hardware and recoverable to stock.

**Architecture:** Phase A adds pure-Python packing to the existing library/CLI with TDD. Phase B adds `firmware/hello/`, a bare-metal Cortex-M7 project that mimics the stock copy-to-ITCM startup, builds with `arm-none-eabi-gcc` against a pinned TinyUSB, and exposes a CDC-ACM banner. Phase C wires CI, docs, hardware validation and the v0.4.0 release.

**Tech Stack:** Python 3.10+ (stdlib only, pytest, ruff), C11, `arm-none-eabi-gcc`, GNU ld, TinyUSB (MIT), NXP SDK files bundled with TinyUSB (BSD-3), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-26-fb200-hello-firmware-design.md`

**Conventions:** Work directly on `main` (user preference). Commit after every task with the message given in the task. Do not commit `fb200-stock.mr`, `fb200-proof.mr`, or anything under `firmware/hello/.deps/` / `firmware/hello/build/`. The venv is `.venv`; use `.venv/bin/python`, `.venv/bin/pytest`, `.venv/bin/ruff`, `.venv/bin/fb200`.

**Hardware safety:** Every `fw flash ... --yes` step is a safety gate. Tasks 1–8 are offline. Task 9 pauses and asks the user before touching the pedal. Keep the local stock image; recovery is `fb200 fw flash fb200-stock.mr --yes`.

---

## Task 1: `pack_app_image` in the firmware library

**Files:**
- Modify: `src/fb200/firmware.py`
- Test: `tests/test_fw_pack.py` (create)

- [ ] **Step 1: Write the failing tests**

Create `tests/test_fw_pack.py`:

```python
import pytest

from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader, pack_app_image


def make_template(app_size: int = 512) -> MrFile:
    header = MrHeader(
        product_tag="FB200", send_cmd=0x02, rec_cmd=0x03, timeout=10000,
        update_block=2, update_addr=b"\x03\x00\x00\x00", version=0,
    )
    blocks = [
        MrBlock(MrBlockTag(send_cmd=0x04, rec_cmd=0x05, start_page=0x40, rom_id=0),
                bytes(app_size)),
        MrBlock(MrBlockTag(send_cmd=0x06, rec_cmd=0x07, start_page=0x00, rom_id=0),
                bytes(64)),
    ]
    return MrFile(header, blocks)


def test_pack_produces_single_block():
    packed = pack_app_image(make_template(), b"\x01\x02\x03")
    assert packed.header.update_block == 1
    assert len(packed.blocks) == 1
    assert packed.blocks[0].data[:3] == b"\x01\x02\x03"


def test_pack_pads_to_template_block_size():
    packed = pack_app_image(make_template(app_size=512), b"abc")
    assert len(packed.blocks[0].data) == 512
    assert packed.blocks[0].data[3:] == b"\xff" * 509


def test_pack_preserves_header_and_tag_fields():
    template = make_template()
    packed = pack_app_image(template, b"abc")
    assert packed.header.product_tag == "FB200"
    assert packed.header.send_cmd == 0x02
    assert packed.header.rec_cmd == 0x03
    assert packed.header.update_addr == b"\x03\x00\x00\x00"
    assert packed.header.version == 0
    assert packed.blocks[0].tag.send_cmd == 0x04
    assert packed.blocks[0].tag.rec_cmd == 0x05
    assert packed.blocks[0].tag.start_page == 0x40
    assert packed.blocks[0].tag.rom_id == 0
    # the stock header's raw bytes (magic, reserved fields) are preserved
    assert packed.to_bytes()[0:9] == b"Mooer_TAG"


def test_pack_round_trips_through_parser():
    packed = pack_app_image(make_template(), b"abc")
    reparsed = MrFile.from_bytes(packed.to_bytes())
    assert reparsed.header.update_block == 1
    assert reparsed.blocks[0].data == packed.blocks[0].data


def test_pack_rejects_wrong_product():
    template = make_template()
    template.header = MrHeader(product_tag="OTHER", send_cmd=0x02, rec_cmd=0x03)
    with pytest.raises(ValueError, match="FB200"):
        pack_app_image(template, b"abc")


def test_pack_rejects_empty_app():
    with pytest.raises(ValueError, match="empty"):
        pack_app_image(make_template(), b"")


def test_pack_rejects_oversize_app():
    with pytest.raises(ValueError, match="exceeds"):
        pack_app_image(make_template(app_size=8), b"123456789")


def test_pack_rejects_template_without_blocks():
    template = MrFile(MrHeader(product_tag="FB200"), [])
    with pytest.raises(ValueError, match="block"):
        pack_app_image(template, b"abc")


def test_packed_stock_sized_image_plans_392_writes():
    from fb200.updater import build_flash_plan

    template = make_template(app_size=200_704)
    packed = pack_app_image(template, b"\x00" * 200_704)
    plan = build_flash_plan(packed)
    assert plan.write_count == 392
    assert plan.total_bytes == 200_704
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `.venv/bin/pytest tests/test_fw_pack.py -v`
Expected: collection error / `ImportError: cannot import name 'pack_app_image'`.

- [ ] **Step 3: Implement `pack_app_image`**

Append to `src/fb200/firmware.py` after the `MrFile` class (before `_parse_header`):

```python
def pack_app_image(template: MrFile, app: bytes) -> MrFile:
    """Build a single-block (application-only) ``.mr`` image.

    The template supplies the header and block-0 tag; the bootloader's erase
    and write commands come from those fields. The payload is padded with
    ``0xFF`` (erased-flash value) to the template's block-0 size so the flash
    plan covers exactly the stock application pages.
    """
    if template.header.product_tag != "FB200":
        raise FirmwareError(
            f"template is for {template.header.product_tag!r} (expected 'FB200')"
        )
    if not template.blocks:
        raise FirmwareError("template has no blocks")
    if not app:
        raise FirmwareError("application binary is empty")
    limit = len(template.blocks[0].data)
    if len(app) > limit:
        raise FirmwareError(
            f"application binary exceeds template block 0 ({len(app)} > {limit} bytes)"
        )
    header = MrHeader(
        tag=template.header.tag,
        product_tag=template.header.product_tag,
        send_cmd=template.header.send_cmd,
        rec_cmd=template.header.rec_cmd,
        timeout=template.header.timeout,
        update_block=1,
        update_addr=template.header.update_addr,
        version=template.header.version,
        raw=template.header.raw,
    )
    block = MrBlock(template.blocks[0].tag, app.ljust(limit, b"\xff"))
    return MrFile(header, [block])
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `.venv/bin/pytest tests/test_fw_pack.py -v`
Expected: 10 passed.

- [ ] **Step 5: Full suite + lint**

Run: `.venv/bin/pytest -q && .venv/bin/ruff check .`
Expected: all pass (previous count 112 + 10 = 122 passed, 1 skipped, 2 deselected), ruff clean.

- [ ] **Step 6: Commit**

```bash
git add src/fb200/firmware.py tests/test_fw_pack.py
git commit -m "feat(fw): add app-only .mr packer"
```

---

## Task 2: `fb200 fw pack` CLI command

**Files:**
- Modify: `src/fb200/cli.py` (`build_parser` + new handler)
- Test: `tests/test_cli_fw_pack.py` (create)

- [ ] **Step 1: Write the failing tests**

Create `tests/test_cli_fw_pack.py`:

```python
from fb200 import cli
from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader


def make_template(path, app_size: int = 512, product: str = "FB200"):
    header = MrHeader(
        product_tag=product, send_cmd=0x02, rec_cmd=0x03, timeout=10000,
        update_block=2, update_addr=b"\x03\x00\x00\x00", version=0,
    )
    blocks = [
        MrBlock(MrBlockTag(send_cmd=0x04, rec_cmd=0x05, start_page=0x40),
                bytes(app_size)),
        MrBlock(MrBlockTag(send_cmd=0x06, rec_cmd=0x07, start_page=0x00),
                bytes(64)),
    ]
    path.write_bytes(MrFile(header, blocks).to_bytes())
    return path


def test_fw_pack_writes_single_block_image(tmp_path, capsys):
    template = make_template(tmp_path / "template.mr")
    app = tmp_path / "app.bin"
    app.write_bytes(b"\xde\xad\xbe\xef")
    out = tmp_path / "hello.mr"
    assert cli.main(
        ["fw", "pack", "--template", str(template), str(app), "-o", str(out)]
    ) == 0
    packed = MrFile.from_path(out)
    assert packed.header.update_block == 1
    assert len(packed.blocks[0].data) == 512
    assert packed.blocks[0].data[:4] == b"\xde\xad\xbe\xef"
    assert "packed" in capsys.readouterr().out


def test_fw_pack_default_output_name(tmp_path):
    template = make_template(tmp_path / "template.mr")
    app = tmp_path / "hello.bin"
    app.write_bytes(b"abc")
    assert cli.main(["fw", "pack", "--template", str(template), str(app)]) == 0
    assert (tmp_path / "hello.mr").exists()


def test_fw_pack_rejects_oversize_app(tmp_path, capsys):
    template = make_template(tmp_path / "template.mr", app_size=8)
    app = tmp_path / "big.bin"
    app.write_bytes(b"x" * 9)
    out = tmp_path / "out.mr"
    assert cli.main(
        ["fw", "pack", "--template", str(template), str(app), "-o", str(out)]
    ) == 1
    assert "exceeds" in capsys.readouterr().err


def test_fw_pack_rejects_wrong_product(tmp_path, capsys):
    template = make_template(tmp_path / "template.mr", product="OTHER")
    app = tmp_path / "app.bin"
    app.write_bytes(b"x")
    assert cli.main(["fw", "pack", "--template", str(template), str(app)]) == 1
    assert "FB200" in capsys.readouterr().err


def test_fw_pack_missing_app_file(tmp_path, capsys):
    template = make_template(tmp_path / "template.mr")
    assert cli.main(
        ["fw", "pack", "--template", str(template), str(tmp_path / "nope.bin")]
    ) == 1
    assert "cannot read" in capsys.readouterr().err
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `.venv/bin/pytest tests/test_cli_fw_pack.py -v`
Expected: FAIL — argparse error `invalid choice: 'pack'`.

- [ ] **Step 3: Add the handler**

Insert into `src/fb200/cli.py` after `_cmd_fw_patch_string`:

```python
def _cmd_fw_pack(args) -> int:
    from fb200.firmware import MrFile, pack_app_image

    template = MrFile.from_path(args.template)
    try:
        app = Path(args.app).read_bytes()
    except OSError as exc:
        raise FirmwareError(f"cannot read {args.app}: {exc}") from exc
    packed = pack_app_image(template, app)
    out = Path(args.output) if args.output else Path(args.app).with_suffix(".mr")
    try:
        out.write_bytes(packed.to_bytes())
    except OSError as exc:
        raise FirmwareError(f"cannot write {out}: {exc}") from exc
    print(
        f"packed {len(app)} bytes into {out} "
        f"({len(packed.blocks[0].data)}-byte block 0)"
    )
    return 0
```

- [ ] **Step 4: Register the subparser**

In `build_parser`, after the `p_patch` block, add:

```python
    p_pack = fw_sub.add_parser("pack", help="pack a raw binary into an app-only .mr")
    p_pack.add_argument("--template", required=True,
                        help="stock .mr whose header/tag fields are reused")
    p_pack.add_argument("app", help="raw application binary")
    p_pack.add_argument("--app-only", action="store_true",
                        help="single-block application image (default and only mode)")
    p_pack.add_argument("-o", "--output")
    p_pack.set_defaults(func=_cmd_fw_pack)
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `.venv/bin/pytest tests/test_cli_fw_pack.py -v`
Expected: 5 passed.

- [ ] **Step 6: Full suite + lint**

Run: `.venv/bin/pytest -q && .venv/bin/ruff check .`
Expected: 127 passed, 1 skipped, 2 deselected; ruff clean.

- [ ] **Step 7: Commit**

```bash
git add src/fb200/cli.py tests/test_cli_fw_pack.py
git commit -m "feat(cli): add fw pack command"
```

---

## Task 3: Vendor-free synthetic template tool

**Files:**
- Create: `firmware/hello/tools/synthetic_template.py`
- Test: `tests/test_synthetic_template.py` (create)

- [ ] **Step 1: Write the failing test**

Create `tests/test_synthetic_template.py`:

```python
import subprocess
import sys
from pathlib import Path

from fb200.firmware import MrFile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "firmware" / "hello" / "tools" / "synthetic_template.py"


def test_generates_stock_shaped_template(tmp_path):
    out = tmp_path / "template.mr"
    subprocess.run(
        [sys.executable, str(SCRIPT), "-o", str(out)],
        check=True,
        cwd=ROOT,
    )
    mr = MrFile.from_path(out)
    assert mr.header.product_tag == "FB200"
    assert mr.header.update_block == 2
    assert mr.header.send_cmd == 0x02
    assert mr.header.rec_cmd == 0x03
    assert mr.header.update_addr == b"\x03\x00\x00\x00"
    assert len(mr.blocks[0].data) == 200_704
    assert mr.blocks[0].tag.send_cmd == 0x04
    assert mr.blocks[0].tag.start_page == 0x40
    assert set(mr.blocks[0].data) == {0}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/pytest tests/test_synthetic_template.py -v`
Expected: FAIL — script does not exist (subprocess `FileNotFoundError` / returncode 2).

- [ ] **Step 3: Write the script**

Create `firmware/hello/tools/synthetic_template.py`:

```python
#!/usr/bin/env python3
"""Generate a vendor-free FB200 .mr template with stock-shaped fields.

Used by CI and for local pack tests. The payload is all zeros; this file must
never be flashed to a pedal.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "src"))

from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader  # noqa: E402

APP_SIZE = 200_704
MODELS_SIZE = 3_286_016


def build() -> MrFile:
    header = MrHeader(
        product_tag="FB200",
        send_cmd=0x02,
        rec_cmd=0x03,
        timeout=10000,
        update_block=2,
        update_addr=b"\x03\x00\x00\x00",
        version=0,
    )
    blocks = [
        MrBlock(
            MrBlockTag(
                start_addr=641,
                stop_addr=641 + APP_SIZE,
                block_size=APP_SIZE,
                send_cmd=0x04,
                rec_cmd=0x05,
                timeout=10000,
                start_page=0x40,
                rom_id=0,
            ),
            bytes(APP_SIZE),
        ),
        MrBlock(
            MrBlockTag(
                start_addr=641 + APP_SIZE + 512,
                stop_addr=641 + APP_SIZE + 512 + MODELS_SIZE,
                block_size=MODELS_SIZE,
                send_cmd=0x06,
                rec_cmd=0x07,
                timeout=10000,
                start_page=0x00,
                rom_id=0,
            ),
            bytes(MODELS_SIZE),
        ),
    ]
    return MrFile(header, blocks)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--app-size-only", action="store_true",
                        help="emit only block 0 (smaller file, for pack tests)")
    args = parser.parse_args(argv)
    mr = build()
    if args.app_size_only:
        mr = MrFile(mr.header, mr.blocks[:1])
        mr.header.update_block = 1
    Path(args.output).write_bytes(mr.to_bytes())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

Note: the default output is ~3.5 MB; CI uses the default (it must exercise the stock sizes), local tests may use `--app-size-only`.

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/pytest tests/test_synthetic_template.py -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/hello/tools/synthetic_template.py tests/test_synthetic_template.py
git commit -m "feat(fw): add vendor-free synthetic template tool"
```

---

## Task 4: Firmware skeleton — linker, startup, ITCM copy, build

**Files:**
- Create: `firmware/hello/Makefile`, `firmware/hello/linker.ld`, `firmware/hello/.gitignore`
- Create: `firmware/hello/src/boot_header.S`, `firmware/hello/src/startup.c`, `firmware/hello/src/vectors.c`, `firmware/hello/src/main.c`
- Test: `tests/test_hello_image.py` (create)

Toolchain: install locally with `brew install --cask gcc-arm-embedded` (macOS) or skip — the test is skipped without `arm-none-eabi-gcc`, and CI (Task 7) installs it.

- [ ] **Step 1: Write the failing test**

Create `tests/test_hello_image.py`:

```python
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "hello"
BIN = FW / "build" / "fb200-hello.bin"

pytestmark = pytest.mark.skipif(
    shutil.which("arm-none-eabi-gcc") is None or shutil.which("make") is None,
    reason="arm-none-eabi toolchain not installed",
)


def build():
    subprocess.run(["make", "clean", "build", "layout"], cwd=FW, check=True)
    return BIN.read_bytes()


def test_image_boot_header_and_size():
    data = build()
    assert len(data) <= 200_704
    sp, reset = int.from_bytes(data[0:4], "little"), int.from_bytes(data[4:8], "little")
    assert sp == 0x20058000
    assert reset & 1
    assert 0x60010000 <= reset < 0x60010000 + len(data)


def test_layout_symbols():
    subprocess.run(["make", "layout"], cwd=FW, check=True, capture_output=True)
    text = (FW / "build" / "layout.txt").read_text()
    syms = dict(
        (name, addr) for addr, name in re.findall(r"^([0-9a-f]{8}) \S+ (\S+)$", text, re.M)
    )
    assert int(syms["_estack"], 16) == 0x20058000
    assert int(syms["__itcm_start__"], 16) == 0
    int(syms["__itcm_lma__"], 16)  # exists and parses
    assert int(syms["app_main"], 16) < 0x40000
    reset = int(syms["reset_stub"], 16)
    assert 0x60010000 <= reset < 0x60010000 + len(BIN.read_bytes())
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/pytest tests/test_hello_image.py -v`
Expected: FAIL/SKIP collection — `make` target missing or files absent (if toolchain installed: `No rule to make target`).

- [ ] **Step 3: Write the linker script**

Create `firmware/hello/linker.ld`:

```ld
/* fb200-hello: code runs from ITCM at 0x0; the image lives in FlexSPI flash
 * at 0x60010000 behind an 8-byte boot header ([SP][reset stub]) consumed by
 * the stock bootloader. See docs/HARDWARE.md sections 2-3. */

ENTRY(reset_stub)

MEMORY
{
  BOOT   (rx)  : ORIGIN = 0x60010000, LENGTH = 8
  FLASH  (rx)  : ORIGIN = 0x60010008, LENGTH = 200696
  ITCM   (rwx) : ORIGIN = 0x00000000, LENGTH = 0x40000
  DTCM   (rwx) : ORIGIN = 0x20000000, LENGTH = 0x58000
}

SECTIONS
{
  .boot_header : { KEEP(*(.boot_header)) } > BOOT

  .boot_stub : {
    KEEP(*(.boot_stub))
    . = ALIGN(4);
  } > FLASH

  .itcm : {
    . = ALIGN(4);
    __itcm_start__ = .;
    KEEP(*(.vectors))
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
    __itcm_end__ = .;
  } > ITCM AT> FLASH
  __itcm_lma__ = LOADADDR(.itcm);

  .data : {
    . = ALIGN(4);
    __data_start__ = .;
    *(.data*)
    . = ALIGN(4);
    __data_end__ = .;
  } > DTCM AT> FLASH
  __data_lma__ = LOADADDR(.data);

  .bss (NOLOAD) : {
    . = ALIGN(4);
    __bss_start__ = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > DTCM

  _estack = ORIGIN(DTCM) + LENGTH(DTCM);

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
```

- [ ] **Step 4: Write the boot header**

Create `firmware/hello/src/boot_header.S`:

```asm
/* Boot header consumed by the stock bootloader: image[0] = initial SP,
 * image[1] = absolute (thumb) reset stub address. */
    .syntax unified
    .section .boot_header,"a",%progbits
    .word 0x20058000
    .word reset_stub + 1
```

- [ ] **Step 5: Write the startup stub**

Create `firmware/hello/src/startup.c`:

```c
/* Flash-resident reset stub: mirrors the stock startup (docs/HARDWARE.md 3.1).
 * Configures FlexRAM/TCM, copies the image to ITCM, sets VTOR = 0 and jumps.
 */
#include <stdint.h>

extern uint32_t __itcm_start__[], __itcm_end__[], __itcm_lma__[];
extern uint32_t __data_start__[], __data_end__[], __data_lma__[];
extern uint32_t __bss_start__[], __bss_end__[];
void app_main(void);

#define GPR(n) (*(volatile uint32_t *)(0x400AC000u + (n)))

__attribute__((section(".boot_stub"), used, noreturn))
void reset_stub(void)
{
    __asm volatile ("cpsid i" ::: "memory");

    GPR(0x38) = 0x00AA0000u;   /* IOMUXC_GPR14 */
    GPR(0x40) = 0x00200007u;   /* IOMUXC_GPR16: TCM control */
    GPR(0x44) = 0xFFAAAAA9u;   /* IOMUXC_GPR17: FlexRAM banks */

    uint32_t sp = *(volatile uint32_t *)0x60010000u;
    __asm volatile ("msr msp, %0" :: "r" (sp) : "memory");

    uint32_t *dst = __itcm_start__;
    uint32_t *src = __itcm_lma__;
    while (dst < __itcm_end__) {
        *dst++ = *src++;
    }

    dst = __data_start__;
    src = __data_lma__;
    while (dst < __data_end__) {
        *dst++ = *src++;
    }

    for (uint32_t *b = __bss_start__; b < __bss_end__; ) {
        *b++ = 0;
    }

    *(volatile uint32_t *)0xE000ED08u = 0;   /* SCB->VTOR = ITCM base */
    __asm volatile ("cpsie i" ::: "memory");
    app_main();
    __builtin_unreachable();
}
```

- [ ] **Step 6: Write the vector table**

Create `firmware/hello/src/vectors.c`:

```c
/* 256-entry vector table; copied to ITCM 0x0 and selected via VTOR = 0.
 * Entry [1] keeps the flash reset-stub address (as stock does). */
#include <stdint.h>

extern uint32_t _estack;
extern void reset_stub(void);
void Default_Handler(void)
{
    for (;;) {
    }
}

__attribute__((section(".vectors"), used))
const void *const g_vectors[256] = {
    &_estack,
    reset_stub,
    [2 ... 255] = Default_Handler,
};
```

- [ ] **Step 7: Write a temporary main (replaced in Task 6)**

Create `firmware/hello/src/main.c`:

```c
void app_main(void)
{
    for (;;) {
        __asm volatile ("wfi");
    }
}
```

- [ ] **Step 8: Write the Makefile**

Create `firmware/hello/Makefile`:

```make
# fb200-hello firmware build. Phase A (Task 4) builds only our own sources;
# Task 5 adds TinyUSB.
TARGET  := build/fb200-hello
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
NM      := $(CROSS)nm

CPUFLAGS := -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16
CFLAGS   := $(CPUFLAGS) -O2 -g3 -ffreestanding -fno-builtin -fno-common \
            -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
            -Wall -Wextra -Wno-unused-parameter -std=gnu11 -I src
LDFLAGS  := $(CPUFLAGS) -nostdlib -T linker.ld -Wl,-Map=$(TARGET).map,--cref

SRC_C := src/startup.c src/vectors.c src/main.c
SRC_S := src/boot_header.S
OBJS  := $(patsubst %,$(TARGET)/%.o,$(SRC_C) $(SRC_S))

.PHONY: all build clean layout deps

all: build

build: $(TARGET).bin

$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(TARGET)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

layout: $(TARGET).elf
	$(NM) $(TARGET).elf | grep -E '(_estack|__itcm_|__data_|__bss_|reset_stub|app_main)' | sort > build/layout.txt
	cat build/layout.txt

clean:
	rm -rf build

# TinyUSB fetching is added in Task 5.
deps:
	@echo "no dependencies yet"
```

Create `firmware/hello/.gitignore`:

```
.deps/
build/
```

- [ ] **Step 9: Build and verify tests pass**

If the toolchain is installed:

```bash
cd firmware/hello && make clean build layout
cd ../.. && .venv/bin/pytest tests/test_hello_image.py -v
```

Expected: binary builds; 2 tests pass. Without the toolchain they skip — proceed and let CI prove it in Task 7.

- [ ] **Step 10: Commit**

```bash
git add firmware/hello tests/test_hello_image.py
git commit -m "feat(fw): fb200-hello skeleton with ITCM startup"
```

---

## Task 5: TinyUSB dependency and BSP wiring

**Files:**
- Create: `firmware/hello/tinyusb.lock`
- Create: `firmware/hello/board/board_config.h`, `firmware/hello/board/README.md`
- Modify: `firmware/hello/Makefile`
- Modify: `tests/test_hello_image.py` (add a link check)

- [ ] **Step 1: Pin TinyUSB**

Find the latest stable release tag and its tarball checksum:

```bash
curl -s https://api.github.com/repos/hathach/tinyusb/releases/latest | grep -E '"tag_name"|tarball'
```

Create `firmware/hello/tinyusb.lock` with the chosen tag and the SHA-256 of
`https://github.com/hathach/tinyusb/archive/refs/tags/<TAG>.tar.gz` (download
once with `curl -L` and run `shasum -a 256` / `sha256sum`):

```
TINYUSB_TAG=0.19.0
TINYUSB_URL=https://github.com/hathach/tinyusb/archive/refs/tags/0.19.0.tar.gz
TINYUSB_SHA256=<sha256 of the tarball>
```

(Use the actual latest stable tag; the values above show the format.)

- [ ] **Step 2: Add fetch/extract to the Makefile**

In `firmware/hello/Makefile`, replace the `deps:` target and add variables near the top:

```make
TUSB_DIR := .deps/tinyusb

-include tinyusb.lock
TINYUSB_TAG    ?= 0.19.0
TINYUSB_URL    ?= https://github.com/hathach/tinyusb/archive/refs/tags/$(TINYUSB_TAG).tar.gz

deps: $(TUSB_DIR)/src/tusb.c

$(TUSB_DIR)/src/tusb.c:
	@mkdir -p .deps
	curl -fsSL -o .deps/tinyusb.tar.gz "$(TINYUSB_URL)"
	echo "$(TINYUSB_SHA256)  .deps/tinyusb.tar.gz" | shasum -a 256 -c - || \
	  echo "$(TINYUSB_SHA256)  .deps/tinyusb.tar.gz" | sha256sum -c -
	@rm -rf .deps/tinyusb.tmp && mkdir -p .deps/tinyusb.tmp
	tar -xzf .deps/tinyusb.tar.gz -C .deps/tinyusb.tmp --strip-components=1
	mv .deps/tinyusb.tmp $(TUSB_DIR)
```

- [ ] **Step 3: Inspect the BSP layout**

```bash
cd firmware/hello
make deps
ls .deps/tinyusb/hw/bsp/mimxrt10xx/boards/
sed -n '1,60p' .deps/tinyusb/hw/bsp/mimxrt10xx/boards/mimxrt1060_evk/board.mk
sed -n '1,40p' .deps/tinyusb/hw/bsp/mimxrt10xx/family.mk
```

Expected board: `mimxrt1060_evk`. If it is absent, use `mimxrt1050_evk`
instead and note it in `firmware/hello/README.md`. Confirm the exact filenames
of `board.c`, `clock_config.c/h`, and the SDK driver include/source lists in
`board.mk` / `family.mk`; the Makefile values in Step 4 mirror them.

- [ ] **Step 4: Wire TinyUSB into the Makefile**

Extend `firmware/hello/Makefile` with (adjusting names to what Step 3 showed):

```make
TOP        := $(abspath $(TUSB_DIR))
BOARD_PATH := $(TOP)/hw/bsp/mimxrt10xx/boards/mimxrt1060_evk
include $(TOP)/hw/bsp/mimxrt10xx/family.mk
include $(BOARD_PATH)/board.mk

CFLAGS += -I $(TOP)/src -I $(BOARD_PATH)
CFLAGS += -DCFG_TUSB_MCU=OPT_MCU_MIMXRT10XX

TUSB_SRC := \
  $(TOP)/src/tusb.c \
  $(TOP)/src/common/tusb_fifo.c \
  $(TOP)/src/device/usbd.c \
  $(TOP)/src/device/usbd_control.c

# SRC_C/INC provided by family.mk + board.mk cover the SDK subset and the
# BSP board sources (board.c, clock_config.c, ...).
SRC_C += $(TUSB_SRC)

build: deps
```

Check with `grep -n 'SRC_C\|INC' .deps/tinyusb/hw/bsp/mimxrt10xx/boards/mimxrt1060_evk/board.mk .deps/tinyusb/hw/bsp/mimxrt10xx/family.mk` and mirror any additional
required variables (e.g. `SRC_S`, `LD_FILE` should NOT be used — we provide our
own `linker.ld`).

- [ ] **Step 5: Add the board config header**

Create `firmware/hello/board/board_config.h`:

```c
/* Board facts for the FB200. Confirm against PCB photos before flashing. */
#pragma once

#define BOARD_XTAL_HZ 24000000u
#define HELLO_USB_VID 0xCAFE
#define HELLO_USB_PID 0x4001
#define HELLO_USB_MANUFACTURER "fb200-tools"
#define HELLO_USB_PRODUCT "FB200 Hello"
#define HELLO_USB_SERIAL "HELLO-0001"
```

Create `firmware/hello/board/README.md`:

```markdown
# Board glue

`board_config.h` holds FB200 facts. The BSP sources (`board.c`,
`clock_config.c`, MPU setup) are compiled directly from the pinned TinyUSB
release under `.deps/tinyusb/hw/bsp/mimxrt10xx/` (reference board
`mimxrt1060_evk`) so there is exactly one copy of each file:

- TinyUSB: MIT.
- Files derived from the NXP MCUXpresso SDK inside TinyUSB: BSD-3-Clause.

Crystal frequency is assumed to be 24 MHz (`BOARD_XTAL_HZ`); confirm from the
PCB before flashing. If the crystal differs, adapt the clock configuration in
`clock_config.c` from the BSP (or add a custom one here).
```

Create `firmware/hello/src/tusb_config.h`:

```c
#pragma once

#define CFG_TUSB_MCU            OPT_MCU_MIMXRT10XX
#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED

#define CFG_TUD_CDC             1
#define CFG_TUD_CDC_RX_BUFSIZE  64
#define CFG_TUD_CDC_TX_BUFSIZE  64
#define CFG_TUD_CDC_EP_BUFSIZE  64

#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))
```

Add `-I board` to `CFLAGS`.

- [ ] **Step 6: Build**

```bash
cd firmware/hello && make clean build layout
```

Expected: links successfully; binary ≤ 200,704 bytes. Iterate on missing
`SRC_C`/`INC` entries from Step 3 until it builds — this is the only step
allowed to require inspection of the fetched tree.

- [ ] **Step 7: Add the link-check to the image test**

In `tests/test_hello_image.py`, extend `test_layout_symbols` (or add a new
test) asserting the binary now contains TinyUSB: run
`arm-none-eabi-nm build/fb200-hello.elf | grep -c tusb_init` and assert ≥ 1.
Use `subprocess.run(..., check=True, capture_output=True, text=True)`.

- [ ] **Step 8: Run tests and commit**

```bash
cd ../.. && .venv/bin/pytest tests/test_hello_image.py -v
git add firmware/hello
git commit -m "feat(fw): wire pinned TinyUSB and BSP into fb200-hello"
```

---

## Task 6: USB descriptors, CDC banner and echo

**Files:**
- Create: `firmware/hello/src/usb_descriptors.c`, `firmware/hello/src/usb_descriptors.h`
- Modify: `firmware/hello/src/main.c`

- [ ] **Step 1: Write the descriptors**

Create `firmware/hello/src/usb_descriptors.h`:

```c
#pragma once

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
};
```

Create `firmware/hello/src/usb_descriptors.c`:

```c
#include "tusb.h"
#include "board/board_config.h"

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = HELLO_USB_VID,
    .idProduct = HELLO_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * (TUD_CDC_DESC_LEN))
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(0, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static const char *const strings[] = {
        [STRID_MANUFACTURER] = HELLO_USB_MANUFACTURER,
        [STRID_PRODUCT] = HELLO_USB_PRODUCT,
        [STRID_SERIAL] = HELLO_USB_SERIAL,
        [STRID_CDC] = "FB200 Hello CDC",
    };
    uint8_t chr_count;
    if (index == STRID_LANGID) {
        _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 + 2);
        _desc_str[1] = 0x0409;
        chr_count = 1;
    } else if (index < sizeof(strings) / sizeof(strings[0]) && strings[index]) {
        const char *str = strings[index];
        chr_count = 0;
        while (str[chr_count] && chr_count < 31) {
            _desc_str[1 + chr_count] = (uint16_t)str[chr_count];
            chr_count++;
        }
        _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    } else {
        return NULL;
    }
    return _desc_str;
}
```

- [ ] **Step 2: Write main.c**

Replace `firmware/hello/src/main.c`:

```c
#include "board_config.h"
#include "tusb.h"

static const char banner[] = "FB200 hello - fb200-tools custom firmware\r\n";

void app_main(void)
{
    board_init();
    tusb_init();

    bool announced = false;
    while (1) {
        tud_task();

        if (tud_cdc_connected() && !announced) {
            tud_cdc_write(banner, sizeof(banner) - 1);
            tud_cdc_write_flush();
            announced = true;
        } else if (!tud_cdc_connected()) {
            announced = false;
        }

        if (tud_cdc_available()) {
            char buf[64];
            uint32_t n = tud_cdc_read(buf, sizeof(buf));
            if (n) {
                tud_cdc_write(buf, n);
                tud_cdc_write_flush();
            }
        }
    }
}
```

- [ ] **Step 3: Build and test**

```bash
cd firmware/hello && make clean build layout && cd ../..
.venv/bin/pytest tests/test_hello_image.py -v
```

Expected: builds; tests pass; binary still ≤ 200,704 bytes (check with
`ls -l firmware/hello/build/fb200-hello.bin`; if it ever exceeds, trim the
vector table/config rather than the padding rule).

- [ ] **Step 4: Commit**

```bash
git add firmware/hello/src
git commit -m "feat(fw): CDC-ACM hello device with banner"
```

---

## Task 7: CI firmware job

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Read the existing workflow**

Run: `cat .github/workflows/ci.yml`
Expected: a `test` matrix job with `actions/checkout@v4`, `actions/setup-python@v5`,
`pip install -e ".[dev]"`, `pytest`, `ruff`.

- [ ] **Step 2: Add the firmware job**

Append a job that mirrors the existing style:

```yaml
  firmware:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: "3.12"
      - name: Install toolchain
        run: sudo apt-get update && sudo apt-get install -y gcc-arm-none-eabi make curl
      - name: Install package
        run: pip install -e .
      - name: Build firmware
        working-directory: firmware/hello
        run: make deps build
      - name: Generate synthetic template
        run: python firmware/hello/tools/synthetic_template.py -o firmware/hello/build/synthetic-template.mr
      - name: Pack flashable image
        run: fb200 fw pack --template firmware/hello/build/synthetic-template.mr firmware/hello/build/fb200-hello.bin -o firmware/hello/build/fb200-hello.mr
      - name: Inspect packed image
        run: fb200 fw inspect firmware/hello/build/fb200-hello.mr
      - uses: actions/upload-artifact@v4
        with:
          name: fb200-hello
          path: |
            firmware/hello/build/fb200-hello.bin
            firmware/hello/build/fb200-hello.mr
```

- [ ] **Step 3: Run the same steps locally where possible**

```bash
cd firmware/hello && make clean deps build && cd ../..
.venv/bin/python firmware/hello/tools/synthetic_template.py -o firmware/hello/build/synthetic-template.mr
.venv/bin/fb200 fw pack --template firmware/hello/build/synthetic-template.mr firmware/hello/build/fb200-hello.bin -o firmware/hello/build/fb200-hello.mr
.venv/bin/fb200 fw inspect firmware/hello/build/fb200-hello.mr
.venv/bin/fb200 fw flash firmware/hello/build/fb200-hello.mr      # dry run, no --yes
```

Expected: inspect shows `Blocks: 1`; dry run shows 392 write frames and
`dry run: pass --yes to actually flash` (no hardware access).

- [ ] **Step 4: Commit, push, watch CI**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: build fb200-hello and pack firmware artifacts"
git push origin main
gh run list --limit 3
gh run watch $(gh run list --limit 1 --json databaseId -q '.[0].databaseId') --exit-status
```

Expected: all jobs pass. If `firmware` fails, fix locally, amend with a new
commit, push, repeat (do not force-push).

---

## Task 8: Documentation and changelog

**Files:**
- Create: `firmware/hello/README.md`
- Modify: `README.md`, `docs/RESEARCH.md`, `CHANGELOG.md`, `docs/superpowers/specs/2026-09-26-fb200-hello-firmware-design.md` (§5.1 layout note)

- [ ] **Step 1: Write `firmware/hello/README.md`**

Content requirements (write it, no placeholders):

- What it is: minimal custom firmware proving boot + USB on the FB200.
- Requirements: `arm-none-eabi-gcc` (`brew install --cask gcc-arm-embedded` on
  macOS, `gcc-arm-none-eabi` on Debian/Ubuntu), `make`, `curl`.
- Build: `make deps build`; artifacts in `build/`.
- Pack + flash (from the repo root):

  ```bash
  python firmware/hello/tools/synthetic_template.py -o build/synthetic-template.mr
  fb200 fw pack --template build/synthetic-template.mr firmware/hello/build/fb200-hello.bin -o build/fb200-hello.mr
  fb200 fw flash build/fb200-hello.mr          # dry run
  fb200 fw flash build/fb200-hello.mr --yes    # write (read the recovery docs first)
  ```
- Success signal: `ioreg -p IOUSB -l -w 0 | grep -i cafe` (macOS) or
  `lsusb | grep -i cafe`; CDC port `FB200 Hello CDC` prints
  `FB200 hello - fb200-tools custom firmware`.
- Recovery: `fb200 fw flash fb200-stock.mr --yes` (or `--no-jump` if in
  update mode); link `docs/UPDATE_AND_RECOVERY.md`.
- Layout/startup summary: 8-byte boot header, `.boot_stub` in flash, copy to
  ITCM, `VTOR=0`; link `docs/HARDWARE.md` §2–§3.
- Licensing: our code MIT; TinyUSB MIT; SDK-derived files BSD-3.
- Warning: experimental, unofficial, can leave the pedal in the bootloader.

- [ ] **Step 2: Update the top-level README**

- Add to Features: `firmware/hello` — minimal custom firmware that boots and
  enumerates over USB (see `firmware/hello/README.md`).
- Add a Status table row: `| v0.4 (current) | In progress | app-only .mr packer, fb200-hello custom firmware |`
- Add `fb200 fw pack` to the Usage firmware block:

  ```bash
  fb200 fw pack --template stock.mr app.bin -o hello.mr   # app-only image
  ```

- [ ] **Step 3: Update RESEARCH.md and CHANGELOG.md**

- RESEARCH.md timeline: add `| 09-26 | fb200-hello: first custom firmware boots and enumerates over USB (CDC); see firmware/hello/ |`
  (only after Task 9 succeeds — if executing tasks in order, defer this line
  to Task 9; otherwise add now and adjust wording to "prepared").
- CHANGELOG.md: add under a new heading above `[0.3.0]`:

  ```markdown
  ## [Unreleased]

  ### Added

  - `fb200 fw pack` for building app-only (single-block) `.mr` images.
  - `firmware/hello`: minimal custom firmware that boots on the FB200 and
    enumerates as a CDC-ACM device (`0xCAFE:0x4001`), with a pinned TinyUSB
    build in CI.
  ```

- [ ] **Step 4: Amend the spec layout note**

In `docs/superpowers/specs/2026-09-26-fb200-hello-firmware-design.md` §5.1,
change the `board/` line to note that BSP sources are compiled directly from
the pinned `.deps/tinyusb` tree and only `board_config.h`/`README.md` live in
`firmware/hello/board/`.

- [ ] **Step 5: Verify and commit**

```bash
.venv/bin/pytest -q && .venv/bin/ruff check .
git add README.md CHANGELOG.md docs/RESEARCH.md docs/superpowers/specs/2026-09-26-fb200-hello-firmware-design.md firmware/hello/README.md
git commit -m "docs: fb200-hello firmware, pack tool and changelog"
git push origin main
```

---

## Task 9: Hardware validation (SAFETY GATE — ask the user first)

**Files:**
- Modify: `docs/UPDATE_AND_RECOVERY.md` (§7 results)

- [ ] **Step 1: Stop and ask the user**

Use the question tool: confirm the pedal is connected, the stock image is at
`./fb200-stock.mr`, and that flashing `fb200-hello.mr --yes` is authorized.
Do not proceed without an explicit yes.

- [ ] **Step 2: Dry run**

```bash
.venv/bin/fb200 fw flash firmware/hello/build/fb200-hello.mr
```

Expected: `Blocks: 1`, 392 write frames, dry-run message.

- [ ] **Step 3: Flash and verify enumeration**

```bash
.venv/bin/fb200 fw flash firmware/hello/build/fb200-hello.mr --yes
sleep 5
ioreg -p IOUSB -l -w 0 | grep -iE "cafe|FB200 Hello"
ls /dev/tty.usbmodem*
```

Read the banner (e.g. `screen /dev/tty.usbmodem* 115200` then Ctrl-A Ctrl-K,
or `stty -f ... raw && cat ...`). Expected: device present; banner text.
If nothing enumerates, do NOT retry blindly — go to recovery.

- [ ] **Step 4: Recover to stock immediately**

```bash
.venv/bin/fb200 fw flash fb200-stock.mr --yes
sleep 5
.venv/bin/fb200 info
```

Expected: `FB200 V1.0.1`, USB audio + HID back. If the pedal is in the
bootloader because something failed, use `--no-jump` instead.

- [ ] **Step 5: Record results**

Append to `docs/UPDATE_AND_RECOVERY.md` §7 (keep the table style):

```markdown
### fb200-hello custom firmware (2026-09-26)

| Check | Result |
|-------|--------|
| `fb200-hello.mr` pack | PASS — 1 block, 392 write frames |
| Flash app-only image | <PASS/FAIL> — device enumerated as `0xCAFE:0x4001` … |
| CDC banner | <observed text> |
| Stock recovery | PASS — `fb200 info` reported `FB200 V1.0.1`; hardware tests pass |
| Models preserved | <yes/no/unknown> — stock block 1 was <re-written/left alone> |
```

Fill `<...>` with the actual observed values.

- [ ] **Step 6: Commit and push**

```bash
git add docs/UPDATE_AND_RECOVERY.md
git commit -m "docs: record fb200-hello hardware validation"
git push origin main
```

---

## Task 10: Release v0.4.0

**Files:**
- Modify: `pyproject.toml`, `src/fb200/__init__.py`, `CHANGELOG.md`

- [ ] **Step 1: Bump versions**

`pyproject.toml`: `version = "0.3.0"` → `version = "0.4.0"`.
`src/fb200/__init__.py`: `__version__ = "0.3.0"` → `"0.4.0"`.
`CHANGELOG.md`: rename `## [Unreleased]` to `## [0.4.0] - 2026-09-26`.

- [ ] **Step 2: Verify**

```bash
.venv/bin/pip install -e ".[dev]" -q
.venv/bin/python -c "import fb200; print(fb200.__version__)"
.venv/bin/pytest -q && .venv/bin/ruff check .
```

Expected: `0.4.0`; all green.

- [ ] **Step 3: Commit, tag, push, release**

```bash
git add pyproject.toml src/fb200/__init__.py CHANGELOG.md
git commit -m "release: v0.4.0"
git tag v0.4.0
git push origin main --tags
gh release create v0.4.0 --title "v0.4.0 — app-only packing and fb200-hello custom firmware" \
  --notes "Adds fb200 fw pack and the fb200-hello custom firmware milestone (boots and enumerates over USB as a CDC-ACM device). Flashing is opt-in (--yes); recovery is documented in docs/UPDATE_AND_RECOVERY.md."
```

- [ ] **Step 4: Watch CI**

```bash
gh run watch $(gh run list --limit 1 --json databaseId -q '.[0].databaseId') --exit-status
```

Expected: success.

---

## Task 11: Final whole-range review

- [ ] **Step 1: Dispatch the review**

Dispatch a fresh subagent to review `v0.3.0..HEAD` for code quality and spec
compliance, with special attention to: `pack_app_image` validation, CLI exit
codes, startup/linker correctness vs the stock contract in `docs/HARDWARE.md`
§3, Makefile reproducibility (pinned TinyUSB + checksum), and CI artifacts.
Provide the reviewer the spec path and the hardware results.

- [ ] **Step 2: Fix findings**

Apply fixes (or dispatch the task implementer), re-run the suite, commit and
push as `fix: address final v0.4 review`. Re-review only the fixes.

- [ ] **Step 3: Confirm clean state**

```bash
git status --short
.venv/bin/pytest -q && .venv/bin/ruff check .
gh run list --limit 3
```

Expected: clean tree; suite green; CI success.

---

## Self-review checklist (completed during planning)

- Spec §2 success criteria: pack + inspect + plan (Tasks 1–2), CI build/pack
  (Task 7), hardware enumeration + banner (Task 9), stock recovery (Task 9),
  docs/CHANGELOG/CI green (Tasks 7–8).
- Spec §5.1 layout: Tasks 3–6 (board/ deviation documented in Task 8 Step 4).
- Spec §5.4 pack behavior: Task 1 steps 3 + tests.
- Spec §6 CI: Task 7.
- Spec §7 safety: Task 9 gate.
- Spec §10 release: Task 10.
- No `TBD`/`TODO`; every code step contains complete code; symbol names
  (`__itcm_lma__`, `reset_stub`, `pack_app_image`, `HELLO_USB_*`) are
  consistent across tasks.
