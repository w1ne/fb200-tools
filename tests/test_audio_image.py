import re
import shutil
import subprocess
import sys
from pathlib import Path

import fwbuild
import pytest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
sys.path.insert(0, str(ROOT / "firmware" / "tools"))

pytestmark = pytest.mark.skipif(
    any(shutil.which(t) is None for t in ("arm-none-eabi-gcc", "make", "curl", "git", "python3")),
    reason="toolchain not installed",
)


def build() -> tuple[bytes, bytes]:
    out = fwbuild.build(FW)
    return (out / "fb200-app.vectors.bin").read_bytes(), (out / "fb200-app.blob.bin").read_bytes()


def symbols() -> dict[str, int]:
    text = (fwbuild.build(FW) / "layout.txt").read_text()
    return {n: int(a, 16) for a, n in re.findall(r"^([0-9a-f]{8}) \S+ (\S+)$", text, re.MULTILINE)}


def test_vectors_and_entry():
    vectors, blob = build()
    assert len(vectors) == 0x400
    assert int.from_bytes(vectors[0:4], "little") == 0x20058000
    assert int.from_bytes(vectors[4:8], "little") == 0x600104D9
    syms = symbols()
    assert syms["stage2"] == 0x4D6
    assert len(blob) == syms["__blob_end__"] - 0x400
    assert len(blob) <= 0x1DBC8          # vendor entry 0 payload limit


def test_data_blob_is_ocramdata_then_dtcmdata():
    """The slot data blob (flash 0x60041000) holds the .ocramdata tables, then
    the .dtcmdata tables; stage2_main copies each from its load address."""
    build()
    syms = symbols()
    ocram = syms["__ocramdata_end__"] - syms["__ocramdata_start__"]
    dtcm = syms["__dtcmdata_end__"] - syms["__dtcmdata_start__"]
    assert syms["__ocramdata_load__"] == 0x60041000
    assert syms["__dtcmdata_load__"] == 0x60041000 + ocram
    assert syms["__dtcmdata_start__"] == 0x20018B44     # the startup probes this word
    data = (fwbuild.build(FW) / "fb200-app.dtcmdata.bin").read_bytes()
    assert len(data) == ocram + dtcm <= 0x20000         # SLOT_DATA_MAX


def elf_symbols(variant: str = "app") -> dict[str, int]:
    elf = fwbuild.build(FW, variant) / f"fb200-{variant}.elf"
    out = subprocess.run(["arm-none-eabi-nm", str(elf)], check=True, capture_output=True,
                         text=True).stdout
    return {n: int(a, 16) for a, _t, n in re.findall(r"^([0-9a-f]{8}) (\S) (\S+)$", out, re.MULTILINE)}


def elf_sizes(variant: str = "app") -> dict[str, tuple[int, int]]:
    """name -> (address, size) of every sized symbol."""
    elf = fwbuild.build(FW, variant) / f"fb200-{variant}.elf"
    out = subprocess.run(["arm-none-eabi-nm", "-S", str(elf)], check=True, capture_output=True,
                         text=True).stdout
    return {n: (int(a, 16), int(z, 16))
            for a, z, n in re.findall(r"^([0-9a-f]{8}) ([0-9a-f]{8}) \S (\S+)$", out, re.MULTILINE)}


ITCM = range(0x400, 0x1F000)


@pytest.mark.parametrize("variant", ["app", "recovery"])
def test_all_code_in_itcm(variant):
    """Every function of both images runs from ITCM (main before this fix ran
    the app's cold code from OCRAM at 0x20210000, which the pedal does not
    have). Spot checks too, so a symbol that vanishes fails here."""
    out = fwbuild.build(FW, variant)
    nm = subprocess.run(["arm-none-eabi-nm", str(out / f"fb200-{variant}.elf")], check=True,
                        capture_output=True, text=True).stdout
    code = re.findall(r"^([0-9a-f]{8}) [tTwW] (\S+)$", nm, re.MULTILINE)
    assert code and [n for a, n in code if int(a, 16) >= 0x20000] == []
    if variant == "app":
        syms = elf_symbols()
        for f in ("stage2_main", "app_main", "engine_task", "usb_audio_task", "console_task",
                  "ui_task", "proto_feed", "log_printf", "CLOCK_InitArmPll"):
            assert syms[f] in ITCM, f


def test_ocram_holds_the_budgeted_buffers():
    """OCRAM (32 kB): the user IR staging for 512 taps, the EQ and the delay
    line, nothing else; no long-IR tail and no 512-point FFT tables (long IRs
    off, engine.c ENGINE_IR_TAPS). The delay line holds DELAY_MS_MAX at 44.1 kHz."""
    syms, sizes = elf_symbols(), elf_sizes()
    inside = {n: z for n, (a, z) in sizes.items() if 0x20200000 <= a < 0x20300000}
    assert set(inside) == {"s_ir", "s_eq", "s_dly_line"}, inside
    assert inside["s_ir"] == 512 * 4
    delay_h = (FW / "src" / "dsp" / "delay.h").read_text()
    ms = int(re.search(r"#define DELAY_MS_MAX (\d+)", delay_h).group(1))
    assert inside["s_dly_line"] == 2 * (44100 * ms // 1000 + 4)
    assert "s_cab_tail" not in syms and "twiddleCoef_rfft_512" not in syms
    assert syms["__ocramdata_start__"] == syms["__ocramdata_end__"]
    assert 0x20200000 <= syms["__ocram_start__"] and syms["__ocram_end__"] <= 0x20208000


def test_nothing_hot_runs_outside_itcm():
    """Call graph from every ISR, engine_task, usb_audio_task, the USB audio
    class driver and every driver callback: all of it in ITCM, and none of it
    loads cold const data (firmware/tools/hot_path.py; trivially true while
    all code is in ITCM, the check for moving cold code out)."""
    import hot_path
    bad = hot_path.check(fwbuild.build(FW) / "fb200-app.elf")
    assert bad == [], "\n".join(bad)


def test_hot_path_check_catches_cold_audio_code():
    """Negative control: an audio function (gain_process, called by
    engine_task) and an ISR callback (rgb.c's eDMA `done`) taken as outside
    ITCM must fail the check."""
    import hot_path
    bad = "\n".join(hot_path.check(fwbuild.build(FW) / "fb200-app.elf",
                                   pretend_cold=frozenset({"gain_process", "done"})))
    assert "gain_process" in bad and "engine_task" in bad
    assert "done @" in bad


def test_bss_uses_the_stock_memset_region():
    build()
    syms = symbols()
    assert 0x20018B44 <= syms["__bss_start__"] <= 0x2004E3E8   # .bss may be aligned up
    assert syms["__bss_end__"] <= 0x2004E3E8
    assert syms["_estack"] == 0x20058000


# The memory that exists on the pedal (i.MX RT1052, measured on hardware:
# IOMUXC_GPR17 = 0xffaaaaa9, GPR16 = 0x00200007; docs/FIRMWARE_BRINGUP.md,
# "Memory map"). Above OCRAM_END there is nothing: writes are dropped, reads
# return 0, no fault. The ELF must not place anything there.
REAL_MEMORY = {
    "ITCM": range(0x00020000),                # 4 banks, 128 kB
    "DTCM": range(0x20000000, 0x20058000),    # 11 banks, 352 kB
    "OCRAM": range(0x20200000, 0x20208000),   # 1 bank, 32 kB
    "flash": range(0x60000000, 0x60800000),   # FlexSPI XIP (load addresses)
}


def sections(elf: Path) -> list[tuple[str, int, int, int, list[str]]]:
    """(name, size, vma, lma, flags) of every section, from objdump -h."""
    out = subprocess.run(["arm-none-eabi-objdump", "-h", "-w", str(elf)], check=True,
                         capture_output=True, text=True).stdout
    rows = re.findall(r"^\s*\d+ (\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+"
                      r"[0-9a-f]{8}\s+\S+\s+(.*)$", out, re.MULTILINE)
    return [(n, int(s, 16), int(v, 16), int(lma, 16), [f.strip() for f in fl.split(",")])
            for n, s, v, lma, fl in rows]


def outside_real_memory(elf: Path) -> list[str]:
    """Allocated sections (run address) and loaded sections (load address)
    that do not lie inside one real memory region."""
    def inside(start: int, size: int) -> bool:
        return any(start in r and start + size <= r.stop for r in REAL_MEMORY.values())
    bad = []
    for name, size, vma, lma, flags in sections(elf):
        if "ALLOC" not in flags or size == 0:
            continue
        if not inside(vma, size):
            bad.append(f"{name}: run address {vma:#010x}..{vma + size:#010x}")
        if "LOAD" in flags and not inside(lma, size):
            bad.append(f"{name}: load address {lma:#010x}..{lma + size:#010x}")
    return bad


@pytest.mark.parametrize("variant", ["app", "recovery"])
def test_every_section_is_in_real_memory(variant):
    """Nothing in memory that the pedal does not have: main before the fix
    put .ocramtext, .ocramdata and .ocram at 0x20210000.. (an RT1062 layout),
    and the app crashed at boot."""
    elf = fwbuild.build(FW, variant) / f"fb200-{variant}.elf"
    assert sections(elf), "objdump output not parsed"
    bad = outside_real_memory(elf)
    assert bad == [], "\n".join(bad)


def test_no_undefined_symbols():
    build()
    undef = subprocess.run(
        ["arm-none-eabi-nm", "-u", str(fwbuild.build(FW) / "fb200-app.elf")],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    assert undef == ""


def dry_run(tmp_path, data: bool) -> str:
    """Vendor loader -> recovery -> copier -> app stage2 -> app_main, emulated
    with the pedal's memory map (firmware/tools/boot_dry_run.py)."""
    pytest.importorskip("unicorn")
    from stock_emu import find_stock_mr
    stock = find_stock_mr()
    if stock is None:
        pytest.skip("fb200-stock.mr not found (set FB200_STOCK_MR)")
    from fb200 import images
    rec_dir, app_dir = fwbuild.build(FW, "recovery"), fwbuild.build(FW)
    part = lambda d, v, k: (d / f"fb200-{v}.{k}.bin").read_bytes()
    rec = images.build_recovery(part(rec_dir, "recovery", "vectors"),
                                part(rec_dir, "recovery", "blob"),
                                part(rec_dir, "recovery", "copier"))
    slot = images.build_slot(part(app_dir, "app", "vectors"), part(app_dir, "app", "blob"),
                             part(app_dir, "app", "dtcmdata") if data else b"")
    (tmp_path / "app.slot").write_bytes(slot)
    (tmp_path / "twostage.mr").write_bytes(images.twostage_mr(stock.read_bytes(), rec, slot))
    return subprocess.run(
        [sys.executable, str(ROOT / "firmware" / "tools" / "boot_dry_run.py"),
         "--elf", str(rec_dir / "fb200-recovery.elf"), "--app-elf", str(app_dir / "fb200-app.elf"),
         "--app-slot", str(tmp_path / "app.slot"), "--max-instructions", "40000000",
         str(tmp_path / "twostage.mr")], capture_output=True, text=True, check=False).stdout


def test_boot_reaches_app_main(tmp_path):
    out = dry_run(tmp_path, data=True)
    assert "reached app app_main: True" in out and "DRY RUN OK" in out, out


def test_slot_without_its_data_goes_back_to_recovery(tmp_path):
    """A v2 slot with no data passes recovery's check (CRC of 0 bytes), but
    its tables would be garbage: stage2_main must reset into recovery with
    a fault crumb (recovery then stays on the console) instead of running it."""
    out = dry_run(tmp_path, data=False)
    assert "reached app app_main: False" in out and "software reset requested" in out, out
    need = len((fwbuild.build(FW) / "fb200-app.dtcmdata.bin").read_bytes())
    assert f"crumbs: fa000000 00000000 {need:08x} 60041000" in out, out
