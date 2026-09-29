"""fb200 command line interface."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from contextlib import contextmanager
from pathlib import Path

from fb200 import protocol
from fb200.errors import Fb200Error, FirmwareError, InvalidArgumentError
from fb200.pedal import IR_SLOT_COUNT, FB200Device
from fb200.protocol import write_frame
from fb200.transport import HidapiTransport


def _open_device() -> FB200Device:
    transport = HidapiTransport().open()
    return FB200Device(transport)


@contextmanager
def _with_device():
    device = _open_device()
    try:
        yield device
    finally:
        device.transport.close()


def _slot_arg(value: str) -> int:
    try:
        slot = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid slot: {value!r}") from exc
    if not 1 <= slot <= IR_SLOT_COUNT:
        raise argparse.ArgumentTypeError(f"slot must be 1..{IR_SLOT_COUNT}")
    return slot


def _delete_slot_arg(value: str) -> int:
    """1..9 (stock user slots, HID) or 20..83 (long IR store, console)."""
    from fb200 import longir

    try:
        slot = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid slot: {value!r}") from exc
    if not (1 <= slot <= IR_SLOT_COUNT or longir.FIRST <= slot <= longir.LAST):
        raise argparse.ArgumentTypeError(
            f"slot must be 1..{IR_SLOT_COUNT} or {longir.FIRST}..{longir.LAST} (long IRs)")
    return slot


def _long_slot_arg(value: str) -> int:
    from fb200 import longir

    try:
        slot = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid slot: {value!r}") from exc
    if not longir.FIRST <= slot <= longir.LAST:
        raise argparse.ArgumentTypeError(f"long IR slot must be {longir.FIRST}..{longir.LAST}")
    return slot


def _hex_frame_arg(value: str) -> str:
    text = value.replace(" ", "")
    try:
        bytes.fromhex(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid hex frame: {value!r}") from exc
    return text


def _cmd_info(args) -> int:
    with _with_device() as device:
        info = device.info()
    print(f"Product          : {info.product}")
    print(f"App version      : {info.app_version}")
    print(f"Firmware version : {info.firmware_version}")
    print(f"Bluetooth version: {info.bluetooth_version}")
    print(f"Hardware rev     : {info.hardware_rev}")
    return 0


def _cmd_ir_list(args) -> int:
    if args.long:
        from fb200 import console, longir

        with console.Console(args.port) as con:
            res = longir.ls(con)
        if not res["available"]:
            print(res["text"], file=sys.stderr)
            return 1
        for e in res["slots"]:
            print(f"{e.slot}: {e.name}  {e.taps} taps  gain {e.gain:.4f}  source {e.rate} Hz"
                  + ("" if e.ok else "  BAD DATA (plays as bypass)"))
        print(f"{len(res['slots'])} of {longir.SLOTS} long slots used "
              f"(cab types {longir.FIRST}..{longir.LAST})", file=sys.stderr)
        return 0
    with _with_device() as device:
        for slot in device.ir_list():
            print(f"{slot.index}: {slot.name if slot.name else '(empty)'}")
    return 0


def _cmd_ir_put(args) -> int:
    from fb200 import console, longir

    samples, rate = longir.load_wav(args.wav, **_ir_options(args))
    name = longir.sanitize_name(args.name or Path(args.wav).stem)
    with console.Console(args.port) as con:
        res = longir.put(con, args.slot, samples, name, rate)
    print(f"stored '{res.name}' in long slot {res.slot}: {res.taps} taps, gain {res.gain:.4f} "
          f"(select it with cab type {res.slot})", file=sys.stderr)
    return 0


def _cmd_ir_delete(args) -> int:
    from fb200 import longir

    if args.slot >= longir.FIRST:
        from fb200 import console

        with console.Console(args.port) as con:
            held = longir.delete(con, args.slot)
        print(f"deleted long slot {args.slot}" if held else f"long slot {args.slot} was empty")
        return 0
    with _with_device() as device:
        deleted = device.ir_delete(args.slot)
    if not deleted:
        print(f"failed to delete slot {args.slot}", file=sys.stderr)
        return 1
    print(f"deleted slot {args.slot}")
    return 0


def _blend_arg(value: str) -> tuple[str, float]:
    path, sep, mix = value.rpartition(":")
    try:
        weight = float(mix)
    except ValueError:
        weight = -1.0
    if not sep or not path or not 0.0 <= weight <= 1.0:
        raise argparse.ArgumentTypeError(f"expected FILE:MIX with MIX 0..1, got {value!r}")
    return path, weight


def _add_ir_process_args(p: argparse.ArgumentParser) -> None:
    from fb200.wav import CHANNELS, MAX_TAPS

    g = p.add_argument_group("processing (defaults: channel 0, 44.1 kHz, 1024 samples, as stock)")
    g.add_argument("--channel", choices=CHANNELS, default="left",
                   help="left (channel 0), right, or sum (mean of channels)")
    g.add_argument("--trim", action="store_true",
                   help="cut silence before the onset (-60 dB rel. peak, 8 samples pre-roll)")
    g.add_argument("--taps", type=int, metavar="N",
                   help=f"truncate to N taps (1..{MAX_TAPS}) with a half-Hann fade-out; "
                        "stock slots play 512; `ir put` default 4096 (trailing zeros dropped)")
    g.add_argument("--lowcut", type=float, metavar="HZ", help="2nd-order Butterworth high pass")
    g.add_argument("--highcut", type=float, metavar="HZ", help="2nd-order Butterworth low pass")
    g.add_argument("--blend", type=_blend_arg, metavar="FILE:MIX",
                   help="mix a second IR in (aligned by onset), e.g. other.wav:0.3")
    g.add_argument("--minphase", action="store_true",
                   help="cepstral minimum phase (needs numpy: pip install 'fb200-tools[ir]')")
    g.add_argument("--normalize", action="store_true", help="scale the peak to 1.0")


def _ir_options(args) -> dict:
    """The `fb200.wav.process_ir` options of `ir import` / `ir process`."""
    return {"channel": args.channel, "taps": args.taps, "trim": args.trim,
            "lowcut": args.lowcut, "highcut": args.highcut, "blend": args.blend,
            "minphase": args.minphase, "normalize": args.normalize}


def _cmd_ir_process(args) -> int:
    from fb200.wav import process_ir, write_wav

    samples = process_ir(args.wav, **_ir_options(args))
    write_wav(args.out, samples)
    print(f"wrote {args.out} ({len(samples)} taps, 44.1 kHz float)", file=sys.stderr)
    return 0


def _cmd_ir_import(args) -> int:
    from fb200.wav import IR_LENGTH

    if args.taps is not None and args.taps > IR_LENGTH:
        raise InvalidArgumentError(f"a pedal slot holds {IR_LENGTH} taps; use --taps <= {IR_LENGTH}")

    def progress(done: int, total: int) -> None:
        print(f"\rframe {done}/{total}", end="", file=sys.stderr)

    with _with_device() as device:
        name = device.import_wav(args.slot, args.wav, args.name, progress=progress,
                                 **_ir_options(args))
    print(f"\nimported '{name}' into slot {args.slot}", file=sys.stderr)
    return 0


def _cmd_ir_backup(args) -> int:
    with _with_device() as device:
        info = device.info()
        slots = device.ir_list()
    out_dir = Path(args.directory)
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "device": info.product,
        "firmware_version": info.firmware_version,
        "exported_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "note": "IR payloads cannot be downloaded from the device; only names are backed up.",
        "slots": [{"index": s.index, "name": s.name} for s in slots],
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(f"wrote {out_dir / 'manifest.json'}")
    return 0


def _cmd_probe(args) -> int:
    import time

    with _with_device() as device:
        if args.send:
            write_frame(device.transport, bytes.fromhex(args.send))
        if args.listen:
            deadline = time.monotonic() + args.listen
            while time.monotonic() < deadline:
                report = device.transport.read_report(200)
                if report:
                    print(report.hex(" "))
    return 0


def _cmd_fw_inspect(args) -> int:
    from fb200.firmware import MrFile

    mr = MrFile.from_path(args.file)
    if args.json:
        payload = {
            "header": {
                "product_tag": mr.header.product_tag,
                "send_cmd": mr.header.send_cmd,
                "rec_cmd": mr.header.rec_cmd,
                "timeout": mr.header.timeout,
                "update_block": mr.header.update_block,
                "update_addr": mr.header.update_addr.hex(),
                "version": mr.header.version,
            },
            "blocks": [
                {
                    "index": i,
                    "start_addr": b.tag.start_addr,
                    "stop_addr": b.tag.stop_addr,
                    "start_page": b.tag.start_page,
                    "send_cmd": b.tag.send_cmd,
                    "rec_cmd": b.tag.rec_cmd,
                    "rom_id": b.tag.rom_id,
                    "size": len(b.data),
                }
                for i, b in enumerate(mr.blocks)
            ],
        }
        print(json.dumps(payload, indent=2))
        return 0
    print(f"Product tag : {mr.header.product_tag}")
    print(f"Blocks      : {mr.header.update_block}")
    print(f"Data size   : {mr.total_data_size} bytes")
    for i, b in enumerate(mr.blocks):
        print(f"  block {i}: {len(b.data)} bytes  start_page=0x{b.tag.start_page:x} "
              f"send_cmd=0x{b.tag.send_cmd:02x} rom_id={b.tag.rom_id}")
    if args.strings:
        for hit in mr.find_strings():
            if args.filter and args.filter not in hit.text:
                continue
            print(f"  [{hit.block}:0x{hit.offset:06x}] {hit.text}")
    return 0


def _cmd_fw_extract_block(args) -> int:
    from fb200.firmware import MrFile

    mr = MrFile.from_path(args.file)
    if not 0 <= args.index < len(mr.blocks):
        raise InvalidArgumentError(f"block index must be 0..{len(mr.blocks) - 1}")
    try:
        Path(args.output).write_bytes(mr.blocks[args.index].data)
    except OSError as exc:
        raise FirmwareError(f"cannot write {args.output}: {exc}") from exc
    print(f"wrote {args.output} ({len(mr.blocks[args.index].data)} bytes)")
    return 0


def _cmd_fw_patch_string(args) -> int:
    from fb200.firmware import MrFile

    mr = MrFile.from_path(args.file)
    count = mr.patch_string(args.find, args.replace)
    out = Path(args.output) if args.output else Path(args.file).with_suffix(".patched.mr")
    try:
        out.write_bytes(mr.to_bytes())
    except OSError as exc:
        raise FirmwareError(f"cannot write {out}: {exc}") from exc
    print(f"patched {count} occurrence(s); wrote {out}")
    return 0


def _cmd_fw_pack(args) -> int:
    from fb200.firmware import MrFile, pack_app_image

    template = MrFile.from_path(args.template)
    try:
        app = Path(args.app).read_bytes()
    except OSError as exc:
        raise FirmwareError(f"cannot read {args.app}: {exc}") from exc
    packed = pack_app_image(template, app)
    out = Path(args.output) if args.output else Path(args.app).with_suffix(".mr")
    inputs = {Path(args.template).resolve(), Path(args.app).resolve()}
    if out.resolve() in inputs:
        raise FirmwareError(f"refusing to overwrite input file {out}")
    try:
        out.write_bytes(packed.to_bytes())
    except OSError as exc:
        raise FirmwareError(f"cannot write {out}: {exc}") from exc
    print(
        f"packed {len(app)} bytes into {out} "
        f"({len(packed.blocks[0].data)}-byte block 0)"
    )
    return 0


def _cmd_fw_flash(args) -> int:
    import time

    from fb200 import updater as updater_module
    from fb200.firmware import MrFile
    from fb200.protocol import UPDATE_PID, UPDATE_VID

    mr = MrFile.from_path(args.file)
    if mr.header.product_tag != "FB200":
        print(
            f"error: refusing to flash image for {mr.header.product_tag!r} (expected 'FB200')",
            file=sys.stderr,
        )
        return 1
    plan = updater_module.build_flash_plan(mr)
    print(f"Image : {args.file}")
    print(f"Blocks: {len(mr.blocks)}")
    print(f"Data  : {plan.total_bytes} bytes in {plan.write_count} write frames")
    if not args.yes:
        print("dry run: pass --yes to actually flash")
        return 0

    if args.no_jump:
        path = HidapiTransport.find_path(UPDATE_VID, UPDATE_PID)
        if path is None:
            print(
                f"error: pedal is not in update mode ({UPDATE_VID:04x}:{UPDATE_PID:04x})",
                file=sys.stderr,
            )
            return 1
    else:
        with _with_device() as app:
            info = app.info()
            print(f"target: {info.product} {info.firmware_version}")
            app.enter_bootloader()
        print("waiting for bootloader device...")
        path = updater_module.wait_for_device(UPDATE_VID, UPDATE_PID)
        if path is None:
            print("error: bootloader device did not appear", file=sys.stderr)
            return 1

    boot_transport = HidapiTransport(path=path, vid=UPDATE_VID, pid=UPDATE_PID).open()
    try:
        def progress(done: int, total: int) -> None:
            print(f"\rwriting {done}/{total}", end="", file=sys.stderr)

        result = updater_module.FirmwareUpdater(boot_transport).flash(
            mr, dry_run=False, progress=progress
        )
    finally:
        boot_transport.close()
    print(f"\nwrote {result.bytes_written} bytes", file=sys.stderr)

    if updater_module.wait_for_device(protocol.VID, protocol.PID_APP, timeout_s=20) is None:
        print("error: device did not re-enumerate after flash", file=sys.stderr)
        return 3

    deadline = time.monotonic() + 5
    while True:
        try:
            with _with_device() as device:
                info = device.info()
            break
        except Fb200Error:
            if time.monotonic() >= deadline:
                print(
                    "error: flash wrote OK but the device did not answer the version query",
                    file=sys.stderr,
                )
                return 3
            time.sleep(0.5)
    print(f"device back online: {info.product} {info.firmware_version}")
    return 0


def _cmd_console(args) -> int:
    from fb200 import console

    with console.Console(args.port) as con:
        if not args.commands:
            return console.interactive(con)
        for cmd in args.commands:
            out = con.run(cmd, max_s=args.timeout)
            if out:
                print(out)
    return 0


def _cmd_crash(args) -> int:
    import re
    import subprocess

    from fb200 import console

    with console.Console(args.port) as con:
        dump = con.run("crashdump")
    print(dump)
    if not args.elf or dump.startswith("no crash dump"):
        return 0
    regs = dict(re.findall(r"\b(PC|LR)=([0-9a-f]{8})", dump))
    stack = [int(w, 16) for line in dump.splitlines() if line.strip().startswith("[sp+")
             for w in line.split("]")[1].split()]
    # ITCM code lives below 0x20000; a return address has the Thumb bit set.
    cands = [("PC", int(regs.get("PC", "0"), 16)), ("LR", int(regs.get("LR", "0"), 16))]
    cands += [(f"stack[{i}]", w) for i, w in enumerate(stack) if w & 1 and 0x400 <= w < 0x20000]
    print("\nsymbolized (stack entries are return-address candidates):")
    for label, addr in cands:
        if not 0x400 <= addr < 0x20000:
            continue
        out = subprocess.run(["arm-none-eabi-addr2line", "-fpC", "-e", args.elf, hex(addr & ~1)],
                             capture_output=True, text=True, check=False).stdout.strip()
        print(f"  {label:>10} {addr:08x}  {out}")
    return 0


def _cmd_mcp(args) -> int:
    from fb200.mcp_server import serve

    serve(args.port)
    return 0


def _cmd_fw_twostage(args) -> int:
    from fb200 import images, release

    out = images.twostage_mr(Path(args.stock).read_bytes(),
                             release.image(args.recovery, "recovery"),
                             release.image(args.app, "app"))
    Path(args.output).write_bytes(out)
    print(f"wrote {args.output} ({len(out)} bytes). It contains vendor data from your .mr: "
          "do not share it. Flash it in update mode (hold A+D while plugging in):\n"
          f"  fb200 fw flash {args.output} --yes --no-jump")
    return 0


def _cmd_update(args) -> int:
    from fb200 import console, images, release, stockdata
    from fb200.firmware import MrFile

    image = Path(args.file)
    if args.target == "block0":
        data = MrFile.from_path(image).blocks[0].data
    elif args.target == "stock":
        data = image.read_bytes()                    # FILE is the stock .mr: built below
    else:
        data = release.image(args.file, args.target)
    if args.target == "app":
        images.check_slot(data)
    if args.target == "recovery" and data[images.BOOT_OFF:images.BLOB_OFF] == \
            b"\xff" * (images.BLOB_OFF - images.BOOT_OFF):
        if not args.stock:
            print("error: this recovery image has no vendor loader; add --stock FB200.mr",
                  file=sys.stderr)
            return 2
        data = images.splice_vendor_loader(data, images.stock_image(
            Path(args.stock).read_bytes()).blocks[0].data)
    with console.Console(args.port, echo=(lambda line: print("  <", line)) if args.verbose else None) as con:
        if args.target == "stock":
            # write the newest format the running firmware accepts: an older app
            # rejects a newer blob and would lose its stock sound
            formats = stockdata.formats_from_reply(con.run("fwstock"))
            version = max(v for v in formats if v <= stockdata.VERSION)
            data = stockdata.build(data, version)
            print(f"stock data version {version} (pedal accepts {formats})")
        res = console.update(con, args.target, data, reset=not args.no_reset, force=args.force)
    print(f"{res.target}: {res.size} bytes crc {res.crc:08x} written in {res.seconds:.1f}s"
          + ("" if args.no_reset else "; reset sent"))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="fb200", description="Tools for the FLAMMA FB200 pedal")
    sub = parser.add_subparsers(dest="command", required=True)

    p_info = sub.add_parser("info", help="show device and firmware versions")
    p_info.set_defaults(func=_cmd_info)

    p_ir = sub.add_parser("ir", help="manage impulse response slots")
    ir_sub = p_ir.add_subparsers(dest="ir_command", required=True)

    p_list = ir_sub.add_parser("list", aliases=["ls"], help="list IR slots")
    p_list.add_argument("--long", action="store_true",
                        help="the long IR store (slots 20..83, open firmware console)")
    p_list.add_argument("--port", help="CDC device for --long (default: /dev/cu.usbmodemAUDIO*)")
    p_list.set_defaults(func=_cmd_ir_list)

    p_put = ir_sub.add_parser("put", help="store a WAV as a long IR (up to 4096 taps) in slot "
                                          "20..83 (open firmware console)")
    p_put.add_argument("slot", type=_long_slot_arg, help="20..83 = the cab type that plays it")
    p_put.add_argument("wav")
    p_put.add_argument("--name", help="up to 23 characters (default: the file name)")
    p_put.add_argument("--port", help="CDC device (default: /dev/cu.usbmodemAUDIO*)")
    _add_ir_process_args(p_put)
    p_put.set_defaults(func=_cmd_ir_put)

    p_import = ir_sub.add_parser("import", help="import a WAV into a slot")
    p_import.add_argument("slot", type=_slot_arg)
    p_import.add_argument("wav")
    p_import.add_argument("--name")
    _add_ir_process_args(p_import)
    p_import.set_defaults(func=_cmd_ir_import)

    p_process = ir_sub.add_parser("process", help="process a WAV IR into a file (no pedal)")
    p_process.add_argument("wav")
    p_process.add_argument("-o", "--out", required=True, help="output WAV (mono float, 44.1 kHz)")
    _add_ir_process_args(p_process)
    p_process.set_defaults(func=_cmd_ir_process)

    p_delete = ir_sub.add_parser("delete", help="delete a slot (1..9, or 20..83: long IRs)")
    p_delete.add_argument("slot", type=_delete_slot_arg)
    p_delete.add_argument("--port", help="CDC device for a long slot")
    p_delete.set_defaults(func=_cmd_ir_delete)

    p_backup = ir_sub.add_parser("backup", help="write a slot-name manifest")
    p_backup.add_argument("directory")
    p_backup.set_defaults(func=_cmd_ir_backup)

    p_probe = sub.add_parser("probe", help="advanced raw frame tool")
    p_probe.add_argument("--send", type=_hex_frame_arg,
                         help="hex frame to send (full AA55 frame including CRC)")
    p_probe.add_argument("--listen", type=float, default=0.0, help="seconds to listen")
    p_probe.set_defaults(func=_cmd_probe)

    p_fw = sub.add_parser("fw", help="inspect and patch .mr firmware")
    fw_sub = p_fw.add_subparsers(dest="fw_command", required=True)

    p_inspect = fw_sub.add_parser("inspect", help="show container structure")
    p_inspect.add_argument("file")
    p_inspect.add_argument("--json", action="store_true")
    p_inspect.add_argument("--strings", action="store_true")
    p_inspect.add_argument("--filter")
    p_inspect.set_defaults(func=_cmd_fw_inspect)

    p_extract = fw_sub.add_parser("extract-block", help="extract a block payload")
    p_extract.add_argument("file")
    p_extract.add_argument("index", type=int)
    p_extract.add_argument("output")
    p_extract.set_defaults(func=_cmd_fw_extract_block)

    p_patch = fw_sub.add_parser("patch-string", help="same-length string patch")
    p_patch.add_argument("file")
    p_patch.add_argument("--find", required=True)
    p_patch.add_argument("--replace", required=True)
    p_patch.add_argument("-o", "--output")
    p_patch.set_defaults(func=_cmd_fw_patch_string)

    p_pack = fw_sub.add_parser("pack", help="pack a raw binary into an app-only .mr")
    p_pack.add_argument("--template", required=True,
                        help="stock .mr whose header/tag fields are reused")
    p_pack.add_argument("app", help="raw application binary")
    p_pack.add_argument("--app-only", action="store_true",
                        help="single-block application image (default and only mode)")
    p_pack.add_argument("-o", "--output")
    p_pack.set_defaults(func=_cmd_fw_pack)

    p_two = fw_sub.add_parser("twostage", help="make the first-install .mr of the open "
                                               "firmware from your stock .mr")
    p_two.add_argument("stock", help="your official FB200 firmware .mr")
    p_two.add_argument("--recovery", default="latest", help="recovery image, or `latest`")
    p_two.add_argument("--app", default="latest", help="app slot image, or `latest`")
    p_two.add_argument("-o", "--output", default="fb200-twostage.mr")
    p_two.set_defaults(func=_cmd_fw_twostage)

    p_flash = fw_sub.add_parser("flash", help="flash a .mr image (dry-run unless --yes)")
    p_flash.add_argument("file")
    p_flash.add_argument("--yes", action="store_true",
                         help="actually write; without it the image is only validated")
    p_flash.add_argument("--no-jump", action="store_true",
                         help="assume the pedal is already in update mode")
    p_flash.set_defaults(func=_cmd_fw_flash)

    p_con = sub.add_parser("console", help="open-firmware USB console (interactive, or run commands)")
    p_con.add_argument("commands", nargs="*", help="commands to run; none = interactive")
    p_con.add_argument("--port", help="CDC device (default: /dev/cu.usbmodemAUDIO*)")
    p_con.add_argument("--timeout", type=float, default=5.0,
                       help="max seconds to collect each command's output")
    p_con.set_defaults(func=_cmd_console)

    p_crash = sub.add_parser("crash", help="read the last crash dump (survives reset); symbolize with --elf")
    p_crash.add_argument("--elf", help="the ELF that crashed, e.g. firmware/audio/build/fb200-app.elf")
    p_crash.add_argument("--port")
    p_crash.set_defaults(func=_cmd_crash)

    p_mcp = sub.add_parser("mcp", help="MCP server on stdio: an AI agent drives the pedal "
                                       "(needs the mcp extra)")
    p_mcp.add_argument("--port", help="CDC device (default: /dev/cu.usbmodemAUDIO*)")
    p_mcp.set_defaults(func=_cmd_mcp)

    p_up = sub.add_parser("update", help="flash the open firmware over its USB console (no A+D)")
    p_up.add_argument("target", choices=["app", "recovery", "stock", "block0"],
                      help="app = .slot, recovery = recovery .bin, stock = the sound data from "
                           "your stock .mr (once), block0 = migration from a .mr")
    p_up.add_argument("file", help="image file, `latest` (download the latest release; app "
                                   "and recovery), or the stock .mr (stock)")
    p_up.add_argument("--port")
    p_up.add_argument("--stock", help="stock .mr: supplies the vendor loader for a recovery "
                                      "image that has none")
    p_up.add_argument("--no-reset", action="store_true")
    p_up.add_argument("--force", action="store_true", help="skip the mapping check")
    p_up.add_argument("-v", "--verbose", action="store_true", help="show the console dialogue")
    p_up.set_defaults(func=_cmd_update)

    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except Fb200Error as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
