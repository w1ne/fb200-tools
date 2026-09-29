"""Host tests for the long IR store (firmware/audio/src/irstore/irstore.c).

The C harness (firmware/audio/tests/irstore_host_test.c) runs irstore.c on
a fake 8 MB flash: the console upload session (irput), the two-copy slot
table, the slot load the engine does for cab types 20..83, delete, and the
chip-size check. The Python codec (fb200.longir) is checked against the C
one both ways."""

from __future__ import annotations

import functools
import random
import shutil
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

import pytest

from fb200 import longir

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "audio"

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="host C compiler not installed")


@functools.cache
def build() -> Path:
    exe = Path(tempfile.mkdtemp(prefix="irstore_host_")) / "irstore_host_test"
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-DIRSTORE_HOST_TEST",
         "-I", str(FW / "src"), str(FW / "tests" / "irstore_host_test.c"),
         str(FW / "src" / "irstore" / "irstore.c"), str(FW / "src" / "crc32.c"),
         "-o", str(exe)],
        check=True,
    )
    return exe


class Harness:
    def __init__(self, binary: Path, tmp: Path) -> None:
        self.tmp = tmp
        self.n = 0
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

    def file(self, data: bytes = b"") -> Path:
        self.n += 1
        path = self.tmp / f"f{self.n}.bin"
        path.write_bytes(data)
        return path

    def rx(self, data: bytes, chunk: int = 1000) -> None:
        self.cmd(f"rx {self.file(data)} {chunk}")

    def flash(self, off: int, n: int) -> bytes:
        path = self.file()
        self.cmd(f"flash {off:#x} {n} {path}")
        return path.read_bytes()

    def poke(self, off: int, data: bytes) -> None:
        self.cmd(f"poke {off:#x} {self.file(data)}")

    def get(self, slot: int) -> tuple[int, list[str]]:
        (line,) = self.cmd(f"get {slot}")
        w = line.split()
        return int(w[1]), w[2:]

    def load(self, slot: int, max_taps: int = 4096) -> tuple[int, list[float]]:
        path = self.file()
        (line,) = self.cmd(f"load {slot} {max_taps} {path}")
        n = int(line.split()[1])
        vals = list(struct.unpack("<4096f", path.read_bytes()))
        return n, vals

    def put(self, cab: int, samples: list[float], name: str = "cab", rate: int = 48000,
            tasks: int = 20, crc: int | None = None) -> list[str]:
        data = longir.pack_samples(samples)
        crc = zlib.crc32(data) if crc is None else crc
        out = self.cmd(f"put {cab} {len(samples)} {crc:#x} {name} {rate}")
        assert out[-1] == "put 0", out
        self.rx(data)
        return self.cmd(f"task {tasks}")

    def close(self) -> None:
        if self.p.stdin:
            self.p.stdin.close()
        self.p.wait(timeout=5)


@pytest.fixture
def h(tmp_path):
    harness = Harness(build(), tmp_path)
    yield harness
    harness.close()


def ir(n: int, seed: int = 1) -> list[float]:
    rnd = random.Random(seed)
    return [struct.unpack("<f", struct.pack("<f", rnd.uniform(-1, 1) * 0.999 ** i))[0]
            for i in range(n)]


def test_layout_matches_the_python_side():
    assert (longir.BASE, longir.END, longir.TABLE) == (0x400000, 0x502000, 3092)
    assert longir.data_offset(20) == 0x402000 and longir.data_offset(83) == 0x4FE000


def test_upload_writes_data_then_table_and_loads_back(h):
    x = ir(4096)
    out = h.put(20, x, name="Ampeg_8x10", rate=48000)
    crc = zlib.crc32(longir.pack_samples(x))
    assert f"ir done crc={crc:08x} ok slot 20 taps 4096 gain 0.7500" in out, out
    assert out[-1] == "active 0"
    # one sector per task call: 4 data sectors, then the table (copy A: no table yet)
    (w,) = h.cmd("writes")
    assert w.split()[1:] == ["5", "402000", "403000", "404000", "405000", "400000"], w
    r, fields = h.get(0)
    assert r == 1 and fields == ["4096", "48000", "0.750000", f"{crc:08x}", "Ampeg_8x10"]
    n, vals = h.load(0)
    assert n == 4096 and vals == x
    assert h.cmd("current") == ["current 0 1"]


def test_short_ir_zero_pads_what_the_gain_rule_sees(h):
    """The stock gain rule reads 512 taps: past a short IR it must see zeros,
    not the buffer's old content."""
    out = h.put(21, [0.5] * 100)
    assert any("ok slot 21 taps 100" in line for line in out), out
    assert h.cmd("gainseen") == ["gainseen 100 taken 0"]
    n, vals = h.load(1)
    assert n == 100 and vals[:100] == [0.5] * 100 and vals[100] == -7.0   # nothing past taps


def test_table_alternates_copies_and_keeps_other_slots(h):
    h.put(20, ir(300, 1), name="one")
    h.put(83, ir(4096, 2), name="last")
    assert h.cmd("current") == ["current 1 2"]
    h.put(20, ir(500, 3), name="one_b")
    assert h.cmd("current") == ["current 0 3"]
    assert h.get(0)[1][0] == "500" and h.get(0)[1][4] == "one_b"
    assert h.get(63)[1][4] == "last" and h.load(63)[1] == ir(4096, 2)
    # the Python codec reads the C table
    seq, entries = longir.decode_table(h.flash(longir.index_offset(0), longir.TABLE))
    assert seq == 3 and [(e.slot, e.taps, e.name) for e in entries] == [(20, 500, "one_b"),
                                                                          (83, 4096, "last")]


def test_python_table_is_read_by_the_firmware(h):
    x = ir(1000, 7)
    data = longir.pack_samples(x)
    e = longir.LongIr(25, 1000, 96000, 1.25, zlib.crc32(data), "from_python")
    h.poke(longir.data_offset(25), data)
    h.poke(longir.index_offset(1), longir.encode_table([e], seq=41))
    assert h.cmd("current") == ["current 1 41"]
    r, fields = h.get(5)
    assert r == 1 and fields == ["1000", "96000", "1.250000", f"{e.crc:08x}", "from_python"]
    assert h.load(5)[1][:1000] == x


def test_a_failed_write_keeps_the_previous_table(h):
    h.put(20, ir(64), name="keep")                 # table copy A, seq 1
    h.cmd("failwrite 1")                           # slot 21's data sector fails
    out = h.put(21, ir(64, 2), name="lost")
    assert any("ir FAILED: flash write -2" in line for line in out), out
    h.cmd("failwrite 2")                           # now its table write (copy B) fails
    out = h.put(21, ir(64, 2), name="lost")
    assert any("ir FAILED: table write -3" in line for line in out), out
    assert h.get(0)[0] == 1 and h.get(1)[0] == 0 and h.cmd("current") == ["current 0 1"]


def test_a_torn_table_copy_loses_to_the_valid_one(h):
    """Power lost while copy B was programmed: B holds part of a newer table
    (bad CRC). A, the older one, stays current; with both bad the store is empty."""
    h.put(20, ir(64), name="a")                    # copy A, seq 1
    h.put(21, ir(64), name="b")                    # copy B, seq 2
    newer = h.flash(longir.index_offset(1), longir.TABLE)
    h.poke(longir.index_offset(1), newer[:2000] + b"\xff" * (longir.TABLE - 2000))
    assert h.cmd("current") == ["current 0 1"]
    assert h.get(0)[0] == 1 and h.get(1)[0] == 0
    h.poke(longir.index_offset(0) + 100, b"\x55")
    assert h.cmd("current") == ["current -1 0"] and h.get(0)[0] == 0
    # the next write starts over from an empty table, in copy A
    h.put(22, ir(8), name="c")
    assert h.cmd("current") == ["current 0 1"] and h.get(2)[0] == 1 and h.get(0)[0] == 0


def test_bad_crc_upload_writes_nothing(h):
    out = h.put(22, ir(256), crc=0x12345678)
    assert any("BAD (nothing written)" in line for line in out), out
    assert h.cmd("writes") == ["writes 0"] and h.get(2)[0] == 0
    assert h.cmd("gainseen")[0].endswith("taken 0")   # the buffer went back


def test_corrupt_slot_data_is_bypassed(h):
    h.put(30, ir(2048))
    h.poke(longir.data_offset(30) + 4000, b"\x00\x00\x80\x7f")    # +inf, CRC now wrong
    n, _ = h.load(10)
    assert n == 0                  # the engine bypasses the cab, as an empty stock slot
    assert h.get(10)[0] == 1       # the entry itself is still listed (irls shows BAD)


def test_load_caps_at_the_engines_ir_ram(h):
    x = ir(4096)
    h.put(40, x)
    n, vals = h.load(20, max_taps=512)
    assert n == 512 and vals[:512] == x[:512] and vals[512] == -7.0


def test_nan_taps_play_as_zero(h):
    x = ir(16)
    data = bytearray(longir.pack_samples(x))
    data[8:12] = b"\x00\x00\xc0\x7f"                      # NaN, with a matching CRC
    out = h.cmd(f"put 50 16 {zlib.crc32(data):#x} nan 44100")
    assert out[-1] == "put 0"
    h.rx(bytes(data))
    h.cmd("task 10")
    n, vals = h.load(30)
    assert n == 16 and vals[2] == 0.0 and vals[3] == x[3]


def test_idle_stream_aborts_and_frees_the_buffer(h):
    data = longir.pack_samples(ir(4096))
    assert h.cmd(f"put 20 4096 {zlib.crc32(data):#x} x 44100")[-1] == "put 0"
    h.rx(data[:5000])
    assert h.cmd("task 5") == ["active 1"]
    h.cmd("tick 3001")
    out = h.cmd("task")
    assert "ir aborted at 5000/16384 bytes (nothing written)" in out and out[-1] == "active 0"
    assert h.cmd("writes") == ["writes 0"]
    assert h.cmd(f"put 20 4096 {zlib.crc32(data):#x} x 44100")[-1] == "put 0"   # usable again


@pytest.mark.parametrize("args, why", [
    ("19 100 0x1 a", "usage"), ("84 100 0x1 a", "usage"), ("20 0 0x1 a", "usage"),
    ("20 4097 0x1 a", "usage"), ("20 100 0x1 " + "n" * 24, "usage"),
])
def test_upload_arguments_are_checked(h, args, why):
    out = h.cmd(f"put {args}")
    assert out[-1] == "put -1" and why in out[0], out


def test_busy_buffer_and_second_upload_are_refused(h):
    h.cmd("buf busy")
    out = h.cmd("put 20 10 0x1 a")
    assert out[-1] == "put -3" and "busy" in out[0]
    h.cmd("buf free")
    assert h.cmd("put 20 10 0x1 a")[-1] == "put 0"
    assert h.cmd("put 21 10 0x1 b")[-1] == "put -3"


def test_small_chip_refuses_the_store(h):
    """Nothing beyond the chip: a 4 MB chip (or an unknown size) has no store."""
    for cap in (0x400000, 0, longir.END - 1):
        h.cmd(f"cap {cap}")
        out = h.cmd("put 20 10 0x1 a")
        assert out[-1] == "put -2" and "store not available" in out[0], out
        assert h.get(0)[0] == -1 and h.load(0)[0] == -1
        assert h.cmd("del 20") == ["del -1"]
    h.cmd(f"cap {longir.END}")
    assert h.cmd("put 20 10 0x1 a")[-1] == "put 0"
    assert h.cmd("writes") == ["writes 0"]


def test_full_table_all_64_slots(h):
    for cab in range(longir.FIRST, longir.LAST + 1):
        out = h.put(cab, ir(8, cab), name=f"s{cab}")
        assert any(f"ok slot {cab} taps 8" in line for line in out), out
    seq, entries = longir.decode_table(h.flash(longir.index_offset(h_current(h)), longir.TABLE))
    assert seq == 64 and [e.slot for e in entries] == list(range(20, 84))
    assert all(h.load(i)[1][:8] == ir(8, i + 20) for i in (0, 31, 63))


def h_current(h: Harness) -> int:
    return int(h.cmd("current")[0].split()[1])


def test_delete(h):
    h.put(20, ir(64), name="a")
    h.put(21, ir(64), name="b")
    assert h.cmd("del 0") == ["del 1"]
    assert h.get(0)[0] == 0 and h.get(1)[0] == 1
    assert h.cmd("del 0") == ["del 0"]            # already empty: no flash write
    h.cmd("writes")
    assert h.cmd("del 0") == ["del 0"] and h.cmd("writes") == ["writes 0"]
    h.cmd("buf busy")
    assert h.cmd("del 1") == ["del -2"]


def test_empty_store_and_erased_flash(h):
    assert h.cmd("current") == ["current -1 0"]
    assert h.get(0)[0] == 0 and h.load(0)[0] == 0 and h.cmd("del 0") == ["del 0"]


def test_slot_numbers_and_names(h):
    assert h.cmd("slot 19") == ["slot -1"] and h.cmd("slot 20") == ["slot 0"]
    assert h.cmd("slot 83") == ["slot 63"] and h.cmd("slot 84") == ["slot -1"]
    assert h.cmd("nameok a") == ["nameok 1"] and h.cmd("nameok " + "x" * 23) == ["nameok 1"]
    assert h.cmd("nameok " + "x" * 24) == ["nameok 0"] and h.cmd("nameok") == ["nameok 0"]


def test_entry_codec_rejects_malformed():
    good = longir.LongIr(20, 10, 44100, 1.0, 5, "x")
    raw = longir.encode_entry(good)
    assert longir.decode_entry(raw, 20) == good
    assert longir.decode_entry(b"\xff" * 48, 20) is None                  # erased
    assert longir.decode_entry(bytes(48), 20) is None                     # deleted
    bad_gain = longir.encode_entry(longir.LongIr(20, 10, 44100, -1.0, 5, "x"))
    assert longir.decode_entry(bad_gain, 20) is None
    table = longir.encode_table([good], seq=3)
    assert longir.decode_table(table) == (3, [good])
    assert longir.decode_table(table[:-1] + bytes([table[-1] ^ 1])) is None


def test_sanitize_and_trim():
    assert longir.sanitize_name("My Cab 4x10 (SM57).wav") == "My_Cab_4x10_(SM57).wav"
    assert len(longir.sanitize_name("x" * 40)) == 23 and longir.sanitize_name("") == "IR"
    assert longir.trim_tail([1.0, 0.5, 0.0, 0.0]) == [1.0, 0.5]
    assert longir.trim_tail([0.0, 0.0]) == [0.0]
    with pytest.raises(Exception, match="1..4096"):
        longir.pack_samples([0.0] * 4097)
