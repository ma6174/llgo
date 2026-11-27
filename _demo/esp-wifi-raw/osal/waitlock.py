#!/usr/bin/env python3
# One-shot /tmp/esp-emu.lock waiter shared by the gate scripts. The lock
# protocol across the repo is mkdir-based; some parallel agents use flock on
# the same path (leaving a regular file behind), so a stale-looking lock is
# only cleared when no emulator consumer process is running.
import os, subprocess, time, sys

lock = '/tmp/esp-emu.lock'
deadline = time.time() + 3600
while time.time() < deadline:
    try:
        os.mkdir(lock)
        print('GOT-LOCK (mkdir)', flush=True)
        sys.exit(0)
    except FileExistsError:
        out = subprocess.run(['ps', 'aux'], capture_output=True, text=True).stdout
        busy = ('esp-emu' in out or 'esp_emu' in out
                or 'TestRunEmbedEmulator' in out or 'TestS31ReflectSmoke' in out
                or 'flock /tmp/esp-emu.lock' in out)
        if not busy:
            try:
                if os.path.isdir(lock):
                    os.rmdir(lock)
                else:
                    os.remove(lock)
                print('cleared stale lock, retrying', flush=True)
                continue
            except OSError:
                pass
    time.sleep(5)
raise SystemExit('lock wait timed out')
