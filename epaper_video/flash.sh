#!/usr/bin/env bash
# Build + flash the e-paper video player.
#
#   ./flash.sh [invert] [settle_ms] [port]
#     invert  1 = white artwork on black [default], 0 = black on white
#     settle  exposure per frame in ms [default 100]. NOT a settling time --
#             this panel keeps driving for as long as you give it, so too low
#             leaves frames faint/torn and too high saturates them to black
#             (measured: 100 and 500 both good, 4000 solid black). The largest
#             value that has not started to darken is also the safest to
#             unplug at.
#     port    serial port [default: the first /dev/cu.usbserial-* found]
#
# Frames come from clips.h, which is generated and gitignored. Rebuild it from
# every photo in pictures/ with:
#   (cd .. && python3 make_eink_video.py)     # paths are relative to the root
set -euo pipefail

INV="${1:-1}"
SETTLE="${2:-100}"
# Auto-detect: macOS renumbers this device on every replug, and a stale
# hardcoded port fails the upload with a message about the ESP not being
# connected, which sends you looking at the board instead of at the path.
PORT="${3:-$(ls /dev/cu.usbserial-* 2>/dev/null | head -1)}"
SKETCH_DIR="$(cd "$(dirname "$0")" && pwd)"

# The board is a V1.2A (green sticker) / UC8176. The other driver is kept in
# driver_variants/ssd16xx_no_sticker for the older no-sticker boards.
cp "$SKETCH_DIR/driver_variants/uc8176_green_sticker/EPD.cpp" "$SKETCH_DIR/EPD.cpp"
echo ">> exposure ${SETTLE}ms/frame, invert=$INV"

# UploadSpeed pinned to 115200: this board's CH340 corrupts packets at 460800+.
FQBN="esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=huge_app,PSRAM=opi,CDCOnBoot=default,UploadSpeed=115200"
FLAGS="-DINVERT=$INV -DSETTLE_MS=$SETTLE"

arduino-cli compile \
  --fqbn "$FQBN" \
  --build-property "compiler.cpp.extra_flags=$FLAGS" \
  --build-property "compiler.c.extra_flags=$FLAGS" \
  "$SKETCH_DIR"

arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$SKETCH_DIR"
echo ">> done. Watch timing with:  arduino-cli monitor -p $PORT -c baudrate=115200"
