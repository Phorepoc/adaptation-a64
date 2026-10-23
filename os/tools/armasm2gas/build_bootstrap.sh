#!/usr/bin/env bash
command -v py >/dev/null 2>&1 || py() { python3 "$@"; }   # Linux has python3, no py launcher
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# build_bootstrap.sh -- translate + assemble + link the WHOLE
# Symbian syborg bootstrap (RVCT armasm -> GAS -> ELF) with our modern toolchain.
#
# The bootstrap is four independent assembly units that each INCLUDE bootcpu.inc
# (which pulls config.inc + bootmacro.inc + the four h2inc-generated .inc) and link
# together:  generic bootmain.s / bootcpu.s / bootutils.s + the syborg PSL syborg.s.
#
# Prereq: run tools/armasm2gas/h2inc.py first (or h2inc_selftest.sh) to (re)generate
# the bootstrap-inc/ directory.  Config is pinned to the syborg target:
#   CFG_MMFlexible (flexible memory model, build-selected via :DEF:), Cortex-A8 (config.inc),
#   unicore (SMP unset -> arm_types/arm_gic dropped), no custom vectors.
#
# Output: $OUT/boot_merged.o  (relocatable ELF, all cross-unit symbols resolved).
# Env:  OUT=<dir> to keep the artifacts;  ARM_BIN=<toolchain bin dir> to override.
set -euo pipefail
cd "$(dirname "$0")/../.."

BIN_DEFAULT="/c/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin"
BIN="${ARM_BIN:-$BIN_DEFAULT}"
AS="$BIN/arm-none-eabi-as"; LD="$BIN/arm-none-eabi-ld"; NM="$BIN/arm-none-eabi-nm"
[ -x "$AS" ] || AS="$(command -v arm-none-eabi-as || true)"
[ -x "$LD" ] || LD="$(command -v arm-none-eabi-ld || true)"
[ -x "$NM" ] || NM="$(command -v arm-none-eabi-nm || true)"
[ -n "$AS" ] && [ -n "$LD" ] || { echo "FAIL: arm-none-eabi toolchain not found"; exit 1; }

KH=baseline/symbian-2010/oss.FCL.sf.os.kernelhwsrv/kernel/eka
QB=baseline/symbian-2010/oss.FCL.sf.adapt.Qemu/baseport/syborg
# BOARD: syborg (default, the 2010 PSL), virt (OURS, QEMU virt) or a64 (OURS, the PinePhone) -- docs/phorepoc/pinephone-port.md.
BOARD="${BOARD:-syborg}"
case "$BOARD" in
  syborg) PSLD="$QB"; PSL_SRC="$QB/bootstrap/syborg.s" ;;
  virt)   PSLD=src/phorepoc-arm/virt; PSL_SRC="$PSLD/bootstrap/virt.s" ;;
  a64)    PSLD=src/phorepoc-arm/a64; PSL_SRC="$PSLD/bootstrap/a64.s"   # OURS, the PinePhone (always an A53)
          case " ${EXTRA_DEFS:-} " in *" -D CFG_A53 "*) ;; *) EXTRA_DEFS="${EXTRA_DEFS:-} -D CFG_A53" ;; esac ;;
  *) echo "FAIL: BOARD must be syborg|virt|a64"; exit 1 ;;
esac

# MM_MODEL: flexible (default) or multiple (2010 syborg reference). Selects the
# generated .inc dir, the mmboot.h include path, and the CFG_MM* define.
MM_MODEL="${MM_MODEL:-flexible}"
case "$MM_MODEL" in
  flexible) G=.bootstrap-gen/bootstrap-inc;          MM_DEF="-D CFG_MMFlexible" ;;
  multiple) G=.bootstrap-gen/bootstrap-inc-multiple; MM_DEF="-D CFG_MMMultiple" ;;
  *) echo "FAIL: MM_MODEL must be flexible|multiple"; exit 1 ;;
esac
case " ${EXTRA_DEFS:-} " in *" -D SMP "*) G="$G-smp" ;; esac   # SMP: its own generated includes (h2inc SMP=1)

[ -f "$G/bootdefs.inc" ] || { echo "FAIL: run tools/armasm2gas/h2inc.py (MM_MODEL=$MM_MODEL) first"; exit 1; }

OUT="${OUT:-$(mktemp -d)}"; mkdir -p "$OUT"
INC=(-I "$G" -I "$PSLD" -I "$PSLD/bootstrap" -I "$KH/include/kernel/arm" \
     -I "$KH/include/kernel" -I "$KH/include" -I "$KH/include/memmodel/epoc/$MM_MODEL/arm")

declare -A SRC=(
  [bootmain]="$KH/kernel/arm/bootmain.s"
  [bootcpu]="$KH/kernel/arm/bootcpu.s"
  [bootutils]="$KH/kernel/arm/bootutils.s"
  [psl]="$PSL_SRC"
)
# EXTRA_DEFS: space-separated extra -D symbols (e.g. "-D CFG_DebugBootRom"). MM model
# defaults to flexible; override the whole set via DEFS if needed.
DEFS="${DEFS:-$MM_DEF} ${EXTRA_DEFS:-}"
OBJS=()
for u in bootmain bootcpu bootutils psl; do
  py tools/armasm2gas/armasm2gas.py "${SRC[$u]}" "${INC[@]}" $DEFS -o "$OUT/$u.S"
  # CFG_A53 (ARMv8 AArch32): no ThumbEE, so the AP boot code's TEEHBR restore (bootcpu.s, MCR p14,6,...) is UNDEF
  # -- drop it (asserted to be exactly one line), as the nanokernel's deltas do (tools/kernel/nkernsmp_gt_patch.py)
  if [ "$u" = bootcpu ] && [[ " $DEFS " == *" -D CFG_A53 "* ]]; then
    n=$(grep -c -i -E "^[[:space:]]*mcr[[:space:]]+p14,[[:space:]]*6," "$OUT/$u.S" || true)
    [ "$n" = 1 ] || { echo "FAIL: CFG_A53 TEEHBR delta: expected 1 MCR p14,6 in bootcpu, found $n"; exit 1; }
    sed -i -E '/^[[:space:]]*[Mm][Cc][Rr][[:space:]]+p14,[[:space:]]*6,/d' "$OUT/$u.S"
  fi
  "$AS" -mcpu=cortex-a8 "$OUT/$u.S" -o "$OUT/$u.o"
  OBJS+=("$OUT/$u.o")
  echo "  assembled $u"
done

"$LD" -r -o "$OUT/boot_merged.o" "${OBJS[@]}"
undef="$("$NM" "$OUT/boot_merged.o" | grep -E ' U ' | awk '{print $2}' | sort -u || true)"
if [ -n "$undef" ]; then
  echo "FAIL: unresolved symbols after link:"; echo "$undef"; exit 1
fi
echo "PASS: bootstrap translated + assembled + linked, 0 unresolved  ->  $OUT/boot_merged.o"
