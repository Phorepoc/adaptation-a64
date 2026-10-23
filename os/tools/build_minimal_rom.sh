#!/usr/bin/env bash
command -v py >/dev/null 2>&1 || py() { python3 "$@"; }   # Linux has python3, no py launcher
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# build_minimal_rom.sh -- Track B M2: the whole from-source pipeline, one command.
#
#   h2inc (bootstrap .inc)  ->  translate+assemble+link bootstrap (armasm2gas)
#   ->  objcopy raw binary  ->  rombuild.py  ->  a structurally valid XIP ROM
#   (the pdk rombuild has no round-trip verify).
#
# Everything here is from the EPL-2010 open source + our own tools -> publishable.
# Output artifacts land in .bootstrap-build/ (gitignored).  A *bootable* ROM still needs
# a real from-source ekern.exe as the primary (M3); this proves the ROM-build capability.
set -euo pipefail
cd "${KVIEW:-$(dirname "$0")/..}"   # KVIEW: the tree root (the pdk assembles it for the public repos)

BIN_DEFAULT="/c/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin"
BIN="${ARM_BIN:-$BIN_DEFAULT}"
LD="$BIN/arm-none-eabi-ld"; OC="$BIN/arm-none-eabi-objcopy"
[ -x "$LD" ] || LD="$(command -v arm-none-eabi-ld || true)"
[ -x "$OC" ] || OC="$(command -v arm-none-eabi-objcopy || true)"
[ -n "$LD" ] && [ -n "$OC" ] || { echo "FAIL: arm-none-eabi toolchain not found"; exit 1; }

OUT="$PWD/.bootstrap-build"; mkdir -p "$OUT"
ROM_BASE="${ROM_BASE:-0x80000000}"

MM_MODEL="${MM_MODEL:-flexible}"
echo "[1/4] h2inc: regenerate bootstrap includes (MM_MODEL=$MM_MODEL)"
SMP="${SMP:-}" MM_MODEL="$MM_MODEL" py tools/armasm2gas/h2inc.py \
  --out-dir ".bootstrap-gen/bootstrap-inc$([ "$MM_MODEL" = multiple ] && echo -multiple)$([ "${SMP:-}" = 1 ] && echo -smp)" >/dev/null

echo "[2/4] translate + assemble + link bootstrap${EXTRA_DEFS:+ ($EXTRA_DEFS)}"
OUT="$OUT" MM_MODEL="$MM_MODEL" EXTRA_DEFS="${EXTRA_DEFS:-}" bash tools/armasm2gas/build_bootstrap.sh | sed 's/^/      /'

# Link the bootstrap at 0 (NOT rom_base): the Symbian boot table is DCD <func> entries
# that BootCall treats as CODE OFFSETS added to iCodeBase (the actual run base, physical
# 0x10000 early / virtual later). Linking at 0 makes the entries offsets; linking at
# rom_base makes them absolute linked addresses and the first BootCall (a serial print)
# jumps to an unmapped 0x8xxxxxxx and faults. Position-independent code -> same bytes.
# The ROM header's rom_base (0x80000000) is separate, set by rombuild for the header fields.
echo "[3/4] link bootstrap @ 0x0 (offsets) + objcopy -> raw binary"
"$LD" -Ttext=0x0 -e TheRomHeader -o "$OUT/boot.elf" \
      "$OUT/bootmain.o" "$OUT/bootcpu.o" "$OUT/bootutils.o" "$OUT/psl.o"
"$OC" -O binary "$OUT/boot.elf" "$OUT/boot.bin"
echo "      boot.bin = $(wc -c < "$OUT/boot.bin") bytes"

echo "[4/4] rombuild"
py tools/rombuild.py "$OUT/boot.bin" "$OUT/minimal.rom" --rom-base "$ROM_BASE" \
  | sed 's/^/      /'

echo "M2 pipeline OK -> $OUT/minimal.rom"
