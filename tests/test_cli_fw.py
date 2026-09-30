# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

import json

from fb200 import cli
from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader


def make_fixture(path):
    header = MrHeader(product_tag="FB200", send_cmd=0x02, rec_cmd=0x03, update_block=1)
    blocks = [MrBlock(MrBlockTag(send_cmd=0x04, rec_cmd=0x05, start_page=0x40),
                      b"AT+BDFB200 Audio" + bytes(32))]
    path.write_bytes(MrFile(header, blocks).to_bytes())
    return path


def test_fw_inspect_json(tmp_path, capsys):
    path = make_fixture(tmp_path / "fw.mr")
    assert cli.main(["fw", "inspect", str(path), "--json"]) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["header"]["product_tag"] == "FB200"
    assert payload["header"]["update_block"] == 1
    assert payload["blocks"][0]["send_cmd"] == 4
    assert payload["blocks"][0]["size"] == 48


def test_fw_inspect_strings(tmp_path, capsys):
    path = make_fixture(tmp_path / "fw.mr")
    assert cli.main(["fw", "inspect", str(path), "--strings"]) == 0
    out = capsys.readouterr().out
    assert "FB200 Audio" in out


def test_fw_extract_block(tmp_path):
    path = make_fixture(tmp_path / "fw.mr")
    out = tmp_path / "block0.bin"
    assert cli.main(["fw", "extract-block", str(path), "0", str(out)]) == 0
    assert out.read_bytes().startswith(b"AT+BDFB200 Audio")


def test_fw_patch_string_writes_new_file(tmp_path, capsys):
    path = make_fixture(tmp_path / "fw.mr")
    out = tmp_path / "patched.mr"
    code = cli.main(["fw", "patch-string", str(path), "--find", "FB200 Audio",
                     "--replace", "FB200 Tools", "-o", str(out)])
    assert code == 0
    assert b"FB200 Tools" in out.read_bytes()
    assert path.read_bytes() != out.read_bytes()


def test_fw_inspect_missing_file_errors_cleanly(tmp_path, capsys):
    assert cli.main(["fw", "inspect", str(tmp_path / "nope.mr")]) == 1
    err = capsys.readouterr().err
    assert "error:" in err
    assert "Traceback" not in err


def test_fw_inspect_bad_magic_errors_cleanly(tmp_path, capsys):
    path = tmp_path / "bad.mr"
    path.write_bytes(b"NotMooer!" + bytes(200))
    assert cli.main(["fw", "inspect", str(path)]) == 1
    err = capsys.readouterr().err
    assert "error:" in err
    assert "Traceback" not in err


def test_fw_extract_block_out_of_range_errors_cleanly(tmp_path, capsys):
    path = make_fixture(tmp_path / "fw.mr")
    out = tmp_path / "out.bin"
    assert cli.main(["fw", "extract-block", str(path), "99", str(out)]) == 1
    err = capsys.readouterr().err
    assert "error:" in err
    assert "Traceback" not in err
    assert not out.exists()


def test_fw_patch_string_missing_text_errors_cleanly(tmp_path, capsys):
    path = make_fixture(tmp_path / "fw.mr")
    out = tmp_path / "patched.mr"
    code = cli.main(["fw", "patch-string", str(path), "--find", "NOPE!",
                     "--replace", "NOPE?", "-o", str(out)])
    assert code == 1
    err = capsys.readouterr().err
    assert "error:" in err
    assert "Traceback" not in err
    assert not out.exists()
