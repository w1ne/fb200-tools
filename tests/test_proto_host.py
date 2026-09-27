"""Host tests for the firmware app protocol (firmware/audio/src/proto).

The C harness (firmware/audio/tests/proto_host_test.c) runs proto.c on top of
the real UI edit buffer (ui.c) and an in-memory flash. Frames are built with
fb200.protocol.pack_frame, and the real FB200Device client talks to the
harness through a HID-report transport, so the client and the firmware are
checked against each other."""

from __future__ import annotations

import shutil
import struct
import subprocess
from pathlib import Path

import pytest

from fb200.pedal import FB200Device
from fb200.protocol import crc16, iter_reports, pack_frame

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"
OUT = FW / "build" / "proto_host_test"

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="host C compiler not installed")

PRESET_FLASH = 0x71000
SETTINGS_FLASH = 0x80000


@pytest.fixture(scope="module")
def harness_bin() -> Path:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
         "-I", str(FW / "src"), str(FW / "tests" / "proto_host_test.c"),
         str(FW / "src" / "proto" / "proto.c"), str(FW / "src" / "ui" / "ui.c"),
         str(FW / "src" / "ui" / "lightbar.c"),
         "-o", str(OUT)],
        check=True,
    )
    return OUT


class Harness:
    def __init__(self, binary: Path) -> None:
        self.p = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)

    def cmd(self, line: str) -> list[str]:
        assert self.p.stdin and self.p.stdout
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        out = []
        while True:
            got = self.p.stdout.readline()
            assert got, "harness died"
            got = got.rstrip("\n")
            if got == ".":
                return out
            out.append(got)

    def feed(self, data: bytes, t: str = "u") -> list[str]:
        return self.cmd(f"feed {t} {data.hex()}")

    def send(self, fn: int, data: bytes = b"", t: str = "u") -> list[tuple[str, int, bytes]]:
        return frames(self.feed(pack_frame(fn, data), t))

    def value(self, line: str) -> bytes:
        (out,) = [x for x in self.cmd(line) if not x.startswith("HOOK")]
        return bytes.fromhex(out.split()[1])

    def edit(self) -> bytes:
        return self.value("edit")

    def settings(self) -> bytes:
        return self.value("settings")

    def flash(self, off: int, n: int) -> bytes:
        return self.value(f"flash {off:x} {n}")

    def close(self) -> None:
        if self.p.stdin:
            self.p.stdin.close()
        self.p.wait(timeout=5)


def parse(frame: bytes) -> tuple[int, bytes]:
    assert frame[:2] == b"\xaa\x55"
    length = int.from_bytes(frame[2:4], "little")
    assert len(frame) == length + 6
    assert crc16(frame[2:4 + length]) == frame[4 + length:]
    return frame[4], frame[5:4 + length]


def frames(lines: list[str]) -> list[tuple[str, int, bytes]]:
    out = []
    for line in lines:
        if line.startswith("TX "):
            _, t, hexs = line.split()
            fn, payload = parse(bytes.fromhex(hexs))
            out.append((t, fn, payload))
    return out


@pytest.fixture
def h(harness_bin):
    harness = Harness(harness_bin)
    yield harness
    harness.close()


class HarnessTransport:
    """HID transport (64-byte reports, [len][data]) into the harness."""

    def __init__(self, harness: Harness) -> None:
        self.h = harness
        self.pending: list[bytes] = []

    def write_report(self, report: bytes) -> None:
        for line in self.h.feed(report[1:1 + report[0]]):
            if line.startswith("TX u "):
                self.pending.extend(iter_reports(bytes.fromhex(line.split()[2])))

    def read_report(self, timeout_ms: int = 500) -> bytes | None:
        return self.pending.pop(0) if self.pending else None

    def close(self) -> None:
        pass


# ---------------------------------------------------------------- framing

def test_version_reply_matches_stock_layout(h):
    (reply,) = h.send(0x00)
    t, fn, p = reply
    assert (t, fn, len(p)) == ("u", 0x01, 55)
    assert p[:32].rstrip(b"\0") == b"FB200"
    assert p[32:39] == b"V1.0.0\0" and p[46:53] == b"V1.0.0\0" and p[53:55] == b"A\0"
    assert p[39:46].startswith(b"V")
    info = FB200Device(HarnessTransport(h)).info()
    assert info.product == "FB200" and info.hardware_rev == "A"


def test_reply_goes_to_the_requesting_transport(h):
    (reply,) = h.send(0x00, t="b")
    assert reply[0] == "b" and reply[1] == 0x01


def test_byte_by_byte_garbage_and_bad_crc(h):
    good = pack_frame(0x00)
    bad = bytearray(good)
    bad[-1] ^= 0xFF
    stream = b"\x00\x55\xaa\x13" + bytes(bad) + b"\xaa" + good + good
    out = []
    for b in stream:
        out += frames(h.feed(bytes([b])))
    assert [fn for _, fn, _ in out] == [0x01, 0x01]


def test_frame_split_across_feeds_and_two_frames_in_one(h):
    good = pack_frame(0xFA)
    assert frames(h.feed(good[:3])) == []
    (r,) = frames(h.feed(good[3:]))
    assert r[1] == 0xFB and r[2][:5] == b"FB200" and len(r[2]) == 41
    both = frames(h.feed(pack_frame(0x00) + pack_frame(0xFA)))
    assert [fn for _, fn, _ in both] == [0x01, 0xFB]


def test_identify_byte_and_oversize_frames_are_ignored(h):
    assert h.send(0x11, b"\x00") == []                       # identify byte 0x11 + fn 00
    huge = b"\xaa\x55" + (0x3FB).to_bytes(2, "little") + bytes(0x3FB + 2)
    assert frames(h.feed(huge + pack_frame(0x00)))[0][1] == 0x01   # resyncs after it


# ---------------------------------------------------------------- presets

def test_select_preset_notifies_ble(h):
    out = h.send(0x98, bytes([6]))
    assert [(t, fn) for t, fn, _ in out] == [("b", 0x98), ("b", 0x83)]
    assert out[0][2] == bytes([6])
    assert h.cmd("index") == ["IDX 6"]
    assert h.edit()[:9] == b"Preset 06"
    assert h.settings()[0x16] == 6 and h.settings()[0x21] == 2 and h.settings()[0x22] == 1
    h.cmd("notify 3")
    out = h.send(0x98, bytes([7]))
    assert sorted(t for t, fn, _ in out if fn == 0x98) == ["b", "u"]
    assert h.send(0x98, bytes([40])) == []


def test_read_and_write_preset(h):
    (r,) = h.send(0x96, bytes([5]))
    assert r[1] == 0x97 and r[2][0] == 5 and r[2][1:] == h.flash(PRESET_FLASH + 5 * 0x200, 256)
    new = bytearray(256)
    new[:6] = b"MyTone"
    struct.pack_into("<HH", new, 0x2C, 1, 4)
    out = h.send(0x97, bytes([12]) + bytes(new))
    assert [(fn, p) for _, fn, p in out] == [(0x99, b"\x01"), (0x98, bytes([12]))]
    assert h.flash(PRESET_FLASH + 12 * 0x200, 256) == bytes(new)
    assert h.edit() == bytes(new) and h.cmd("index") == ["IDX 12"]
    live = bytearray(new)
    live[:4] = b"Live"
    assert h.send(0x97, b"\xff" + bytes(live)) == []                 # edit buffer only
    assert h.edit() == bytes(live)
    assert h.flash(PRESET_FLASH + 12 * 0x200, 256) == bytes(new)


def test_rename_preset(h):
    out = h.send(0x99, bytes([3]) + b"Renamed".ljust(20, b"\0"))
    assert [(t, fn) for t, fn, _ in out] == [("b", 0x97), ("b", 0xA1)]
    assert h.flash(PRESET_FLASH + 3 * 0x200, 20) == b"Renamed".ljust(20, b"\0")
    assert out[1][2][0] == 3 and out[1][2][1:8] == b"Renamed"


def test_module_writes_clamps_and_defaults(h):
    h.send(0x98, bytes([0]))
    e = h.edit()
    amp_type = struct.unpack_from("<H", e, 0x2E)[0]
    # same amp type: payload taken as is (clamped), no echo
    out = h.send(0x82, struct.pack("<8H", 1, amp_type, 10, 20, 30, 1, 40, 150))
    assert out == []
    assert struct.unpack_from("<8H", h.edit(), 0x2C) == (1, amp_type, 10, 20, 30, 1, 40, 100)
    # new amp type: stock defaults for the model are loaded and echoed to BLE
    (echo,) = h.send(0x82, struct.pack("<8H", 1, 3, 0, 0, 0, 0, 0, 0))
    assert echo[:2] == ("b", 0x82)
    assert struct.unpack("<8H", echo[2]) == (1, 3, 100, 35, 50, 1, 40, 65)
    # delay time clamp 40..2500 without a type change
    dly_type = struct.unpack_from("<H", h.edit(), 0x8E)[0]
    h.send(0x85, struct.pack("<5H", 1, dly_type, 101, 50, 3000))
    assert struct.unpack_from("<5H", h.edit(), 0x8C) == (1, dly_type, 100, 50, 2500)
    h.send(0x85, struct.pack("<5H", 1, dly_type, 1, 2, 10))
    assert struct.unpack_from("<H", h.edit(), 0x94)[0] == 40
    # 0x80 (no defaults), enable > 1 -> 1, type > 21 -> 1
    assert h.send(0x80, struct.pack("<6H", 7, 99, 1, 2, 3, 4)) == []
    assert struct.unpack_from("<6H", h.edit(), 0x14) == (1, 1, 1, 2, 3, 4)
    # module order
    h.send(0xA0, bytes([1, 2, 3, 4, 5, 6, 7]))
    assert h.edit()[0xBC:0xC3] == bytes([1, 2, 3, 4, 5, 6, 7])


def test_sync_dump_on_connect(h):
    out = h.send(0x94, t="b")
    assert [fn for _, fn, _ in out] == [0xA1, 0xB0, 0xB7, 0xBA, 0xBB, 0xB5, 0x83, 0xC9]
    assert all(t == "b" for t, _, _ in out)
    d = {fn: p for _, fn, p in out}
    assert d[0xA1][0] == 0 and d[0xA1][1:] == h.edit()
    assert len(d[0xB0]) == 13 and d[0xB0][10] == 1 and d[0xB0][12] == 100
    assert d[0xBA] == bytes([0, 0, 0, 100, 110, 0])
    assert d[0xBB] == bytes([75, 1]) and d[0xB5] == b"\x01"
    assert d[0x83] == h.edit()[0x44:0x50]


# ---------------------------------------------------------------- settings

def test_settings_block_and_hooks(h):
    lines = h.feed(pack_frame(0xB0, bytes([9, 0, 13, 20, 30, 40, 99, 5, 1, 1, 0, 7, 80])))
    assert "HOOK bt_enable 0" in lines
    (cab,) = frames(lines)
    assert cab[1] == 0x83 and struct.unpack_from("<H", cab[2], 0)[0] == 0   # global cab off
    s = h.settings()
    assert s[0x19] == 0 and s[0x1A:0x1F] == bytes([13, 20, 30, 40, 9]) and s[0x17] == 0
    assert s[0x24] == 7 and s[0x28] == 80 and s[0x2D] == 1
    lines = h.feed(pack_frame(0xB3, b"FB200MyBass".ljust(20, b"\0")))
    assert any(x.startswith("HOOK bt_name 464232") for x in lines)
    assert h.flash(0x83000, 20) == b"FB200MyBass".ljust(20, b"\0")
    h.send(0xBA, bytes([1, 0, 12, 90, 120, 0]))
    h.cmd("tick 3100")                                         # saved 3 s after the change
    assert h.flash(0x81000, 6) == bytes([1, 0, 12, 90, 120, 0])
    h.send(0xDA, bytes(range(30)))
    (aux,) = h.send(0xD9)
    assert aux[1] == 0xDA and aux[2] == bytes(range(30))
    h.send(0xC9, b"\x01")
    assert h.settings()[0x20] == 1


def test_bootloader_and_factory_reset(h):
    lines = h.feed(pack_frame(0xC1))
    # never the vendor bootloader flag: A+D recovery depends on it
    assert "HOOK bootloader" in lines and h.flash(0x86000, 1) != b"\x00"
    assert frames(lines) == [("u", 0xC2, b"\x01")]
    h.cmd("factory 1")
    lines = h.feed(pack_frame(0xB2), "b")
    assert "HOOK factory_reset" in lines
    assert sorted((t, fn, p) for t, fn, p in frames(lines)) == [("b", 0xB2, b"\x01"),
                                                                 ("u", 0xB2, b"\x01")]


def test_factory_reset_refuses_without_factory_presets(h):
    """A version 1 stock data blob has no factory presets: nothing changes."""
    h.cmd("factory 0")
    before = h.flash(PRESET_FLASH, 40 * 0x200)
    assert frames(h.feed(pack_frame(0xB2), "b"))[0][2] == b"\x00"
    assert h.flash(PRESET_FLASH, 40 * 0x200) == before


def test_factory_reset_as_stock(h):
    dev = FB200Device(HarnessTransport(h))
    assert dev.ir_import(2, "User Cab", [0.5] * 1024)
    h.send(0xBA, bytes([1, 0, 12, 90, 120, 0]))
    h.send(0xB0, bytes([0, 1, 20, 13, 13, 13, 13, 9, 0, 1, 0, 0, 100]))   # gain, tuner cal, BT off
    tap(h, "c", "d")
    tap(h, "b")                                                # preset 5
    master = h.settings()[0x18]
    h.cmd("factory 1")
    lines = h.feed(pack_frame(0xB2), "b")
    assert "HOOK bt_enable 1" in lines and frames(lines)[0][2] == b"\x01"
    names = [h.flash(PRESET_FLASH + i * 0x200, 20).rstrip(b"\0") for i in range(40)]
    assert names == [f"Factory {i:02}".encode() for i in range(20)] + [b"EMPTY"] * 20
    s = h.settings()
    assert s[:2] == b"B1" and s[0x17] == 1 and s[0x18] == master and s[0x19] == 1
    assert s[0x1A:0x1F] == bytes([13] * 5) and s[0x2C:0x2F] == bytes([5, 0, 1])
    assert s[0x16] == 0 and s[0x1F] == 0 and s[0x20] == 0
    assert h.flash(SETTINGS_FLASH, 0x31) == s
    assert h.flash(0x81000, 6) == bytes([0, 0, 0, 100, 110, 0]) and drums(h)[:4] == [0, 0, 100, 110]
    assert h.cmd("index") == ["IDX 0"] and h.edit()[:10] == b"Factory 00" and disp(h) == "P0A"
    assert all(s.empty for s in dev.ir_list()) and h.flash(0x87000 + 50, 6) == b"Empty\0"


# ---------------------------------------------------------------- front panel

def tap(h, *sws: str) -> list[str]:
    """Press the switches together, then release them (a single press or a chord)."""
    out = []
    for sw in sws:
        out += h.cmd(f"fsw {sw} press")
    for sw in sws:
        out += h.cmd(f"fsw {sw} release")
    return out


def hold(h, sw: str) -> list[str]:
    """Hold one switch past the 1 s long press, then release it."""
    return h.cmd(f"fsw {sw} press") + h.cmd(f"fsw {sw} long") + h.cmd(f"fsw {sw} release")


def disp(h) -> str:
    return h.cmd("disp")[0][5:]


def edited(h, name: bytes) -> bytes:
    """Put an edited preset (only in the edit buffer) on the pedal."""
    e = bytearray(h.edit())
    e[:20] = name.ljust(20, b"\0")
    h.send(0x97, b"\xff" + bytes(e))
    return bytes(e)


def test_hold_any_switch_saves_to_that_slot(h):
    e = edited(h, b"Edited")
    out = frames(hold(h, "c"))
    assert [(t, fn) for t, fn, _ in out] == [("b", 0x97), ("b", 0x98), ("b", 0xB0)]
    assert out[0][2] == bytes([2]) + e and out[1][2] == bytes([2])
    assert h.flash(PRESET_FLASH + 2 * 0x200, 256) == e
    assert h.cmd("index") == ["IDX 2"] and h.edit() == e      # not reloaded
    assert disp(h) == "SAV" and h.settings()[0x16] == 2 and h.settings()[0x21] == 2
    assert h.flash(PRESET_FLASH, 256)[:9] == b"Preset 00"      # the old slot is untouched


def test_hold_saves_in_live_mode_without_toggling(h):
    tap(h, "b", "c")                                           # live mode
    assert disp(h) == "L0A"
    e = edited(h, b"Live Save")
    hold(h, "d")
    assert h.flash(PRESET_FLASH + 3 * 0x200, 256) == e
    assert h.edit() == e                                       # D did not toggle the comp
    assert h.cmd("index") == ["IDX 3"]


def test_bank_chord_keeps_the_edits_and_saves_to_the_shown_bank(h):
    e = edited(h, b"Save As")
    assert frames(tap(h, "c", "d")) == []                      # browse: nothing loaded
    assert disp(h) == "P1A" and h.edit() == e and h.cmd("index") == ["IDX 0"]
    tap(h, "c", "d")
    assert disp(h) == "P2A"
    tap(h, "a", "b")
    hold(h, "b")                                               # bank 1, slot B
    assert h.flash(PRESET_FLASH + 5 * 0x200, 256) == e and h.cmd("index") == ["IDX 5"]
    assert h.flash(PRESET_FLASH, 256)[:9] == b"Preset 00"


def test_bank_browse_loads_from_the_shown_bank_or_times_out(h):
    tap(h, "a", "b")                                           # bank 0 -> 9
    h.cmd("tick 150")
    assert disp(h) == "   "                                    # the stock flashes the bank
    h.cmd("tick 150")
    assert disp(h) == "P9A"
    tap(h, "b")
    assert h.cmd("index") == ["IDX 37"] and h.edit()[:9] == b"Preset 37"
    tap(h, "c", "d")                                           # 9 -> 0, not loaded
    h.cmd("tick 1000")
    h.cmd("tick 1000")                                         # stock: ~1.7 s
    assert disp(h) == "P9b" and h.cmd("index") == ["IDX 37"]
    tap(h, "a")
    assert h.cmd("index") == ["IDX 36"]


def drums(h) -> list[int]:
    """on, rhythm 0..39, level, bpm, last tap time"""
    return [int(v) for v in h.cmd("drums")[0].split()[1:]]


def test_app_drum_and_mode_commands_act_live(h):
    assert h.send(0xBA, bytes([1, 0, 12, 90, 120, 0])) == []      # no echo
    assert drums(h)[:4] == [1, 12, 90, 120]
    h.send(0xBA, bytes([0, 0, 50, 150, 0x2C, 1]))                  # clamped like the stock
    assert drums(h)[:4] == [0, 0, 100, 300]
    h.send(0xC9, b"\x01")                                          # rhythm mode from the app
    assert disp(h) == "d01"
    out = frames(tap(h, "b"))                                      # BA carries the live values
    assert [(t, fn, p) for t, fn, p in out] == [("b", 0xBA, bytes([0, 0, 1, 100, 0x2C, 1]))]
    h.send(0xC9, b"\x00")
    assert disp(h) == "P0A"
    h.send(0xB8, bytes([5, 0, 1, 1]))                              # tuner on from the app
    assert disp(h) == " - "
    assert [fn for _, fn, _ in frames(tap(h, "a"))] == [0xB0]
    assert disp(h) == "P0A" and h.settings()[0x2D] == 0            # any switch leaves it
    s = h.settings()
    h.send(0xB0, bytes([0, 1, 13, 13, 13, 13, 13, 5, 1, 1, 1, 0, 100]))   # tuner on via B0
    assert disp(h) == " - " and h.settings()[0x2D] == 1 and s[0x2D] == 0


def test_rhythm_mode_buttons_as_stock(h):
    h.cmd("fsw c press")
    h.cmd("fsw b press")
    h.cmd("fsw b long")                                        # C held + B long
    h.cmd("fsw b release")
    h.cmd("fsw c release")
    assert disp(h) == "d01" and drums(h)[:2] == [0, 0]
    tap(h, "a")                                                # A: rhythm - 1, wraps
    assert drums(h)[1] == 39 and disp(h) == "d40"
    tap(h, "b")
    tap(h, "b")                                                # B: rhythm + 1
    assert drums(h)[1] == 1 and disp(h) == "d02"
    h.cmd("tick 500")
    tap(h, "c")                                                # C: tap tempo
    assert drums(h)[4] > 1000
    tap(h, "d")                                                # D: play / stop
    assert drums(h)[0] == 1
    tap(h, "d")
    assert drums(h)[0] == 0


def test_live_mode_c_turns_amp_and_cab_off_if_either_is_on(h):
    tap(h, "b", "c")                                           # live mode
    e = bytearray(h.edit())
    struct.pack_into("<H", e, 0x2C, 0)                         # amp off, cab on
    h.send(0x97, b"\xff" + bytes(e))
    out = frames(tap(h, "c"))
    assert [fn for _, fn, _ in out] == [0x82, 0x83]
    assert struct.unpack_from("<H", h.edit(), 0x2C)[0] == 0 and struct.unpack_from("<H", h.edit(), 0x44)[0] == 0
    tap(h, "c")                                                # both off -> both on
    assert struct.unpack_from("<H", h.edit(), 0x2C)[0] == 1 and struct.unpack_from("<H", h.edit(), 0x44)[0] == 1


def test_knob_leds_off_in_tuner_mode(h):
    h.cmd("tick 10")
    assert h.cmd("leds")[0][5 + 14] == "1"                     # MASTER LED is always on
    h.send(0xB8, bytes([5, 0, 1, 1]))
    h.cmd("tick 10")
    assert h.cmd("leds")[0] == "LEDS " + "0" * 16
    tap(h, "a")                                                # leave the tuner
    h.cmd("tick 10")
    assert h.cmd("leds")[0][5 + 14] == "1"


# ------------------------------------------------------ footswitch light rings
# LEDs 0-9 are in the D dome, 10-19 C, 20-29 B, 30-39 A (camera, UI_AND_STORAGE §3).
RING_FIRST = {"a": 0, "b": 10, "c": 20, "d": 30}
OFF = (0, 0, 0)
RED = (0x3F, 0, 0)          # the stock sends every byte >> 2 (0x17d54)


def rings(h) -> tuple[dict[str, tuple[int, int, int]], int]:
    """The colour (r, g, b) on the wire of each ring (all 10 LEDs equal), and the frame count."""
    _, px, n = h.cmd("rgb")[0].split()
    leds = [tuple(bytes.fromhex(px[i * 6:i * 6 + 6])) for i in range(40)]
    out = {}
    for sw, first in RING_FIRST.items():
        ring = leds[first:first + 10]
        assert len(set(ring)) == 1, f"ring {sw} is not one colour: {ring}"
        out[sw] = ring[0]
    return out, int(n)


def ring_colours(h) -> list[tuple[int, int, int]]:
    return [rings(h)[0][sw] for sw in "abcd"]


def set_light(h, colour: int, level: int) -> None:
    """B0 from the app: [11] = S+0x24+slot (colour), [12] = S+0x28+slot (level)."""
    s = h.settings()
    h.send(0xB0, bytes([s[0x16], s[0x19], *s[0x1A:0x1F], *s[0x2C:0x2F], s[0x17], colour, level]))


def test_light_ring_preset_mode_shows_the_loaded_slot(h):
    h.cmd("tick 20")
    assert ring_colours(h) == [RED, OFF, OFF, OFF]             # default colour 0, level 100
    tap(h, "c")
    h.cmd("tick 20")
    assert ring_colours(h) == [OFF, OFF, RED, OFF]
    tap(h, "a", "b")                                           # bank browse: the loaded slot stays
    h.cmd("tick 20")
    assert ring_colours(h) == [OFF, OFF, RED, OFF]


@pytest.mark.parametrize("index, rgb", [
    (0, 0xFF0000), (1, 0xFF3300), (2, 0xFFFF00), (3, 0x00FF00), (4, 0x00FFFF),
    (5, 0x0000FF), (6, 0xFF00FF), (7, 0xFF0330), (8, 0xFFFFFF), (9, 0x420000), (72, 0xFFFFFF),
])
def test_light_ring_colours_from_the_app(h, index, rgb):
    set_light(h, index, 100)
    h.cmd("tick 20")
    want = tuple(((rgb >> sh) & 0xFF) >> 2 for sh in (16, 8, 0))
    assert ring_colours(h) == [want, OFF, OFF, OFF]


@pytest.mark.parametrize("level, want", [(100, 0x3F), (50, 0x29), (0, 0x13), (250, 0x3F)])
def test_light_ring_level_scales_30_to_100_percent(h, level, want):
    # stock 0x17908: byte * (u8)(30 + level * 0.7) / 100, then >> 2
    set_light(h, 8, level)
    h.cmd("tick 20")
    assert ring_colours(h)[0] == (want, want, want)


def test_light_ring_colour_is_per_slot(h):
    set_light(h, 5, 100)                                       # slot A blue
    tap(h, "b")
    set_light(h, 3, 100)                                       # slot B green
    h.cmd("tick 20")
    assert ring_colours(h) == [OFF, (0, 0x3F, 0), OFF, OFF]
    tap(h, "a")
    h.cmd("tick 20")
    assert ring_colours(h) == [(0, 0, 0x3F), OFF, OFF, OFF]


def test_light_rings_live_mode_show_the_modules(h):
    tap(h, "b", "c")                                           # live mode; amp + cab on, rest off
    h.cmd("tick 20")
    assert ring_colours(h) == [OFF, OFF, RED, OFF]
    for sw in "abd":
        tap(h, sw)
    h.cmd("tick 20")
    # stock colours (0x20004a2c..38): A reverb purple, B mod orange, C amp red, D comp green
    assert ring_colours(h) == [(0x20, 0, 0x20), (0x3F, 0x19, 0), RED, (0, 0x3F, 0)]
    tap(h, "c")                                                # amp and cab off
    h.cmd("tick 20")
    assert ring_colours(h)[2] == OFF


def test_light_rings_off_in_tuner_mode(h):
    h.cmd("tick 20")
    h.send(0xB8, bytes([5, 0, 1, 1]))
    h.cmd("tick 20")
    assert ring_colours(h) == [OFF] * 4


def test_light_rings_rhythm_mode(h):
    h.send(0xC9, b"\x01")
    h.send(0xBA, bytes([0, 0, 0, 100, 120, 0]))                # 120 BPM: a beat is 500 ms
    h.cmd("tick 20")                                           # a beat starts here
    h.cmd("tick 100")
    assert ring_colours(h) == [OFF] * 4                        # first half of the beat: off
    h.cmd("tick 200")
    assert ring_colours(h) == [OFF, OFF, RED, OFF]             # second half: C red
    h.cmd("tick 220")
    assert ring_colours(h)[2] == OFF                           # next beat
    h.cmd("fsw a press")
    h.cmd("tick 20")
    assert ring_colours(h)[0] == RED                           # A lit while held
    h.cmd("fsw a release")
    h.cmd("fsw b press")
    h.cmd("tick 20")
    assert ring_colours(h)[:2] == [OFF, RED]
    h.cmd("fsw b release")
    tap(h, "d")                                                # play
    h.cmd("tick 20")
    assert ring_colours(h)[3] == RED
    tap(h, "d")                                                # stop
    h.cmd("tick 20")
    assert ring_colours(h)[3] == OFF


def test_save_blinks_the_ring_for_1_s(h):
    h.cmd("tick 20")
    h.cmd("fsw c press")
    h.cmd("fsw c long")                                        # saved now; the blink starts
    h.cmd("fsw c release")
    assert h.cmd("index") == ["IDX 2"]
    seen = []
    for _ in range(11):
        h.cmd("tick 100")
        seen.append(ring_colours(h)[2])
    # stock 0x67e0: off 0-200 ms, on 200-400, off, on 600-800, off, then the normal ring
    assert seen == [OFF, RED, RED, OFF, OFF, RED, RED, OFF, OFF, RED, RED]
    assert all(c == OFF for c in ring_colours(h)[:2])


def test_light_ring_frames_only_on_change_and_at_most_every_20_ms(h):
    h.cmd("tick 20")
    _, n = rings(h)
    for _ in range(10):
        h.cmd("tick 20")
    assert rings(h)[1] == n                                    # nothing changed: no DMA frame
    tap(h, "b")
    assert rings(h)[1] == n + 1                                # a change goes out at once ...
    tap(h, "c")
    h.cmd("tick 5")
    assert rings(h)[1] == n + 1 and ring_colours(h)[1] == RED  # ... but not within 20 ms
    h.cmd("tick 15")
    assert rings(h)[1] == n + 2 and ring_colours(h)[2] == RED


# ---------------------------------------------------------------- IR slots

def test_ir_import_list_query_delete_with_the_client(h):
    dev = FB200Device(HarnessTransport(h))
    samples = [i / 1024 for i in range(1024)]
    assert dev.ir_import(4, "Test Cab", samples)
    slots = dev.ir_list()
    assert [s.name for s in slots if not s.empty] == ["Test Cab"] and slots[3].name == "Test Cab"
    assert h.flash(0x87000 + 3 * 50, 8) == b"Test Cab" and h.flash(0x88000, 9)[3] == 1
    data = h.flash(0x89000 + 3 * 0x2800, 0x2800)
    assert data[:4096] == b"".join(struct.pack("<f", s) for s in samples)
    assert data[4096:] == bytes(0x2800 - 4096)
    # data read-back chunk 2 (0x63 op 2)
    (r,) = h.send(0x63, bytes([1, 4, 0, 2]))
    assert r[2][:8] == bytes([1, 4, 0, 1, 21, 2, 0x00, 0x02]) and r[2][8:] == data[512:1024]
    # list 0x65 -> 0x66 records of 59 bytes
    (lst,) = h.send(0x65, bytes([1, 3, 0, 5, 0]))
    assert lst[1] == 0x66 and len(lst[2]) == 3 * 59
    rec = lst[2][59:118]
    assert rec[:5] == bytes([1, 4, 0, 1, 2]) and rec[5:13] == b"Test Cab" and rec[55] == 50
    # rename, then delete
    out = h.send(0x67, bytes([1, 4, 0, 2]) + b"Renamed".ljust(50, b"\0"))
    assert [(t, fn) for t, fn, _ in out] == [("u", 0x68), ("b", 0x69)] and out[0][2] == bytes([1, 4, 0, 1])
    assert h.flash(0x87000 + 3 * 50, 7) == b"Renamed"
    assert dev.ir_delete(4)
    assert h.flash(0x88000, 9)[3] == 0 and h.flash(0x87000 + 3 * 50, 6) == b"Empty\0"
    assert all(s.empty for s in dev.ir_list())
    out = h.send(0x67, bytes([1, 4, 0, 2]) + b"X".ljust(50, b"\0"))       # rename empty slot
    assert out[0][2] == bytes([1, 4, 0, 0])


def test_ir_upload_replies_per_frame_and_hook(h):
    frames_sent = []
    name = b"Hook IR"
    frames_sent.append(bytes([1, 2, 0, 9, 0]) + struct.pack("<H", len(name)) + name)
    for i in range(1, 9):
        frames_sent.append(bytes([1, 2, 0, 9, i]) + struct.pack("<H", 512) + bytes([i]) * 512)
    lines = []
    for i, f in enumerate(frames_sent):
        got = h.feed(pack_frame(0x61, f))
        replies = [x for x in frames(got) if x[1] == 0x62]
        assert replies == [("u", 0x62, bytes([1, 2, 0, i]))]
        lines += got
    assert "HOOK ir 1" in lines
    assert ("b", 0x69) in [(t, fn) for t, fn, _ in frames(lines)]
