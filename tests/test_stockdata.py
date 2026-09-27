"""Stock data blob (src/fb200/stockdata.py) against the firmware's stock_data_t.

Runs without vendor data: a synthetic StockData goes through pack(), and the
firmware's own stock_check() and struct layout read it back. The real .mr
path is covered by test_drums_tuner.py and test_stock_dsp_parity.py.
"""

import shutil
import struct
import subprocess
import zlib

import pytest
from test_dsp_host import FW, STOCK_SRC

from fb200 import stockdata as sd
from fb200.errors import FirmwareError

READER = r"""
#include <stdio.h>
#include <stddef.h>
#include "stock_host.h"
int main(void)
{
    stock_from_env();
    if (!g_stock) return 2;
    const stock_data_t *s = g_stock;
    printf("%zu %zu %zu\n", sizeof *s, offsetof(stock_data_t, drum_events),
           offsetof(stock_data_t, drum_beats));
    printf("%.17g %.17g %.17g %.17g %.17g\n", s->amp_models[9].ws[255], s->amp_models[3].post[9][4],
           s->amp_models[2].level, s->amp_aa[3], s->cab_taps[7][511]);
    printf("%.17g %.17g %.17g %.17g\n", s->cab_gain[9], s->tone_presence[31][4],
           s->tone_mid[4][31][4], s->tone_bass[0][0]);
    printf("%u %u %u %u\n", (unsigned)s->drum_events[4711], (unsigned)s->drum_lens[89],
           (unsigned)s->drum_rhythm[39], (unsigned)s->drum_beats[89]);
    return 0;
}
"""


def synthetic() -> sd.StockData:
    """Distinct, exactly representable values in every field."""
    def seq(n, start):
        return [float(start + i) / 64 for i in range(n)]
    amps = [{"ws": seq(sd.AMP_WS, 1000 * k), "pre": seq(50, 1000 * k + 300),
             "post": seq(50, 1000 * k + 400), "gains": seq(5, 1000 * k + 500)}
            for k in range(sd.AMP_MODELS)]
    lens = [52] * 89 + [4712 - 52 * 89]
    return sd.StockData(
        amps=amps, amp_aa=[0.5, 0.25, 0.125, 0.0625, 0.03125],
        cab_taps=[seq(sd.CAB_TAPS, 10000 * i) for i in range(sd.CABS)],
        cab_gain=seq(sd.CABS, 7), tone={"bass": seq(160, 1), "presence": seq(160, 2),
                                        "treble": seq(160, 3),
                                        "mid": [seq(160, 100 * j) for j in range(5)]},
        drum_events=list(range(4712)), drum_lens=lens,
        drum_rhythm=list(range(40)), drum_beats=[1 + i % 9 for i in range(90)])


@pytest.fixture(scope="module")
def reader(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("host C compiler not installed")
    work = tmp_path_factory.mktemp("stockdata")
    src = work / "reader.c"
    src.write_text(READER)
    exe = work / "reader"
    subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(FW / "src"),
                    "-I", str(FW / "tests"), str(src), *map(str, STOCK_SRC), "-o", str(exe)],
                   check=True)
    return work, exe


def read_back(reader, blob: bytes) -> subprocess.CompletedProcess:
    work, exe = reader
    path = work / "stock.blob"
    path.write_bytes(blob)
    return subprocess.run([str(exe)], capture_output=True, text=True, check=False,
                          env={"FB200_STOCK_BLOB": str(path)})


def test_firmware_reads_the_python_layout(reader):
    d = synthetic()
    blob = sd.pack(d)
    sd.verify(blob)
    r = read_back(reader, blob)
    assert r.returncode == 0, r.stderr
    lines = [ln.split() for ln in r.stdout.splitlines()]
    body = struct.calcsize(f"<{sd.AMP_MODELS * 361 + 5 + 10 * 513 + 8 * 160}f")
    assert [int(v) for v in lines[0]] == [len(blob), 16 + body, 16 + body + 4712 * 4 + 180 + 40]
    assert [float(v) for v in lines[1]] == [d.amps[9]["ws"][255], d.amps[3]["post"][49],
                                            d.amps[2]["gains"][4], d.amp_aa[3],
                                            d.cab_taps[7][511]]
    assert [float(v) for v in lines[2]] == [d.cab_gain[9], d.tone["presence"][159],
                                            d.tone["mid"][4][159], d.tone["bass"][0]]
    assert [int(v) for v in lines[3]] == [4711, d.drum_lens[89], 39, d.drum_beats[89]]


@pytest.mark.parametrize("damage", ["magic", "version", "crc", "short"])
def test_firmware_rejects_a_bad_blob(reader, damage):
    blob = bytearray(sd.pack(synthetic()))
    if damage == "magic":
        blob[0] ^= 1
    elif damage == "version":
        blob[4] += 1
    elif damage == "crc":
        blob[-1] ^= 1
    else:
        blob = blob[:-4]
    r = read_back(reader, bytes(blob))
    assert r.returncode == 3, r.stdout + r.stderr
    with pytest.raises(FirmwareError):
        sd.verify(bytes(blob))


def test_blob_fits_its_flash_area():
    blob = sd.pack(synthetic())
    assert len(blob) <= sd.FLASH_SIZE and len(blob) % 4 == 0
    assert struct.unpack_from("<I", blob, 12)[0] == zlib.crc32(blob[16:]) & 0xFFFFFFFF


def test_lz_literals_and_overlapping_match():
    # token 0x13: 2 literals, match length 1 + 2 at offset 2 (overlapping copy)
    assert sd.arm_lz_decompress(bytes([0x13, 0x61, 0x62, 0x02]), 0, 5) == b"ababa"
    # token 0x0c + 2-byte offset form, zero-length literal/match extension bytes
    stream = bytes([0x03, 0x00, 0x78, 0x79, 0x1E, 0x7A, 0x03, 0x00])
    assert sd.arm_lz_decompress(stream, 0, 6) == b"xyzxyz"


def test_rejects_files_that_are_not_fb200_stock():
    with pytest.raises(FirmwareError):
        sd.build(b"not a firmware file" * 10)
