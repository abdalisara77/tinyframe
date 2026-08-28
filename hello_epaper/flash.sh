#!/usr/bin/env bash
# Build + flash the CrowPanel 4.2" e-paper hello-world.
#
#   ./flash.sh 1   -> V1.2A, green circular sticker on the back (UC8176-family)
#   ./flash.sh 0   -> V1.0 / V1.2, no sticker (SSD1683)
#
# The two revisions ship different display controllers, so both the EPD.cpp
# driver and the init/update calls in the sketch have to match the board.
set -euo pipefail

REV="${1:-1}"
PORT="${2:-/dev/cu.usbserial-110}"
SKETCH_DIR="$(cd "$(dirname "$0")" && pwd)"

if [[ "$REV" == "1" ]]; then
  SRC="$SKETCH_DIR/driver_variants/uc8176_green_sticker/EPD.cpp"
  echo ">> board revision V1.2A (green sticker) / UC8176-family driver"
else
  SRC="$SKETCH_DIR/driver_variants/ssd16xx_no_sticker/EPD.cpp"
  echo ">> board revision V1.0/V1.2 (no sticker) / SSD1683 driver"
fi
cp "$SRC" "$SKETCH_DIR/EPD.cpp"

# UploadSpeed pinned to 115200: the CH340 on this board corrupts packets at
# 460800+ ("Invalid head of packet" / "Unable to verify flash chip connection").
FQBN="esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=huge_app,PSRAM=opi,CDCOnBoot=default,UploadSpeed=115200"

arduino-cli compile \
  --fqbn "$FQBN" \
  --build-property "compiler.cpp.extra_flags=-DGREEN_STICKER_REV=$REV" \
  --build-property "compiler.c.extra_flags=-DGREEN_STICKER_REV=$REV" \
  "$SKETCH_DIR"

arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$SKETCH_DIR"
echo ">> done. Watch serial with:  arduino-cli monitor -p $PORT -c baudrate=115200"
