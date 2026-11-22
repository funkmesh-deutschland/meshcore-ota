#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
MESHCORE_COMMIT="${MESHCORE_COMMIT:-75b04ef}"
MESHCORE_DIR="${MESHCORE_DIR:-$ROOT/build/MeshCore}"
ENV_NAME="${ENV_NAME:-Xiao_nrf52_repeater_mcota_test}"
TAG="${TAG:-dev}"
OUT="${OUT:-$ROOT/build/out}"
PIO="${PIO:-pio}"
PYTHON="${PYTHON:-python3}"
CROSS="${CROSS:-arm-none-eabi-}"
LAYOUT="${LAYOUT:-S140_V7}"
LOADER_ADDR="${LOADER_ADDR:-0x27000}"
APP_ADDR="${APP_ADDR:-0x28000}"
STATUS_ADDR="${STATUS_ADDR:-0xEA000}"
BOARD="${BOARD:-XIAO_NRF52}"
PAGES="${PAGES:-1}"
COMPACT="${COMPACT:-}"

if [ ! -d "$MESHCORE_DIR/.git" ]; then
  git clone https://github.com/meshcore-dev/MeshCore "$MESHCORE_DIR"
fi
git -C "$MESHCORE_DIR" checkout -q -f "$MESHCORE_COMMIT"
git -C "$MESHCORE_DIR" clean -q -fd -e .pio
git -C "$MESHCORE_DIR" apply "$HERE/meshcore.patch"
mkdir -p "$MESHCORE_DIR/src/helpers/mcota"
cp "$ROOT"/core/*.c "$ROOT"/core/*.h "$HERE"/mcota/* "$ROOT/loader/nrf52/layout.h" "$ROOT/loader/nrf52/xflash.h" "$MESHCORE_DIR/src/helpers/mcota/"
cp "$HERE"/boards/*.ld "$MESHCORE_DIR/boards/"

(cd "$MESHCORE_DIR" && PLATFORMIO_BUILD_FLAGS="-DMCOTA_TAG='\"$TAG\"'" "$PIO" run -e "$ENV_NAME")
make -B -C "$ROOT/loader/nrf52" CROSS="$CROSS" LAYOUT="$LAYOUT" BOARD="$BOARD" PAGES="$PAGES" COMPACT="$COMPACT" OUT="$ROOT/build/loader"

mkdir -p "$OUT"
"${CROSS}objcopy" -O binary "$MESHCORE_DIR/.pio/build/$ENV_NAME/firmware.elf" "$OUT/$ENV_NAME-$TAG.bin"
"$PYTHON" "$ROOT/tools/mcota.py" uf2 --out "$OUT/$ENV_NAME-$TAG.uf2" \
  --part "$LOADER_ADDR:$ROOT/build/loader/loader_$LAYOUT.bin" \
  --part "$APP_ADDR:$OUT/$ENV_NAME-$TAG.bin" \
  --part "$STATUS_ADDR:fill=0x2000"
sha256sum "$OUT/$ENV_NAME-$TAG.bin" "$OUT/$ENV_NAME-$TAG.uf2"
