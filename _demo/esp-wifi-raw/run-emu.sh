#!/usr/bin/env bash
# Run the wifi firmware under esp-emu: merge the pinned bootloader and
# partition table (idfparts/, harvested from the same espressif/idf image)
# with the app image at the XIP offsets, then boot.
#
# LOCKING: the CALLER must hold /tmp/esp-emu.lock (mkdir) around the whole
# invocation - this script does not take or release the lock itself, it
# only runs the emulator inside it.
#
# The application drives USB-Serial/JTAG (esp32s31-flash device path), so
# --console usb-jtag is required or every print is swallowed.
set -uo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
EMU="${ESP_EMU:-$HOME/.local/bin/esp-emu}"
LOG="${1:-$DIR/emu-run.log}"

python3 - "$DIR" <<'EOF'
import sys, os
d = sys.argv[1]
flash = bytearray(b'\xff' * (4 * 1024 * 1024))
for off, name in ((0x2000, 'bootloader.bin'), (0x8000, 'partition-table.bin'), (0x10000, 'wifi.bin.bin')):
    p = os.path.join(d, 'idfparts' if 'bin' in ('bootloader.bin', 'partition-table.bin') and not name.startswith('wifi') else d, name)
    if name == 'wifi.bin.bin':
        p = os.path.join(d, name)
    else:
        p = os.path.join(d, 'idfparts', name)
    flash[off:off + os.path.getsize(p)] = open(p, 'rb').read()
open(os.path.join(d, 'flash.bin'), 'wb').write(flash)
print('flash.bin:', len(flash), 'bytes')
EOF

"$EMU" --chip esp32s31 --firmware "$DIR/flash.bin" \
    --console usb-jtag --timeout 30s >"$LOG" 2>&1
RC=$?
echo "emu exit=$RC (log: $LOG)"
tr -d '\r' <"$LOG" | grep -vE "^\[INFO|^\[WARN|^\s*$" | tail -30
exit $RC
