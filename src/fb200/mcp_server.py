"""MCP server: an AI agent (Claude Code, Claude Desktop) drives the pedal.

`fb200 mcp` serves the tools on stdio. The pedal must run the open firmware:
the tools use its USB console (CDC), its control interface (HID, the stock app
protocol) and its USB audio. One lock serializes all pedal access. There is
no firmware flash tool: use `fb200 update`.

Needs the `mcp` extra (pip install 'fb200-tools[mcp]'). The HID tools need
hidapi (the `hid` extra), the audio tool numpy and sounddevice.
"""

from __future__ import annotations

import functools
import re
import threading
import time
from collections.abc import Callable
from contextlib import contextmanager
from typing import Literal

from fb200.errors import CommunicationError, Fb200Error, InvalidArgumentError

INSTRUCTIONS = """\
Tools for the FLAMMA FB200 bass pedal with the open firmware, on USB.
Effect edits change the live edit buffer of the current preset; `save_preset`
stores it. Chain: in -> gate -> comp -> amp -> cab -> mod -> delay -> reverb ->
master -> USB capture / DAC. To hear a change, run `audio_test` (default: the
firmware test signal into the chain input, captured over USB audio) before
and after. Knob-type values are 0..100."""

# Console commands that stream a firmware image: never over MCP.
FLASH_COMMANDS = ("fwbegin", "fwrec", "fwstock")
ERROR_MARKERS = ("usage:", "unknown command", "bad ", "not allowed")
CHAIN_SIGNALS = {"sine": "sine", "noise": "white", "impulse": "impulse"}


def _num_pairs(text: str) -> dict[str, int]:
    """`key=12 other=3` -> {'key': 12, 'other': 3}."""
    return {k: int(v) for k, v in re.findall(r"([A-Za-z_]+)=(-?\d+)", text)}


def parse_preset(text: str) -> dict:
    """Parse the `preset` console line."""
    m = re.search(r"preset (\d+):(.*)", text)
    if not m:
        raise CommunicationError(f"unexpected preset reply: {text!r}")
    out: dict = {"index": int(m.group(1))}
    for block, fields in re.findall(r"(amp|cab|mod|rev)((?: \w+=\d+)+)", m.group(2)):
        out[block] = _num_pairs(fields)
    master = re.search(r"master=(\d+)", text)
    if master:
        out["master"] = int(master.group(1))
    return out


DELAY_RE = re.compile(r"delay (on|off)( \(stock preset, never plays\))?: time (\d+) ms fb (\d+) "
                      r"mix (\d+) lowcut (\d+) \((-?\d+) Hz\) tone (\d+)")


def parse_delay(text: str) -> dict:
    m = DELAY_RE.search(text)
    if not m:
        raise CommunicationError(f"unexpected delay reply: {text!r}")
    return {"on": m.group(1) == "on", "plays": m.group(2) is None,
            "time_ms": int(m.group(3)), "feedback": int(m.group(4)), "mix": int(m.group(5)),
            "lowcut": int(m.group(6)), "lowcut_hz": int(m.group(7)), "tone": int(m.group(8))}


def parse_profile(text: str) -> dict:
    stages = {m[0]: int(m[1]) for m in re.findall(r"prof (\S+) (\d+) cycles/block", text)}
    over = re.search(r"prof over (\d+) blocks \(budget (\d+)\)", text)
    if not stages or not over:
        raise CommunicationError(f"unexpected prof reply: {text!r}")
    total, budget = sum(stages.values()), int(over.group(2))
    return {"stages": stages, "total": total, "budget": budget, "blocks": int(over.group(1)),
            "load_pct": round(100.0 * total / budget, 1) if budget else None}


def _open_console(port: str | None):
    from fb200.console import Console

    return Console(port)


def _open_hid():
    from fb200.pedal import FB200Device
    from fb200.transport import HidapiTransport

    return FB200Device(HidapiTransport().open())


class Pedal:
    """Serialized access to one pedal. The console stays open between calls
    and reopens after an error; the HID device opens per call."""

    def __init__(self, port: str | None = None, console_factory: Callable | None = None,
                 device_factory: Callable | None = None) -> None:
        self.lock = threading.RLock()
        self._console_factory = console_factory or (lambda: _open_console(port))
        self._device_factory = device_factory or _open_hid
        self._con = None

    def run(self, command: str, timeout: float = 5.0) -> str:
        with self.lock:
            try:
                if self._con is None:
                    self._con = self._console_factory()
                return self._con.run(command, max_s=timeout)
            except (Fb200Error, OSError) as exc:
                self.close()
                if isinstance(exc, Fb200Error):
                    raise
                raise CommunicationError(f"console: {exc}") from exc

    def check(self, command: str, timeout: float = 5.0) -> str:
        """run(), but a usage/unknown-command reply is an error."""
        out = self.run(command, timeout)
        if any(m in out for m in ERROR_MARKERS):
            raise InvalidArgumentError(f"`{command}`: {out.strip()}")
        return out

    @contextmanager
    def device(self):
        with self.lock:
            dev = self._device_factory()
            try:
                yield dev
            finally:
                dev.transport.close()

    def close(self) -> None:
        with self.lock:
            if self._con is not None:
                try:
                    self._con.close()
                except OSError:
                    pass
                self._con = None


class PedalTools:
    """The MCP tools as plain methods (tests call them without the mcp package)."""

    settle_s = 0.3          # after a test signal starts, before the capture

    def __init__(self, pedal: Pedal, audio=None) -> None:
        self.pedal = pedal
        self._audio = audio

    @property
    def audio(self):
        if self._audio is None:
            try:
                from fb200 import audio
            except ImportError as exc:
                raise InvalidArgumentError("audio_test needs numpy and sounddevice: "
                                           "pip install numpy sounddevice") from exc
            self._audio = audio
        return self._audio

    # ------------------------------------------------------------ status

    def pedal_status(self) -> dict:
        """Firmware slot info, engine stats (gain, mute, drops, peaks), the
        current preset (amp/cab/mod/reverb enable and type, master), USB audio
        stream state and the stock sound data state. Console only."""
        out = {cmd: self.pedal.run(cmd) for cmd in ("fwinfo", "stats", "usb", "stock", "preset")}
        out["preset_parsed"] = parse_preset(out["preset"])
        return out

    def pedal_info(self) -> dict:
        """Product and version strings over the control interface (HID)."""
        with self.pedal.device() as dev:
            info = dev.info()
        return {"product": info.product, "app_version": info.app_version,
                "firmware_version": info.firmware_version,
                "bluetooth_version": info.bluetooth_version, "hardware_rev": info.hardware_rev}

    def console(self, command: str, timeout_s: float = 5.0) -> str:
        """Run one open-firmware console command and return its output.

        Commands (see `help`):
        audio: usb | sai | codec | creg <reg> [val] | gain [db] | mute [on|off] |
          testgen off|sine|white|impulse [freq] (to the output) |
          tin <same> (into the chain input, -20 dBFS) | meters on|off | cpu | prof
        effects: preset [0-39] | save | delay [on|off] [time] [fb] [mix] [lowcut] [tone] |
          tuner on|off | drums [on|off|<1-40>|bpm <n>|level <0-100>] | stock
        ui: ui | uimon on|off | disp <text> | kled <0-15> on|off | power |
          rgb 0xRRGGBB [led] | factory [yes] (resets ALL presets)
        bt: bt | bt send <AT+...> | btaudio
        debug: stats | src | hb on|off | clocks | crumbs | crashdump | crashclear |
          peek <addr> [len] | peek32 <addr> [n] | poke <addr> <u8> | poke32 <addr> <u32> |
          crc <addr> <len> | scan | dump [bus addr] | fwinfo | fwtest
        danger: crash / hang (fault tests: the pedal reboots into recovery),
          reset | reboot, recovery.
        Numbers are decimal or 0x hex; no negative numbers. Flash streaming
        (fwbegin/fwrec/fwstock) is refused here: use `fb200 update`."""
        words = command.split()
        if words and words[0] in FLASH_COMMANDS and len(words) > 1:
            raise InvalidArgumentError("flashing is not available over MCP; use `fb200 update`")
        return self.pedal.run(command, timeout_s)

    def preset(self, index: int | None = None) -> dict:
        """The current preset summary; with index (0..39), select that preset first."""
        if index is not None and not 0 <= index <= 39:
            raise InvalidArgumentError("preset index must be 0..39")
        text = self.pedal.check("preset" if index is None else f"preset {index}")
        return parse_preset(text)

    def save_preset(self) -> str:
        """Store the edit buffer into the current preset slot (flash)."""
        return self.pedal.check("save")

    # ------------------------------------------------------------ effects (HID)

    def get_effects(self) -> dict:
        """All effect blocks of the edit buffer (comp, gate, amp, cab, mod,
        delay, reverb) with their raw field values, read over HID."""
        with self.pedal.device() as dev:
            return dev.modules()

    def _set(self, name: str, **fields) -> dict:
        changes = {k: int(v) for k, v in fields.items() if v is not None}
        if "enabled" in changes:
            changes["enabled"] = 1 if changes["enabled"] else 0
        with self.pedal.device() as dev:
            return dev.set_module(name, changes)

    def set_amp(self, enabled: bool | None = None, model: int | None = None,
                gain: int | None = None, bass: int | None = None, mid: int | None = None,
                midfreq: int | None = None, treble: int | None = None,
                volume: int | None = None) -> dict:
        """Change the amp block. model 1..55 (a new model loads its default
        knobs, then the other values given here apply); knobs 0..100. Omitted
        fields keep their value. Returns the block read back."""
        return self._set("amp", enabled=enabled, model=model, gain=gain, bass=bass, mid=mid,
                         midfreq=midfreq, treble=treble, volume=volume)

    def set_cab(self, enabled: bool | None = None, type: int | None = None,
                p1: int | None = None, p2: int | None = None, p3: int | None = None,
                p4: int | None = None) -> dict:
        """Change the cab block. type 11..19 = user IR slots 1..9, lower = stock
        cabs. p1 0..4, p2/p3 0..100, p4 0..9 (pedal clamps). Returns the
        block read back."""
        return self._set("cab", enabled=enabled, type=type, p1=p1, p2=p2, p3=p3, p4=p4)

    def set_comp(self, enabled: bool | None = None, type: int | None = None,
                 attack: int | None = None, threshold: int | None = None,
                 ratio: int | None = None, level: int | None = None) -> dict:
        """Change the compressor block. type 0..21, the others 0..100."""
        return self._set("comp", enabled=enabled, type=type, attack=attack,
                         threshold=threshold, ratio=ratio, level=level)

    def set_gate(self, enabled: bool | None = None, type: int | None = None,
                 threshold: int | None = None) -> dict:
        """Change the noise gate block. type 0..4, threshold 0..100."""
        return self._set("gate", enabled=enabled, type=type, threshold=threshold)

    def set_mod(self, enabled: bool | None = None, type: int | None = None,
                p1: int | None = None, p2: int | None = None, p3: int | None = None,
                p4: int | None = None, p5: int | None = None) -> dict:
        """Change the modulation block. type 0..21 (a new type loads its
        defaults first), p1..p4 0..100, p5 0..240."""
        return self._set("mod", enabled=enabled, type=type, p1=p1, p2=p2, p3=p3, p4=p4, p5=p5)

    def set_reverb(self, enabled: bool | None = None, type: int | None = None,
                   p1: int | None = None, level: int | None = None, decay: int | None = None,
                   p4: int | None = None) -> dict:
        """Change the reverb block. type 0..5 (a new type loads its defaults
        first), p1 0..200, level/decay/p4 0..100."""
        return self._set("reverb", enabled=enabled, type=type, p1=p1, level=level, decay=decay,
                         p4=p4)

    # ------------------------------------------------------------ effects (console)

    def set_delay(self, on: bool | None = None, time_ms: int | None = None,
                  feedback: int | None = None, mix: int | None = None,
                  lowcut: int | None = None, tone: int | None = None) -> dict:
        """Change the bass delay (console `delay`). time 20..1000 ms, feedback,
        mix, lowcut (20..500 Hz on the repeats), tone (100 = off) 0..100.
        Omitted values keep their value. No arguments: read only."""
        values = [time_ms, feedback, mix, lowcut, tone]
        if any(v is not None and v < 0 for v in values):
            raise InvalidArgumentError("delay values must not be negative")
        cmd = ["delay"]
        if on is not None:
            cmd.append("on" if on else "off")
        if any(v is not None for v in values):
            cur = parse_delay(self.pedal.check("delay"))
            keys = ["time_ms", "feedback", "mix", "lowcut", "tone"]
            last = max(i for i, v in enumerate(values) if v is not None)
            cmd += [str(values[i] if values[i] is not None else cur[keys[i]])
                    for i in range(last + 1)]
        return parse_delay(self.pedal.check(" ".join(cmd)))

    def set_output(self, gain_db: int | None = None, mute: bool | None = None) -> dict:
        """Output gain after the master (console `gain`): 0 dB or more, or
        -6 / -12 dB; and the output mute."""
        out = {}
        if gain_db is not None:
            if gain_db >= 0:
                out["gain"] = self.pedal.check(f"gain {gain_db}")
            elif gain_db in (-6, -12):
                # the console takes no negative number: bare `gain` steps 0 -> -6 -> -12 -> 0
                want = f"gain {gain_db} dB"
                for _ in range(3):
                    out["gain"] = self.pedal.check("gain")
                    if want in out["gain"]:
                        break
                else:
                    raise CommunicationError(f"gain did not reach {gain_db} dB: {out['gain']}")
            else:
                raise InvalidArgumentError("gain_db must be >= 0, -6 or -12")
        if mute is not None:
            out["mute"] = self.pedal.check("mute on" if mute else "mute off")
        out["stats"] = self.pedal.run("stats")
        return out

    def drums(self, on: bool | None = None, rhythm: int | None = None, bpm: int | None = None,
              level: int | None = None) -> dict:
        """The drum machine: rhythm 1..40, bpm, level 0..100, on/off."""
        if rhythm is not None and not 1 <= rhythm <= 40:
            raise InvalidArgumentError("rhythm must be 1..40")
        if any(v is not None and v < 0 for v in (bpm, level)):
            raise InvalidArgumentError("bpm and level must not be negative")
        cmds = []
        if rhythm is not None:
            cmds.append(f"drums {rhythm}")
        if bpm is not None:
            cmds.append(f"drums bpm {bpm}")
        if level is not None:
            cmds.append(f"drums level {level}")
        if on is not None:
            cmds.append("drums on" if on else "drums off")
        text = ""
        for cmd in cmds or ["drums"]:
            text = self.pedal.check(cmd)
        out = _num_pairs(text.replace("rhythm ", "rhythm=").replace("bpm ", "bpm=")
                         .replace("level ", "level="))
        out["on"] = text.startswith("drums on")
        out["text"] = text
        return out

    def tuner(self, on: bool = True) -> dict:
        """Tuner on (with one reading: note, octave, cents, frequency) or off."""
        text = self.pedal.check("tuner on" if on else "tuner off")
        out: dict = _num_pairs(text)
        freq = re.search(r"freq=(-?\d+\.\d+)", text)
        if freq:
            out["freq"] = float(freq.group(1))
        out["text"] = text
        return out

    # ------------------------------------------------------------ debug

    def cpu_profile(self) -> dict:
        """CPU cycles per audio block for each stage (`prof`) and the block
        average/max (`cpu`). Both reset their counters on read."""
        out = parse_profile(self.pedal.check("prof"))
        out["cpu"] = self.pedal.check("cpu")
        return out

    def crash_dump(self, clear: bool = False) -> str:
        """The last crash dump (survives reset); clear=True erases it after reading."""
        dump = self.pedal.run("crashdump")
        if clear:
            self.pedal.run("crashclear")
        return dump

    # ------------------------------------------------------------ IRs (HID)

    def ir_list(self) -> list[dict]:
        """The 9 user IR slots and their names (None = empty)."""
        with self.pedal.device() as dev:
            return [{"slot": s.index, "name": s.name} for s in dev.ir_list()]

    def ir_import(self, slot: int, wav_path: str, name: str | None = None) -> dict:
        """Import a WAV (any rate; channel 0, resampled to 44.1 kHz, first
        1024 samples) into user IR slot 1..9. Select it with set_cab type 10 + slot."""
        with self.pedal.device() as dev:
            stored = dev.import_wav(slot, wav_path, name)
        return {"slot": slot, "name": stored}

    # ------------------------------------------------------------ audio

    def audio_test(self, signal: Literal["sine", "sweep", "noise", "impulse", "wav"] = "sine",
                   source: Literal["chain", "usb"] = "chain", freq_hz: int = 1000,
                   seconds: float = 2.0, level_dbfs: float = -20.0,
                   wav_path: str | None = None, save_wav: str | None = None) -> dict:
        """Play a test signal and capture the pedal's USB audio output; return
        levels (RMS, peak per channel), THD for a sine, an octave-band
        response (dB re 1 kHz) for a sweep or noise. save_wav writes the capture.

        source "chain" (default): the firmware test generator feeds the chain
        input (`tin`, -20 dBFS; sine, noise or impulse), so the capture holds
        every effect. source "usb": the host plays the signal (sine, sweep,
        noise or a WAV at level_dbfs) to the pedal's USB audio. Note: the
        firmware mixes USB playback into the analog output after the effects,
        not into the capture, so "usb" hears the played signal only when the
        firmware routes it into the chain."""
        if not 0.2 <= seconds <= 30.0:
            raise InvalidArgumentError("seconds must be 0.2..30")
        audio = self.audio
        with self.pedal.lock:
            if source == "chain":
                if signal not in CHAIN_SIGNALS:
                    raise InvalidArgumentError("source 'chain' plays sine, noise or impulse")
                self.pedal.check(f"tin {CHAIN_SIGNALS[signal]} {int(freq_hz)}")
                try:
                    time.sleep(self.settle_s)
                    rec = audio.record(seconds)
                finally:
                    self.pedal.run("tin off")
                level = -20.0
            elif source == "usb":
                x = audio.make_signal(signal, seconds, freq_hz, level_dbfs, wav_path)
                rec = audio.play_record(x)
                level = level_dbfs
                if signal == "sine":        # skip the stream start
                    rec = rec[int(0.25 * audio.FS):]
            else:
                raise InvalidArgumentError("source must be 'chain' or 'usb'")
        result = {"source": source, "signal": signal, "seconds": seconds}
        result.update(audio.analyze(rec, signal, audio.FS, freq_hz, seconds, level))
        if save_wav:
            from fb200.wav import write_wav

            write_wav(save_wav, [tuple(row) for row in rec.tolist()], audio.FS)
            result["saved"] = save_wav
        return result

    # ------------------------------------------------------------ registry

    TOOLS = ("pedal_status", "pedal_info", "console", "preset", "save_preset", "get_effects",
             "set_amp", "set_cab", "set_comp", "set_gate", "set_mod", "set_reverb", "set_delay",
             "set_output", "drums", "tuner", "cpu_profile", "crash_dump", "ir_list",
             "ir_import", "audio_test")


def _sdk():
    """The server class and ToolError of the installed mcp SDK (2.x or 1.x)."""
    try:
        from mcp.server.mcpserver import MCPServer
        from mcp.server.mcpserver.exceptions import ToolError
        return MCPServer, ToolError
    except ImportError:
        pass
    try:
        from mcp.server.fastmcp import FastMCP
        from mcp.server.fastmcp.exceptions import ToolError
        return FastMCP, ToolError
    except ImportError as exc:
        raise Fb200Error("the MCP server needs the mcp package: "
                         "pip install 'fb200-tools[mcp]'") from exc


def build_server(tools: PedalTools):
    server_cls, tool_error = _sdk()
    server = server_cls("fb200", instructions=INSTRUCTIONS)

    def wrap(fn):
        @functools.wraps(fn)
        def call(*args, **kwargs):
            try:
                return fn(*args, **kwargs)
            except Exception as exc:     # every failure is a tool error, never a crash
                raise tool_error(f"{type(exc).__name__}: {exc}") from exc
        return call

    for name in PedalTools.TOOLS:
        server.tool(name=name)(wrap(getattr(tools, name)))
    return server


def serve(port: str | None = None) -> None:
    """Serve the tools on stdio until the client disconnects."""
    tools = PedalTools(Pedal(port))
    server = build_server(tools)
    try:
        server.run()
    finally:
        tools.pedal.close()
