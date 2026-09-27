"""Stock sound data blob: built from the user's own stock `.mr`.

The open firmware images contain no vendor data. The stock amp models, cab
IRs, tone-stack tables, drum rhythms and factory presets stay in the user's
stock firmware file.
This module unpacks them (the Arm scatter-load LZ77 of the stock loader, no
emulator) and packs them in the firmware's layout: `stock_data_t` in
firmware/audio/src/dsp/stock_data.h. `fb200 update stock` writes the blob once
to its own flash area (F:0x61000). App updates never touch that area.

Pure Python (struct, zlib) so that the same code runs in the web updater
(Pyodide). The output is vendor data: never commit or publish it.
"""

from __future__ import annotations

import math
import struct
import zlib
from dataclasses import dataclass

from fb200.errors import FirmwareError
from fb200.firmware import MrFile

MAGIC = 0x44534246            # "FBSD"
VERSION = 2                   # 2: + factory presets; the firmware still accepts 1
FLASH_OFFSET = 0x61000
FLASH_SIZE = 0x10000

AMP_MODELS, AMP_WS, AMP_SOS = 10, 256, 10
CABS, CAB_TAPS = 10, 512
TONE_STEPS, TONE_MID_BANKS = 32, 5
DRUM_EVENTS, DRUM_PATTERNS, DRUM_RHYTHMS = 4712, 90, 40
FACTORY_NAMED, PRESET_SIZE = 20, 0x100     # + one "EMPTY" preset (stock_factory_t)

# Stock V1.0.1 block 0 (docs/FIRMWARE_FORMAT.md). The loader table entry 0
# copies payload 0x7d4.. to ITCM 0x400, entry 1 copies 0x1e39c.. to DTCM
# 0x20000000, entry 2 LZ-decompresses 0x22d3c.. to DTCM 0x200049a0.
ITCM_PAYLOAD_OFF, ITCM_BASE = 0x7D4, 0x400
DTCM1_OFF, DTCM1_LEN, DTCM1_BASE = 0x1E39C, 0x49A0, 0x20000000
DTCM2_OFF, DTCM2_LEN, DTCM2_BASE = 0x22D3C, 0x141A4, 0x200049A0

# Stock RAM layout (44.1 kHz designs, float32):
AMP_BASE, AMP_STRIDE = 0x2000954C, 0x630   # ws[256], then pre/post SOS and gains
CAB_BASE = 0x20011E3C                      # 10 x (512 taps + gain)
TONE_BASS, TONE_PRESENCE, TONE_TREBLE = 0x2000D32C, 0x2000D5AC, 0x2000D82C
TONE_MID, TONE_MID_STRIDE = 0x2000DD2C, 0x280
AMP_AA_LITERALS = 0x318C                   # ITCM: 3x-rate anti-alias b0, b1, c1, c2
DRUM_LENS_ITCM = 0x1C8AC                   # 90 u32 event-list lengths
DRUM_UNITS, DRUM_TENS, DRUM_BEATS = 0x20007FD8, 0x20007F38, 0x20008078
FACTORY_PRESETS = 0x20004E40               # 20 named + 20 "EMPTY" presets, 0x100 each

HEADER = struct.Struct("<4I")              # magic, version, size, crc


def arm_lz_decompress(src: bytes, pos: int, length: int) -> bytes:
    """Arm Compiler scatter-load LZ77 (the stock loader's routine at block 0 +0x43c)."""
    out = bytearray()
    tok = src[pos]
    pos += 1
    while True:
        lit = tok & 3
        if lit == 0:
            lit = src[pos]
            pos += 1
        mlen = tok >> 4
        if mlen == 0:
            mlen = src[pos]
            pos += 1
        n = lit - 1
        out += src[pos:pos + n]
        pos += n
        if mlen:
            off = src[pos]
            pos += 1
            if tok & 0xC == 0xC:
                off += src[pos] << 8
                pos += 1
            else:
                off += (tok & 0xC) << 6
            p = len(out) - off
            for _ in range(mlen + 2):
                out.append(out[p])
                p += 1
        if len(out) >= length:
            return bytes(out[:length])
        tok = src[pos]
        pos += 1


@dataclass
class StockRam:
    """The parts of the stock RAM image that hold the sound data."""

    itcm: bytes          # ITCM from ITCM_BASE
    dtcm: bytes          # DTCM from 0x20000000 (entries 1 and 2)

    def floats(self, addr: int, count: int) -> list[float]:
        return list(struct.unpack_from(f"<{count}f", self.dtcm, addr - DTCM1_BASE))

    def words(self, addr: int, count: int) -> list[int]:
        return list(struct.unpack_from(f"<{count}I", self.dtcm, addr - DTCM1_BASE))

    def raw(self, addr: int, count: int) -> bytes:
        return self.dtcm[addr - DTCM1_BASE:addr - DTCM1_BASE + count]


def unpack_stock_ram(block0: bytes) -> StockRam:
    try:
        itcm = block0[ITCM_PAYLOAD_OFF:DTCM1_OFF]
        dtcm = block0[DTCM1_OFF:DTCM1_OFF + DTCM1_LEN] + \
            arm_lz_decompress(block0, DTCM2_OFF, DTCM2_LEN)
    except IndexError:
        raise FirmwareError("stock data not found: is this the FB200 stock firmware?") from None
    if len(dtcm) != DTCM1_LEN + DTCM2_LEN:
        raise FirmwareError("stock data not found: is this the FB200 stock firmware?")
    return StockRam(itcm, dtcm)


def _sos_rows(c: list[float], first: int) -> list[float]:
    """rows [b0 b1 b2 a0 a1 a2], y = b0x+b1x1+b2x2-a1y1-a2y2 (a0 unused)
    -> CMSIS DF1 {b0, b1, b2, -a1, -a2}"""
    rows = []
    for r in range(AMP_SOS):
        b0, b1, b2, _a0, a1, a2 = c[first + 6 * r: first + 6 * r + 6]
        rows += [b0, b1, b2, -a1, -a2]
    return rows


@dataclass
class StockData:
    amps: list[dict]
    amp_aa: list[float]
    cab_taps: list[list[float]]
    cab_gain: list[float]
    tone: dict
    drum_events: list[int]
    drum_lens: list[int]
    drum_rhythm: list[int]
    drum_beats: list[int]
    factory_presets: bytes      # 20 named + 1 "EMPTY" preset, 0x100 each


def extract(ram: StockRam) -> StockData:
    b0, b1, c1, c2 = struct.unpack_from("<4f", ram.itcm, AMP_AA_LITERALS - ITCM_BASE)
    amps = []
    for k in range(AMP_MODELS):
        rec = ram.floats(AMP_BASE + k * AMP_STRIDE, AMP_STRIDE // 4)
        c = rec[AMP_WS:]
        amps.append({"ws": rec[:AMP_WS], "pre": _sos_rows(c, 8), "post": _sos_rows(c, 74),
                     "gains": [c[0x110 // 4], c[0x218 // 4], c[0x21C // 4], c[0x220 // 4],
                               c[0x224 // 4]]})   # pre_gain out_gain drive_scale(2) level
    cabs = [ram.floats(CAB_BASE + i * (CAB_TAPS + 1) * 4, CAB_TAPS + 1) for i in range(CABS)]
    tone = {"bass": ram.floats(TONE_BASS, TONE_STEPS * 5),
            "presence": ram.floats(TONE_PRESENCE, TONE_STEPS * 5),
            "treble": ram.floats(TONE_TREBLE, TONE_STEPS * 5),
            "mid": [ram.floats(TONE_MID + j * TONE_MID_STRIDE, TONE_STEPS * 5)
                    for j in range(TONE_MID_BANKS)]}
    events = ram.words(DTCM1_BASE, DRUM_EVENTS)
    lens = list(struct.unpack_from(f"<{DRUM_PATTERNS}I", ram.itcm, DRUM_LENS_ITCM - ITCM_BASE))
    units, tens = ram.floats(DRUM_UNITS, DRUM_RHYTHMS), ram.floats(DRUM_TENS, DRUM_RHYTHMS)
    data = StockData(
        amps=amps, amp_aa=[b0, b1, b0, c1, -c2],       # CMSIS DF1 order
        cab_taps=[c[:CAB_TAPS] for c in cabs], cab_gain=[c[CAB_TAPS] for c in cabs],
        tone=tone, drum_events=events, drum_lens=lens,
        drum_rhythm=[int(u + 10 * t) for u, t in zip(units, tens)],
        drum_beats=ram.words(DRUM_BEATS, DRUM_PATTERNS),
        # the stock's 20 EMPTY presets differ only in the module order field
        # (0xbc, no effect on the sound): the first one stands for all
        factory_presets=ram.raw(FACTORY_PRESETS, (FACTORY_NAMED + 1) * PRESET_SIZE))
    check(data)
    return data


def check(d: StockData) -> None:
    """Fail loudly on an image whose tables are not where this module expects."""
    def bad(msg: str):
        raise FirmwareError(f"unexpected stock data ({msg}): is this the FB200 stock "
                            "firmware V1.0.1?")
    values = list(d.amp_aa)
    for m in d.amps:
        values += m["ws"] + m["pre"] + m["post"] + m["gains"]
        if not all(abs(v) <= 1.5 for v in m["ws"]) or m["ws"][0] != 0.0:
            bad("amp waveshaper out of range")
    aa = d.amp_aa
    if abs(sum(aa[:3]) / (1 - aa[3] - aa[4]) - 1) > 1e-3 or not 0 < aa[0] < 0.2:
        bad("amp anti-alias literals are not a unity-gain low-pass")
    for taps, gain in zip(d.cab_taps, d.cab_gain):
        values += taps
        if not 0.9 <= max(abs(v) for v in taps) <= 1.0 or not 0.01 < gain < 1.0:
            bad("cab IR not (near) peak-normalised or gain out of range")
    for t in [d.tone["bass"], d.tone["presence"], d.tone["treble"], *d.tone["mid"]]:
        values += t
        if not all(0.2 < t[5 * s] < 3.0 for s in range(TONE_STEPS)):
            bad("tone-stack b0 out of range")
    if not all(math.isfinite(v) for v in values):
        bad("non-finite coefficient")
    if sum(d.drum_lens) != len(d.drum_events):
        bad("drum length table does not tile the event table")
    off = 0
    for n in d.drum_lens:
        if (d.drum_events[off + n - 1] >> 8) & 0xFF != 0xFF:
            bad("drum event list without an end marker")
        off += n
    if not all(0 <= p < DRUM_PATTERNS for p in d.drum_rhythm) or \
            not all(1 <= b <= 9 for b in d.drum_beats):
        bad("rhythm map or beats table out of range")
    presets = [d.factory_presets[i * PRESET_SIZE:(i + 1) * PRESET_SIZE]
               for i in range(FACTORY_NAMED + 1)]
    names = [p[:20].split(b"\0")[0] for p in presets]
    if len(d.factory_presets) != (FACTORY_NAMED + 1) * PRESET_SIZE or names[-1] != b"EMPTY" or \
            not all(n and n.isascii() and n.decode().isprintable() for n in names):
        bad("factory presets not found")


def pack(d: StockData, version: int = VERSION) -> bytes:
    """stock_data_t (firmware/audio/src/dsp/stock_data.h), little endian; version
    2 adds the factory presets (stock_factory_t) after it."""
    def f(values):
        return struct.pack(f"<{len(values)}f", *values)

    body = b"".join(f(m["ws"]) + f(m["pre"]) + f(m["post"]) + f(m["gains"]) for m in d.amps)
    body += f(d.amp_aa)
    body += b"".join(f(t) for t in d.cab_taps) + f(d.cab_gain)
    body += f(d.tone["bass"]) + f(d.tone["presence"]) + f(d.tone["treble"])
    body += b"".join(f(t) for t in d.tone["mid"])
    body += struct.pack(f"<{DRUM_EVENTS}I", *d.drum_events)
    body += struct.pack(f"<{DRUM_PATTERNS}H", *d.drum_lens)
    body += bytes(d.drum_rhythm) + bytes(d.drum_beats)
    body += b"\0" * (-(HEADER.size + len(body)) % 4)
    if version >= 2:
        body += d.factory_presets
    size = HEADER.size + len(body)
    return struct.pack("<3I", MAGIC, version, size) + \
        struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF) + body


def build(mr: MrFile | bytes, version: int = VERSION) -> bytes:
    """The stock data blob for F:0x61000, from a stock .mr (parsed or raw bytes).
    Version 1 is for firmware that accepts only 1 (see `formats_from_reply`)."""
    if isinstance(mr, (bytes, bytearray)):
        mr = MrFile.from_bytes(bytes(mr))
    if mr.header.product_tag != "FB200" or not mr.blocks:
        raise FirmwareError("not an FB200 firmware file")
    return pack(extract(unpack_stock_ram(mr.blocks[0].data)), version)


def formats_from_reply(reply: str) -> list[int]:
    """Stock data versions the pedal accepts, from its reply to `fwstock` with no
    arguments. Firmware before 0.7 does not list them and accepts only 1."""
    for line in reply.splitlines():
        if line.startswith("fwstock formats:"):
            return [int(v) for v in line.split(":", 1)[1].split()]
    return [1]


def verify(blob: bytes) -> None:
    """Header and CRC check, as the firmware's stock_check() (versions 1 and 2)."""
    if len(blob) < HEADER.size:
        raise FirmwareError("stock data blob too short")
    magic, version, size, crc = HEADER.unpack_from(blob)
    if magic != MAGIC or version not in (1, VERSION) or size != len(blob) or size > FLASH_SIZE:
        raise FirmwareError("not a stock data blob of this firmware version")
    if zlib.crc32(blob[HEADER.size:]) & 0xFFFFFFFF != crc:
        raise FirmwareError("stock data blob CRC mismatch")
