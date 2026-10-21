#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# nkern_compile_check.sh — compile the UNMODIFIED 2010 nanokernel
# (nkern + its ARM CPU layer) from source with our modern toolchain (toolchain/symcc -k,
# arm-none-eabi-g++ 14.2). Proves the from-source kernel back-end digests cleanly, the
# same way build_euser_dll.py proves euser. Does NOT link or run (that needs ekern +
# memory model + variant + a ROM — M2/M3). Run from repo root:  bash tools/kernel/nkern_compile_check.sh
#
# Findings (2026-09-16): 18/18 compile. Two known gaps handled elsewhere:
#   * nkern/arm/nk_entry.cia  — only has __GCC32__ (old GCCE) and __ARMCC__ (RVCT)
#     branches; our modern EABI toolchain matches neither -> needs a small entry/export
#     glue branch (a startup seam, like the emitter/euser entry). EXCLUDED here.
#   * baseport/syborg/bootstrap/syborg.s — raw RVCT armasm (AREA/PROC/GBLL/MACRO/DCD/
#     INCLUDE), needs an armasm->GAS port. The real M1 cost; separate task.
# .cia TUs need -fno-{function,data}-sections (PC-relative literal pools in hand asm,
# same fix as euser's uc_realx.cia).
set -eu
REPO="${KVIEW:-$(cd "$(dirname "$0")/../.." && pwd)}"
B="$REPO/baseline/kernelhwsrv/kernel/eka"
MM="$B/memmodel"
OUT="${OUT:-$REPO/.nkern-build}"; mkdir -p "$OUT"

# CRITICAL: the nanokernel MUST be compiled with __MEMMODEL_MULTIPLE__ (the same memory model
# as ekern). ncsched.cia's reschedule tail gates its address-space-switch (load the user
# process page directory into TTBR0 via TScheduler::iProcessHandler == DoProcessSwitch) behind
# `#if defined(__MEMMODEL_MULTIPLE__) || defined(__MEMMODEL_FLEXIBLE__)`; without that define
# the switch is compiled out, so a user process is never made addressable and its first thread
# faults the instant EpocThreadFunction touches the (unmapped) user stack -- exactly the "user
# thread scheduled but never reaches its SWI" symptom.
#
# We deliberately DO NOT add the ARMv7/Cortex-A8 CPU defines here (as ekern uses): they would
# enable the SET_CAR/VFP/DACR coprocessor-save macros in the reschedule frame, several of which
# expand without a trailing ';' under our GCC .cia path (RVCT-ism) and, more importantly, would
# change the nkern exception-frame layout. The nkern frame is self-consistent at the current
# (symcc-default) arch and interoperates with ekern; keep it unchanged and add ONLY the memory
# model. Revisit arch alignment only if the user->kernel SVC frame later proves mismatched.
# MMODEL=flexible: the same for the flexible model (the SMP-capable one), objects in .nkern-flex-build/.
if [ "${MMODEL:-multiple}" = flexible ]; then
  MMN=flexible; MMDEF=(-D__MEMMODEL_FLEXIBLE__ -DMM_FLEXIBLE); OUT="${OUT%-build}-flex-build"; mkdir -p "$OUT"
  MMSRC=(-I"$MM/epoc/flexible" -I"$MM/epoc/flexible/mmu" -I"$MM/epoc/flexible/mmu/arm" -I"$MM/epoc/flexible/arm")
else
  MMN=multiple; MMDEF=(-D__MEMMODEL_MULTIPLE__ -DMM_MULTIPLE); MMSRC=(-I"$MM/epoc/multiple" -I"$MM/epoc/multiple/arm")
fi
EXTRA=("${MMDEF[@]}"
       -I"$B/include/memmodel/epoc" -I"$B/include/memmodel/epoc/$MMN"
       -I"$B/include/memmodel/epoc/$MMN/arm" -I"$B/include/memmodel/epoc/mmubase"
       "${MMSRC[@]}" -I"$MM/epoc/mmubase" -I"$MM/epoc"
       -I"$B/klib" -I"$B/common"
       -I"$REPO/baseline/symbian-2010/oss.FCL.sf.os.kernelhwsrv/halservices/hal/inc")
EXTRA+=(${KDEFS:-})   # KDEFS='-D_DEBUG': a debug/KTRACE kernel (same defs for nkern, ekern and the link step)

CPP="nkern/nklib.cpp nkern/nkern.cpp nkern/sched.cpp nkern/dfcs.cpp nkern/nk_timer.cpp \
     nkern/nkerns.cpp nkern/arm/ncsched.cpp nkern/arm/ncutils.cpp nkern/arm/ncthrd.cpp \
     nkern/arm/ncglob.cpp"
CIA="nkern/arm/ncsched.cia nkern/arm/nctimer.cia nkern/arm/ncutils.cia nkern/arm/ncutilf.cia \
     nkern/arm/vectors.cia nkern/arm/ncmonitor.cia nkern/arm/nklib.cia common/arm/atomics.cia"

pass=0; fail=0
for tu in $CPP $CIA; do
  # -mno-unaligned-access: kernel runs with SCTLR.A=1; never let GCC emit unaligned
  # halfword/word accesses for misaligned fields (see ekern_compile_check.sh comment).
  if SYMCC_ARM=1 bash "$REPO/toolchain/symcc" -k "$B/$tu" -O2 -mno-unaligned-access \
        -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
        -o "$OUT/$(basename "$tu").o" >/dev/null 2>"$OUT/err.log"; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "FAIL $tu"; grep -iE "error:|No such|not fixed" "$OUT/err.log" | head -2
  fi
done
echo "=== nkern compile check: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
