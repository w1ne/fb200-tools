# FB200 Audio Engine Bring-Up (Milestone 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An open firmware (`firmware/audio/`) that boots in the FB200 vendor image format, brings up codec + SAI + USB audio at 48 kHz, and passes audio through a float32 DSP hook — measured to match or beat stock.

**Architecture:** Vendor-format block 0 (vectors at 0x0, stock boot region at 0x400, our ITCM payload at 0x7D4 ≤ 121,800 B, bss in the stock memset region). MCUXpresso `fsl_sai`/`fsl_edma`/`fsl_dmamux`/`fsl_lpi2c` drivers + TinyUSB (UAC2 + CDC). Codec/clock/GPIO facts come from a Phase 0 reverse-engineering pass whose primary tool is a probe firmware that scans I²C and dumps codec registers (including reading back the stock app's configuration via the 0xC1-jump trick, no power cycle).

**Tech Stack:** arm-none-eabi-gcc 16.1, TinyUSB 0.21.0 (pinned), MCUXpresso SDK subset (fetched via TinyUSB deps), Python 3 (pytest, numpy/scipy, sounddevice) for host tests and measurements, Unicorn emulator (dry-run gate).

**Spec:** `docs/superpowers/specs/2026-09-27-fb200-audio-engine-design.md`

---

## File structure

```
firmware/tools/pack_vendor_image.py        # moved from hello (shared)
firmware/audio/
  Makefile                                 # deps fetch + build (adapt hello's)
  tinyusb.lock                             # same pin as hello
  linker.ld                                # vectors 0x0, .blob 0x400 @ LMA 0x600107d4, bss 0x20018B44
  src/stage2.S, src/startup.c, src/vectors.c   # vendor-format boot (adapt hello's)
  src/main.c                               # init order, superloop, CDC commands
  src/audio/sai.c                          # SAI1 + eDMA ping-pong
  src/audio/codec.c                        # LPI2C + codec init/volume/mute
  src/audio/usb_audio.c                    # TinyUSB UAC2 callbacks, FIFOs, feedback
  src/audio/engine.c                       # block loop, mixing, drift, meters
  src/dsp/dsp.h, src/dsp/dsp.c             # framework (host-compilable)
  src/dsp/gain.c, src/dsp/testgen.c        # milestone-1 nodes
  src/debug/cdc_log.c                      # non-blocking log over CDC
  src/compat/                              # copied from hello
  board/board_config.h                     # pins/clock facts from Phase 0
  tests/dsp_host_test.c                    # host test binary (asserts)
  tests/fixtures/                          # codec dump, SAI/clock notes
tests/test_audio_image.py                  # toolchain-gated image checks
tests/test_dsp_host.py                     # compiles + runs the host DSP test
tests/hardware/test_audio_smoke.py         # hardware-gated smoke (CDC/audio)
tools/measure.py                           # numpy/scipy measurement suite
docs/AUDIO_PATH.md                         # Phase 0 findings + measurements
```

---

## Task 0: Move the vendor packer to `firmware/tools/`

**Files:**
- Move: `firmware/hello/tools/pack_vendor_image.py` → `firmware/tools/pack_vendor_image.py`
- Modify: `tests/test_fw_pack_vendor.py`, `firmware/hello/README.md`, `.github/workflows/ci.yml`

- [ ] **Step 1: Move the file and fix its repo-root computation**

```bash
mkdir -p firmware/tools
git mv firmware/hello/tools/pack_vendor_image.py firmware/tools/pack_vendor_image.py
```

In `firmware/tools/pack_vendor_image.py`, change:

```python
REPO_ROOT = Path(__file__).resolve().parents[3]
```
to:
```python
REPO_ROOT = Path(__file__).resolve().parents[2]
```

- [ ] **Step 2: Update the test's script path**

In `tests/test_fw_pack_vendor.py`:

```python
SCRIPT = ROOT / "firmware" / "tools" / "pack_vendor_image.py"
```

- [ ] **Step 3: Update CI and docs references**

In `.github/workflows/ci.yml` replace `python firmware/hello/tools/pack_vendor_image.py` with `python firmware/tools/pack_vendor_image.py`. In `firmware/hello/README.md` replace `firmware/hello/tools/pack_vendor_image.py` with `firmware/tools/pack_vendor_image.py` (two occurrences: build tree and the pack command).

- [ ] **Step 4: Run the tests**

Run: `.venv/bin/pytest tests/test_fw_pack_vendor.py -q`
Expected: `4 passed`

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor: move the vendor image packer to firmware/tools/"
```

---

## Task 1: `firmware/audio/` scaffolding (vendor format, builds, CI)

**Files:**
- Create: `firmware/audio/Makefile`, `firmware/audio/tinyusb.lock`, `firmware/audio/linker.ld`
- Create: `firmware/audio/src/{stage2.S,startup.c,vectors.c,main.c}`, `firmware/audio/board/board_config.h`
- Create: `tests/test_audio_image.py`
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Copy the hello scaffolding and adapt**

```bash
mkdir -p firmware/audio/{src,board,tools}
cp firmware/hello/tinyusb.lock firmware/audio/tinyusb.lock
cp -r firmware/hello/src/compat firmware/audio/src/compat
cp firmware/hello/src/{stage2.S,startup.c,vectors.c} firmware/audio/src/
cp firmware/hello/board/board_config.h firmware/audio/board/
```

Changes to the copies:
- `firmware/audio/src/stage2.S`: unchanged (vendor entry stub at ITCM 0x4D6).
- `firmware/audio/src/startup.c`: in `stage2_main`, keep the GPR/FPU/SP setup and the vector-table copy; **delete** the `.bss` zero loop and the `__bss_*` externs (the vendor loader's entry 4 memsets bss before we run), but add a writability check:

```c
    /* Safety by construction: these ranges are the stock firmware's own.
     * Verify they are writable and report; the exact FlexRAM split behind
     * IOMUXC_GPR17 is not verified. */
    volatile uint32_t *bss = (volatile uint32_t *)0x20018B44u;
    bss[0] = 0xA5A5A5A5u;
    bss[0x358A4 / 4 - 1] = 0x5A5A5A5Au;
    int bss_ok = (bss[0] == 0xA5A5A5A5u && bss[0x358A4 / 4 - 1] == 0x5A5A5A5Au);
    bss[0] = 0;
    bss[0x358A4 / 4 - 1] = 0;
    (void)bss_ok;   /* logged by main.c via a global set below */
```

Export it for logging: `int g_bss_writable = 1;` at file scope, set `g_bss_writable = bss_ok;` above, and declare `extern int g_bss_writable;` in `main.c`.
- `firmware/audio/src/vectors.c`: change the comment only (handlers unchanged; `[1]` stays `0x600104D9`).

- [ ] **Step 2: Write the linker script**

Create `firmware/audio/linker.ld`:

```ld
/* fb200-audio: vendor image format.
 *   block0 0x000..0x400  vector table (copied to ITCM 0x0 by stage2)
 *   block0 0x400..0x7D4  vendor boot region (supplied by the packer)
 *   block0 0x7D4..0x1E39C ITCM payload (entry 0 memcpy -> ITCM 0x400)
 *   entry 4 memset range 0x20018B44..0x2004E3E8 = our .bss
 * Entry point: ITCM 0x4D6 (stage2.S). */

ENTRY(stage2)

MEMORY
{
  ITCM   (rwx) : ORIGIN = 0x00000000, LENGTH = 0x20000
  FLASH  (rx)  : ORIGIN = 0x60010000, LENGTH = 0x31000
  DTCM   (rw)  : ORIGIN = 0x20018B44, LENGTH = 0x358A4
}

SECTIONS
{
  .vectors 0x0 : { KEEP(*(.vectors)) } > ITCM AT> FLASH

  .blob 0x400 : AT(0x600107d4) {
    . += 0xd6;
    KEEP(*(.stage2))
    *(.text*)
    *(.rodata*)
    *(.data*)
    . = ALIGN(4);
    __blob_end__ = .;
  } > ITCM

  .bss (NOLOAD) : {
    . = ALIGN(4);
    __bss_start__ = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > DTCM

  _estack = 0x20058000;

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
```

- [ ] **Step 3: Write `main.c`**

Create `firmware/audio/src/main.c`:

```c
#include <stdint.h>
#include "tusb.h"
#include "bsp/board_api.h"
#include "dsp/dsp.h"
#include "debug/cdc_log.h"

extern int g_bss_writable;

void app_main(void)
{
    board_init();
    tusb_init();
    cdc_log_init();
    log_printf("fb200-audio up, bss_writable=%d\r\n", g_bss_writable);
    while (1) {
        tud_task();
        cdc_log_task();
    }
}
```

- [ ] **Step 4: Write the Makefile (adapt hello's)**

Copy `firmware/hello/Makefile` to `firmware/audio/Makefile`, then:
- `SRC_C`: replace `src/main.c src/memfuncs.c src/system_clock.c src/usb_descriptors.c src/compat/stubs.c` with `src/main.c src/memfuncs.c src/system_clock.c src/usb_descriptors.c src/compat/stubs.c src/dsp/dsp.c src/dsp/gain.c src/dsp/testgen.c src/debug/cdc_log.c` (the audio files arrive in later tasks; create empty stubs now so the build is green: `touch src/dsp/dsp.c src/dsp/gain.c src/dsp/testgen.c src/debug/cdc_log.c` and put a single `#include` line in each).
- Keep `src/stage2.S` in `SRC_S`.
- Add to `TUSB_INC`: `-I src` (already present via CFLAGS) and nothing else.
- The `layout` target greps `stage2|app_main|__bss_|__blob_end__|_estack` (same as hello).
- Artifacts: `$(TARGET).vectors.bin` and `$(TARGET).blob.bin` (same objcopy rules as hello).
- `TARGET := build/fb200-audio`.

- [ ] **Step 5: Write the image test**

Create `tests/test_audio_image.py` (mirror `tests/test_hello_image.py` with these assertions):

```python
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
VECTORS = FW / "build" / "fb200-audio.vectors.bin"
BLOB = FW / "build" / "fb200-audio.blob.bin"

pytestmark = pytest.mark.skipif(
    any(shutil.which(t) is None for t in ("arm-none-eabi-gcc", "make", "curl", "git", "python3")),
    reason="toolchain not installed",
)


def build() -> tuple[bytes, bytes]:
    subprocess.run(["make", "clean", "build", "layout"], cwd=FW, check=True)
    return VECTORS.read_bytes(), BLOB.read_bytes()


def test_vectors_and_entry():
    vectors, blob = build()
    assert len(vectors) == 0x400
    assert int.from_bytes(vectors[0:4], "little") == 0x20058000
    assert int.from_bytes(vectors[4:8], "little") == 0x600104D9
    text = (FW / "build" / "layout.txt").read_text()
    syms = {n: int(a, 16) for a, n in re.findall(r"^([0-9a-f]{8}) \S+ (\S+)$", text, re.M)}
    assert syms["stage2"] == 0x4D6
    assert syms["__bss_start__"] == 0x20018B44
    assert syms["__bss_end__"] <= 0x2004E3E8
    assert len(blob) == syms["__blob_end__"] - 0x400
    assert len(blob) <= 0x1DBC8          # vendor entry 0 payload limit
```

- [ ] **Step 6: Build and run the test**

Run: `.venv/bin/pytest tests/test_audio_image.py -q`
Expected: `1 passed` (first build takes a few minutes: deps fetch + compile)

- [ ] **Step 7: Add the CI job**

In `.github/workflows/ci.yml`, duplicate the `firmware` job as `firmware-audio`: same steps, but `working-directory: firmware/audio`, `make deps build`, and the pack step uses `firmware/audio/build/fb200-audio.{vectors,blob}.bin` with `-o firmware/audio/build/fb200-audio.mr`, artifact name `fb200-audio`.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "firmware: audio project scaffolding in the vendor image format"
```

---

## Task 2: DSP framework + gain + testgen (host-tested)

**Files:**
- Create: `firmware/audio/src/dsp/dsp.h`, `firmware/audio/src/dsp/dsp.c`
- Create: `firmware/audio/src/dsp/gain.c`, `firmware/audio/src/dsp/testgen.c`
- Create: `firmware/audio/tests/dsp_host_test.c`, `tests/test_dsp_host.py`

- [ ] **Step 1: Write the failing host test**

Create `firmware/audio/tests/dsp_host_test.c`:

```c
/* Host test for the DSP framework: built and run by tests/test_dsp_host.py. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dsp/dsp.h"
#include "dsp/gain.h"
#include "dsp/testgen.h"

static void fill(dsp_block_t *b, float v) {
    for (int ch = 0; ch < DSP_CHANNELS; ch++)
        for (int i = 0; i < DSP_BLOCK; i++) b->data[ch][i] = v;
}

int main(void) {
    /* gain: 0.5x, fully settled */
    dsp_block_t b;
    fill(&b, 1.0f);
    gain_ctx_t g;
    gain_init(&g, 0.5f);
    dsp_chain_t chain = {0};
    dsp_chain_add(&chain, gain_process, &g);
    for (int k = 0; k < 100; k++) dsp_chain_run(&chain, &b, DSP_BLOCK);
    assert(fabsf(b.data[0][0] - 0.5f) < 1e-3f);
    assert(fabsf(b.data[1][DSP_BLOCK - 1] - 0.5f) < 1e-3f);

    /* gain smoothing: no step larger than 1% of the delta per sample */
    fill(&b, 1.0f);
    gain_set(&g, 1.0f);
    float prev = 0.5f;
    for (int i = 0; i < DSP_BLOCK; i++) {
        float now = b.data[0][i];
        assert(now - prev < 0.01f + 1e-6f);
        prev = now;
    }

    /* testgen sine: RMS ~ amp/sqrt(2), 1 kHz at 48 kHz */
    testgen_ctx_t tg;
    testgen_init(&tg, 48000.0f);
    testgen_set(&tg, TESTGEN_SINE, 0.5f, 1000.0f);
    double sum = 0;
    int n = 0;
    for (int blk = 0; blk < 50; blk++) {
        dsp_chain_t tchain = {0};
        dsp_chain_add(&tchain, testgen_process, &tg);
        fill(&b, 0.0f);
        dsp_chain_run(&tchain, &b, DSP_BLOCK);
        for (int i = 0; i < DSP_BLOCK; i++) { sum += b.data[0][i] * b.data[0][i]; n++; }
    }
    double rms = sqrt(sum / n);
    assert(fabs(rms - 0.5 / sqrt(2.0)) < 0.01);
    assert(b.data[0][0] == b.data[1][0]);   /* duplicated to both channels */

    /* testgen off is a no-op */
    testgen_set(&tg, TESTGEN_OFF, 0.0f, 0.0f);
    fill(&b, 0.25f);
    dsp_chain_run(&(dsp_chain_t){0}, &b, DSP_BLOCK);
    assert(b.data[0][0] == 0.25f);

    printf("dsp host tests OK\n");
    return 0;
}
```

Create `tests/test_dsp_host.py`:

```python
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
OUT = FW / "build" / "dsp_host_test"

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None, reason="host C compiler not installed"
)


def test_dsp_host_suite():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    sources = [str(p) for p in sorted((FW / "src" / "dsp").glob("*.c"))]
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "dsp_host_test.c"), *sources, "-lm", "-o", str(OUT)],
        check=True,
    )
    result = subprocess.run([str(OUT)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "dsp host tests OK" in result.stdout
```

- [ ] **Step 2: Run it to verify it fails**

Run: `.venv/bin/pytest tests/test_dsp_host.py -q`
Expected: FAIL (missing `dsp/dsp.h`)

- [ ] **Step 3: Implement the framework**

Create `firmware/audio/src/dsp/dsp.h`:

```c
#ifndef FB200_DSP_H
#define FB200_DSP_H
#include <stddef.h>

#define DSP_BLOCK 32
#define DSP_CHANNELS 2
#define DSP_MAX_NODES 8

typedef struct { float data[DSP_CHANNELS][DSP_BLOCK]; } dsp_block_t;

typedef void (*dsp_process_fn)(void *ctx, dsp_block_t *b, size_t n);

typedef struct { dsp_process_fn process; void *ctx; } dsp_node_t;

typedef struct { dsp_node_t nodes[DSP_MAX_NODES]; size_t count; } dsp_chain_t;

static inline void dsp_chain_add(dsp_chain_t *c, dsp_process_fn fn, void *ctx)
{
    if (c->count < DSP_MAX_NODES) c->nodes[c->count++] = (dsp_node_t){fn, ctx};
}

static inline void dsp_chain_run(const dsp_chain_t *c, dsp_block_t *b, size_t n)
{
    for (size_t i = 0; i < c->count; i++) c->nodes[i].process(c->nodes[i].ctx, b, n);
}

/* one-pole smoothing for click-free parameter changes */
typedef struct { float target, current, coeff; } dsp_smooth_t;

static inline void dsp_smooth_init(dsp_smooth_t *s, float value, float coeff)
{
    s->target = s->current = value;
    s->coeff = coeff;
}

static inline void dsp_smooth_set(dsp_smooth_t *s, float value) { s->target = value; }

static inline float dsp_smooth_next(dsp_smooth_t *s)
{
    s->current += s->coeff * (s->target - s->current);
    return s->current;
}
#endif
```

Create `firmware/audio/src/dsp/dsp.c`:

```c
#include "dsp.h"
/* framework is header-only; translation unit kept for build symmetry */
```

Create `firmware/audio/src/dsp/gain.h`:

```c
#ifndef FB200_DSP_GAIN_H
#define FB200_DSP_GAIN_H
#include "dsp.h"
typedef struct { dsp_smooth_t gain; } gain_ctx_t;
void gain_init(gain_ctx_t *c, float value);
void gain_set(gain_ctx_t *c, float value);
void gain_process(void *ctx, dsp_block_t *b, size_t n);
#endif
```

Create `firmware/audio/src/dsp/gain.c`:

```c
#include "gain.h"
void gain_init(gain_ctx_t *c, float value) { dsp_smooth_init(&c->gain, value, 0.01f); }
void gain_set(gain_ctx_t *c, float value) { dsp_smooth_set(&c->gain, value); }
void gain_process(void *ctx, dsp_block_t *b, size_t n)
{
    gain_ctx_t *c = ctx;
    for (size_t i = 0; i < n; i++) {
        float g = dsp_smooth_next(&c->gain);
        for (size_t ch = 0; ch < DSP_CHANNELS; ch++) b->data[ch][i] *= g;
    }
}
```

Create `firmware/audio/src/dsp/testgen.h`:

```c
#ifndef FB200_DSP_TESTGEN_H
#define FB200_DSP_TESTGEN_H
#include "dsp.h"
typedef enum { TESTGEN_OFF = 0, TESTGEN_SINE, TESTGEN_WHITE, TESTGEN_IMPULSE } testgen_mode_t;
typedef struct {
    testgen_mode_t mode;
    float amp, phase, step, fs;
    uint32_t rng, impulse_countdown;
} testgen_ctx_t;
void testgen_init(testgen_ctx_t *c, float fs);
void testgen_set(testgen_ctx_t *c, testgen_mode_t mode, float amp, float freq);
void testgen_process(void *ctx, dsp_block_t *b, size_t n);
#endif
```

Create `firmware/audio/src/dsp/testgen.c`:

```c
#include <math.h>
#include "testgen.h"

void testgen_init(testgen_ctx_t *c, float fs)
{
    *c = (testgen_ctx_t){ .mode = TESTGEN_OFF, .amp = 0.0f, .fs = fs, .rng = 0x12345678u };
}

void testgen_set(testgen_ctx_t *c, testgen_mode_t mode, float amp, float freq)
{
    c->mode = mode;
    c->amp = amp;
    c->step = 6.28318530718f * freq / c->fs;
    c->phase = 0.0f;
    c->impulse_countdown = 0;
}

void testgen_process(void *ctx, dsp_block_t *b, size_t n)
{
    testgen_ctx_t *c = ctx;
    if (c->mode == TESTGEN_OFF) return;
    for (size_t i = 0; i < n; i++) {
        float s = 0.0f;
        switch (c->mode) {
        case TESTGEN_SINE:
            s = c->amp * sinf(c->phase);
            c->phase += c->step;
            if (c->phase > 6.28318530718f) c->phase -= 6.28318530718f;
            break;
        case TESTGEN_WHITE:
            c->rng ^= c->rng << 13; c->rng ^= c->rng >> 17; c->rng ^= c->rng << 5;
            s = c->amp * ((int32_t)c->rng / 2147483648.0f);
            break;
        case TESTGEN_IMPULSE:
            s = (c->impulse_countdown == 0) ? c->amp : 0.0f;
            c->impulse_countdown = (c->impulse_countdown == 0) ? (uint32_t)c->fs : c->impulse_countdown - 1;
            break;
        default: break;
        }
        b->data[0][i] = b->data[1][i] = s;
    }
}
```

- [ ] **Step 4: Run the host test**

Run: `.venv/bin/pytest tests/test_dsp_host.py -q`
Expected: `1 passed`

- [ ] **Step 5: Build the firmware**

Run: `make -C firmware/audio build`
Expected: builds; `build/fb200-audio.blob.bin` still ≤ 121,800 B

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "dsp: block framework, gain and test generator with host tests"
```

---

## Task 3: CDC debug log

**Files:**
- Create: `firmware/audio/src/debug/cdc_log.h`, `firmware/audio/src/debug/cdc_log.c`
- Modify: `firmware/audio/src/main.c` (already calls `cdc_log_init`/`cdc_log_task`)

- [ ] **Step 1: Implement the log**

Create `firmware/audio/src/debug/cdc_log.h`:

```c
#ifndef FB200_CDC_LOG_H
#define FB200_CDC_LOG_H
#include <stddef.h>
void cdc_log_init(void);
void cdc_log_task(void);
void cdc_log_write(const char *data, size_t len);
void log_printf(const char *fmt, ...);
#endif
```

Create `firmware/audio/src/debug/cdc_log.c`:

```c
#include <stdarg.h>
#include <stdio.h>
#include "tusb.h"
#include "cdc_log.h"

static char ring[1024];
static volatile size_t head, tail;

void cdc_log_init(void) { head = tail = 0; }

void cdc_log_write(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        size_t next = (head + 1) % sizeof ring;
        if (next == tail) break;            /* drop on overflow, never block */
        ring[head] = data[i];
        head = next;
    }
}

void log_printf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) cdc_log_write(buf, (size_t)((n < (int)sizeof buf) ? n : (int)sizeof buf - 1));
}

void cdc_log_task(void)
{
    if (!tud_cdc_connected()) return;
    if (tail == head) return;
    size_t written = 0;
    while (tail != head && written < 64) {
        uint8_t ch = (uint8_t)ring[tail];
        tail = (tail + 1) % sizeof ring;
        tud_cdc_write(&ch, 1);
        written++;
    }
    tud_cdc_write_flush();
}
```

- [ ] **Step 2: Build**

Run: `make -C firmware/audio build`
Expected: builds; blob size still within the limit

- [ ] **Step 3: Hardware check (manual)**

Flash (see Task 4 Step 4 for the exact command) and open the CDC port:

```bash
.venv/bin/python - <<'EOF'
import time, serial
s = serial.Serial("/dev/tty.usbmodemHELLO_00011", 115200, timeout=3)
time.sleep(0.5)
print(s.read(512))
EOF
```
Expected: `fb200-audio up, bss_writable=1` (or `0` if the range check fails — investigate if 0).

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "debug: non-blocking CDC log for the audio firmware"
```

---

## Task 4: Probe firmware — I²C scan + codec register dump (hardware RE)

**Files:**
- Create: `firmware/audio/src/audio/i2c_probe.h`, `firmware/audio/src/audio/i2c_probe.c`
- Modify: `firmware/audio/src/main.c` (command dispatch), `firmware/audio/Makefile` (add `fsl_lpi2c.c`, `fsl_gpio.c` already present)
- Create: `tests/fixtures/` output (see Step 5)

- [ ] **Step 1: Implement the probe**

Create `firmware/audio/src/audio/i2c_probe.h`:

```c
#ifndef FB200_I2C_PROBE_H
#define FB200_I2C_PROBE_H
#include <stdint.h>
void i2c_probe_init(void);
void i2c_scan_all(void);
void i2c_dump(uint8_t bus, uint8_t addr);
#endif
```

Create `firmware/audio/src/audio/i2c_probe.c`:

```c
#include "fsl_lpi2c.h"
#include "debug/cdc_log.h"
#include "i2c_probe.h"

static LPI2C_Type *const kBuses[4] = { LPI2C1, LPI2C2, LPI2C3, LPI2C4 };

static bool probe_addr(LPI2C_Type *base, uint8_t addr)
{
    lpi2c_master_transfer_t t = {
        .slaveAddress = addr, .direction = kLPI2C_Write, .data = NULL, .dataSize = 0,
    };
    return LPI2C_MasterTransferBlocking(base, &t) == kStatus_Success;
}

void i2c_probe_init(void)
{
    /* Codec/board clock facts are confirmed in Task 5; the EVK BSP clock tree
     * already brings up the LPI2C clocks used here. */
    for (int i = 0; i < 4; i++) {
        lpi2c_master_config_t cfg;
        LPI2C_MasterGetDefaultConfig(&cfg);
        cfg.baudRate_Hz = 100000u;
        LPI2C_MasterInit(kBuses[i], &cfg, 24000000u);
    }
}

void i2c_scan_all(void)
{
    for (int bus = 0; bus < 4; bus++) {
        log_printf("bus%d:", bus + 1);
        for (uint8_t a = 0x08; a <= 0x77; a++) {
            if (probe_addr(kBuses[bus], a)) log_printf(" %02x", a);
        }
        log_printf("\r\n");
    }
}

void i2c_dump(uint8_t bus, uint8_t addr)
{
    if (bus < 1 || bus > 4) { log_printf("bus must be 1..4\r\n"); return; }
    LPI2C_Type *base = kBuses[bus - 1];
    for (uint8_t reg = 0x00; reg < 0x80; reg++) {
        uint8_t v = 0;
        lpi2c_master_transfer_t t = {
            .slaveAddress = addr, .direction = kLPI2C_Write, .data = &reg, .dataSize = 1,
            .flags = kLPI2C_TransferNoStopFlag,
        };
        if (LPI2C_MasterTransferBlocking(base, &t) != kStatus_Success) {
            log_printf("%02x: ERR\r\n", reg);
            continue;
        }
        t = (lpi2c_master_transfer_t){
            .slaveAddress = addr, .direction = kLPI2C_Read, .data = &v, .dataSize = 1,
        };
        if (LPI2C_MasterTransferBlocking(base, &t) == kStatus_Success) log_printf("%02x: %02x\r\n", reg, v);
        else log_printf("%02x: ERR\r\n", reg);
    }
}
```

- [ ] **Step 2: Wire the commands in `main.c`**

In `main.c`: call `i2c_probe_init();` after `board_init();`, and in the loop:

```c
        if (tud_cdc_available()) {
            char cmd = (char)tud_cdc_read_char();
            if (cmd == 's') i2c_scan_all();
            else if (cmd == 'd') i2c_dump(1, 0x1A);   /* default; adjust after the scan */
            else if (cmd == 'D') { i2c_dump(2, 0x1A); }
        }
```

Add `#include "audio/i2c_probe.h"` and `fsl_lpi2c.c` to the Makefile `SRC_C`.

- [ ] **Step 3: Build and add tests for the pure parts**

Run: `make -C firmware/audio build`
Expected: builds within the payload limit.

- [ ] **Step 4: Hardware: scan and dump (checkpoint — needs the pedal)**

```bash
# 1. From the stock app (or DFU with --no-jump), flash the probe firmware:
.venv/bin/fb200 fw flash firmware/audio/build/fb200-audio.mr --yes --no-jump   # DFU; or --yes from stock
# 2. Open the CDC port and send 's':
.venv/bin/python - <<'EOF'
import time, serial
s = serial.Serial("/dev/tty.usbmodemHELLO_00011", 115200, timeout=3)
time.sleep(0.5); print(s.read(512))
s.write(b"s"); time.sleep(2); print(s.read(4096))
EOF
```
Expected: one bus reports a device (the codec, e.g. `bus1: 1a` or `bus3: 1a`). Record the bus/address in `docs/AUDIO_PATH.md`.

- [ ] **Step 5: Register read-back of the stock configuration (the 0xC1 trick)**

```bash
# With the STOCK app running (no power cycle anywhere in this sequence):
.venv/bin/fb200 fw flash fb200-stock.mr --yes          # ensure stock is running (jumps to DFU, then exit-jump)
# 1. Put the pedal in DFU *without* a power cycle:
.venv/bin/python - <<'EOF'
from fb200.transport import HidapiTransport
from fb200.protocol import VID, PID_APP, CMD_JUMP_BOOTLOADER, pack_frame, write_frame
t = HidapiTransport().open()
try: write_frame(t, pack_frame(CMD_JUMP_BOOTLOADER))
finally: t.close()
EOF
# 2. Flash the probe firmware (--no-jump, the codec keeps its state if its reset
#    is not coupled to the SoC reset) and immediately dump:
.venv/bin/fb200 fw flash firmware/audio/build/fb200-audio.mr --yes --no-jump
.venv/bin/python - <<'EOF'
import time, serial
s = serial.Serial("/dev/tty.usbmodemHELLO_00011", 115200, timeout=3)
time.sleep(0.5)
s.write(b"d"); time.sleep(3)
data = s.read(8192)
open("tests/fixtures/fb200-codec-dump.txt", "wb").write(data)
print(data.decode(errors="replace"))
EOF
```
Expected: a register dump. If all registers read `00`/`ERR`, the codec reset is SoC-coupled — record that in `docs/AUDIO_PATH.md` and rely on the datasheet + static RE (Task 5).

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "audio: I2C probe firmware (bus scan + codec register dump)"
```

---

## Task 5: Static RE — clock topology, SAI config, GPIOs

**Files:**
- Create: `docs/AUDIO_PATH.md`
- Create: `tests/fixtures/fb200-sai-config.txt` (extracted values)

- [ ] **Step 1: Extract the SAI/clock configuration**

Use the Unicorn harness (see `docs/FIRMWARE_BRINGUP.md` §2 for the recipe) to emulate the stock boot, then **isolate the SAI init function** (stock ITCM 0xF180 for SAI1, 0xF408 for SAI2): map the regions, set up the RAM pointers it reads, run from its entry with a code hook, and log every write to `0x40384000..0x4038C000` (SAI1) and `0x400FC000..0x400FD000` (CCM) / `0x400D8000..0x400D9000` (ANALOG). Record the register values.

Expected outcome: SAI TCR2..TCR5 / RCR2..RCR5 values, MCLK divider, and the PLL4 settings for the stock 44.1 kHz rate. Write them to `tests/fixtures/fb200-sai-config.txt` with a header naming the source (ITCM addresses + emulator run).

- [ ] **Step 2: Extract GPIO/analog routing**

Search the stock ITCM image for IOMUXC (`0x401F8000`) and GPIO (`0x401B8000..0x401C4000`) register writes near the audio init (disassembly around the SAI init callers). Record pins for: codec reset, mute/enable, jack detect, output amp, and anything else the audio init touches.

- [ ] **Step 3: Confirm the codec part and clock ratio**

From the Task 4 scan address, the register dump (if any), and the board marking (`NAU88BL21`), identify the exact Nuvoton part and its datasheet: confirm the I²C address, MCLK ratio (256×Fs typical), register map, and the required init registers (clocking, ADC/DAC enables, volume). Record in `docs/AUDIO_PATH.md` with evidence.

- [ ] **Step 4: Write `docs/AUDIO_PATH.md`**

Sections: codec (part, bus, address, register map, init sequence + source), clocking (MCLK source/rate, PLL4, SAI dividers, who is master), GPIO/analog routing (pin list with evidence), audio path behavior (stock bypass/USB routing as far as known), and open questions. Cross-link from `docs/HARDWARE.md` §6.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "docs: audio path RE findings (codec, clocks, GPIOs)"
```

---

## Task 6: Codec driver

**Files:**
- Create: `firmware/audio/src/audio/codec.h`, `firmware/audio/src/audio/codec.c`
- Create: `tests/test_codec_init.py` (host test of the sequence generator)
- Modify: `firmware/audio/src/main.c` (call `codec_init()`)

- [ ] **Step 1: Write the failing host test**

Create `tests/test_codec_init.py`:

```python
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
OUT = FW / "build" / "codec_host_test"

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="no host compiler")


def test_codec_init_sequence():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I", str(FW / "src"),
         str(FW / "tests" / "codec_host_test.c"),
         str(FW / "src" / "audio" / "codec.c"), "-lm", "-o", str(OUT)],
        check=True,
    )
    result = subprocess.run([str(OUT)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
```

Create `firmware/audio/tests/codec_host_test.c`:

```c
/* Host test: the codec init sequence must be non-empty, start with a reset
 * write, and end with the DAC unmuted (soft-mute cleared). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "audio/codec.h"

int main(void)
{
    codec_write_t seq[128];
    size_t n = codec_build_init_sequence(seq, 128);
    assert(n > 8 && n < 128);
    assert(seq[0].reg == 0x00);            /* reset first */
    size_t last = n - 1;
    assert(seq[last].reg != 0x00);
    int seen_unmute = 0;
    for (size_t i = 0; i < n; i++) if (seq[i].reg == CODEC_REG_DAC_CTRL && (seq[i].value & 0x01) == 0) seen_unmute = 1;
    assert(seen_unmute);
    printf("codec init sequence OK (%zu writes)\n", n);
    return 0;
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `.venv/bin/pytest tests/test_codec_init.py -q`
Expected: FAIL (missing `audio/codec.h`)

- [ ] **Step 3: Implement `codec.h`/`codec.c`**

Create `firmware/audio/src/audio/codec.h`:

```c
#ifndef FB200_CODEC_H
#define FB200_CODEC_H
#include <stddef.h>
#include <stdint.h>

/* Values from docs/AUDIO_PATH.md (Task 5). Update there first, then here. */
#define CODEC_I2C_BUS   1
#define CODEC_I2C_ADDR  0x1A
#define CODEC_REG_DAC_CTRL 0x0A     /* placeholder: replace with the real map */

typedef struct { uint8_t reg, value; } codec_write_t;

size_t codec_build_init_sequence(codec_write_t *out, size_t max);
void codec_init(void);
void codec_set_volume(float db);
void codec_mute(int on);
#endif
```

Create `firmware/audio/src/audio/codec.c` (structure; the table values come from Task 5):

```c
#include "fsl_lpi2c.h"
#include "debug/cdc_log.h"
#include "codec.h"

static LPI2C_Type *codec_bus(void) { return LPI2C1; }   /* per CODEC_I2C_BUS */

size_t codec_build_init_sequence(codec_write_t *out, size_t max)
{
    static const codec_write_t seq[] = {
        /* filled from docs/AUDIO_PATH.md: reset, clocking, ADC/DAC enable,
         * volume, output routing, soft-mute off. Registers are written in
         * order; values are validated against the codec datasheet. */
        { 0x00, 0x00 },   /* reset */
    };
    size_t n = sizeof seq / sizeof seq[0];
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = seq[i];
    return n;
}

static int codec_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    lpi2c_master_transfer_t t = {
        .slaveAddress = CODEC_I2C_ADDR, .direction = kLPI2C_Write,
        .data = buf, .dataSize = 2,
    };
    return LPI2C_MasterTransferBlocking(codec_bus(), &t) == kStatus_Success;
}

void codec_init(void)
{
    codec_write_t seq[128];
    size_t n = codec_build_init_sequence(seq, 128);
    size_t ok = 0;
    for (size_t i = 0; i < n; i++) if (codec_write(seq[i].reg, seq[i].value)) ok++;
    log_printf("codec: %u/%u writes ok\r\n", (unsigned)ok, (unsigned)n);
}
```

The host test's `codec_host_test.c` only compiles `codec.c`, so guard the hardware
parts with `#ifndef CODEC_HOST_TEST` around the `fsl_lpi2c.h` include and the
`codec_write`/`codec_init` bodies; define `CODEC_HOST_TEST` in the host test
compile command (add `-DCODEC_HOST_TEST` in `tests/test_codec_init.py`).

- [ ] **Step 4: Run the host test**

Run: `.venv/bin/pytest tests/test_codec_init.py -q`
Expected: `1 passed` after the table contains a real sequence (Task 5); until then, keep the test red and mark it `xfail` with a reason referencing Task 5 — remove the marker in Task 5's commit.

- [ ] **Step 5: Hardware: codec init + register read-back**

Flash and send `d`; expect sane register values after init (not all zero) and `codec: N/N writes ok` in the log.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "audio: codec driver with datasheet-derived init sequence"
```

---

## Task 7: SAI + eDMA ping-pong + passthrough engine

**Files:**
- Create: `firmware/audio/src/audio/sai.h`, `firmware/audio/src/audio/sai.c`
- Create: `firmware/audio/src/audio/engine.h`, `firmware/audio/src/audio/engine.c`
- Modify: `firmware/audio/src/main.c`, `firmware/audio/Makefile` (add `fsl_sai.c`, `fsl_edma.c`, `fsl_dmamux.c`)

- [ ] **Step 1: Implement `sai.c` (ping-pong, 32 frames)**

```c
#include "fsl_sai.h"
#include "fsl_edma.h"
#include "fsl_dmamux.h"
#include "audio/sai.h"

#define SAI_BASE SAI1
#define DMA_BASE DMA0
#define DMAMUX_BASE DMAMUX
#define DMA_RX_CH 0
#define DMA_TX_CH 1
#define DMA_RX_REQ kDmaRequestMuxSai1Rx
#define DMA_TX_REQ kDmaRequestMuxSai1Tx

static edma_handle_t rx_dma, tx_dma;
static sai_transfer_t rx_xfer[2], tx_xfer[2];
static volatile uint32_t rx_done[2], tx_done[2];

/* Double-buffer scheme: two SAI buffers per direction; on completion the
 * callback marks the buffer done and re-arms the same buffer. The engine
 * consumes/produces on the done flags. */

static void rx_cb(edma_handle_t *h, void *param, bool transferDone, uint32_t tcds)
{
    (void)h; (void)transferDone; (void)tcds;
    rx_done[(uintptr_t)param]++;
}

static void tx_cb(edma_handle_t *h, void *param, bool transferDone, uint32_t tcds)
{
    (void)h; (void)transferDone; (void)tcds;
    tx_done[(uintptr_t)param]++;
}

void sai_audio_init(void)
{
    /* Clocking (MCLK 12.288 MHz for 48 kHz) is configured with the clock
     * tree from Task 5; format below follows the codec datasheet. */
    sai_transceiver_t cfg;
    SAI_GetClassicI2SConfig(&cfg, kSAI_DataWidth32, kSAI_Stereo, 1u << 30);
    cfg.masterSlave = kSAI_Master;
    cfg.frameSyncWidth = 32;             /* 2 x 32-bit slots per frame */
    cfg.syncMode = kSAI_ModeAsync;
    cfg.mclkOutputEnable = true;
    SAI_Init(SAI_BASE);
    SAI_TxSetConfig(SAI_BASE, &cfg);
    SAI_RxSetConfig(SAI_BASE, &cfg);

    DMAMUX_Init(DMAMUX_BASE);
    DMAMUX_SetSource(DMAMUX_BASE, DMA_RX_CH, DMA_RX_REQ);
    DMAMUX_EnableChannel(DMAMUX_BASE, DMA_RX_CH);
    DMAMUX_SetSource(DMAMUX_BASE, DMA_TX_CH, DMA_TX_REQ);
    DMAMUX_EnableChannel(DMAMUX_BASE, DMA_TX_CH);

    EDMA_Init(DMA_BASE, &(edma_config_t){0});
    EDMA_CreateHandle(&rx_dma, DMA_BASE, DMA_RX_CH);
    EDMA_CreateHandle(&tx_dma, DMA_BASE, DMA_TX_CH);
    EDMA_SetCallback(&rx_dma, rx_cb, (void *)(uintptr_t)0);
    EDMA_SetCallback(&tx_dma, tx_cb, (void *)(uintptr_t)0);

    SAI_TxEnableDMA(SAI_BASE, kSAI_FIFORequestDMAEnable, true);
    SAI_RxEnableDMA(SAI_BASE, kSAI_FIFORequestDMAEnable, true);
    SAI_TxEnable(SAI_BASE, true);
    SAI_RxEnable(SAI_BASE, true);
}
```

The exact `sai_transceiver_t` fields and DMA transfer setup are pinned by the
SAI fixture from Task 5; the implementer fills in `EDMA_PrepareTransfer` +
`EDMA_SubmitTransfer` for both buffers per direction (see the SDK's
`drivers/sai` and `drivers/edma` headers in the deps tree).

- [ ] **Step 2: Implement `engine.c`**

```c
#include <string.h>
#include "dsp/dsp.h"
#include "dsp/gain.h"
#include "dsp/testgen.h"
#include "audio/engine.h"
#include "audio/sai.h"

static dsp_block_t block;
static dsp_chain_t chain;
static gain_ctx_t gain;
static testgen_ctx_t testgen;

void engine_init(void)
{
    gain_init(&gain, 1.0f);
    testgen_init(&testgen, 48000.0f);
    dsp_chain_add(&chain, gain_process, &gain);
    dsp_chain_add(&chain, testgen_process, &testgen);
}

void engine_set_gain_db(float db) { gain_set(&gain, powf(10.0f, db / 20.0f)); }
void engine_set_testgen(testgen_mode_t m, float amp, float freq) { testgen_set(&testgen, m, amp, freq); }

/* Called from the superloop when an RX buffer is complete. */
void engine_run(const float *in, float *out, size_t n)
{
    for (size_t ch = 0; ch < DSP_CHANNELS; ch++)
        memcpy(block.data[ch], in + ch * n, n * sizeof(float));
    dsp_chain_run(&chain, &block, n);
    for (size_t ch = 0; ch < DSP_CHANNELS; ch++)
        memcpy(out + ch * n, block.data[ch], n * sizeof(float));
}
```

(Interleaving convention is fixed in `sai.h`: `sai_audio_read(float *dst, size_t n)` / `sai_audio_write(const float *src, size_t n)` with deinterleaved channel blocks. The engine consumes/produces on the DMA done flags in `main.c`'s loop.)

- [ ] **Step 3: Wire the superloop**

In `main.c`:

```c
    sai_audio_init();
    engine_init();
    ...
    while (1) {
        tud_task();
        cdc_log_task();
        if (sai_audio_block_ready()) {
            float in[2 * DSP_BLOCK], out[2 * DSP_BLOCK];
            sai_audio_read(in, DSP_BLOCK);
            engine_run(in, out, DSP_BLOCK);
            sai_audio_write(out, DSP_BLOCK);
        }
    }
```

- [ ] **Step 4: Hardware checkpoint — first audio**

Flash, play a guitar/test tone into the analog input, listen on the analog
output; send `t` over CDC to enable the 1 kHz testgen and confirm tone at the
outputs. Record the result in `docs/AUDIO_PATH.md`.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "audio: SAI + eDMA ping-pong and the passthrough engine"
```

---

## Task 8: USB UAC2 audio (playback + capture) with FIFOs

**Files:**
- Create: `firmware/audio/src/audio/usb_audio.h`, `firmware/audio/src/audio/usb_audio.c`
- Modify: `firmware/audio/src/usb_descriptors.c` (UAC2 + CDC), `tusb_config.h`
- Modify: `firmware/audio/src/audio/engine.c` (mix USB playback; feed USB capture)

- [ ] **Step 1: Enable UAC2 in `tusb_config.h`**

```c
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUD_CDC 1
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256
#define CFG_TUD_AUDIO 1
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT 2
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX 2
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX 2
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX 2
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX 2
#define CFG_TUD_AUDIO_FUNC_1_EP_SZ_OUT 96        /* 32 frames * 2 ch * 16 bit / 1 ms, with margin */
#define CFG_TUD_AUDIO_FUNC_1_EP_SZ_IN 96
#define CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE 48000
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP 1
```

Start with **feedback disabled** (`0`) for the first working loopback; enable it in Step 4 only if drift causes dropouts (documented fallback: adaptive).

- [ ] **Step 2: Write the UAC2 descriptors**

Base the descriptor set on TinyUSB's UAC2 device example in the deps tree
(`.deps/tinyusb/examples/device/uac2_headset/src/usb_descriptors.c`), adapted:
2 playback channels (host → pedal) and 2 capture channels (pedal → host), 48 kHz,
16-bit, one IAD, and the CDC function. Keep the string descriptors from the
hello firmware (`fb200-tools` / `FB200 Audio`).

- [ ] **Step 3: Implement the audio callbacks + FIFOs**

Create `firmware/audio/src/audio/usb_audio.c`:

```c
#include <string.h>
#include "tusb.h"
#include "audio/usb_audio.h"

/* SPSC ring for host playback -> engine, and engine -> host capture. */
#define RING_FRAMES 1024
static int16_t play_ring[RING_FRAMES * 2];
static volatile size_t play_head, play_tail;
static int16_t cap_ring[RING_FRAMES * 2];
static volatile size_t cap_head, cap_tail;

/* TinyUSB: host playback arrives here (interleaved int16, 2 ch) */
bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id,
                                   uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport; (void)func_id; (void)ep_out; (void)cur_alt_setting;
    int16_t buf[96];
    uint16_t got = tud_audio_read(buf, n_bytes_received);
    for (uint16_t i = 0; i + 1 < got; i += 2) {
        size_t next = (play_head + 1) % RING_FRAMES;
        if (next == play_tail) break;             /* drop on overflow */
        play_ring[play_head * 2 + 0] = buf[i];
        play_ring[play_head * 2 + 1] = buf[i + 1];
        play_head = next;
    }
    return true;
}

/* TinyUSB: host capture pulls from here */
bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t func_id, uint8_t ep_in,
                                   uint8_t cur_alt_setting)
{
    (void)rhport; (void)func_id; (void)ep_in; (void)cur_alt_setting;
    int16_t buf[96];
    uint16_t frames = 0;
    while (frames < 48 && cap_tail != cap_head) {
        buf[frames * 2 + 0] = cap_ring[cap_tail * 2 + 0];
        buf[frames * 2 + 1] = cap_ring[cap_tail * 2 + 1];
        cap_tail = (cap_tail + 1) % RING_FRAMES;
        frames++;
    }
    if (frames == 0) { tud_audio_write(buf, 0); return true; }
    tud_audio_write(buf, frames * 4);
    return true;
}

/* engine side */
size_t usb_audio_pull(float *dst, size_t frames);        /* playback -> engine */
void usb_audio_push(const float *src, size_t frames);    /* engine -> capture */
```

Implement `usb_audio_pull`/`usb_audio_push` with the same rings and int16
conversion (clamp to [-1, 1) before scaling by 32767).

- [ ] **Step 4: Wire into the engine and test**

`engine_run` mixes `usb_audio_pull()` into the DAC output (clean monitor) and
pushes the processed guitar to `usb_audio_push()`. Hardware test:

```bash
# macOS: list the device
system_profiler SPAudioDataType | grep -A2 -i "FB200"
# Loopback: play a sweep to the pedal, record from it (see tools/measure.py, Task 10)
```

Expected: the pedal appears as an audio device; playback is audible on the
analog outs; capture records the processed input. If dropouts occur, enable
`CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP` and implement `tud_audio_feedback_params_cb`
using the measured FIFO fill (fallback: adaptive mode).

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "audio: USB UAC2 playback and capture with SPSC rings"
```

---

## Task 9: Engine polish — drift, meters, CDC commands

**Files:**
- Modify: `firmware/audio/src/audio/engine.c`, `firmware/audio/src/main.c`
- Create: `firmware/audio/tests/engine_host_test.c` (drift/drop-insert logic)
- Modify: `tests/test_dsp_host.py` (also build the engine test)

- [ ] **Step 1: Write the failing drift test**

`engine_host_test.c` asserts the drift logic: given a playback ring with 0 or
2 frames available, the engine inserts/duplicates samples and increments the
`drop`/`insert` counters without changing long-term latency (FIFO fill stays
within bounds). Include the counters in the engine header
(`engine_stats_t { uint32_t fifo_drops, fifo_inserts, dma_errors; }`) and
expose `engine_stats()`.

- [ ] **Step 2: Run it to verify it fails**

Run: `.venv/bin/pytest tests/test_dsp_host.py -q`
Expected: FAIL (no `engine_host_test.c` target yet)

- [ ] **Step 3: Implement drift + meters + commands**

- Drift: if the playback ring is empty, reuse the previous sample pair and
  `fifo_inserts++`; if it is full for >N blocks, advance the tail and
  `fifo_drops++`.
- Meters: peak/RMS per channel per block, logged at 1 Hz over CDC when enabled.
- CDC commands (one byte each): `g` cycle gain 0/−6/−12 dB, `t` testgen
  sine on/off, `w` white on/off, `i` impulse, `m` mute toggle, `s` stats dump.
  Keep the Task 4 `s`/`d` probe commands reachable via `p` + subcommand to
  avoid collisions.

- [ ] **Step 4: Run the tests**

Run: `.venv/bin/pytest tests/test_dsp_host.py -q`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "audio: drift compensation, meters and CDC control commands"
```

---

## Task 10: Measurement suite + stock baseline

**Files:**
- Create: `tools/measure.py`
- Create: `docs/MEASUREMENTS.md`
- Modify: `pyproject.toml` (add `numpy`, `scipy`, `sounddevice` to the `dev` extra)

- [ ] **Step 1: Write the measurement tool**

`tools/measure.py` (numpy/scipy/sounddevice):

- `--device NAME` selects the pedal's USB audio device (CoreAudio).
- `fr`: exponential sweep 20 Hz–20 kHz, deconvolve with the inverse sweep →
  frequency response in dB.
- `thd`: 1 kHz sine at −6 dBFS → FFT, THD+N in % and dB, fundamental level.
- `noise`: 2 s of silence → RMS, A-weighted (scipy.signal).
- `latency`: single impulse → cross-correlation between playback and capture →
  samples → ms.
- `run-all`: runs all four, writes a JSON + markdown table to
  `docs/MEASUREMENTS.md` (append a section per run with a timestamp and the
  firmware identity from `fb200 info`/the CDC banner).

- [ ] **Step 2: Measure the stock baseline**

Flash stock (`fb200 fw flash fb200-stock.mr --yes`), select a flat/clean
preset (document which), run `--run-all`, save the section. Note: stock runs at
44.1 kHz — record the device's nominal rate in the results.

- [ ] **Step 3: Measure our firmware**

Flash `fb200-audio.mr` (`--app-only`, A+D + `--no-jump`), run `--run-all`,
append the section. Include the analog loopback (mixer) results as a second
run with `--device "TrueMix600"` (or whichever device the mixer exposes) and
the pedal inline.

- [ ] **Step 4: Compare against the acceptance table**

Fill in a comparison table in `docs/MEASUREMENTS.md` with pass/fail per
acceptance criterion. If a target fails, open a follow-up task with the
measured numbers.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "tools: measurement suite and first stock-vs-audio results"
```

---

## Task 11: Docs, release and final review

**Files:**
- Modify: `README.md`, `CHANGELOG.md`, `docs/AUDIO_PATH.md`, `docs/HARDWARE.md` (cross-links)
- Modify: `pyproject.toml`, `src/fb200/__init__.py` (version 0.5.0)

- [ ] **Step 1: Update the docs**

- `README.md`: status table row for v0.5 (open firmware milestone 1: audio
  engine), link to `docs/AUDIO_PATH.md` and `docs/MEASUREMENTS.md`.
- `CHANGELOG.md`: `## [0.5.0]` with the audio engine, DSP framework, codec
  RE findings, measurement results, and the UAC2 status.
- `firmware/audio/README.md`: build, flash (A+D + `--no-jump`, `--app-only`),
  CDC commands, measurement workflow, recovery.

- [ ] **Step 2: Bump the version and run the full suite**

```bash
.venv/bin/pytest -q && .venv/bin/ruff check .
```
Expected: all green.

- [ ] **Step 3: Final review**

Use the `requesting-code-review` skill over the whole milestone diff; fix
findings; re-run tests.

- [ ] **Step 4: Tag and release**

```bash
git tag -a v0.5.0 -m "v0.5.0: open firmware audio engine"
git push origin v0.5.0
gh release create v0.5.0 --title "v0.5.0 — open firmware audio engine" --notes-file CHANGELOG.md
```

---

## Hardware checkpoints (user required)

Tasks 3, 4, 6, 7, 8, 10 need the pedal. Every flash follows the proven flow:
from stock → `--yes` (0xC1 jump), or from DFU (A+D) → `--yes --no-jump`.
Pre-flash gate for every image: dry-run through the Unicorn emulator (the
recipe in `docs/FIRMWARE_BRINGUP.md` §2) before touching hardware.
