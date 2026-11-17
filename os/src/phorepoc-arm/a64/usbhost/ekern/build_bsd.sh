#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-1.0
#
# build_bsd.sh <out-dir> -- the C side of the kernel's USB host (the unmodified FreeBSD USB stack on its kshim, our
# phe_usb_hc.c and phe_usb_libc.c), compiled for the 32-bit ARM kernel and collected into ONE relocatable object,
# <out-dir>/usbhost_bsd.o, with the kshim's SYSINIT tables generated as FreeBSD's stand/kshim/kshim.mk does.
# usbd_get_page is wrapped inside the object (phe_usb_hc.c). Called by tools/kernel/link_ekern.sh (A64_USBHOST=1).
# Env: FB (default third_party/freebsd-usb next to src/), GCC (arm-none-eabi-gcc), HOSTCC (cc), USBHOST_DEFS.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
O="$1"; mkdir -p "$O"
FB="${FB:-$HERE/../../../../../third_party/freebsd-usb}"
GCC="${GCC:-arm-none-eabi-gcc}"
LD="${LD:-${GCC%gcc}ld}"; OBJCOPY="${OBJCOPY:-${GCC%gcc}objcopy}"
HOSTCC="${HOSTCC:-cc}"
U="$FB/sys/dev/usb"
KSRCS=(
  "$FB/stand/kshim/bsd_kernel.c" "$FB/stand/kshim/bsd_busspace.c"
  "$U/controller/usb_controller.c" "$U/controller/ehci.c" "$U/controller/generic_ehci.c" "$U/controller/ohci.c"
  "$U/usb_core.c" "$U/usb_debug.c" "$U/usb_device.c" "$U/usb_dynamic.c" "$U/usb_error.c"
  "$U/usb_handle_request.c" "$U/usb_hid.c" "$U/usb_hub.c" "$U/usb_lookup.c" "$U/usb_msctest.c"
  "$U/usb_parse.c" "$U/usb_request.c" "$U/usb_transfer.c" "$U/usb_util.c"
  "$HERE/../phe_usb_hc.c"
  ${USBHOST_EXTRA_KSRCS:-}
)
# USBHOST_PHY=1 (the A64): the Allwinner USB PHY driver (aw_usbphy.c, unmodified) on our FDT/clk/reset/PHY stand-ins
[ "${USBHOST_PHY:-0}" = 1 ] && KSRCS+=( "$FB/sys/arm/allwinner/aw_usbphy.c" "$HERE/../phe_usb_fdt.c" )
KSRCS+=(
)
SRCS=( "$FB/stand/usb/usb_busdma_loader.c" )
# the kernel's ABI: ARMv7-A, ARM state, soft float, 32-bit enums, no unaligned accesses, no libc headers
ARCH=( -march=armv7-a -marm -mfloat-abi=soft -fno-short-enums -mno-unaligned-access -ffreestanding -fno-strict-aliasing
  -O2 -fno-pic -fno-common )
CFLAGS=( "${ARCH[@]}" -nostdinc -isystem "$("$GCC" -print-file-name=include)" -D_STANDALONE -DHAVE_MALLOC
  -Dprintf=phe_usb_printf -Dsnprintf=phe_usb_snprintf
  -Wall -Wno-unused -Wno-pointer-sign -Wno-unused-but-set-variable
  -I"$FB/stand/kshim" -I"$FB/sys" -I"$HERE/../kinc"
  -DUSB_GLOBAL_INCLUDE_FILE='"bsd_global.h"' -D'__uintptr_t=unsigned long' -DUSB_MSCTEST_BULK_SIZE=65536
  ${USBHOST_DEFS:-} )
objs=(); sysobjs=()
for s in "${KSRCS[@]}" "${SRCS[@]}"; do
  o="$O/$(basename "${s%.c}").o"; "$GCC" "${CFLAGS[@]}" -c "$s" -o "$o"; objs+=("$o")
done
for s in "${KSRCS[@]}"; do
  o="$O/$(basename "${s%.c}").o"; "$OBJCOPY" -j .debug.sysinit -O binary "$o" "${o%.o}.osys"; sysobjs+=("${o%.o}.osys")
done
cat "${sysobjs[@]}" > "$O/sysinit.bin"
[ -x "$O/sysinit" ] || "$HOSTCC" -O2 -I"$FB/stand/kshim" -o "$O/sysinit" "$FB/stand/usb/tools/sysinit.c"
"$O/sysinit" -i "$O/sysinit.bin" -o "$O/sysinit_data.c" -k sysinit -s sysinit_data
"$O/sysinit" -i "$O/sysinit.bin" -o "$O/sysuninit_data.c" -R -k sysuninit -s sysuninit_data
for s in "$O/sysinit_data.c" "$O/sysuninit_data.c"; do
  o="${s%.c}.o"; "$GCC" "${CFLAGS[@]}" -c "$s" -o "$o"; objs+=("$o")
done
"$GCC" "${ARCH[@]}" -Wall -c "$HERE/phe_usb_libc.c" -o "$O/phe_usb_libc.o"; objs+=("$O/phe_usb_libc.o")
# one object; the .debug.sysinit records stay out of the kernel image
"$LD" -r --wrap=usbd_get_page -o "$O/usbhost_bsd.o" "${objs[@]}"
"$OBJCOPY" --remove-section=.debug.sysinit "$O/usbhost_bsd.o"
echo "usbhost: $O/usbhost_bsd.o"
