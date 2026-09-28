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


def test_data_blob_is_ocramtext_ocramdata_dtcmdata():
    """The slot data blob (flash 0x60041000) holds the cold code (.ocramtext),
    the .ocramdata tables, then the .dtcmdata tables; stage2_main copies each
    from its load address."""
    build()
    syms = symbols()
    text = syms["__ocramtext_end__"] - syms["__ocramtext_start__"]
    ocram = syms["__ocramdata_end__"] - syms["__ocramdata_start__"]
    dtcm = syms["__dtcmdata_end__"] - syms["__dtcmdata_start__"]
    assert syms["__ocramtext_load__"] == 0x60041000
    assert syms["__ocramdata_load__"] == 0x60041000 + text
    assert syms["__dtcmdata_load__"] == 0x60041000 + text + ocram
    assert syms["__dtcmdata_start__"] == 0x20018B44     # the startup probes this word
    assert syms["__ocramtext_start__"] == 0x20210000    # OCRAM2, above the vendor data
    assert syms["__ocramtext_start__"] % 32 == 0 and text % 32 == 0   # whole cache lines
    assert syms["__ocramdata_start__"] == syms["__ocramtext_end__"]
    data = (fwbuild.build(FW) / "fb200-app.dtcmdata.bin").read_bytes()
    assert len(data) == text + ocram + dtcm <= 0x20000  # SLOT_DATA_MAX
    assert text > 0x8000                                # the cold code is there


def elf_symbols(variant: str = "app") -> dict[str, int]:
    elf = fwbuild.build(FW, variant) / f"fb200-{variant}.elf"
    out = subprocess.run(["arm-none-eabi-nm", str(elf)], check=True, capture_output=True,
                         text=True).stdout
    return {n: int(a, 16) for a, _t, n in re.findall(r"^([0-9a-f]{8}) (\S) (\S+)$", out, re.MULTILINE)}


ITCM = range(0x400, 0x1F000)
OCRAM = range(0x20210000, 0x20280000)


def test_hot_and_cold_placement():
    """Hot: ITCM; cold: OCRAM (Makefile COLD_SRC). Spot checks both ways, so
    a placement rule that silently stops matching fails here."""
    syms = elf_symbols()
    for hot in ("stage2_main", "app_main", "engine_task", "usb_audio_task", "fault_record",
                "Default_Handler", "wdog_feed", "dcd_int_handler", "EDMA_HandleIRQ",
                "memcpy", "crc32_ieee"):
        assert syms[hot] in ITCM, hot
    for cold in ("console_task", "ui_task", "display_task", "proto_feed", "log_printf",
                 "fw_begin", "tud_descriptor_configuration_cb", "CLOCK_InitArmPll"):
        assert syms[cold] in OCRAM, cold


def test_nothing_hot_runs_from_ocram():
    """Call graph from every ISR, engine_task, usb_audio_task, the USB audio
    class driver and every driver callback: all of it in ITCM, and none of it
    loads cold const data (firmware/tools/hot_path.py)."""
    import hot_path
    bad = hot_path.check(fwbuild.build(FW) / "fb200-app.elf")
    assert bad == [], "\n".join(bad)


def test_hot_path_check_catches_cold_audio_code(tmp_path):
    """Negative control: an audio file (gain.c, called by engine_task) and an
    ISR callback file (rgb.c) moved to OCRAM must fail the check."""
    import hot_path
    out = tmp_path / "build"
    shutil.copytree(fwbuild.build(FW), out)             # relink only: objects are current
    (out / "fb200-app" / "cold.ld").unlink()
    subprocess.run(["make", f"BUILD={out}", "build",
                    "COLD_SRC=src/debug/console.c src/dsp/gain.c src/ui/rgb.c"],
                   cwd=FW, check=True, capture_output=True)
    bad = "\n".join(hot_path.check(out / "fb200-app.elf"))
    assert "gain_process" in bad and "engine_task" in bad
    assert "done @" in bad                               # rgb.c's eDMA callback


def test_recovery_has_no_ocram_code():
    """Recovery never copies a data blob: all its code must be in ITCM."""
    out = fwbuild.build(FW, "recovery")
    syms = elf_symbols("recovery")
    assert syms["__ocramtext_start__"] == syms["__ocramtext_end__"]
    assert (out / "fb200-recovery.dtcmdata.bin").read_bytes() == b""
    nm = subprocess.run(["arm-none-eabi-nm", str(out / "fb200-recovery.elf")], check=True,
                        capture_output=True, text=True).stdout
    code = re.findall(r"^([0-9a-f]{8}) [tTwW] (\S+)$", nm, re.MULTILINE)
    assert code and all(int(a, 16) < 0x20000 for a, _n in code)


def test_bss_uses_the_stock_memset_region():
    build()
    syms = symbols()
    assert 0x20018B44 <= syms["__bss_start__"] <= 0x2004E3E8   # .bss may be aligned up
    assert syms["__bss_end__"] <= 0x2004E3E8
    assert syms["_estack"] == 0x20058000


def test_no_undefined_symbols():
    build()
    undef = subprocess.run(
        ["arm-none-eabi-nm", "-u", str(fwbuild.build(FW) / "fb200-app.elf")],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    assert undef == ""


def dry_run(tmp_path, data: bool) -> str:
    """Vendor loader -> recovery -> copier -> app stage2 -> app_main -> OCRAM
    code, emulated (firmware/tools/boot_dry_run.py)."""
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


def test_boot_runs_ocram_code(tmp_path):
    out = dry_run(tmp_path, data=True)
    assert "app .ocramtext copied: True" in out and "reached back in ITCM: True" in out, out
    assert "DRY RUN OK" in out, out


def test_slot_without_its_data_goes_back_to_recovery(tmp_path):
    """A v2 slot with no data passes recovery's check (CRC of 0 bytes), but
    its cold code would be garbage: stage2_main must reset into recovery with
    a fault crumb (recovery then stays on the console) instead of running it."""
    out = dry_run(tmp_path, data=False)
    assert "reached app app_main: False" in out and "software reset requested" in out, out
    need = len((fwbuild.build(FW) / "fb200-app.dtcmdata.bin").read_bytes())
    assert f"crumbs: fa000000 00000000 {need:08x} 60041000" in out, out
