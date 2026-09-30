#!/usr/bin/env python3
# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

"""Flash the FB200 over the open firmware's USB console (no DFU, no A+D).

Thin wrapper over `fb200 update` (src/fb200/console.py); kept for scripts.

  usb_update.py app build/out/fb200-app.slot
  usb_update.py recovery build/out/fb200-recovery.bin
  usb_update.py block0 fb200-twostage.mr   # one-time migration
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "src"))

from fb200.cli import main

if __name__ == "__main__":
    args = sys.argv[1:]
    if "--stock" not in args:
        args += ["--stock", str(REPO_ROOT / "fb200-stock.mr")]
    sys.exit(main(["update", "-v", *args]))
