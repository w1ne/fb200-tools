#!/usr/bin/env bash
# Serve web/ locally, as the release workflow deploys it.
#
#   web/dev-serve.sh [--fake] [PORT]
#
# Copies the Python modules the updater runs in Pyodide into web/py/fb200/.
# Firmware for web/firmware/: out/ (from pack_images.py) if it exists, else
# with --fake a dummy app slot and recovery image that pass every check but
# are NOT firmware (for layout and first-install tests only; never flash them).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
fake=0
if [[ "${1:-}" == "--fake" ]]; then fake=1; shift; fi
port="${1:-8000}"

mkdir -p "$here/py/fb200" "$here/firmware"
cp "$root"/src/fb200/{__init__,errors,firmware,images,stockdata}.py "$here/py/fb200/"

if [[ -f "$root/out/manifest.json" ]]; then
  cp "$root"/out/{manifest.json,fb200-app.slot,fb200-recovery.bin} "$here/firmware/"
  echo "firmware: from out/"
elif [[ $fake == 1 ]]; then
  PYTHONPATH="$root/src" python3 - "$here/firmware" <<'PY'
import json, sys, zlib
from pathlib import Path
from fb200 import images, stockdata
out = Path(sys.argv[1])
cmds = b"fwbegin\0fwrec\0fwstock\0"
rec = images.build_recovery(b"\0" * 0x400, cmds + b"\xaa" * 1000, b"\xbb" * 64)
slot = images.build_slot(b"\0" * 0x400, cmds + b"\xcc" * 4000, b"\xdd" * 256)
crc = lambda d: f"{zlib.crc32(d) & 0xFFFFFFFF:08x}"
(out / "fb200-recovery.bin").write_bytes(rec)
(out / "fb200-app.slot").write_bytes(slot)
(out / "manifest.json").write_text(json.dumps({
    "version": "dev-fake",
    "app": {"file": "fb200-app.slot", "size": len(slot), "crc32": crc(slot)},
    "recovery": {"file": "fb200-recovery.bin", "size": len(rec), "crc32": crc(rec)},
    "stock_data_version": stockdata.VERSION}, indent=2) + "\n")
PY
  echo "firmware: FAKE images (do not flash)"
else
  echo "firmware: none. Build out/ with firmware/tools/pack_images.py, or pass --fake." >&2
fi

echo "serving $here on http://localhost:$port/"
exec python3 -m http.server "$port" --bind 127.0.0.1 --directory "$here"
