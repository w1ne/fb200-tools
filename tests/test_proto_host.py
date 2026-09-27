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
    lines = h.feed(pack_frame(0xB2), "b")
    assert "HOOK factory_reset" in lines
    assert sorted((t, fn, p) for t, fn, p in frames(lines)) == [("b", 0xB2, b"\x01"),
                                                                 ("u", 0xB2, b"\x01")]


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
