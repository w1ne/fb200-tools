# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Latest-release download (src/fb200/release.py), against a local directory."""

import json
import zlib

import pytest

from fb200 import release
from fb200.errors import FirmwareError


def publish(tmp_path, data: bytes, crc: int | None = None) -> str:
    (tmp_path / "fb200-app.slot").write_bytes(data)
    crc = zlib.crc32(data) & 0xFFFFFFFF if crc is None else crc
    (tmp_path / "manifest.json").write_text(json.dumps(
        {"version": "v9.9.9", "app": {"file": "fb200-app.slot", "size": len(data),
                                      "crc32": f"{crc:08x}"}}))
    return tmp_path.as_uri() + "/"


def test_fetch_checks_the_manifest(tmp_path):
    assert release.fetch("app", publish(tmp_path, b"slot" * 100)) == ("v9.9.9", b"slot" * 100)


def test_fetch_refuses_a_crc_mismatch(tmp_path):
    with pytest.raises(FirmwareError, match="CRC"):
        release.fetch("app", publish(tmp_path, b"slot" * 100, crc=1))


def test_download_failure_is_a_firmware_error(tmp_path):
    with pytest.raises(FirmwareError, match="download failed"):
        release.fetch("app", (tmp_path / "missing").as_uri() + "/")
