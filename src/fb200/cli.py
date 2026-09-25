"""fb200 command line interface."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from contextlib import contextmanager
from pathlib import Path

from fb200.errors import Fb200Error
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
