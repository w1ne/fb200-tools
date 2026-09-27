#!/usr/bin/env bash
# Build the open FB200 firmware images.
#
#   firmware/tools/build_images.sh [path/to/FB200_stock.mr] [outdir]
#
# Always writes the vendor-free images (fb200-app.slot, fb200-recovery.bin,
# manifest.json): the same files as a GitHub release. With your own stock
# .mr it also writes the personal first-install image fb200-twostage.mr and
# the stock sound data. Those contain vendor data: do not redistribute them.
#
# Needs: an Arm GNU toolchain with newlib (CROSS, default arm-none-eabi-),
# make, curl, python3. The boot emulation check runs when PY_UNICORN names a
# Python with `unicorn`. See docs/INSTALL.md.
set -euo pipefail

STOCK_MR=${1:-}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${2:-$ROOT/out}
CROSS=${CROSS:-arm-none-eabi-}
PY=${PY_UNICORN:-python3}

fail() { echo "error: $*" >&2; exit 1; }
echo '#include <math.h>' | "${CROSS}gcc" -x c -fsyntax-only - 2>/dev/null \
  || fail "${CROSS}gcc has no C library (math.h); use the Arm GNU Toolchain or install newlib (docs/INSTALL.md)"
STOCK_ARGS=()
if [[ -n "$STOCK_MR" ]]; then
  [[ -f "$STOCK_MR" ]] || fail "stock image not found: $STOCK_MR"
  STOCK_ARGS=(--stock "$(cd "$(dirname "$STOCK_MR")" && pwd)/$(basename "$STOCK_MR")")
fi

cd "$ROOT/firmware/audio"
for variant in recovery app; do
  echo "== building $variant"
  make -s build VARIANT=$variant CROSS="$CROSS" >/dev/null
done
cd "$ROOT"
python3 firmware/tools/pack_images.py firmware/audio/build/fb200-recovery \
  firmware/audio/build/fb200-app -o "$OUT" \
  --version "$(git describe --tags --always --dirty 2>/dev/null || echo dev)" ${STOCK_ARGS[@]+"${STOCK_ARGS[@]}"}
if [[ -n "$STOCK_MR" ]] && "$PY" -c "import unicorn" 2>/dev/null; then
  echo "== boot emulation (vendor loader -> recovery -> app)"
  "$PY" firmware/tools/boot_dry_run.py --elf firmware/audio/build/fb200-recovery.elf \
    --app-elf firmware/audio/build/fb200-app.elf --app-slot "$OUT/fb200-app.slot" \
    --max-instructions 120000000 "$OUT/fb200-twostage.mr" | tail -1
fi

cat <<MSG

Images in $OUT:
  fb200-app.slot       firmware updates over USB:  fb200 update app $OUT/fb200-app.slot
  fb200-recovery.bin   recovery updates over USB (rarely needed; add --stock FB200.mr)
MSG
if [[ -n "$STOCK_MR" ]]; then cat <<MSG
  fb200-twostage.mr    first install, through the pedal's update mode (A+D)
Then write the stock sound data once:  fb200 update stock $STOCK_MR
See docs/INSTALL.md.
MSG
fi
