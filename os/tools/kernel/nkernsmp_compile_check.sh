#!/usr/bin/env bash
# Portions Copyright (c) Nokia Corporation and/or its subsidiary(-ies)
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

# nkernsmp_compile_check.sh -- PinePhone SMP rung 0: compile the UNMODIFIED 2010 ARM SMP nanokernel
# (kernel/eka/nkernsmp + nkernsmp/arm, the source list of nkernsmp/nkern.mmp under MARM) with our modern
# toolchain, the SMP sibling of nkern_compile_check.sh. Does NOT link or run. Objects -> .nkernsmp-build/.
# Run from repo root:  bash tools/kernel/nkernsmp_compile_check.sh
# __SMP__ + an ARMv7 CPU (SMP needs LDREX/STREX/DMB); the GIC/SCU/local-timer access is via TScheduler/
# TSubScheduler fields (include/nkernsmp/arm),.
set -eu
REPO="${KVIEW:-$(cd "$(dirname "$0")/../.." && pwd)}"
B="$REPO/baseline/kernelhwsrv/kernel/eka"
MM="$B/memmodel"
OUT="${OUT:-$REPO/.nkernsmp-build}"; mkdir -p "$OUT"

# The SMP kernel runs the FLEXIBLE memory model (the multiple model's ARM glue is UP-only, rung 1); CPU = the
# Cortex-A9 SMP config (the only ARMv7 SMP CPU cpudefs.h knows) at -march=armv7-a, as ekern_compile_check SMP=1.
# GT=1 (default): the GENERIC TIMER instead of the A9 private timer (virt / the PinePhone's A53) -- the overlay
# src/phorepoc-arm/nkernsmp-gt/arm_tmr.h + the three asm/header deltas of tools/kernel/nkernsmp_gt_patch.py
# (patched copies in $OUT/gt/). GT=0 = the unmodified A9 build.
GTD=(); PREINC=""; NCSCHED_CIA="$B/nkernsmp/arm/ncsched.cia"
if [ "${GT:-1}" = 1 ]; then
  py "$REPO/tools/kernel/nkernsmp_gt_patch.py" "$B" "$OUT/gt" >/dev/null
  GTD=(-D__CPU_ARM_HAS_GENERIC_TIMER); PREINC="$REPO/src/phorepoc-arm/nkernsmp-gt $OUT/gt/inc"
  NCSCHED_CIA="$OUT/gt/ncsched.cia"
fi
EXTRA=(-march=armv7-a -D__CPU_HAS_VFP -D__VFP_V3 -U__MARM_ARMV5__ -D__MARM_ARMV7__ -D__CPU_CORTEX_A9__
       -D__SMP__ -D__MEMMODEL_FLEXIBLE__ -DMM_FLEXIBLE "${GTD[@]}"
       -D__CPU_ARM_HAS_GIC -D__NKERN_TIMESTAMP_USE_INLINE_BSP_CODE__ -D__IN_KERNEL__
       -DNK_MAX_IRQS=160   # GIC ids 32-191: the A64 Mali sits at 129-135 (stock 96 = ids 32-127; GPU rung G3)
       -I"$B/include/nkernsmp" -I"$B/include/nkernsmp/arm"
       -I"$B/include/memmodel/epoc" -I"$B/include/memmodel/epoc/flexible"
       -I"$B/include/memmodel/epoc/flexible/arm" -I"$B/include/memmodel/epoc/mmubase"
       -I"$MM/epoc/flexible" -I"$MM/epoc/flexible/mmu" -I"$MM/epoc/flexible/mmu/arm" -I"$MM/epoc/mmubase" -I"$MM/epoc"
       -I"$B/klib" -I"$B/common"
       -I"$REPO/baseline/symbian-2010/oss.FCL.sf.os.kernelhwsrv/halservices/hal/inc")

CPP="nkernsmp/nkern.cpp nkernsmp/nkerns.cpp nkernsmp/sched.cpp nkernsmp/dfcs.cpp nkernsmp/nk_timer.cpp
     nkernsmp/nk_irq.cpp nkernsmp/nk_bal.cpp nkern/nklib.cpp
     nkernsmp/arm/ncsched.cpp nkernsmp/arm/ncirq.cpp nkernsmp/arm/ncutils.cpp nkernsmp/arm/ncthrd.cpp
     nkernsmp/arm/ncglob.cpp nkernsmp/arm/nccpu.cpp"
CIA="common/arm/atomics.cia nkernsmp/arm/vectors.cia nkernsmp/arm/ncsched.cia nkernsmp/arm/nctimer.cia
     nkernsmp/arm/ncutilf.cia nkernsmp/arm/ncirq.cia nkernsmp/arm/ncthrd.cia nkernsmp/arm/ncutils.cia
     nkernsmp/arm/nccpu.cia nkern/arm/nklib.cia"

pass=0; fail=0
for tu in $CPP $CIA; do
  o="$OUT/$(echo "$tu" | tr / _).o"
  src="$B/$tu"; [ "$tu" = nkernsmp/arm/ncsched.cia ] && src="$NCSCHED_CIA"
  [ "${GT:-1}" = 1 ] && [ "$tu" = nkernsmp/arm/ncutils.cia ] && src="$OUT/gt/ncutils.cia"   # GT: TEEHBR delta
  if SYMCC_PREINC="$PREINC" SYMCC_NKERN=nkernsmp SYMCC_ARM=1 bash "$REPO/toolchain/symcc" -k "$src" -O2 -mno-unaligned-access \
        -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
        -o "$o" >/dev/null 2>"$o.err"; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "FAIL $tu ($(grep -c 'error' "$o.err") error lines)"
    grep -iE "error" "$o.err" | head -${ERRS:-2}
  fi
done
echo "=== nkernsmp compile check: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
