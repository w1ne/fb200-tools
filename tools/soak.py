#!/usr/bin/env python3
# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Soak test for the open FB200 firmware on a real pedal.

Runs for --minutes (or --cycles) and, every cycle:

  - selects a random preset (HID 0x98; reloads it from flash, nothing is saved)
  - writes random in-range module parameters to the edit buffer (HID 0x80..0x86)
  - pings HID (0x00 version) and lists the IR slots (HID 0x65)
  - runs console commands: stats, sai, usb, cpu, crumbs, stack
  - every --audio-every cycles: an audio_test capture (1 kHz sine into the chain)

and checks: engine skips / DMA errors / DSP resets and SAI over/underruns do
not grow, the crumbs (boot/fault record) do not change, console and HID
answer in time, the stack keeps --min-stack-free bytes free (`stack`, on
firmware that has it) and the capture is not silent. One CSV row per cycle,
a log, a summary; exit 1 on any fault, 2 if the pedal cannot be reached.

Nothing is written to flash. At the end the original preset is selected and
its edit buffer (with any unsaved edits it had) is put back.

  python3 tools/soak.py --minutes 30 --port /dev/cu.usbmodemAUDIO1
"""

from __future__ import annotations

import argparse
import csv
import math
import random
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from fb200.errors import Fb200Error
from fb200.mcp_server import Pedal, PedalTools, _num_pairs
from fb200.pedal import MODULES, PRESET_COUNT

CMD_SELECT_PRESET = 0x98
CMD_WRITE_PRESET = 0x97          # [0xFF][256 bytes]: edit buffer only, no flash
CMD_IR_LIST = 0x65
REPLY_IR_LIST = 0x66
IR_RECORD = 59

STACK_RE = re.compile(r"stack: used (\d+) of (\d+) bytes, free (\d+)")
CPU_RE = re.compile(r"\((\d+)% / (\d+)%\)")
CRUMBS_RE = re.compile(r"crumbs ((?:[0-9a-f]{8} ?){4})", re.IGNORECASE)

# Counters that must not grow: stats (engine) and sai. drops/inserts are the
# USB clock-drift corrections: recorded, not faults.
ENGINE_FAULTS = ("skips", "dma_errs", "resets")
SAI_FAULTS = ("ovf", "unf")
COLUMNS = ("time_s", "cycle", "preset", "drops", "inserts", "dma_errs", "skips", "resets",
           "sai_ovf", "sai_unf", "usb_ovf", "usb_unf", "cpu_max_pct", "stack_used", "stack_free",
           "audio_peak_dbfs", "faults")


def param_range(module: str, field: str) -> tuple[int, int]:
    """In-range values of a module field (docs/PROTOCOL.md 5.3, proto.c set_module)."""
    if module == "delay" and field == "time_ms":
        return 40, 2500
    return 0, 100


class Soak:
    def __init__(self, tools: PedalTools, log, rng: random.Random, *, use_hid: bool = True,
                 audio_every: int = 5, min_stack_free: int = 1024, reply_s: float = 3.0) -> None:
        self.tools = tools
        self.pedal = tools.pedal
        self.log = log
        self.rng = rng
        self.use_hid = use_hid
        self.audio_every = audio_every
        self.min_stack_free = min_stack_free
        self.reply_s = reply_s
        self.base: dict[str, int] = {}
        self.crumbs: str | None = None
        self.has_stack = True
        self.original: tuple[int, bytes] | None = None
        self.faults: list[str] = []

    # ------------------------------------------------------------ console

    def console(self, cmd: str) -> str:
        out = self.pedal.run(cmd, timeout=self.reply_s)
        if not out.strip():
            raise Fb200Error(f"console did not answer `{cmd}` within {self.reply_s} s")
        return out

    def counters(self) -> dict[str, int]:
        stats = _num_pairs(self.console("stats"))
        sai = _num_pairs(self.console("sai"))
        usb = _num_pairs(self.console("usb").split("\n")[0])
        out = {k: stats[k] for k in ("drops", "inserts", *ENGINE_FAULTS) if k in stats}
        out.update({f"sai_{k}": sai[k] for k in SAI_FAULTS if k in sai})
        out.update({f"usb_{k}": usb[k] for k in ("ovf", "unf") if k in usb})
        return out

    def read_crumbs(self) -> str | None:
        m = CRUMBS_RE.search(self.console("crumbs"))
        return m.group(1).strip().lower() if m else None

    def stack(self) -> tuple[int, int] | None:
        """(used, free), or None on firmware without `stack`."""
        if not self.has_stack:
            return None
        out = self.console("stack")
        m = STACK_RE.search(out)
        if m is None:
            if "unknown command" in out:
                self.has_stack = False
                self.log("stack: not in this firmware (skipped)")
                return None
            raise Fb200Error(f"unexpected `stack` reply: {out!r}")
        return int(m.group(1)), int(m.group(3))

    # ------------------------------------------------------------ HID

    def hid_cycle(self, row: dict) -> None:
        with self.pedal.device() as dev:
            if self.original is None:
                self.original = dev.edit_buffer()
            dev.info()                                   # heartbeat: 0x00 -> 0x01
            preset = self.rng.randrange(PRESET_COUNT)
            dev.send(CMD_SELECT_PRESET, bytes([preset]))
            row["preset"] = preset
            for _ in range(3):
                name = self.rng.choice(list(MODULES))
                fields = MODULES[name][2][2:]            # not enabled/type: no model change
                field = self.rng.choice(fields)
                lo, hi = param_range(name, field)
                dev.set_module(name, {field: self.rng.randint(lo, hi)})
            packet = dev.request(CMD_IR_LIST, bytes([1, 1, 0, 9, 0]), expect=REPLY_IR_LIST,
                                 retries=1)
            if len(packet) - 1 != 9 * IR_RECORD:
                raise Fb200Error(f"IR list reply has {len(packet) - 1} bytes, want {9 * IR_RECORD}")

    def restore(self) -> None:
        if self.original is None:
            return
        index, edit = self.original
        with self.pedal.device() as dev:
            dev.send(CMD_SELECT_PRESET, bytes([index]))
            dev.send(CMD_WRITE_PRESET, bytes([0xFF]) + edit)
            now = dev.edit_buffer()
        if now != (index, edit):
            raise Fb200Error("the original preset/edit buffer did not come back")
        self.log(f"restored preset {index} and its edit buffer")

    # ------------------------------------------------------------ cycle

    def start(self) -> None:
        self.base = self.counters()
        self.crumbs = self.read_crumbs()
        self.log(f"start: counters {self.base}, crumbs {self.crumbs}")
        if self.crumbs and not self.crumbs.startswith("00000000"):
            self.log(f"note: the pedal booted after an event (crumbs {self.crumbs}); "
                     "only a change counts as a fault")

    def cycle(self, n: int, t0: float) -> dict:
        row: dict = {"time_s": round(time.monotonic() - t0, 1), "cycle": n}
        faults: list[str] = []
        try:
            if self.use_hid:
                self.hid_cycle(row)
            now = self.counters()
            row.update(now)
            for key in (*ENGINE_FAULTS, *(f"sai_{k}" for k in SAI_FAULTS)):
                if key in now and key in self.base and now[key] > self.base[key]:
                    faults.append(f"{key} +{now[key] - self.base[key]}")
            self.base.update({k: now[k] for k in now})
            cpu = CPU_RE.search(self.console("cpu"))
            if cpu:
                row["cpu_max_pct"] = int(cpu.group(2))
                if int(cpu.group(2)) >= 100:
                    faults.append(f"cpu max {cpu.group(2)}% of the block budget")
            crumbs = self.read_crumbs()
            if crumbs != self.crumbs:
                faults.append(f"crumbs changed {self.crumbs} -> {crumbs} (reset/fault)")
                self.crumbs = crumbs
            st = self.stack()
            if st is not None:
                row["stack_used"], row["stack_free"] = st
                if st[1] < self.min_stack_free:
                    faults.append(f"stack free {st[1]} < {self.min_stack_free} bytes")
            if self.audio_every and n % self.audio_every == 0:
                res = self.tools.audio_test(signal="sine", seconds=1.0)
                peak = res.get("left", {}).get("peak_dbfs")
                row["audio_peak_dbfs"] = peak
                if peak is None or math.isnan(peak) or peak < -60:
                    faults.append(f"audio capture silent or invalid (peak {peak} dBFS)")
        except Fb200Error as exc:
            faults.append(f"{type(exc).__name__}: {exc}")
        except OSError as exc:
            faults.append(f"OSError: {exc}")
        row["faults"] = "; ".join(faults)
        self.faults += [f"cycle {n}: {f}" for f in faults]
        return row


def audio_available(tools: PedalTools, log) -> bool:
    try:
        from fb200 import audio

        audio.find_device()
        tools._audio = audio
        return True
    except Exception as exc:  # noqa: BLE001 - numpy/sounddevice/device missing
        log(f"warning: audio_test skipped ({type(exc).__name__}: {exc})")
        return False


def run(tools: PedalTools, args, log=print, audio: bool | None = None) -> int:
    """The soak loop. Returns the exit code (0 ok, 1 faults, 2 no pedal)."""
    rng = random.Random(args.seed)
    if audio is None:
        audio = not args.no_audio and audio_available(tools, log)
    soak = Soak(tools, log, rng, use_hid=not args.no_hid, audio_every=args.audio_every if audio else 0,
                min_stack_free=args.min_stack_free, reply_s=args.reply_s)
    try:
        soak.start()
    except (Fb200Error, OSError) as exc:
        log(f"cannot reach the pedal: {exc}")
        return 2
    t0 = time.monotonic()
    deadline = t0 + args.minutes * 60
    n = 0
    with open(args.csv, "w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=COLUMNS, extrasaction="ignore")
        writer.writeheader()
        try:
            while (args.cycles is None and time.monotonic() < deadline) or \
                    (args.cycles is not None and n < args.cycles):
                n += 1
                row = soak.cycle(n, t0)
                writer.writerow(row)
                fh.flush()
                log(f"cycle {n}: preset {row.get('preset', '-')} "
                    f"stack {row.get('stack_used', '-')} {row['faults'] or 'ok'}")
                if args.interval:
                    time.sleep(args.interval)
        except KeyboardInterrupt:
            log("interrupted")
        finally:
            try:
                if soak.use_hid:
                    soak.restore()
            except (Fb200Error, OSError) as exc:
                soak.faults.append(f"restore: {exc}")
            tools.pedal.close()
    elapsed = time.monotonic() - t0
    if soak.faults:
        log(f"FAIL: {len(soak.faults)} fault(s) in {n} cycles, {elapsed / 60:.1f} min:")
        for f in soak.faults:
            log(f"  {f}")
        return 1
    log(f"PASS: {n} cycles, {elapsed / 60:.1f} min, no faults (csv: {args.csv})")
    return 0


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", help="CDC console (default: auto, /dev/cu.usbmodemAUDIO*)")
    ap.add_argument("--minutes", type=float, default=30.0)
    ap.add_argument("--cycles", type=int, help="run this many cycles instead of --minutes")
    ap.add_argument("--interval", type=float, default=1.0, help="seconds between cycles")
    ap.add_argument("--csv", default="soak.csv")
    ap.add_argument("--log", help="also append the log to this file")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--no-audio", action="store_true", help="skip the audio_test captures")
    ap.add_argument("--no-hid", action="store_true", help="console only (no preset/param/IR)")
    ap.add_argument("--audio-every", type=int, default=5, help="capture every N cycles")
    ap.add_argument("--min-stack-free", type=int, default=1024, help="bytes")
    ap.add_argument("--reply-s", type=float, default=3.0, help="console reply timeout")
    return ap.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    logfile = open(args.log, "a") if args.log else None  # noqa: SIM115

    def log(msg: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        if logfile:
            logfile.write(line + "\n")
            logfile.flush()

    try:
        return run(PedalTools(Pedal(args.port)), args, log)
    finally:
        if logfile:
            logfile.close()


if __name__ == "__main__":
    sys.exit(main())
