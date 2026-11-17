#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-1.0
#
# build.sh -- build the QEMU orangepi-pc USB host bench (bench.elf) from the unmodified FreeBSD USB sources.
# Env: FB = the FreeBSD tree (default os/third_party/freebsd-usb), O = output dir, CROSS = toolchain prefix,
# USB_POOL_SIZE = the kshim's static malloc pool (2 MiB: EHCI + OHCI schedules and transfers fit).
# Every FreeBSD file is compiled as-is; the kshim's SYSINIT tables are collected the way FreeBSD's
# stand/kshim/kshim.mk does it (objcopy .debug.sysinit -> stand/usb/tools/sysinit -> sysinit_data.c).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
FB="${FB:-$HERE/../../../../../third_party/freebsd-usb}"
O="${O:-$HERE/out}"
CROSS="${CROSS:-arm-none-eabi-}"
mkdir -p "$O"

U="$FB/sys/dev/usb"
KSRCS=(
  "$FB/stand/kshim/bsd_kernel.c" "$FB/stand/kshim/bsd_busspace.c"
  "$U/controller/usb_controller.c" "$U/controller/ehci.c" "$U/controller/generic_ehci.c" "$U/controller/ohci.c"
  "$U/usb_core.c" "$U/usb_debug.c" "$U/usb_device.c" "$U/usb_dynamic.c" "$U/usb_error.c"
  "$U/usb_handle_request.c" "$U/usb_hid.c" "$U/usb_hub.c" "$U/usb_lookup.c" "$U/usb_msctest.c"
  "$U/usb_parse.c" "$U/usb_request.c" "$U/usb_transfer.c" "$U/usb_util.c"
  "$HERE/../phe_usb_hc.c"
  ${EXTRA_KSRCS:-}
)
SRCS=( "$FB/stand/usb/usb_busdma_loader.c" )

ARCH=( -mcpu=cortex-a7 -marm -mfloat-abi=soft -O2 -g -ffunction-sections -fdata-sections -fno-pic )
# the FreeBSD side is a _STANDALONE build: no libc headers, the kshim defines the types (as stand/usb does)
CFLAGS=( "${ARCH[@]}" -nostdinc -isystem "$("${CROSS}gcc" -print-file-name=include)" -D_STANDALONE
  -Wall -Wno-unused -Wno-pointer-sign -Wno-unused-but-set-variable
  -I"$FB/stand/kshim" -I"$FB/sys" -I"$HERE/../kinc" -I"$O"
  -DUSB_GLOBAL_INCLUDE_FILE='"bsd_global.h"'  # little-endian: the kshim's own byte-order macros
  -D'__uintptr_t=unsigned long' -DUSB_POOL_SIZE=${USB_POOL_SIZE:-2097152} -DUSB_MSCTEST_BULK_SIZE=65536 )

objs=(); sysobjs=()
for s in "${KSRCS[@]}" "${SRCS[@]}"; do
  o="$O/$(basename "${s%.c}").o"
  "${CROSS}gcc" "${CFLAGS[@]}" -c "$s" -o "$o"
  objs+=("$o")
done
for s in "${KSRCS[@]}"; do
  o="$O/$(basename "${s%.c}").o"
  "${CROSS}objcopy" -j .debug.sysinit -O binary "$o" "${o%.o}.osys"
  sysobjs+=("${o%.o}.osys")
done
cat "${sysobjs[@]}" > "$O/sysinit.bin"
[ -x "$O/sysinit" ] || cc -O2 -I"$FB/stand/kshim" -o "$O/sysinit" "$FB/stand/usb/tools/sysinit.c"
"$O/sysinit" -i "$O/sysinit.bin" -o "$O/sysinit_data.c" -k sysinit -s sysinit_data
"$O/sysinit" -i "$O/sysinit.bin" -o "$O/sysuninit_data.c" -R -k sysuninit -s sysuninit_data
for s in "$O/sysinit_data.c" "$O/sysuninit_data.c"; do
  o="$O/$(basename "${s%.c}").o"
  "${CROSS}gcc" "${CFLAGS[@]}" -c "$s" -o "$o"
  objs+=("$o")
done
# the host side (newlib)
for s in "$HERE/bench.c" ${EXTRA_SRCS:-}; do
  o="$O/$(basename "${s%.c}").o"
  "${CROSS}gcc" "${ARCH[@]}" -Wall -c "$s" -o "$o"
  objs+=("$o")
done
"${CROSS}gcc" -mcpu=cortex-a7 -marm -c "$HERE/start.S" -o "$O/start.o"
"${CROSS}gcc" -mcpu=cortex-a7 -marm -mfloat-abi=soft -nostartfiles -specs=nano.specs -T "$HERE/bench.ld" \
  -Wl,--gc-sections -o "$O/bench.elf" "$O/start.o" "${objs[@]}" -lc -lgcc
"${CROSS}size" "$O/bench.elf"
