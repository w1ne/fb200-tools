# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Long IR store of the open firmware (firmware/audio/src/irstore/irstore.h).

64 slots of up to 4096 taps (float32, 44.1 kHz), selected as cab types
20..83, written over the USB console (CDC): `irput`, `irls`, `irdel`
(docs/PROTOCOL.md "Long IR store"). The table codec here mirrors irstore.c;
tests/test_irstore_host.py checks one against the other.
"""

from __future__ import annotations

import re
import struct
import zlib
from collections.abc import Iterable
from dataclasses import dataclass

from fb200.errors import CommunicationError, InvalidArgumentError

FIRST = 20                      # cab type of slot 0
SLOTS = 64
LAST = FIRST + SLOTS - 1        # 83
MAX_TAPS = 4096
NAME_MAX = 23
SAMPLE_RATE = 44100

BASE = 0x400000                 # flash offset
SECTOR = 0x1000
SLOT_SIZE = MAX_TAPS * 4
END = BASE + 2 * SECTOR + SLOTS * SLOT_SIZE
MAGIC = 0x52494246              # "FBIR"
VERSION = 1
HDR = 16
ENTRY = 48
TABLE = HDR + SLOTS * ENTRY + 4


def index_offset(copy: int) -> int:
    return BASE + copy * SECTOR


def data_offset(cab: int) -> int:
    return BASE + 2 * SECTOR + (cab - FIRST) * SLOT_SIZE


@dataclass(frozen=True)
class LongIr:
    slot: int                   # cab type 20..83
    taps: int
    rate: int
    gain: float
    crc: int
    name: str
    ok: bool | None = None      # data CRC checked by the pedal (irls); None: not checked


def check_slot(cab: int) -> int:
    if not FIRST <= cab <= LAST:
        raise InvalidArgumentError(f"long IR slot must be {FIRST}..{LAST}")
    return cab


def sanitize_name(name: str) -> str:
    """1..23 printable ASCII characters without spaces (the console splits on them)."""
    out = "".join(c if "!" <= c <= "~" else "_" for c in name)[:NAME_MAX]
    return out or "IR"


def trim_tail(samples: Iterable[float]) -> list[float]:
    """Drop trailing exact zeros (they cost CPU and change nothing); keep >= 1 tap."""
    x = list(samples)
    n = len(x)
    while n > 1 and x[n - 1] == 0.0:
        n -= 1
    return x[:n]


def pack_samples(samples: Iterable[float]) -> bytes:
    x = list(samples)
    if not 1 <= len(x) <= MAX_TAPS:
        raise InvalidArgumentError(f"a long IR has 1..{MAX_TAPS} taps, got {len(x)}")
    return struct.pack(f"<{len(x)}f", *x)


# ---- table codec (irstore.c) ----

def encode_entry(e: LongIr) -> bytes:
    name = e.name.encode("ascii")[:NAME_MAX]
    return struct.pack("<HHIfI24s8x", e.taps, 0, e.rate, e.gain, e.crc, name)


def decode_entry(raw: bytes, cab: int) -> LongIr | None:
    taps, _flags, rate, gain, crc, name = struct.unpack("<HHIfI24s8x", raw)
    if not 1 <= taps <= MAX_TAPS or name[-1] != 0 or not 0.0 < gain < 1e6:
        return None
    return LongIr(cab, taps, rate, gain, crc, name.split(b"\0", 1)[0].decode("ascii", "replace"))


def encode_table(entries: Iterable[LongIr], seq: int) -> bytes:
    body = bytearray(struct.pack("<IHHIHH", MAGIC, VERSION, SLOTS, seq, ENTRY, 0))
    body += bytes(SLOTS * ENTRY)
    for e in entries:
        off = HDR + (check_slot(e.slot) - FIRST) * ENTRY
        body[off:off + ENTRY] = encode_entry(e)
    return bytes(body) + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def decode_table(raw: bytes) -> tuple[int, list[LongIr]] | None:
    """(seq, used entries) of a valid table, else None."""
    if len(raw) < TABLE:
        return None
    raw = raw[:TABLE]
    magic, version, slots, seq, entry, _ = struct.unpack_from("<IHHIHH", raw)
    if (magic, version, slots, entry) != (MAGIC, VERSION, SLOTS, ENTRY) or \
            struct.unpack_from("<I", raw, TABLE - 4)[0] != zlib.crc32(raw[:TABLE - 4]) & 0xFFFFFFFF:
        return None
    out = []
    for i in range(SLOTS):
        e = decode_entry(raw[HDR + i * ENTRY:HDR + (i + 1) * ENTRY], FIRST + i)
        if e is not None:
            out.append(e)
    return seq, out


# ---- console (irls / irput / irdel) ----

IRLS_RE = re.compile(r"^ir (\d+) taps (\d+) rate (\d+) gain (\d+\.\d+) crc ([0-9a-f]{8}) "
                     r"(ok|BAD) name (\S+)\s*$", re.MULTILINE)
SUMMARY_RE = re.compile(r"ir store: (\d+) of (\d+) slots used")
DONE_RE = re.compile(rb"ir done crc=([0-9a-f]{8}) ok slot (\d+) taps (\d+) gain (\d+\.\d+)")


def parse_irls(text: str) -> dict:
    if "ir store: not available" in text:
        return {"available": False, "slots": [], "text": text.strip()}
    if SUMMARY_RE.search(text) is None:
        if "unknown command" in text:
            raise CommunicationError("this firmware has no long IR store: update it")
        raise CommunicationError(f"unexpected irls reply: {text!r}")
    slots = [LongIr(int(s), int(t), int(r), float(g), int(c, 16), n, ok == "ok")
             for s, t, r, g, c, ok, n in IRLS_RE.findall(text)]
    return {"available": True, "slots": slots, "text": text.strip()}


def ls(con) -> dict:
    return parse_irls(con.run("irls", max_s=10.0))


def put(con, cab: int, samples: Iterable[float], name: str, rate: int = SAMPLE_RATE) -> LongIr:
    """Upload taps (44.1 kHz floats) into long slot cab (20..83) over the console."""
    check_slot(cab)
    data = pack_samples(samples)
    taps = len(data) // 4
    crc = zlib.crc32(data) & 0xFFFFFFFF
    name = sanitize_name(name)
    ready = con.command(f"irput {cab} {taps} {crc:#x} {name} {int(rate)}",
                        [b"ir ready", b"ir: ", b"unknown command"], timeout=5.0)
    if b"ir ready" not in ready:
        if b"unknown command" in ready:
            raise CommunicationError("this firmware has no long IR store: update it")
        raise CommunicationError(f"irput refused: {ready.decode(errors='replace')}")
    for off in range(0, len(data), 4096):
        con.write(data[off:off + 4096])
    done = con.expect([b"ir done", b"ir FAILED", b"ir aborted"], timeout=60.0)
    m = DONE_RE.search(done)
    if m is None:
        raise CommunicationError(f"upload failed: {done.decode(errors='replace')}")
    return LongIr(int(m.group(2)), int(m.group(3)), int(rate), float(m.group(4)),
                  int(m.group(1), 16), name, True)


def delete(con, cab: int) -> bool:
    """True if the slot held an IR, False if it was empty."""
    check_slot(cab)
    text = con.run(f"irdel {cab}")
    if f"ir {cab} deleted" in text:
        return True
    if f"ir {cab}: empty" in text:
        return False
    if "unknown command" in text:
        raise CommunicationError("this firmware has no long IR store: update it")
    raise CommunicationError(f"irdel failed: {text.strip()}")


def load_wav(path, **options) -> tuple[list[float], int]:
    """A WAV as long-IR taps (process_ir, default 4096 taps, trailing zeros
    dropped) and its source rate."""
    from fb200.wav import process_ir, read_wav

    opts = dict(options)
    if opts.get("taps") is None:
        opts["taps"] = MAX_TAPS
    rate = read_wav(path)[1]
    return trim_tail(process_ir(path, **opts)), rate
