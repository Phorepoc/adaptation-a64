#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-1.0

# build_shim.sh -- build the AArch64 -> AArch32 entry shim (src/phorepoc-arm/a53shim/shim.S) in the phorepoc-arm
# container. TARGET = the AArch32 bootstrap's physical entry:
#   0x04000000 (default) = QEMU pflash1 (run_virt.sh A53=1)
#   0x40300000           = the in-RAM ROM (CFG_ROMINRAM: U-Boot `go 0x40200000`; run_virt.sh A53=1 ROMINRAM=1)
# Output: .a53-build/shim.bin (or $OUT).
set -eu
cd "$(dirname "$0")/../.."
TARGET="${TARGET:-0x04000000}"; OUT="${OUT:-.a53-build/shim.bin}"
mkdir -p "$(dirname "$OUT")"
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/repo" -w /repo phorepoc-arm bash -c "
  set -e
  aarch64-linux-gnu-gcc -c -DTARGET=$TARGET ${DEFS:-} src/phorepoc-arm/a53shim/shim.S -o /tmp/shim.o
  aarch64-linux-gnu-ld -Ttext=0 /tmp/shim.o -o /tmp/shim.elf
  aarch64-linux-gnu-objcopy -O binary /tmp/shim.elf $OUT"
echo "build_shim: $OUT ($(wc -c < "$OUT") bytes, TARGET=$TARGET)"
