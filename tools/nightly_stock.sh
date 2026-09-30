#!/bin/sh
# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

# Nightly: the slow stock-DSP emulation tests (pytest -m stock), from this checkout.
# They need the vendor fb200-stock.mr, which is not in the repo, so GitHub CI
# cannot run them. Run from launchd/cron on a machine that has the file.
#   FB200_STOCK_MR  the vendor image (required)
#   PY              a Python with `pip install -e ".[dev,stock]"` (default: python3)
#   LOG             log file (default: build/nightly-stock.log)
set -eu
cd "$(dirname "$0")/.."
PY=${PY:-python3}
LOG=${LOG:-build/nightly-stock.log}
mkdir -p "$(dirname "$LOG")"
if PYTHONPATH=src PY_UNICORN="$PY" "$PY" -m pytest -m stock -n auto -q -p no:cacheprovider \
    >"$LOG" 2>&1; then
  tail -1 "$LOG"
else
  tail -1 "$LOG"
  command -v osascript >/dev/null &&
    osascript -e "display notification \"stock tests failed: $LOG\" with title \"fb200 nightly\""
  exit 1
fi
