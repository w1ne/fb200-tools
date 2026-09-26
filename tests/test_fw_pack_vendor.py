import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "firmware" / "hello" / "tools" / "pack_vendor_image.py"
GENERATOR = ROOT / "firmware" / "hello" / "tools" / "synthetic_template.py"

sys.path.insert(0, str(ROOT / "src"))

from fb200.firmware import MrFile

VECTORS_OFF = 0x000
VECTORS_SIZE = 0x400
BLOB_OFF = 0x7D4
BLOB_LIMIT = 0x1E39C - BLOB_OFF
STUB = b"\x72\xb6"
TABLE_HEADER = bytes.fromhex("50030000a0030000")
TABLE_ENTRY0 = bytes.fromhex("d407016000040000c8db0100a0040160")


def _stock_like_template(tmp_path: Path) -> MrFile:
    """Synthetic two-block template patched with the vendor boot-region markers."""
    out = tmp_path / "template.mr"
    subprocess.run([sys.executable, str(GENERATOR), "-o", str(out)], check=True)
    mr = MrFile.from_path(out)
    block0 = bytearray(mr.blocks[0].data)
    block0[0x4D8:0x4DA] = STUB
    block0[0x434:0x43C] = TABLE_HEADER
    block0[0x784:0x794] = TABLE_ENTRY0
    mr.blocks[0].data = bytes(block0)
    out.write_bytes(mr.to_bytes())
    return MrFile.from_path(out)


def _run_packer(tmp_path: Path, template: Path, vectors: bytes, blob: bytes, name: str) -> Path:
    vpath = tmp_path / "vectors.bin"
    bpath = tmp_path / "blob.bin"
    vpath.write_bytes(vectors)
    bpath.write_bytes(blob)
    out = tmp_path / name
    result = subprocess.run(
        [sys.executable, str(SCRIPT), str(template), str(vpath), str(bpath), "-o", str(out)],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode == 0, result.stderr
    return out


def test_pack_vendor_image_layout(tmp_path):
    template = _stock_like_template(tmp_path)
    vectors = bytes(range(256)) * 4
    blob = b"\x11\x22\x33\x44" * 8
    out = _run_packer(tmp_path, tmp_path / "template.mr", vectors, blob, "hello.mr")

    mr = MrFile.from_path(out)
    assert mr.header.product_tag == "FB200"
    assert len(mr.blocks) == 2
    block0 = mr.blocks[0].data
    assert len(block0) == len(template.blocks[0].data)

    assert block0[VECTORS_OFF:VECTORS_OFF + VECTORS_SIZE] == vectors
    # vendor boot region untouched
    assert block0[0x400:0x7D4] == template.blocks[0].data[0x400:0x7D4]
    # payload placed at the loader's entry-0 source and padded with 0xff
    assert block0[BLOB_OFF:BLOB_OFF + len(blob)] == blob
    assert block0[BLOB_OFF + len(blob):BLOB_OFF + BLOB_LIMIT] == b"\xff" * (BLOB_LIMIT - len(blob))
    # stock tail (entries 1-3 data) untouched
    assert block0[0x1E39C:] == template.blocks[0].data[0x1E39C:]
    # model block untouched
    assert mr.blocks[1].data == template.blocks[1].data


def test_pack_vendor_image_rejects_bad_inputs(tmp_path):
    _stock_like_template(tmp_path)
    template = tmp_path / "template.mr"
    good_blob = b"\x00" * 16

    vpath = tmp_path / "v.bin"
    bpath = tmp_path / "b.bin"
    vpath.write_bytes(b"\x00" * 16)
    bpath.write_bytes(good_blob)
    bad_vectors = subprocess.run(
        [sys.executable, str(SCRIPT), str(template), str(vpath), str(bpath), "-o", str(tmp_path / "x.mr")],
        capture_output=True, text=True, check=False,
    )
    assert bad_vectors.returncode != 0
    assert "vectors" in bad_vectors.stderr

    vpath.write_bytes(b"\x00" * VECTORS_SIZE)
    bpath.write_bytes(b"\x00" * (BLOB_LIMIT + 1))
    too_big = subprocess.run(
        [sys.executable, str(SCRIPT), str(template), str(vpath), str(bpath), "-o", str(tmp_path / "y.mr")],
        capture_output=True, text=True, check=False,
    )
    assert too_big.returncode != 0
    assert "blob" in too_big.stderr


def test_pack_vendor_image_rejects_non_stock_template(tmp_path):
    out = tmp_path / "plain.mr"
    subprocess.run([sys.executable, str(GENERATOR), "-o", str(out)], check=True)
    vpath = tmp_path / "v.bin"
    bpath = tmp_path / "b.bin"
    vpath.write_bytes(b"\x00" * VECTORS_SIZE)
    bpath.write_bytes(b"\x00" * 16)
    result = subprocess.run(
        [sys.executable, str(SCRIPT), str(out), str(vpath), str(bpath), "-o", str(tmp_path / "z.mr")],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode != 0
    assert "stock FB200 image" in result.stderr


def test_pack_vendor_image_app_only(tmp_path):
    _stock_like_template(tmp_path)
    template = tmp_path / "template.mr"
    vpath = tmp_path / "v.bin"
    bpath = tmp_path / "b.bin"
    vpath.write_bytes(b"\x00" * VECTORS_SIZE)
    bpath.write_bytes(b"\x00" * 16)
    out = tmp_path / "apponly.mr"
    result = subprocess.run(
        [sys.executable, str(SCRIPT), str(template), str(vpath), str(bpath),
         "-o", str(out), "--app-only"],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode == 0, result.stderr
    mr = MrFile.from_path(out)
    assert len(mr.blocks) == 1
    assert mr.header.update_block == 1
    assert len(mr.blocks[0].data) == 0x31000
