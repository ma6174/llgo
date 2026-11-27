#!/usr/bin/env bash
# Run the vendor IDF station-scan example under esp-emu (gate-3 control
# experiment: does the emulator deliver the wifi MAC IRQ to a vendor IDF
# firmware on S31?). CALLER-visible contract: takes /tmp/esp-emu.lock itself
# (waitlock.py) and releases it on exit.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
EMU="${ESP_EMU:-$HOME/.local/bin/esp-emu}"
LOG="$1"
python3 "$DIR/osal/waitlock.py"
rc=$?
if [ $rc -ne 0 ]; then exit $rc; fi
"$EMU" --chip esp32s31 --console usb-jtag --timeout 45s \
    --firmware /tmp/idfscan/scan_merge.bin >"$LOG" 2>&1
rc=$?
rmdir /tmp/esp-emu.lock 2>/dev/null || rm -f /tmp/esp-emu.lock 2>/dev/null
echo "emu exit=$rc (log: $LOG)"
exit 0
