#!/usr/bin/env bash
# Sanity check: no member of lib-soft may still carry the pre-normalization
# section names (.dram1*, .wifi*iram*, .iram1*, .rodata_wlog*).
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
CACHE="$(ls -d "$HOME"/Library/Caches/llgo/crosscompile/esp-clang-* 2>/dev/null | grep -v '\.lock$' | sort | tail -1)"
RE="$CACHE/bin/llvm-readelf"; AR="$CACHE/bin/llvm-ar"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
bad=0
for lib in "$DIR"/idfparts/lib-soft/*.a; do
    bn="$(basename "$lib" .a)"
    mkdir -p "$tmp/$bn"
    (cd "$tmp/$bn" && "$AR" x "$lib") || { echo "extract fail: $bn"; continue; }
    for m in "$tmp/$bn"/*.o; do
        hits="$("$RE" -S "$m" 2>/dev/null | awk '{ for (i=1; i<=NF; i++) if ($i ~ /^\.(dram1|wifi0iram|wifirxiram|wifislpiram|wifiorslpiram|wifislprxiram|wifiextrairam|iram1|rodata_wlog)/) print $i }' | tr '\n' ' ')"
        if [ -n "$hits" ]; then
            echo "LEFTOVER $(basename "$m") [$bn]: $hits"
            bad=$((bad+1))
        fi
    done
done
echo "members-with-leftovers: $bad"
