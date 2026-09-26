from fb200 import cli
from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader


def make_template(path, app_size: int = 512, product: str = "FB200"):
    header = MrHeader(
        product_tag=product, send_cmd=0x02, rec_cmd=0x03, timeout=10000,
        update_block=2, update_addr=b"\x03\x00\x00\x00", version=0,
    )
    blocks = [
        MrBlock(MrBlockTag(send_cmd=0x04, rec_cmd=0x05, start_page=0x40),
                bytes(app_size)),
        MrBlock(MrBlockTag(send_cmd=0x06, rec_cmd=0x07, start_page=0x00),
                bytes(64)),
    ]
    path.write_bytes(MrFile(header, blocks).to_bytes())
    return path


def test_fw_pack_writes_single_block_image(tmp_path, capsys):
    template = make_template(tmp_path / "template.mr")
    app = tmp_path / "app.bin"
    app.write_bytes(b"\xde\xad\xbe\xef")
    out = tmp_path / "hello.mr"
    assert cli.main(
        ["fw", "pack", "--template", str(template), str(app), "-o", str(out)]
    ) == 0
    packed = MrFile.from_path(out)
    assert packed.header.update_block == 1
    assert len(packed.blocks[0].data) == 512
    assert packed.blocks[0].data[:4] == b"\xde\xad\xbe\xef"
    assert "packed" in capsys.readouterr().out


def test_fw_pack_default_output_name(tmp_path):
    template = make_template(tmp_path / "template.mr")
    app = tmp_path / "hello.bin"
    app.write_bytes(b"abc")
    assert cli.main(["fw", "pack", "--template", str(template), str(app)]) == 0
    assert (tmp_path / "hello.mr").exists()


def test_fw_pack_rejects_oversize_app(tmp_path, capsys):
    template = make_template(tmp_path / "template.mr", app_size=8)
    app = tmp_path / "big.bin"
    app.write_bytes(b"x" * 9)
    out = tmp_path / "out.mr"
    assert cli.main(
        ["fw", "pack", "--template", str(template), str(app), "-o", str(out)]
    ) == 1
    assert "exceeds" in capsys.readouterr().err


def test_fw_pack_rejects_wrong_product(tmp_path, capsys):
    template = make_template(tmp_path / "template.mr", product="OTHER")
    app = tmp_path / "app.bin"
    app.write_bytes(b"x")
    assert cli.main(["fw", "pack", "--template", str(template), str(app)]) == 1
    assert "FB200" in capsys.readouterr().err


def test_fw_pack_missing_app_file(tmp_path, capsys):
    template = make_template(tmp_path / "template.mr")
    assert cli.main(
        ["fw", "pack", "--template", str(template), str(tmp_path / "nope.bin")]
    ) == 1
    assert "cannot read" in capsys.readouterr().err
