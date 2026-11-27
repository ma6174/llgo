#!/usr/bin/env bash
# Wait for the emu lock (both mkdir- and flock-style holders understood),
# then run the gate-1 emu attempt and release.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$1"
python3 /tmp/llgo-wifiosal/_demo/esp-wifi-raw/osal/waitlock.py
rc=$?
if [ $rc -ne 0 ]; then exit $rc; fi
"$DIR/run-emu.sh" "$LOG"
rc=$?
rmdir /tmp/esp-emu.lock 2>/dev/null || rm -f /tmp/esp-emu.lock 2>/dev/null
exit $rc
