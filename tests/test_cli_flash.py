from fb200 import cli
from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader


def make_fixture(path, product: str = "FB200"):
    header = MrHeader(product_tag=product, send_cmd=0x02, rec_cmd=0x03, update_block=1)
    tag = MrBlockTag(send_cmd=0x04, rec_cmd=0x05, start_page=0x40)
    path.write_bytes(MrFile(header, [MrBlock(tag, bytes(512))]).to_bytes())
    return path


def test_fw_flash_dry_run_prints_plan(tmp_path, capsys):
    path = make_fixture(tmp_path / "fw.mr")
    assert cli.main(["fw", "flash", str(path)]) == 0
    out = capsys.readouterr().out
    assert "512 bytes" in out
    assert "dry run" in out


def test_fw_flash_rejects_wrong_product(tmp_path, capsys):
    path = make_fixture(tmp_path / "other.mr", product="OTHER")
    assert cli.main(["fw", "flash", str(path), "--yes"]) == 1
    assert "refusing" in capsys.readouterr().err
