# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

import shutil
import sys
import tempfile
from pathlib import Path

import pytest

from fb200.protocol import pack_frame
from fb200.transport import MockTransport

sys.path.insert(0, str(Path(__file__).parent))

# Firmware and host-harness builds (fwbuild, *_host tests) go to mkdtemp()
# dirs of tens of MB each. One base dir per test process (each xdist worker
# has its own), removed at the end, so runs do not fill the disk.
_TMP = tempfile.mkdtemp(prefix="fb200_pytest_")
tempfile.tempdir = _TMP


def pytest_sessionfinish(session, exitstatus):
    shutil.rmtree(_TMP, ignore_errors=True)


def make_report(frame: bytes) -> bytes:
    report = bytes([len(frame)]) + frame
    return report + bytes(64 - len(report))


def version_payload() -> bytes:
    payload = bytearray(55)
    payload[0:5] = b"FB200"
    payload[32:38] = b"V1.0.0"
    payload[39:45] = b"V1.0.1"
    payload[46:52] = b"V1.0.0"
    payload[53:54] = b"A"
    return bytes(payload)


@pytest.fixture
def version_transport() -> MockTransport:
    return MockTransport(reports=[make_report(pack_frame(0x01, version_payload()))])
