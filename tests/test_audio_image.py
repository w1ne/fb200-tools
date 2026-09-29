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
    # The app is loaded by recovery's copier, which runs at ITCM 0x1F000
    # (recovery.c slot_valid: APP_ITCM_LIMIT); the vendor loader's 0x1DBC8
    # payload limit binds recovery only.
    assert len(blob) <= 0x1F000 - 0x400


def test_recovery_fits_the_vendor_loader():
    out = fwbuild.build(FW, "recovery")
    assert len((out / "fb200-recovery.blob.bin").read_bytes()) <= 0x1DBC8   # entry 0 payload


def test_data_blob_is_xiptext_ocramdata_dtcmdata():
    """The slot data blob (flash 0x60041000) holds the cold code (.xiptext,
    run in place: VMA = LMA), then the .ocramdata and .dtcmdata tables that
    stage2_main copies from their load addresses."""
    build()
    syms = symbols()
    text = syms["__xiptext_end__"] - syms["__xiptext_start__"]
    ocram = syms["__ocramdata_end__"] - syms["__ocramdata_start__"]
    dtcm = syms["__dtcmdata_end__"] - syms["__dtcmdata_start__"]
    assert syms["__xiptext_start__"] == syms["__xiptext_load__"] == 0x60041000
    assert syms["__ocramdata_load__"] == 0x60041000 + text
    assert syms["__dtcmdata_load__"] == 0x60041000 + text + ocram
    assert syms["__dtcmdata_start__"] == 0x20018B44     # the startup probes this word
    assert text % 32 == 0                               # whole cache lines
    data = (fwbuild.build(FW) / "fb200-app.dtcmdata.bin").read_bytes()
    assert len(data) == text + ocram + dtcm <= 0x20000  # SLOT_DATA_MAX
    assert text > 0x8000                                # the cold code is there


def test_ram_map_is_the_pedals():
    """Every RAM section inside the FlexRAM split the pedal has (GPR17 =
    0xFFAAAAA9: ITCM 128 kB, DTCM 0x20000000..0x20057FFF, OCRAM
    0x20200000..0x20207FFF), the low DTCM below the crash dump, and 8 kB
    kept for the stack (measured high-water: under 512 B)."""
    build()
    syms = symbols()
    assert syms["__itcm_bss_start__"] >= syms["__blob_end__"]
    assert syms["__itcm_bss_end__"] <= 0x20000
    assert 0x20000000 <= syms["__dtcm_lo_start__"] and syms["__dtcm_lo_end__"] <= 0x20018A00
    assert syms["__dtcm_hi_start__"] >= syms["__bss_end__"]
    assert syms["__dtcm_hi_end__"] <= syms["__stack_limit__"] == syms["_estack"] - 0x2000
    assert 0x20200000 <= syms["__ocramdata_start__"] and syms["__ocram_end__"] <= 0x20208000
    assert syms["__ocram_start__"] >= syms["__ocramdata_end__"]


def test_big_buffers_placement():
    """The budget (docs/FIRMWARE_BRINGUP.md, "Memory map"): the long-IR tail
    for ENGINE_IR_TAPS in the low DTCM, the delay line for DELAY_MS_MAX at
    44.1 kHz in the DTCM above .bss, the reverb in the ITCM above the code;
    OCRAM holds the user IR staging, the cab, the EQ and nothing else."""
    syms, sizes = elf_symbols(), elf_sizes()
    engine_h = (FW / "src" / "audio" / "engine.h").read_text()
    taps = int(re.search(r"#define ENGINE_IR_TAPS (\d+)", engine_h).group(1))
    delay_h = (FW / "src" / "dsp" / "delay.h").read_text()
    ms = int(re.search(r"#define DELAY_MS_MAX (\d+)", delay_h).group(1))
    assert taps == 4096 and ms == 1000
    assert 0x20000000 <= syms["s_cab_tail"] < 0x20018A00
    assert syms["__dtcm_hi_start__"] <= syms["s_dly_line"] < syms["__dtcm_hi_end__"]
    assert sizes["s_dly_line"][1] == 2 * (44100 * ms // 1000 + 4)
    assert syms["__itcm_bss_start__"] <= syms["s_rev"] < syms["__itcm_bss_end__"]
    inside = {n: z for n, (a, z) in sizes.items() if 0x20200000 <= a < 0x20300000}
    tables = {n for n in inside if syms["__ocramdata_start__"] <= sizes[n][0] < syms["__ocramdata_end__"]}
    assert set(inside) - tables == {"s_ir", "s_eq", "s_cab"}, inside
    assert inside["s_ir"] == taps * 4
    assert "twiddleCoef_rfft_512" in tables


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
XIP = range(0x60041000, 0x60061000)


def test_hot_and_cold_placement():
    """Hot: ITCM; cold: flash, XIP (Makefile COLD_SRC). Spot checks both
    ways, so a placement rule that silently stops matching fails here. The
    flash write path and what it runs during an app update are RAM too."""
    syms = elf_symbols()
    for hot in ("stage2_main", "app_main", "engine_task", "usb_audio_task", "fault_record",
                "Default_Handler", "wdog_feed", "dcd_int_handler", "EDMA_HandleIRQ",
                "memcpy", "crc32_ieee",
                "fw_begin", "fw_session", "flash_store", "FLEXSPI_TransferBlocking",
                "flash_read_id", "flash_capacity",
                "log_printf", "tud_descriptor_configuration_cb", "cdcd_xfer_cb"):
        assert syms[hot] in ITCM, hot
    for cold in ("console_task", "ui_task", "display_task", "proto_feed", "preset_write",
                 "CLOCK_InitArmPll", "irstore_put_task", "irstore_load"):
        assert syms[cold] in XIP, cold


def test_nothing_hot_runs_from_flash():
    """Call graph from every ISR, engine_task, usb_audio_task, the USB audio
    class driver and every driver callback: all of it in ITCM, and none of it
    loads cold const data. Call graph from the flash write path and the USB
    class drivers: none of it in flash (firmware/tools/hot_path.py)."""
    import hot_path
    bad = hot_path.check(fwbuild.build(FW) / "fb200-app.elf")
    assert bad == [], "\n".join(bad)


def test_hot_path_check_catches_cold_audio_code(tmp_path):
    """Negative control: an audio file (gain.c, called by engine_task) and an
    ISR callback file (rgb.c) moved to flash (COLD_SRC) must fail the check."""
    import hot_path
    out = tmp_path / "build"
    shutil.copytree(fwbuild.build(FW), out)             # relink only: objects are current
    (out / "fb200-app" / "cold.ld").unlink()
    (out / "fb200-app.elf").unlink()   # make 3.81 (macOS) compares whole seconds: force the relink
    subprocess.run(["make", f"BUILD={out}", "build",
                    "COLD_EXTRA=src/dsp/gain.c src/ui/rgb.c"],
                   cwd=FW, check=True, capture_output=True)
    bad = "\n".join(hot_path.check(out / "fb200-app.elf"))
    assert "gain_process" in bad and "engine_task" in bad
    assert "done @" in bad                               # rgb.c's eDMA callback


def test_hot_path_check_catches_cold_flash_writes(tmp_path):
    """Negative control: the flash write path (selfupdate.c) or the CDC class
    driver in flash must fail the flash-write check, and so must the HID
    report callback without its fw_xip_gone() gate."""
    import hot_path
    out = tmp_path / "build"
    shutil.copytree(fwbuild.build(FW), out)
    (out / "fb200-app" / "cold.ld").unlink()
    (out / "fb200-app.elf").unlink()   # make 3.81 (macOS) compares whole seconds: force the relink
    cdc = ".deps/tinyusb/src/class/cdc/cdc_device.c"
    subprocess.run(["make", f"BUILD={out}", "build",
                    f"COLD_EXTRA=src/debug/selfupdate.c {cdc}"],
                   cwd=FW, check=True, capture_output=True)
    bad = "\n".join(hot_path.check(out / "fb200-app.elf"))
    assert "flash write: fw_begin @" in bad and "flash write: flash_store @" in bad, bad
    assert "flash write: cdcd_xfer_cb @" in bad, bad
    # the long IR store's path: flash_store -> flash_capacity -> flash_read_id
    assert "flash write: flash_capacity @" in bad and "flash write: flash_read_id @" in bad, bad
    gates = dict(hot_path.GATED)
    try:
        hot_path.GATED[("tud_hid_set_report_cb", "proto_feed")] = "no_such_gate"
        bad = "\n".join(hot_path.check(fwbuild.build(FW) / "fb200-app.elf"))
    finally:
        hot_path.GATED.clear()
        hot_path.GATED.update(gates)
    assert "gate no_such_gate() not called" in bad and "flash write: proto_feed @" in bad, bad


def test_recovery_has_no_xip_code():
    """Recovery never uses a data blob: all its code must be in ITCM."""
    out = fwbuild.build(FW, "recovery")
    syms = elf_symbols("recovery")
    assert syms["__xiptext_start__"] == syms["__xiptext_end__"]
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
    """Vendor loader -> recovery -> copier -> app stage2 -> app_main -> XIP
    code, emulated with the pedal's RAM map (firmware/tools/boot_dry_run.py)."""
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


def test_boot_runs_xip_code(tmp_path):
    out = dry_run(tmp_path, data=True)
    assert "reached app XIP code: True" in out and "reached back in ITCM: True" in out, out
    assert "app .dtcmdata copied: True" in out and "app .ocramdata copied: True" in out, out
    assert "DRY RUN OK" in out, out


def test_slot_without_its_data_goes_back_to_recovery(tmp_path):
    """A v2 slot with no data passes recovery's check (CRC of 0 bytes), but
    its tables would be garbage: stage2_main must reset into recovery with
    a fault crumb (recovery then stays on the console) instead of running it."""
    out = dry_run(tmp_path, data=False)
    assert "reached app app_main: False" in out and "software reset requested" in out, out
    need = len((fwbuild.build(FW) / "fb200-app.dtcmdata.bin").read_bytes())
    assert f"crumbs: fa000000 00000000 {need:08x} 60041000" in out, out
