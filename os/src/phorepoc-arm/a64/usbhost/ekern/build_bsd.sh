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
# USBHOST_SERIAL=1 (default): ucom + uftdi + u3g (unmodified) on our tty layer (phe_usb_tty.c); usbdevs.h is generated
# from FreeBSD's usbdevs list by its own awk script, as the FreeBSD build does
GEN=(); EXTRA_INC=()
if [ "${USBHOST_SERIAL:-1}" = 1 ]; then
  mkdir -p "$O/gen"
  ( cd "$O/gen" && awk -f "$FB/sys/tools/usbdevs2h.awk" "$U/usbdevs" -h )
  GEN=( -I"$O/gen" )
  KSRCS+=( "$U/serial/usb_serial.c" "$U/serial/uftdi.c" "$U/serial/u3g.c" "$HERE/../phe_usb_tty.c" )
fi
# USBHOST_NET=1 (the mobile-data path): the CDC ECM/NCM class driver (FreeBSD if_cdce.c, BSD-4-Clause, unmodified, from
# modem-eg25/third_party) on our mbuf/ifnet/uether replacement (phe_usb_net.c); MODEM_EG25 = the modem-eg25 checkout
if [ "${USBHOST_NET:-0}" = 1 ]; then
  MODEM_EG25="${MODEM_EG25:-$HERE/../../../../../../../modem-eg25}"
  KSRCS+=( "$MODEM_EG25/third_party/freebsd/sys/dev/usb/net/if_cdce.c" "$HERE/../phe_usb_net.c" )
  EXTRA_INC=( -I"$MODEM_EG25/third_party/freebsd/sys" )
fi
SRCS=( "$FB/stand/usb/usb_busdma_loader.c" )
# the kernel's ABI: ARMv7-A, ARM state, soft float, 32-bit enums, no unaligned accesses, no libc headers
ARCH=( -march=armv7-a -marm -mfloat-abi=soft -fno-short-enums -mno-unaligned-access -ffreestanding -fno-strict-aliasing
  -O2 -fno-pic -fno-common )
CFLAGS=( "${ARCH[@]}" -nostdinc -isystem "$("$GCC" -print-file-name=include)" -D_STANDALONE -DHAVE_MALLOC
  -Dprintf=phe_usb_printf -Dsnprintf=phe_usb_snprintf
  -Wall -Wno-unused -Wno-pointer-sign -Wno-unused-but-set-variable
  -I"$FB/stand/kshim" -I"$FB/sys" -I"$HERE/../kinc"
  -DUSB_GLOBAL_INCLUDE_FILE='"bsd_global.h"' -D'__uintptr_t=unsigned long' -DUSB_MSCTEST_BULK_SIZE=65536
  "${GEN[@]}" "${EXTRA_INC[@]}" ${USBHOST_DEFS:-} )
objs=(); sysobjs=()
for s in "${KSRCS[@]}" "${SRCS[@]}"; do
  o="$O/$(basename "${s%.c}").o"; "$GCC" "${CFLAGS[@]}" -c "$s" -o "$o"; objs+=("$o")
  # UCOM_UNLOAD_DRAIN (usb_serial.h) names its record sysuninit_var in every driver: unique per file in FreeBSD's
  # kernel (UNIQ_NAME adds the line), not with the kshim's UNIQ_NAME. The records are the same (ucom_drain_all at
  # unload, which never happens here): weak, so the copies merge
  case "$s" in */serial/*) "$OBJCOPY" --weaken-symbol=sysuninit_var "$o" ;; esac
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
"$GCC" "${ARCH[@]}" ${USBHOST_DEFS:-} -Wall -c "$HERE/phe_usb_libc.c" -o "$O/phe_usb_libc.o"; objs+=("$O/phe_usb_libc.o")
# one object; the .debug.sysinit records stay out of the kernel image
"$LD" -r --wrap=usbd_get_page -o "$O/usbhost_bsd.o" "${objs[@]}"
"$OBJCOPY" --remove-section=.debug.sysinit "$O/usbhost_bsd.o"
echo "usbhost: $O/usbhost_bsd.o"
