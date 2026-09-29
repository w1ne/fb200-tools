"""MCP server tools against a fake console and a fake HID pedal (no hardware)."""

from __future__ import annotations

import asyncio
import struct

import pytest
from conftest import version_payload

from fb200 import protocol
from fb200.errors import CommunicationError, InvalidArgumentError
from fb200.mcp_server import (
    Pedal,
    PedalTools,
    _sdk,
    build_server,
    parse_delay,
    parse_eq,
    parse_loop,
    parse_profile,
)
from fb200.pedal import MODULES, PRESET_SIZE, FB200Device
from fb200.protocol import FrameReader, iter_reports, pack_frame

PRESET_LINE = ("preset 3: amp en=1 model=5 gain=40  cab en=1 type=2  mod en=0 type=1  "
               "rev en=1 type=3  master=80")
PROF = "\n".join([f"prof {s} {c} cycles/block" for s, c in
                  (("in", 100), ("gate", 200), ("amp", 3000), ("cab", 5000), ("out", 50))]
                 + ["prof over 4000 blocks (budget 102400)"])


class FakeConsole:
    """Answers like firmware/audio/src/debug/console.c; logs every command."""

    def __init__(self) -> None:
        self.sent: list[str] = []
        self.gain = 0
        self.usb_route = "out"
        self.delay = {"on": "off", "time": 500, "fb": 30, "mix": 25, "lowcut": 0, "tone": 100}
        self.eq = {"on": False, "hpf": 0.0, "lpf": 0.0,
                   "bands": [[f, 0.0, 1.0] for f in (40.0, 100.0, 250.0, 800.0, 3000.0)]}
        self.loop = {"state": "empty", "level": 100}
        self.closed = False

    def loop_cmd(self, args: list[str]) -> str:
        # console.c cmd_loop (the state machine reduced to what the tool sends)
        lp = self.loop
        err = ""
        if args and args[0] in ("rec", "play", "dub", "stop", "undo", "clear", "tap"):
            nxt = {("empty", "rec"): "rec", ("rec", "rec"): "play", ("rec", "play"): "play",
                   ("play", "dub"): "dub", ("dub", "play"): "play", ("play", "stop"): "stop",
                   ("stop", "play"): "play", ("play", "clear"): "empty", ("stop", "clear"): "empty",
                   ("empty", "tap"): "rec", ("rec", "tap"): "play"}.get((lp["state"], args[0]))
            if nxt is None:
                err = f"loop {args[0]}: not allowed now\r\n"
            else:
                lp["state"] = nxt
        elif len(args) > 1 and args[0] == "level":
            lp["level"] = int(args[1])
        elif args:
            return "usage: loop [rec|play|dub|stop|undo|clear|tap] | loop level <0-100> | loop stats"
        return (err + f"loop {lp['state']}: len_ms=0 pos_ms=0 max_ms=108299 "
                f"undo_max_ms=54294 undo=none level={lp['level']} prep_ms=2310 flash=1")

    def eq_line(self) -> str:
        # console.c cmd_eq, with its clamps
        e = self.eq
        flat = e["on"] and not e["hpf"] and not e["lpf"] and all(g == 0 for _, g, _ in e["bands"])
        def cut(hz: float) -> str:
            return f"{round(hz)} Hz" if hz else "off"

        head = f"eq {'on' if e['on'] else 'off'}{' (flat)' if flat else ''}"
        lines = [f"{head}: hpf {cut(e['hpf'])} lpf {cut(e['lpf'])}"]
        lines += [f"  {i + 1}: {round(f)} Hz {g:+.2f} dB q {q:.2f}"
                  for i, (f, g, q) in enumerate(e["bands"])]
        return "\r\n".join(lines)

    def eq_cmd(self, args: list[str]) -> str:
        usage = "usage: eq [on|off] | eq hpf <20-200 Hz|0> | ..."

        def clamp(v: float, lo: float, hi: float) -> float:
            return min(max(v, lo), hi)

        try:
            if args and args[0] in ("on", "off"):
                self.eq["on"] = args[0] == "on"
            elif len(args) > 1 and args[0] in ("hpf", "lpf"):
                hz = float(args[1])
                lo, hi = (20, 200) if args[0] == "hpf" else (2000, 20000)
                self.eq[args[0]] = clamp(hz, lo, hi) if hz > 0 else 0.0
            elif len(args) > 2 and 1 <= int(args[0]) <= 5:
                q = float(args[3]) if len(args) > 3 else 1.0
                self.eq["bands"][int(args[0]) - 1] = [clamp(float(args[1]), 30, 10000),
                                                      clamp(float(args[2]), -15, 15),
                                                      clamp(q, 0.3, 4)]
            elif args:
                return usage
        except ValueError:
            return usage
        return self.eq_line()

    def delay_line(self) -> str:
        d = self.delay
        return (f"delay {d['on']}: time {d['time']} ms fb {d['fb']} mix {d['mix']} "
                f"lowcut {d['lowcut']} (20 Hz) tone {d['tone']}")

    def run(self, cmd: str, max_s: float = 5.0) -> str:
        self.sent.append(cmd)
        w = cmd.split()
        if w[0] == "preset":
            return PRESET_LINE
        if w[0] == "prof":
            return PROF
        if w[0] == "cpu":
            return "cpu: engine block avg 9000 max 12000 cycles of 102400 (8% / 11%)"
        if w[0] == "delay":
            args = w[1:]
            if args and args[0] in ("on", "off"):
                self.delay["on"] = args.pop(0)
            for key, val in zip(("time", "fb", "mix", "lowcut", "tone"), args):
                self.delay[key] = int(val)
            return self.delay_line()
        if w[0] == "eq":
            return self.eq_cmd(w[1:])
        if w[0] == "cab" and len(w) > 2 and w[1] == "long":
            if w[2].isdigit() and int(w[2]) > 512:     # firmware without long IRs
                return f"cab long {w[2]}: not available (long IRs need more RAM; max 512)"
            return f"cab long {w[2]}: {'ok' if w[2].isdigit() else 'bad taps'}"
        if w[0] == "gain":
            if len(w) > 1:
                if not w[1].isdigit():
                    return "usage: gain [db]"
                self.gain = int(w[1])
            else:
                self.gain = -6 if self.gain > -3 else -12 if self.gain > -9 else 0
            return f"gain {self.gain} dB"
        if w[0] == "mute":
            return f"mute {w[1]}"
        if w[0] == "stats":
            return f"engine: gain={self.gain} dB mute=0 drops=0"
        if w[0] == "drums":
            return "drums on rhythm 7 bpm 120 level 60 samples 99 patterns yes"
        if w[0] == "loop":
            return self.loop_cmd(w[1:])
        if w[0] == "tuner":
            return f"tuner {w[1]}: valid=1 silent=0 note=4 oct=1 cents=-3 freq=41.20 Hz"
        if w[0] == "crashdump":
            return "no crash dump"
        if w[0] == "tin":
            return f"testgen {w[1]} {w[2] if len(w) > 2 else 1000} Hz"
        if w[0] == "save":
            return "saved"
        if w[0] == "usb":
            head = ""
            if len(w) > 1:
                if w[1] not in ("out", "in", "mix"):
                    return "usage: usb [out|in|mix]"
                self.usb_route = w[1]
                head = f"usb route {w[1]}\n"
            return (head + f"usb: route={self.usb_route} spk_alt=1 mic_alt=1 play_fill=40 "
                    "cap_fill=12 ovf=0 unf=0\nusb: host mute=0 volume=0 dB ctrl_stalls=0")
        return "unknown command (try help)"

    def close(self) -> None:
        self.closed = True


class FakeHid:
    """HID transport of the open firmware (src/proto/proto.c): 0x94 -> A1 edit
    buffer, 0x80..0x86 module writes (a new type loads defaults = 50), 0x00."""

    def __init__(self) -> None:
        self.edit = bytearray(PRESET_SIZE)
        struct.pack_into("<8H", self.edit, 0x2C, 1, 5, 40, 50, 50, 1, 50, 70)
        self.reader = FrameReader()
        self.out: list[bytes] = []
        self.frames: list[bytes] = []
        self.closed = False

    def reply(self, fn: int, data: bytes) -> None:
        self.out += list(iter_reports(pack_frame(fn, data)))

    def write_report(self, report: bytes) -> None:
        self.reader.feed_report(report)
        while (packet := self.reader.next_packet()) is not None:
            self.frames.append(packet)
            fn, data = packet[0], packet[1:]
            if fn == protocol.CMD_CONNECT:
                self.reply(0xA1, bytes([3]) + bytes(self.edit))
                self.reply(0xB0, bytes(13))           # the rest of the dump
            elif fn == protocol.CMD_GET_VERSION:
                self.reply(0x01, version_payload())
            elif 0x80 <= fn <= 0x86:
                name = next(n for n, m in MODULES.items() if m[0] == fn - 0x80)
                _, off, fields = MODULES[name]
                values = list(struct.unpack(f"<{len(fields)}H", data))
                if values[1] != struct.unpack_from("<H", self.edit, off + 2)[0]:
                    values[2:] = [50] * (len(fields) - 2)
                struct.pack_into(f"<{len(fields)}H", self.edit, off, *values)

    def read_report(self, timeout_ms: int = 500) -> bytes | None:
        return self.out.pop(0) if self.out else None

    def close(self) -> None:
        self.closed = True


@pytest.fixture
def rig():
    con, hid = FakeConsole(), FakeHid()
    tools = PedalTools(Pedal(console_factory=lambda: con,
                             device_factory=lambda: FB200Device(hid)))
    return tools, con, hid


def test_status_and_preset(rig):
    tools, con, _ = rig
    status = tools.pedal_status()
    assert con.sent == ["fwinfo", "stats", "usb", "stock", "preset"]
    assert status["preset_parsed"] == {"index": 3, "amp": {"en": 1, "model": 5, "gain": 40},
                                       "cab": {"en": 1, "type": 2}, "mod": {"en": 0, "type": 1},
                                       "rev": {"en": 1, "type": 3}, "master": 80}
    assert tools.preset(7)["index"] == 3 and con.sent[-1] == "preset 7"
    with pytest.raises(InvalidArgumentError):
        tools.preset(40)


def test_console_raw_and_flash_refused(rig):
    tools, con, _ = rig
    assert tools.console("bogus") == "unknown command (try help)"
    for cmd in ("fwbegin 100 0x1234", "fwrec 1 2", "fwstock 1 2"):
        with pytest.raises(InvalidArgumentError, match="fb200 update"):
            tools.console(cmd)
    assert con.sent == ["bogus"]


def test_delay_fills_positional_args(rig):
    tools, con, _ = rig
    out = tools.set_delay(on=True, mix=40)
    assert con.sent == ["delay", "delay on 500 30 40"]
    assert out == {"on": True, "plays": True, "time_ms": 500, "feedback": 30, "mix": 40,
                   "lowcut": 0, "lowcut_hz": 20, "tone": 100}
    con.sent.clear()
    tools.set_delay(on=False)
    assert con.sent == ["delay off"]
    con.sent.clear()
    tools.set_delay(tone=60)
    assert con.sent == ["delay", "delay 500 30 40 0 60"]


def test_eq_commands_and_state(rig):
    tools, con, _ = rig
    st = tools.set_eq()
    assert con.sent == ["eq"]
    assert not st["on"] and st["hpf_hz"] == 0 and st["lpf_hz"] == 0
    assert [b["freq_hz"] for b in st["bands"]] == [40, 100, 250, 800, 3000]
    con.sent.clear()
    st = tools.set_eq(on=True, hpf_hz=45, lpf_hz=6000, band=2, gain_db=-4.5, q=1.4)
    # the band's omitted freq comes from the state; `on` goes last
    assert con.sent == ["eq", "eq hpf 45", "eq lpf 6000", "eq 2 100 -4.5 1.4", "eq on"]
    assert st["on"] and not st["flat"] and st["hpf_hz"] == 45 and st["lpf_hz"] == 6000
    assert st["bands"][1] == {"band": 2, "freq_hz": 100, "gain_db": -4.5, "q": 1.4}
    con.sent.clear()
    st = tools.set_eq(band=2, freq_hz=120)
    assert con.sent == ["eq", "eq 2 120 -4.5 1.4"] and st["bands"][1]["freq_hz"] == 120
    con.sent.clear()
    assert tools.set_eq(hpf_hz=0, on=False)["hpf_hz"] == 0
    assert con.sent == ["eq hpf 0", "eq off"]
    for bad in ({"band": 6, "gain_db": 1}, {"gain_db": 3}, {"band": 1, "q": -1}):
        with pytest.raises(InvalidArgumentError):
            tools.set_eq(**bad)


def test_parse_eq_firmware_text():
    text = ("eq on (flat): hpf off lpf 20000 Hz\r\n  1: 40 Hz +0.00 dB q 1.00\r\n"
            "  2: 100 Hz -4.25 dB q 0.72\r\n  3: 250 Hz +0.00 dB q 1.00\r\n"
            "  4: 800 Hz +0.00 dB q 1.00\r\n  5: 3000 Hz +15.00 dB q 4.00\r\n")
    st = parse_eq(text)
    assert st["on"] and st["flat"] and st["hpf_hz"] == 0 and st["lpf_hz"] == 20000
    assert st["bands"][1] == {"band": 2, "freq_hz": 100, "gain_db": -4.25, "q": 0.72}
    assert st["bands"][4]["gain_db"] == 15.0


def test_usb_route_and_cab_long(rig):
    tools, con, _ = rig
    assert tools.usb_route()["route"] == "out"
    assert tools.usb_route("in")["route"] == "in" and con.usb_route == "in"
    assert con.sent == ["usb", "usb in"]
    with pytest.raises(InvalidArgumentError):
        tools.usb_route("dac")
    assert tools.cab_long(512)["text"] == "cab long 512: ok"
    with pytest.raises(InvalidArgumentError, match="not available"):
        tools.cab_long(4096)
    assert tools.cab_long(0)["taps"] == 0 and con.sent[-1] == "cab long 0"
    for bad in (-1, 4097):
        with pytest.raises(InvalidArgumentError):
            tools.cab_long(bad)


def test_output_gain_steps_to_negative(rig):
    tools, con, _ = rig
    tools.set_output(gain_db=-12, mute=True)
    assert con.sent[:3] == ["gain", "gain", "mute on"]
    tools.set_output(gain_db=3)
    assert "gain 3" in con.sent
    with pytest.raises(InvalidArgumentError):
        tools.set_output(gain_db=-5)


def test_drums_tuner_prof_crash(rig):
    tools, con, _ = rig
    d = tools.drums(on=True, rhythm=7, bpm=120)
    assert con.sent == ["drums 7", "drums bpm 120", "drums on"]
    assert d["on"] and d["rhythm"] == 7 and d["bpm"] == 120 and d["level"] == 60
    assert tools.tuner()["freq"] == 41.2
    prof = tools.cpu_profile()
    assert prof["stages"]["cab"] == 5000 and prof["total"] == 8350
    assert prof["load_pct"] == 8.2 and prof["blocks"] == 4000
    con.sent.clear()
    assert tools.crash_dump(clear=True) == "no crash dump"
    assert con.sent == ["crashdump", "crashclear"]


def test_looper_state_and_actions(rig):
    tools, con, _ = rig
    st = tools.looper()
    assert con.sent == ["loop"]
    assert st["state"] == "empty" and st["max_ms"] == 108299 and st["flash"]
    st = tools.looper(level=80, action="rec")
    assert con.sent[1:] == ["loop level 80", "loop rec"]
    assert st["state"] == "rec" and st["level"] == 80 and st["prepared_ms"] == 2310
    assert tools.looper("tap")["state"] == "play"
    with pytest.raises(InvalidArgumentError, match="not allowed"):
        tools.looper("rec")                        # clear first
    assert tools.looper("clear")["state"] == "empty"
    with pytest.raises(InvalidArgumentError):
        tools.looper(level=101)
    with pytest.raises(InvalidArgumentError):
        tools.looper("record")                     # type: ignore[arg-type]


def test_parse_loop_firmware_text():
    st = parse_loop("loop dub: len_ms=4210 pos_ms=1234 max_ms=108299 undo_max_ms=54294 undo=redo "
                    "level=100 prep_ms=5200 flash=1")
    assert st == {"state": "dub", "len_ms": 4210, "pos_ms": 1234, "max_ms": 108299,
                  "undo_max_ms": 54294, "undo": "redo", "level": 100, "prepared_ms": 5200,
                  "flash": True}
    with pytest.raises(CommunicationError):
        parse_loop("loop what")


def test_usage_reply_is_an_error(rig):
    tools, _, _ = rig
    with pytest.raises(InvalidArgumentError, match="usage"):
        tools.pedal.check("gain x")


def test_console_reopens_after_error():
    opened = []

    class Broken(FakeConsole):
        def run(self, cmd, max_s=5.0):
            raise OSError("device gone")

    def factory():
        opened.append(Broken())
        return opened[-1]

    pedal = Pedal(console_factory=factory)
    for _ in range(2):
        with pytest.raises(CommunicationError, match="device gone"):
            pedal.run("stats")
    assert len(opened) == 2 and opened[0].closed


def test_set_amp_over_hid(rig):
    tools, _, hid = rig
    assert tools.get_effects()["amp"] == {"enabled": 1, "model": 5, "gain": 40, "bass": 50,
                                         "mid": 50, "midfreq": 1, "treble": 50, "volume": 70}
    hid.frames.clear()
    amp = tools.set_amp(gain=66, enabled=False)
    writes = [f for f in hid.frames if f[0] == 0x82]
    assert writes == [bytes([0x82]) + struct.pack("<8H", 0, 5, 66, 50, 50, 1, 50, 70)]
    assert amp["gain"] == 66 and amp["enabled"] == 0
    # a new model loads its defaults first; the given knobs apply on top
    hid.frames.clear()
    amp = tools.set_amp(model=9, treble=80)
    writes = [f for f in hid.frames if f[0] == 0x82]
    assert len(writes) == 2
    assert amp == {"enabled": 0, "model": 9, "gain": 50, "bass": 50, "mid": 50, "midfreq": 50,
                   "treble": 80, "volume": 50}
    assert hid.closed


def test_set_blocks_frame_bytes(rig):
    tools, _, hid = rig
    tools.set_gate(enabled=True, threshold=30)
    tools.set_reverb(level=20)
    tools.set_cab(type=12)
    fns = [f[0] for f in hid.frames if f[0] >= 0x80 and f[0] != 0x94]
    assert fns == [0x81, 0x86, 0x83]
    gate = next(f for f in hid.frames if f[0] == 0x81)
    assert gate[1:] == struct.pack("<3H", 1, 0, 30)
    with pytest.raises(InvalidArgumentError):
        FB200Device(hid).set_module("amp", {"bogus": 1})


def test_pedal_info_over_hid(rig):
    tools, _, _ = rig
    assert tools.pedal_info()["firmware_version"] == "V1.0.1"


class FakeAudio:
    FS = 44100

    def __init__(self) -> None:
        import numpy as np

        t = np.arange(self.FS) / self.FS
        sine = 0.1 * np.sin(2 * np.pi * 1000 * t) + 0.001 * np.sin(2 * np.pi * 2000 * t)
        self.capture = np.stack([sine, sine], axis=1)
        self.played = None

    def record(self, seconds):
        return self.capture

    def play_record(self, x):
        self.played = x
        return self.capture

    def __getattr__(self, name):
        from fb200 import audio

        return getattr(audio, name)


def test_audio_test_chain_sine(rig, tmp_path):
    pytest.importorskip("numpy")
    tools, con, _ = rig
    tools._audio = FakeAudio()
    tools.settle_s = 0
    out = tools.audio_test(save_wav=str(tmp_path / "cap.wav"))
    assert con.sent == ["tin sine 1000", "tin off"]
    assert out["left"]["peak_dbfs"] == pytest.approx(-20, abs=0.2)
    assert out["thd"]["fundamental_hz"] == pytest.approx(1000, abs=1)
    assert out["thd"]["thd_pct"] == pytest.approx(1.0, abs=0.05)
    assert (tmp_path / "cap.wav").read_bytes()[:4] == b"RIFF"
    with pytest.raises(InvalidArgumentError):
        tools.audio_test(signal="sweep")


class ReampAudio(FakeAudio):
    """The pedal as a delay line: play_record returns the played signal late."""

    delay = 123

    def play_record(self, x):
        import numpy as np

        self.played = x
        y = np.concatenate([np.zeros(self.delay), x, np.zeros(self.FS // 2 - self.delay)])
        return np.stack([y, y], axis=1)


def test_audio_test_usb_reamps_and_restores_the_route(rig):
    pytest.importorskip("numpy")
    tools, con, _ = rig
    tools._audio = ReampAudio()
    con.usb_route = "mix"
    out = tools.audio_test(signal="sweep", source="usb", seconds=1.0)
    assert con.sent == ["usb", "usb in", "usb mix"]
    assert con.usb_route == "mix"
    assert out["delay_ms"] == pytest.approx(1000 * 123 / 44100, abs=0.01)
    assert all(abs(v) < 1.5 for k, v in out["response_db"].items() if 63 <= int(k) <= 8000)
    assert "delay_ms" not in tools.audio_test(signal="sine", source="usb", seconds=1.0)


def test_audio_test_usb_restores_the_route_on_error(rig):
    pytest.importorskip("numpy")
    tools, con, _ = rig

    class Broken(FakeAudio):
        def play_record(self, x):
            raise OSError("stream died")

    tools._audio = Broken()
    with pytest.raises(OSError):
        tools.audio_test(signal="noise", source="usb", seconds=1.0)
    assert con.sent == ["usb", "usb in", "usb out"]
    assert con.usb_route == "out"


def test_audio_test_usb_needs_the_route_command(rig):
    pytest.importorskip("numpy")
    tools, con, _ = rig
    tools._audio = FakeAudio()
    con.run = lambda cmd, max_s=5.0: con.sent.append(cmd) or "usb: spk_alt=0 mic_alt=0"
    with pytest.raises(CommunicationError, match="usb in"):
        tools.audio_test(signal="sweep", source="usb", seconds=1.0)
    assert con.sent == ["usb"]


def test_delay_frames():
    np = pytest.importorskip("numpy")
    from fb200 import audio

    x = audio.make_signal("noise", 0.5)
    assert audio.delay_frames(x, np.concatenate([np.zeros(77), x])) == 77
    assert audio.delay_frames(x, x) == 0


def test_audio_analysis_noise_and_sweep():
    np = pytest.importorskip("numpy")
    from fb200 import audio

    noise = audio.make_signal("noise", 2.0)
    flat = audio.analyze(np.stack([noise, noise], axis=1), "noise")
    assert all(abs(v) < 1.5 for v in flat["response_db"].values())
    # 3 dB/octave cut to the highs: a one-pole low-pass at ~500 Hz
    lp = np.copy(noise)
    a = np.exp(-2 * np.pi * 500 / audio.FS)
    for i in range(1, len(lp)):
        lp[i] = (1 - a) * noise[i] + a * lp[i - 1]
    resp = audio.analyze(lp[:, None], "noise")["response_db"]
    assert resp["125"] > 5 and resp["8000"] < -15
    sweep = audio.make_signal("sweep", 2.0)
    resp = audio.analyze(sweep[:, None], "sweep", seconds=2.0)["response_db"]
    assert all(abs(v) < 1.5 for k, v in resp.items() if 63 <= int(k) <= 8000)
    assert audio.analyze(np.zeros((1000, 2)), "sine")["warning"]


def test_find_device_by_name():
    pytest.importorskip("numpy")
    from fb200 import audio
    from fb200.errors import DeviceNotFoundError

    class SD:
        @staticmethod
        def query_devices():
            return [{"name": "MacBook Mic", "max_input_channels": 1, "max_output_channels": 0},
                    {"name": "FB200 Audio", "max_input_channels": 2, "max_output_channels": 2}]

    assert audio.find_device(sd=SD) == 1
    assert audio.find_device(output=True, sd=SD) == 1
    with pytest.raises(DeviceNotFoundError, match="MacBook Mic"):
        audio.find_device("Nope", sd=SD)


def test_parsers_reject_garbage():
    with pytest.raises(CommunicationError):
        parse_delay("unknown command")
    with pytest.raises(CommunicationError):
        parse_eq("eq on: hpf off lpf off")               # no bands
    with pytest.raises(CommunicationError):
        parse_profile("")


def _server(rig):
    pytest.importorskip("mcp")
    return build_server(rig[0])


def test_server_lists_tools(rig):
    server = _server(rig)
    tools = asyncio.run(server.list_tools())
    names = {t.name for t in tools}
    assert names == set(PedalTools.TOOLS)
    desc = next(t for t in tools if t.name == "console").description
    assert "crashdump" in desc and "fwbegin" in desc
    assert "eq <band 1-5>" in desc and "cab long" in desc and "usb out|in|mix" in desc


def test_server_call_and_tool_error(rig):
    server = _server(rig)
    result = asyncio.run(server.call_tool("set_delay", {"on": True, "time_ms": 250}))
    assert "250" in str(result)
    assert rig[1].sent[-1] == "delay on 250"
    with pytest.raises(_sdk()[1], match="fb200 update"):   # the client sees isError
        asyncio.run(server.call_tool("console", {"command": "fwbegin 1 2"}))


def test_busy_console_port_is_a_clear_tool_error():
    pytest.importorskip("mcp")
    """Another program holds the console: the tool says which port and what to do,
    and the next call works once the port is free."""
    pty = pytest.importorskip("pty")
    import os

    from fb200.console import Console

    master, slave = pty.openpty()
    port = os.ttyname(slave)
    holder = Console(port)
    try:
        tools = PedalTools(Pedal(port))
        with pytest.raises(CommunicationError, match=f"{port} is busy.*Close it and retry"):
            tools.pedal.run("stats", timeout=0.2)
        server = build_server(tools)
        with pytest.raises(Exception, match="is busy"):
            asyncio.run(server.call_tool("pedal_status", {}))
        holder.close()
        holder = None
        assert tools.pedal.run("stats", timeout=0.2) == ""     # opens now (no pedal answers)
        tools.pedal.close()
    finally:
        if holder is not None:
            holder.close()
        os.close(master)
        os.close(slave)
