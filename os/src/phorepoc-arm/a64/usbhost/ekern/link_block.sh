# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-1.0
#
# link_block.sh -- sourced by tools/kernel/link_ekern.sh for A64_USBHOST=1 (BOARD=a64): the USB host (FreeBSD's USB
# stack on its kshim + our host and SoC glue) linked into ekern. Uses the link script's ccq, EXTRA, ARMGCC/GCC,
# VAROBJS, BOARD_WRAPS and A64DEFS. USBHOST_OBJ = a prebuilt usbhost_bsd.o (build_bsd.sh elsewhere, e.g. in a
# container when the link runs on a host without a native cc for the SYSINIT tool).
UH="${USBHOST_DIR:-src/phorepoc-arm/a64/usbhost}"
# A64_USBHOST_TRACE=1: the bring-up and write-path breadcrumbs (SD-log flushed: a hang shows in slot A's replay).
# Default off; the kernel part and the BSD part (usbhost_bsd.o) get the same -DPHE_USBHOST_TRACE.
if [ "${A64_USBHOST_TRACE:-0}" = 1 ]; then
  USBHOST_DEFS="${USBHOST_DEFS:-} -DPHE_USBHOST_TRACE"
  A64DEFS="${A64DEFS:-} -DPHE_USBHOST_TRACE"
fi
if [ -z "${USBHOST_OBJ:-}" ]; then
  # the H3 model (A64_H3TEST) has no USB PHY to program; the A64 builds aw_usbphy.c
  case " ${EXTRA[*]} " in *" -DA64_H3TEST "*) UPHY=0 ;; *) UPHY=1 ;; esac
  USBHOST_NET="${USBHOST_NET:-0}" MODEM_EG25="${MODEM_EG25:-../modem-eg25}" USBHOST_PHY="$UPHY" GCC="${ARMGCC:-$GCC}" USBHOST_DEFS="${USBHOST_DEFS:-}" bash "$UH/ekern/build_bsd.sh" .variant-build/usbhost >&2
  USBHOST_OBJ=.variant-build/usbhost/usbhost_bsd.o
fi
A64DEFS="${A64DEFS:-} -DA64_USBHOST"
[ "${USBHOST_NET:-0}" = 1 ] && A64DEFS="${A64DEFS} -DA64_USBHOST_NET"
ccq -k "$UH/ekern/phe_usb_ekern.cpp" -O2 -fno-function-sections -fno-data-sections "${EXTRA[@]}" ${A64DEFS:-} \
  -o .variant-build/phe_usb_ekern.o
ccq -k "$UH/ekern/a64_usbhost.cpp" -O2 -fno-function-sections -fno-data-sections "${EXTRA[@]}" ${A64DEFS:-} \
  -o .variant-build/a64_usbhost.o
ccq -k "$UH/ekern/phe_usb_comm.cpp" -O2 -fno-function-sections -fno-data-sections "${EXTRA[@]}" ${A64DEFS:-}   -o .variant-build/phe_usb_comm.o
VAROBJS+=("$USBHOST_OBJ" .variant-build/phe_usb_ekern.o .variant-build/a64_usbhost.o .variant-build/phe_usb_comm.o)
# USBHOST_NET=1: mobile data -- the CDC ECM/NCM class driver (modem-eg25's if_cdce.c, in usbhost_bsd.o above) as the
# "Ethernet.USB" PDD under the Ethernet LDD (phe_usb_ether.cpp)
if [ "${USBHOST_NET:-0}" = 1 ]; then
  ccq -k "$UH/ekern/phe_usb_ether.cpp" -O2 -fno-function-sections -fno-data-sections "${EXTRA[@]}" ${A64DEFS:-} -o .variant-build/phe_usb_ether.o
  VAROBJS+=(.variant-build/phe_usb_ether.o)
  # the REAL Ethernet LDD (d_ethernet.cpp, entry renamed as the Comm LDD's is): the PDD below installs it
  ccq -k "$B/drivers/ethernet/d_ethernet.cpp" -I"$B/drivers/ethernet" -I"$B/include/drivers" -I"$B/include" -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" -DCreateLogicalDevice=CreateLogicalDevice_enet -DKernelModuleEntry=KernelModuleEntry_enet \
    -o .variant-build/d_ethernet.o
  VAROBJS+=(.variant-build/d_ethernet.o)
fi
