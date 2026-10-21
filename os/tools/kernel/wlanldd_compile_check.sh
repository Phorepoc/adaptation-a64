#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-1.0

# wlanldd_compile_check.sh -- compile the REAL 2010 (RCL_3) Symbian WLAN logical device driver (UMAC + OSA + LDD,
# the five mmps of wlan_bearer/wlanldd/wlan_symbian/wlanldd_symbian/group) unmodified for the ARM kernel with the
# modern GCC, kernel-mode, exactly as link_ekern.sh compiles the other built-in drivers (BOARD=a64 SMP=1
# MMODEL=flexible). Objects: .variant-build/wl_ldd_<basename>.o; log: .wlanldd-check.log. Exit 0 = all compiled.
# NDEBUG is NOT defined (the SBS urel build adds it): OsTracePrint/MWlanOsa::FormattedPrint/Assert are live (Kern::Printf,
# Kern::Fault); WLDD_FLAGS=-DNDEBUG compiles the urel form (verified on 3 TUs). TraceDump needs _DEBUG/TRACES: nothing here.
# Header overlays (OURS) live in toolchain/shims/wlanldd_gcc/. Run from anywhere; WL_JOBS=n (default 12). Objects are cached (toolchain/cc_cached).
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"
LOG="$REPO/.wlanldd-check.log"
OUTD="$REPO/.variant-build"

ROOT="$REPO/baseline/symbian-rcl3/oss.FCL.sf.os.wlan"
GRP="$ROOT/wlan_bearer/wlanldd/wlan_symbian/wlanldd_symbian/group"
SHIM="$REPO/toolchain/shims/wlanldd_gcc"

B="$REPO/baseline/symbian-2010/oss.FCL.sf.os.kernelhwsrv/kernel/eka"
MM="$B/memmodel"
HAL="$REPO/baseline/symbian-2010/oss.FCL.sf.os.kernelhwsrv/halservices/hal/inc"
VSPEC="$REPO/src/phorepoc-arm/virt/specific"

# --- the kernel flags: BOARD=a64 SMP=1 MMODEL=flexible, as in link_ekern.sh --------------------------------------
MMN=flexible; MMDEF=(-D__MEMMODEL_FLEXIBLE__ -DMM_FLEXIBLE)
MMSRC=(-I"$MM/epoc/flexible" -I"$MM/epoc/flexible/mmu" -I"$MM/epoc/flexible/mmu/arm" -I"$MM/epoc/flexible/arm")
SMPDEF=(-D__SMP__ -D__CPU_HAS_VFP -D__VFP_V3 -U__CPU_CORTEX_A8N__ -D__CPU_CORTEX_A9__ -D__CPU_ARM_HAS_GIC
        -D__NKERN_TIMESTAMP_USE_INLINE_BSP_CODE__ -D__CPU_ARM_HAS_GENERIC_TIMER)
NO=.nkernsmp-build
export SYMCC_NKERN=nkernsmp
export SYMCC_PREINC="$SHIM $REPO/src/phorepoc-arm/nkernsmp-gt $REPO/$NO/gt/inc"
EXTRA=(-march=armv7-a -mno-unaligned-access -U__MARM_ARMV5__ -D__MARM_ARMV7__ -D__CPU_CORTEX_A8N__ -D__DEMAND_PAGING__
  "${MMDEF[@]}" -D__IN_KERNEL__ -D__EXPORT_MEMCPY__
  -I"$B/include/memmodel/epoc" -I"$B/include/memmodel/epoc/$MMN" -I"$B/include/memmodel/epoc/$MMN/arm"
  -I"$B/include/memmodel/epoc/mmubase" "${MMSRC[@]}" -I"$MM/epoc/mmubase" -I"$B/klib" -I"$B/common"
  -I"$HAL" -I"$VSPEC")
EXTRA+=("${SMPDEF[@]}" -D__A64__)
MED=(-I"$B/include/drivers" -I"$B/include")
# --- the WLAN driver flags ---------------------------------------------------------------------------------------
# RD_WLAN_DDK: bldcodeline.hrh (the product's) defines it; our compiler wrapper does not pre-include it.
# -fno-access-control / -fno-threadsafe-statics: as the user-side WLAN builder. -Wno-*: noise only (the TTrue/TFalse enum-compare
# conditionals, the e32cmn.inl overlay twin refs, a Handle<T> friend) -- no errors needed any workaround.
WLI=(-I"$ROOT/wlan_bearer/wlanldd/wlan_symbian/wlanldd_symbian/inc"
     -I"$ROOT/wlan_bearer/wlanldd/wlan_common/umac_common/inc"
     -I"$ROOT/wlan_bearer/wlanldd/wlan_common/osa_common/inc"
     -I"$ROOT/wlan_plat/wlan_hal_api/inc" -I"$ROOT/wlan_plat/wlan_osa_api/inc"
     -I"$ROOT/wlan_plat/wlan_hpa_api/inc" -I"$ROOT/wlan_plat/wlan_busaccess_api/inc")
WLFLAGS=(-DRD_WLAN_DDK -fno-access-control -fno-threadsafe-statics -Wno-enum-compare -Wno-return-local-addr -Wno-non-template-friend ${WLDD_FLAGS:-})

# --- one TU (worker mode): $1 = src, $2 = obj, rest = -D macros ----------------------------------------------------
if [ "${1:-}" = "--one" ]; then
  shift; src="$1"; obj="$2"; shift 2
  extra=()
  case "$(basename "$src")" in
    WLanLogicalDevice.cpp) extra=(-DCreateLogicalDevice=CreateLogicalDevice_wlanldd -DKernelModuleEntry=KernelModuleEntry_wlanldd) ;;
  esac
  out="$(SYMCC_ARM=1 bash "$REPO/toolchain/symcc" -k "$src" -O2 -fno-function-sections -fno-data-sections \
        "${EXTRA[@]}" "${MED[@]}" "${WLI[@]}" "${WLFLAGS[@]}" "$@" "${extra[@]}" -o "$obj" 2>&1)"
  rc=$?
  { echo "=== $(basename "$src") -> $(basename "$obj")  rc=$rc"; echo "$out"; } > "$obj.log"
  [ $rc -eq 0 ] && grep -q 'symcc exit: 0' <<<"$out" || exit 1
  exit 0
fi

# --- the jobs (tools/kernel/wlanldd_jobs.py parses the mmps) + one toolchain/cc_batch.py run --------------------
# (was a bash loop spawning grep/cat/cut/basename/dirname/tr per TU and a bash worker per compile: ~3 min of process
# starts on this machine, 2026-10-10; now ~1 s + the stale compiles in parallel)
mkdir -p "$OUTD"
: > "$LOG"
printf '%s\0' -O2 -fno-function-sections -fno-data-sections "${EXTRA[@]}" "${MED[@]}" "${WLI[@]}" "${WLFLAGS[@]}" > "$OUTD/.wl_ldd_flags"
n="$(py "$REPO/tools/kernel/wlanldd_jobs.py" "$GRP" "$OUTD/.wl_ldd_flags" "$OUTD/.wl_ldd_jobs" "$OUTD")" || exit 1
echo "wlanldd_compile_check: $n translation units (5 mmps)" | tee -a "$LOG"
py "$REPO/toolchain/cc_batch.py" "$OUTD/.wl_ldd_jobs" -j "${WL_JOBS:-12}" | tee -a "$LOG"
rc=${PIPESTATUS[0]}
cat "$OUTD"/wl_ldd_*.o.log >> "$LOG" 2>/dev/null
[ "$rc" -eq 0 ]
