#!/usr/bin/env bash
# Build the open FB200 firmware images from YOUR OWN stock firmware file.
#
#   firmware/tools/build_images.sh path/to/FB200_stock.mr [outdir]
#
# The stock file supplies the vendor bootloader stub and the stock sound data
# (amp models, cab IRs, tone stack, drum rhythms). None of it is in this
# repository, and the images built here contain it: do not redistribute them.
#
# Needs: an Arm GNU toolchain with newlib (CROSS, default arm-none-eabi-),
# make, curl, and a Python with `unicorn` (PY_UNICORN, default python3) for
# extracting the stock sound data. See docs/INSTALL.md.
set -euo pipefail

STOCK_MR=${1:?usage: build_images.sh <stock .mr> [outdir]}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${2:-$ROOT/out}
CROSS=${CROSS:-arm-none-eabi-}
PY=${PY_UNICORN:-python3}
STOCK_ABS=$(cd "$(dirname "$STOCK_MR")" && pwd)/$(basename "$STOCK_MR")

fail() { echo "error: $*" >&2; exit 1; }
[[ -f "$STOCK_ABS" ]] || fail "stock image not found: $STOCK_MR"
echo '#include <math.h>' | "${CROSS}gcc" -x c -fsyntax-only - 2>/dev/null \
  || fail "${CROSS}gcc has no C library (math.h); use the Arm GNU Toolchain or install newlib (docs/INSTALL.md)"
"$PY" -c "import unicorn" 2>/dev/null \
  || fail "'$PY' cannot import unicorn (pip install unicorn); set PY_UNICORN"

cd "$ROOT/firmware/audio"
for variant in recovery app; do
  echo "== building $variant"
  make -s build VARIANT=$variant CROSS="$CROSS" STOCK_MR="$STOCK_ABS" PY_UNICORN="$PY" >/dev/null
done
cd "$ROOT"
python3 firmware/tools/pack_images.py "$STOCK_ABS" firmware/audio/build/fb200-recovery \
  firmware/audio/build/fb200-app -o "$OUT"
echo "== boot emulation (vendor loader -> recovery -> app)"
"$PY" firmware/tools/boot_dry_run.py --elf firmware/audio/build/fb200-recovery.elf \
  --app-elf firmware/audio/build/fb200-app.elf --app-slot "$OUT/fb200-app.slot" \
  --max-instructions 120000000 "$OUT/fb200-twostage.mr" | tail -1

cat <<MSG

Images in $OUT:
  fb200-twostage.mr    first install, through the pedal's update mode (A+D)
  fb200-app.slot       firmware updates over USB:  fb200 update app $OUT/fb200-app.slot
  fb200-recovery.bin   recovery updates over USB (rarely needed)
See docs/INSTALL.md.
MSG
