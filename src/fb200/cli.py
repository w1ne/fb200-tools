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
    with _with_device() as device:
        for slot in device.ir_list():
            print(f"{slot.index}: {slot.name if slot.name else '(empty)'}")
    return 0


def _cmd_ir_delete(args) -> int:
    with _with_device() as device:
        deleted = device.ir_delete(args.slot)
    if not deleted:
        print(f"failed to delete slot {args.slot}", file=sys.stderr)
        return 1
    print(f"deleted slot {args.slot}")
    return 0


def _cmd_ir_import(args) -> int:
    from fb200.wav import wav_to_ir

    samples = wav_to_ir(args.wav)
    name = args.name or Path(args.wav).stem

    def progress(done: int, total: int) -> None:
        print(f"\rframe {done}/{total}", end="", file=sys.stderr)

    with _with_device() as device:
        device.ir_import(args.slot, name, samples, progress=progress)
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


def _cmd_fw_flash(args) -> int:
    from fb200.firmware import MrFile
    from fb200.protocol import UPDATE_PID, UPDATE_VID
    from fb200.updater import FirmwareUpdater, build_flash_plan, wait_for_device

    mr = MrFile.from_path(args.file)
    if mr.header.product_tag != "FB200":
        print(
            f"error: refusing to flash image for {mr.header.product_tag!r} (expected 'FB200')",
            file=sys.stderr,
        )
        return 1
    plan = build_flash_plan(mr)
    print(f"Image : {args.file}")
    print(f"Blocks: {len(mr.blocks)}")
    print(f"Data  : {plan.total_bytes} bytes in {plan.write_count} write frames")
    if not args.yes:
        print("dry run: pass --yes to actually flash")
        return 0

    if args.no_jump:
        path = HidapiTransport.find_path(UPDATE_VID, UPDATE_PID)
        if path is None:
            print("error: pedal is not in update mode (0483:5703)", file=sys.stderr)
            return 1
    else:
        app = _open_device()
        app.enter_bootloader()
        print("waiting for bootloader device...")
        path = wait_for_device(UPDATE_VID, UPDATE_PID)
        if path is None:
            print("error: bootloader device did not appear", file=sys.stderr)
            return 1

    boot_transport = HidapiTransport(path=path, vid=UPDATE_VID, pid=UPDATE_PID).open()
    try:
        def progress(done: int, total: int) -> None:
            print(f"\rwriting {done}/{total}", end="", file=sys.stderr)

        result = FirmwareUpdater(boot_transport).flash(mr, dry_run=False, progress=progress)
    finally:
        boot_transport.close()
    print(f"\nwrote {result.bytes_written} bytes", file=sys.stderr)

    if wait_for_device(protocol.VID, protocol.PID_APP, timeout_s=20) is None:
        print("error: device did not re-enumerate after flash", file=sys.stderr)
        return 3
    info = _open_device().info()
    print(f"device back online: {info.product} {info.firmware_version}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="fb200", description="Tools for the FLAMMA FB200 pedal")
    sub = parser.add_subparsers(dest="command", required=True)

    p_info = sub.add_parser("info", help="show device and firmware versions")
    p_info.set_defaults(func=_cmd_info)

    p_ir = sub.add_parser("ir", help="manage impulse response slots")
    ir_sub = p_ir.add_subparsers(dest="ir_command", required=True)

    p_list = ir_sub.add_parser("list", help="list IR slots")
    p_list.set_defaults(func=_cmd_ir_list)

    p_import = ir_sub.add_parser("import", help="import a WAV into a slot")
    p_import.add_argument("slot", type=_slot_arg)
    p_import.add_argument("wav")
    p_import.add_argument("--name")
    p_import.set_defaults(func=_cmd_ir_import)

    p_delete = ir_sub.add_parser("delete", help="delete a slot")
    p_delete.add_argument("slot", type=_slot_arg)
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

    p_flash = fw_sub.add_parser("flash", help="flash a .mr image (dry-run unless --yes)")
    p_flash.add_argument("file")
    p_flash.add_argument("--yes", action="store_true",
                         help="actually write; without it the image is only validated")
    p_flash.add_argument("--no-jump", action="store_true",
                         help="assume the pedal is already in update mode")
    p_flash.set_defaults(func=_cmd_fw_flash)

    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except Fb200Error as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
