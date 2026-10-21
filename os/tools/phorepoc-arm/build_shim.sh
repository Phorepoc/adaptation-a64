#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

# build_shim.sh -- build the AArch64 -> AArch32 entry shim (src/phorepoc-arm/a53shim/shim.S) in the phorepoc-arm
# container. TARGET = the AArch32 bootstrap's physical entry:
#   0x04000000 (default) = QEMU pflash1 (run_virt.sh A53=1)
#   0x40300000           = the in-RAM ROM (CFG_ROMINRAM: U-Boot `go 0x40200000`; run_virt.sh A53=1 ROMINRAM=1)
# Output: .a53-build/shim.bin (or $OUT).
set -eu
cd "${KVIEW:-$(dirname "$0")/../..}"   # KVIEW: the tree root (the pdk assembles it for the public repos)
TARGET="${TARGET:-0x04000000}"; OUT="${OUT:-.a53-build/shim.bin}"
mkdir -p "$(dirname "$OUT")"
SHIMCMD="set -e
  aarch64-linux-gnu-gcc -c -DTARGET=$TARGET ${DEFS:-} src/phorepoc-arm/a53shim/shim.S -o /tmp/shim.o
  aarch64-linux-gnu-ld -Ttext=0 /tmp/shim.o -o /tmp/shim.elf
  aarch64-linux-gnu-objcopy -O binary /tmp/shim.elf $OUT"
if command -v aarch64-linux-gnu-gcc >/dev/null; then
  bash -c "$SHIMCMD"                                   # (an AArch64 GCC here)
else
  MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/repo" -w /repo phorepoc-arm bash -c "$SHIMCMD"
fi
echo "build_shim: $OUT ($(wc -c < "$OUT") bytes, TARGET=$TARGET)"
