#!/usr/bin/env bash
# Build PenPlotter firmware and generate firmware.bin for SD card flashing.
# Run from anywhere:  bash firmware/PenPlotter/build.sh

set -e
cd "$(dirname "$0")"

FQBN="MightyCore:avr:1284"
BOARD_OPTS="clock=16MHz_external,BOD=2v7,LTO=Os,pinout=standard,bootloader=no_bootloader"
OBJCOPY="$(find ~/Library/Arduino15 -name avr-objcopy 2>/dev/null | head -1)"

echo "▶ Compiling PenPlotter.ino..."
arduino-cli compile \
  --fqbn "$FQBN" \
  --board-options "$BOARD_OPTS" \
  --output-dir ./build \
  PenPlotter.ino

echo "▶ Converting to firmware.bin..."
"$OBJCOPY" -I ihex -O binary build/PenPlotter.ino.hex firmware.bin

SIZE=$(wc -c < firmware.bin)
echo ""
echo "✅  firmware.bin ready — ${SIZE} bytes"
echo ""
echo "SD card flashing:"
echo "  1. Format a micro SD card as FAT32"
echo "  2. Copy firmware.bin to the ROOT of the SD card (not in any folder)"
echo "  3. Insert card into the printer while powered OFF"
echo "  4. Power on — the Creality bootloader will flash automatically"
echo "  5. The card's firmware.bin will be renamed/deleted when done"
echo ""
echo "USB flashing (alternative):"
echo "  arduino-cli upload --fqbn $FQBN --port /dev/cu.usbserial-* build/PenPlotter.ino.hex"
