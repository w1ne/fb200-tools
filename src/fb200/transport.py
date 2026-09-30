# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Transport layer: hidapi for real hardware, mock for tests.

`hidapi` is imported lazily so the package and test suite work without it
installed (install with `pip install -e '.[hid]'` from a checkout for hardware
use).
"""

from __future__ import annotations

import contextlib

from fb200 import protocol
from fb200.errors import CommunicationError, DeviceNotFoundError
from fb200.protocol import REPORT_SIZE

_HID_HINT = (
    "The 'hidapi' package is required for hardware access. "
    "Install it with: pip install hidapi (or pip install -e '.[hid]' from a checkout)"
)


_GONE_HINT = " (the pedal reset or was unplugged? reconnect and retry)"


class HidapiTransport:
    def __init__(
        self, path: bytes | None = None, vid: int = protocol.VID, pid: int = protocol.PID_APP
    ) -> None:
        self._path = path
        self._vid = vid
        self._pid = pid
        self._dev = None

    @staticmethod
    def find_path(vid: int = protocol.VID, pid: int = protocol.PID_APP) -> bytes | None:
        try:
            import hid
        except ImportError as exc:  # pragma: no cover - environment dependent
            raise DeviceNotFoundError(_HID_HINT) from exc
        for device in hid.enumerate(vid, pid):
            return device["path"]
        return None

    def open(self) -> HidapiTransport:
        try:
            import hid
        except ImportError as exc:  # pragma: no cover - environment dependent
            raise DeviceNotFoundError(_HID_HINT) from exc
        path = self._path or self.find_path(self._vid, self._pid)
        if path is None:
            raise DeviceNotFoundError("FB200 not found. Is it connected and powered on?")
        device = hid.device()
        try:
            device.open_path(path)
            device.set_nonblocking(0)
        except Exception as exc:
            with contextlib.suppress(Exception):
                device.close()
            raise CommunicationError(f"failed to open FB200 HID device: {exc} (another program, "
                                     "e.g. the official editor, may have it open)") from exc
        self._dev = device
        return self

    def write_report(self, report: bytes) -> None:
        if self._dev is None:
            raise CommunicationError("transport is not open")
        try:
            written = self._dev.write(list(report))
        except (OSError, ValueError) as exc:     # hidapi: device gone
            raise CommunicationError(f"HID write failed: {exc}{_GONE_HINT}") from exc
        if written < 0:
            raise CommunicationError(f"HID write failed{_GONE_HINT}")

    def read_report(self, timeout_ms: int = 500) -> bytes | None:
        if self._dev is None:
            raise CommunicationError("transport is not open")
        # hidapi: timeout 0 on a blocking device is hid_read(), which waits
        # forever. Always pass a real timeout.
        try:
            data = self._dev.read(REPORT_SIZE, max(1, int(timeout_ms)))
        except (OSError, ValueError) as exc:     # hidapi: "read error" once the device is gone
            raise CommunicationError(f"HID read failed: {exc}{_GONE_HINT}") from exc
        return bytes(data) if data else None

    def close(self) -> None:
        if self._dev is not None:
            self._dev.close()
            self._dev = None


class MockTransport:
    def __init__(self, reports=None) -> None:
        self.written: list[bytes] = []
        self.closed = False
        self._reports: list[bytes] = list(reports or [])

    def queue(self, report: bytes) -> None:
        self._reports.append(report)

    def write_report(self, report: bytes) -> None:
        self.written.append(bytes(report))

    def read_report(self, timeout_ms: int = 500) -> bytes | None:
        return self._reports.pop(0) if self._reports else None

    def close(self) -> None:
        self.closed = True
