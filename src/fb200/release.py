# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Published firmware images: the latest GitHub release of fb200-tools.

The release carries only vendor-free files (fb200-app.slot,
fb200-recovery.bin) and a manifest.json with their sizes and CRCs.
"""

from __future__ import annotations

import json
import urllib.request
import zlib
from pathlib import Path

from fb200.errors import FirmwareError

LATEST = "https://github.com/w1ne/fb200-tools/releases/latest/download/"


def _get(url: str) -> bytes:
    try:
        with urllib.request.urlopen(url, timeout=60) as r:
            return r.read()
    except OSError as exc:
        raise FirmwareError(f"download failed: {url}: {exc}") from exc


def fetch(kind: str, base: str = LATEST) -> tuple[str, bytes]:
    """(version, image bytes) of `kind` ("app" or "recovery"), checked
    against the release manifest."""
    manifest = json.loads(_get(base + "manifest.json"))
    entry = manifest[kind]
    data = _get(base + entry["file"])
    if len(data) != entry["size"] or f"{zlib.crc32(data) & 0xFFFFFFFF:08x}" != entry["crc32"]:
        raise FirmwareError(f"{entry['file']}: size or CRC does not match the release manifest")
    return manifest["version"], data


def image(spec: str, kind: str) -> bytes:
    """A local file, or `latest` for the latest release."""
    if spec == "latest":
        version, data = fetch(kind)
        print(f"latest release {version}: {kind} image, {len(data)} bytes")
        return data
    return Path(spec).read_bytes()
