
from conftest import make_report, version_payload

from fb200 import cli, protocol
from fb200 import updater as updater_module
from fb200.firmware import MrBlock, MrBlockTag, MrFile, MrHeader
from fb200.pedal import FB200Device
from fb200.protocol import pack_frame
from fb200.transport import MockTransport


def make_fixture(path, product: str = "FB200"):
    header = MrHeader(product_tag=product, send_cmd=0x02, rec_cmd=0x03, update_block=1)
    tag = MrBlockTag(send_cmd=0x04, rec_cmd=0x05, start_page=0x40)
    path.write_bytes(MrFile(header, [MrBlock(tag, bytes(512))]).to_bytes())
    return path


class FakeHidapiTransport:
    def __init__(self, transport, **kwargs):
        self._transport = transport

    def open(self):
        return self

    def write_report(self, report):
        self._transport.write_report(report)

    def read_report(self, timeout_ms=500):
        return self._transport.read_report(timeout_ms)

    def close(self):
        self._transport.close()


def _reports(*frames):
    return [make_report(f) for f in frames]


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


def test_fw_flash_yes_happy_path(tmp_path, monkeypatch, capsys):
    path = make_fixture(tmp_path / "fw.mr")
    app_before = MockTransport(_reports(pack_frame(0x01, version_payload())))
    app_after = MockTransport(_reports(pack_frame(0x01, version_payload())))
    devices = [app_before, app_after]
    monkeypatch.setattr(cli, "_open_device", lambda: FB200Device(devices.pop(0)))

    boot = MockTransport(_reports(pack_frame(0x03, b"\x01"), pack_frame(0x05, b"\x01")))
    monkeypatch.setattr(cli, "HidapiTransport", lambda **kw: FakeHidapiTransport(boot))

    def fake_wait(vid, pid, timeout_s=15.0, poll_s=0.5):
        if vid == protocol.UPDATE_VID:
            assert app_before.closed, "app transport must close before waiting for bootloader"
            return b"boot-path"
        assert boot.closed, "boot transport must close before post-flash wait"
        return b"app-path"

    monkeypatch.setattr(updater_module, "wait_for_device", fake_wait)

    assert cli.main(["fw", "flash", str(path), "--yes"]) == 0
    out = capsys.readouterr().out
    assert "target: FB200" in out
    assert "device back online" in out
    c1 = pack_frame(0xC1)
    assert bytes([len(c1)]) + c1 + bytes(64 - 1 - len(c1)) in app_before.written


def test_fw_flash_mid_flash_failure(tmp_path, monkeypatch, capsys):
    path = make_fixture(tmp_path / "fw.mr")
    app_before = MockTransport(_reports(pack_frame(0x01, version_payload())))
    monkeypatch.setattr(cli, "_open_device", lambda: FB200Device(app_before))
    boot = MockTransport(_reports(pack_frame(0x03, b"\x01"), pack_frame(0x99, b"\x01")))
    monkeypatch.setattr(cli, "HidapiTransport", lambda **kw: FakeHidapiTransport(boot))
    monkeypatch.setattr(updater_module, "wait_for_device", lambda *a, **k: b"boot-path")

    assert cli.main(["fw", "flash", str(path), "--yes"]) == 1
    err = capsys.readouterr().err
    assert "error:" in err
    assert app_before.closed and boot.closed


def test_fw_flash_reenumeration_timeout_exit_3(tmp_path, monkeypatch, capsys):
    path = make_fixture(tmp_path / "fw.mr")
    app_before = MockTransport(_reports(pack_frame(0x01, version_payload())))
    monkeypatch.setattr(cli, "_open_device", lambda: FB200Device(app_before))
    boot = MockTransport(_reports(pack_frame(0x03, b"\x01"), pack_frame(0x05, b"\x01")))
    monkeypatch.setattr(cli, "HidapiTransport", lambda **kw: FakeHidapiTransport(boot))

    def fake_wait(vid, pid, timeout_s=15.0, poll_s=0.5):
        return b"boot-path" if vid == protocol.UPDATE_VID else None

    monkeypatch.setattr(updater_module, "wait_for_device", fake_wait)

    assert cli.main(["fw", "flash", str(path), "--yes"]) == 3
    assert boot.closed


def test_fw_flash_no_jump_without_device(tmp_path, monkeypatch, capsys):
    path = make_fixture(tmp_path / "fw.mr")
    monkeypatch.setattr(cli.HidapiTransport, "find_path", staticmethod(lambda vid, pid: None))
    assert cli.main(["fw", "flash", str(path), "--yes", "--no-jump"]) == 1
    assert "not in update mode" in capsys.readouterr().err
