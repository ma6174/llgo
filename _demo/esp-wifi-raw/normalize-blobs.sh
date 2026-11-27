#!/usr/bin/env bash
# Normalize the closed-source blob archives from ilp32f (hard-float) ELF
# metadata to ilp32 (soft-float) metadata, writing idfparts/lib-soft/.
#
# The blob objects carry EF_RISCV_FLOAT_ABI_SINGLE (e_flags bits 1-2) and a
# Tag_RISCV_float_abi attribute; llgo's esp32s31 targets compile soft-float
# ilp32, and ld.lld hard-fails the mix. The float ABI only describes FP
# register usage: the integer calling convention is identical, every signature
# crossing the OSAL boundary is integer-only, the blob saves/restores its own
# callee-saved fs registers per ABI, and its ISR bodies were verified
# FP-free (idfparts/MANIFEST.md links the P0 survey). So clearing the
# float-ABI metadata (e_flags mask 0x6 and the .riscv.attributes section)
# makes the link accept the objects without changing any instruction.
#
# This is the same trade the esp32s31-idf path makes in reverse: there the
# llgo object's flags are patched to SINGLE so the hard-float IDF link
# accepts them (_demo/common/make-artifact.sh step 4).
#
# Output archives are build artifacts: gitignored, rebuilt by build.sh when
# missing.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
PARTS="$DIR/idfparts"
OUT="$PARTS/lib-soft"

AR="$(ls "$HOME"/Library/Caches/llgo/crosscompile/esp-clang-*/bin/llvm-ar 2>/dev/null | sort | tail -1)"
OBJCOPY="$(ls "$HOME"/Library/Caches/llgo/crosscompile/esp-clang-*/bin/llvm-objcopy 2>/dev/null | sort | tail -1)"
if [[ -z "$AR" || -z "$OBJCOPY" ]]; then
    echo "error: llvm-ar/llvm-objcopy not found in the llgo esp-clang cache" >&2
    exit 1
fi

mkdir -p "$OUT"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

for lib in "$PARTS"/lib/*.a; do
    name="$(basename "$lib")"
    members="$work/$name"
    mkdir -p "$members"
    rm -rf "$members"/*
    (cd "$members" && "$AR" x "$lib")
    for m in "$members"/*.o; do
        "$OBJCOPY" --remove-section .riscv.attributes "$m" 2>/dev/null || true
        python3 - "$m" <<'EOF'
import struct, sys
path = sys.argv[1]
with open(path, 'r+b') as f:
    data = f.read(40)
    flags = struct.unpack_from('<I', data, 36)[0]
    flags &= ~0x6  # EF_RISCV_FLOAT_ABI -> soft
    f.seek(36)
    f.write(struct.pack('<I', flags))
EOF
    done
    (cd "$members" && "$AR" rc "$OUT/$name" *.o)
    echo "normalized: $name"
done
echo "output: $OUT"
