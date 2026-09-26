import subprocess
import sys
from pathlib import Path

from fb200.firmware import MrFile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "firmware" / "hello" / "tools" / "synthetic_template.py"


def test_generates_stock_shaped_template(tmp_path):
    out = tmp_path / "template.mr"
    subprocess.run(
        [sys.executable, str(SCRIPT), "-o", str(out)],
        check=True,
    )
    mr = MrFile.from_path(out)
    assert mr.header.product_tag == "FB200"
    assert mr.header.update_block == 2
    assert mr.header.send_cmd == 0x02
    assert mr.header.rec_cmd == 0x03
    assert mr.header.update_addr == b"\x03\x00\x00\x00"
    assert len(mr.blocks[0].data) == 200_704
    assert mr.blocks[0].tag.send_cmd == 0x04
    assert mr.blocks[0].tag.start_page == 0x40
    assert set(mr.blocks[0].data) == {0}
    assert len(mr.blocks) == 2
    assert len(mr.blocks[1].data) == 3_286_016
    assert set(mr.blocks[1].data) == {0}
    assert mr.blocks[0].tag.start_addr == 641
    assert mr.blocks[0].tag.stop_addr == 641 + 200_704 - 1
    assert mr.blocks[1].tag.start_addr == 641 + 200_704 + 512
    assert mr.blocks[1].tag.stop_addr == 641 + 200_704 + 512 + 3_286_016 - 1
    assert mr.header.version == 0
    assert mr.header.timeout == 10_000
    assert mr.blocks[0].tag.timeout == 50_000
    assert out.stat().st_size == 128 + 512 + 200_704 + 512 + 3_286_016
