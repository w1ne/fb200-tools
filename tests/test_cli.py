import json
from pathlib import Path

import pytest
from conftest import make_report, version_payload

from fb200 import cli
from fb200.pedal import FB200Device
from fb200.protocol import pack_frame
from fb200.transport import MockTransport


def query_reply(name):
    payload = bytearray(16)
    payload[3] = 0 if name is None else 1
    if name:
        encoded = name.encode()
        payload[6:8] = len(encoded).to_bytes(2, "little")
        payload[8:8 + len(encoded)] = encoded
    return pack_frame(0x64, bytes(payload))


@pytest.fixture
def patched_device(monkeypatch):
    def install(reports):
        transport = MockTransport(reports=reports)
        monkeypatch.setattr(cli, "_open_device", lambda: FB200Device(transport))
        return transport
    return install


def test_cli_info_prints_versions(patched_device, capsys):
    patched_device([make_report(pack_frame(0x01, version_payload()))])
    assert cli.main(["info"]) == 0
    out = capsys.readouterr().out
    assert "FB200" in out
    assert "V1.0.1" in out


def test_cli_ir_list_shows_names(patched_device, capsys):
    reports = [make_report(query_reply("My IR" if i == 0 else None)) for i in range(9)]
    patched_device(reports)
    assert cli.main(["ir", "list"]) == 0
    out = capsys.readouterr().out
    assert "1: My IR" in out
    assert "2: (empty)" in out


def test_cli_ir_backup_writes_manifest(patched_device, tmp_path):
    # _cmd_ir_backup calls info() first, then ir_list()
    reports = [make_report(pack_frame(0x01, version_payload()))]
    reports += [make_report(query_reply("Bass" if i == 0 else None)) for i in range(9)]
    patched_device(reports)
    out_path = tmp_path / "backup"
    assert cli.main(["ir", "backup", str(out_path)]) == 0
    manifest = json.loads(Path(out_path, "manifest.json").read_text())
    assert manifest["slots"][0]["name"] == "Bass"
    assert manifest["slots"][1]["name"] is None


def test_cli_error_returns_1_and_prints_to_stderr(monkeypatch, capsys):
    from fb200.errors import CommunicationError

    def boom():
        raise CommunicationError("no device")

    monkeypatch.setattr(cli, "_open_device", boom)
    assert cli.main(["info"]) == 1
    assert "error: no device" in capsys.readouterr().err


def test_cli_ir_delete_success_and_closes_transport(patched_device, capsys):
    transport = patched_device([make_report(pack_frame(0x68, bytes([0, 0, 0, 1])))])
    assert cli.main(["ir", "delete", "3"]) == 0
    assert "deleted slot 3" in capsys.readouterr().out
    assert transport.closed is True


def test_cli_probe_rejects_bad_hex():
    with pytest.raises(SystemExit) as excinfo:
        cli.main(["probe", "--send", "ZZ"])
    assert excinfo.value.code == 2


def test_cli_ir_delete_rejects_out_of_range_slot():
    with pytest.raises(SystemExit) as excinfo:
        cli.main(["ir", "delete", "99"])
    assert excinfo.value.code == 2
