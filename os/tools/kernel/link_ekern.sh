#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# link_ekern.sh -- link the from-source EKA2 kernel (Track B M4).
# Prereqs: bash tools/kernel/nkern_compile_check.sh ; bash tools/kernel/ekern_compile_check.sh
# (they leave objects in .nkern-build/ and .ekern-build/), plus the variant + seam objects
# this script builds. Produces .ekern-build/ekern.elf.
set -eu
cd "${KVIEW:-$(dirname "$0")/../..}"   # KVIEW: the tree root (the pdk assembles it for the public repos)

B="baseline/symbian-2010/oss.FCL.sf.os.kernelhwsrv/kernel/eka"
QB="baseline/symbian-2010/oss.FCL.sf.adapt.Qemu/baseport/syborg"
# BOARD: syborg (default) or virt (OURS, QEMU virt -- ): the variant sources, and on
# virt no Syborg framebuffer/NVMemory/keyboard/pointer kexts (null glue instead)
BOARD="${BOARD:-syborg}"
case "$BOARD" in
  syborg) VSPEC="$QB/specific"; VTUS="syborg.cpp interrupts.cpp assp.cpp syborg.cia interrupts.cia"; SEAM_DEFS=(); BOARD_WRAPS=() ;;
  virt)   VSPEC=src/phorepoc-arm/virt/specific; VTUS="syborg.cpp interrupts.cpp assp.cpp interrupts.cia"
          SEAM_DEFS=(-DEKSEAM_UART=0xC6002000u '-DEKSEAM_UART_S="0xC6002000"' -DEKSEAM_GT_NANOWAIT ${PDTRACE:+-DEKSEAM_PDTRACE})
          BOARD_WRAPS=(--wrap _ZN1K10DoNanoWaitEm ${PDTRACE:+--wrap _ZN22PageDirectoryAllocator5AllocEjRm --wrap _ZN22PageDirectoryAllocator4FreeEj}) ;;   # NanoWait on the generic timer (seam)
  a64)    VSPEC=src/phorepoc-arm/virt/specific; VTUS="syborg.cpp interrupts.cpp assp.cpp interrupts.cia"   # the virt variant, -D__A64__
          SEAM_DEFS=(-DEKSEAM_UART=0xC6003000u '-DEKSEAM_UART_S="0xC6003000"' -DEKSEAM_UART_LSR=0x14 -DEKSEAM_GT_NANOWAIT)
          BOARD_WRAPS=(--wrap _ZN1K10DoNanoWaitEm --wrap _ZN4Kern13ThreadDesReadEP7DThreadPKvR5TDes8ii --wrap _ZN13DPowerHandler13PowerDownDoneEv --wrap _ZN4Kern7RestartEi) ;;   # + kernel-thread desc reads, PowerDownDone logging, Restart keeps slot B (a64_glue.cpp)
  *) echo "FAIL: BOARD must be syborg|virt|a64"; exit 1 ;;
esac
HAL="$(cd baseline/symbian-2010/oss.FCL.sf.os.kernelhwsrv/halservices/hal/inc && pwd)"
MM="$B/memmodel"
BIN_DEFAULT="/c/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin"
BIN="${ARM_BIN:-$BIN_DEFAULT}"
LD="$BIN/arm-none-eabi-ld"; GCC="$BIN/arm-none-eabi-gcc"
[ -x "$GCC" ] || GCC="$(command -v arm-none-eabi-gcc)"   # (Linux / the toolchain image: on the PATH)
[ -x "$LD" ] || LD="$(command -v arm-none-eabi-ld)"
LIBGCC="$(dirname "$("$GCC" -mcpu=cortex-a8 -print-libgcc-file-name 2>/dev/null)")"

# MMODEL=multiple (default) | flexible (the SMP-capable model; objects from `MMODEL=flexible` nkern/ekern compile
# checks.nkern-flex-build/ +.ekern-flex-build/, which also receives ekern.elf).
if [ "${MMODEL:-multiple}" = flexible ]; then
  MMN=flexible; MMDEF=(-D__MEMMODEL_FLEXIBLE__ -DMM_FLEXIBLE); KO=.ekern-flex-build; NO=.nkern-flex-build
  MMSRC=(-I"$MM/epoc/flexible" -I"$MM/epoc/flexible/mmu" -I"$MM/epoc/flexible/mmu/arm" -I"$MM/epoc/flexible/arm")
else
  MMN=multiple; MMDEF=(-D__MEMMODEL_MULTIPLE__ -DMM_MULTIPLE); KO=.ekern-build; NO=.nkern-build
  MMSRC=(-I"$MM/epoc/multiple")
fi
# SMP=1 (PinePhone SMP rung; needs MMODEL=flexible -- the multiple model is UP-only on ARM): objects from
# `bash tools/kernel/nkernsmp_compile_check.sh` (.nkernsmp-build/) + `SMP=1 MMODEL=flexible bash
# tools/kernel/ekern_compile_check.sh` (.ekernsmp-flex-build/); the variant + seam compile against nkernsmp with the
# same CPU/GIC/generic-timer/timestamp defines, and the generic-timer overlay headers first (SYMCC_PREINC).
SMPDEF=()
if [ "${SMP:-0}" = 1 ]; then
  [ "$MMN" = flexible ] || { echo "FAIL: SMP=1 needs MMODEL=flexible"; exit 1; }
  KO=.ekernsmp-flex-build; NO=.nkernsmp-build
  SMPDEF=(-D__SMP__ -D__CPU_HAS_VFP -D__VFP_V3 -U__CPU_CORTEX_A8N__ -D__CPU_CORTEX_A9__ -D__CPU_ARM_HAS_GIC
          -D__NKERN_TIMESTAMP_USE_INLINE_BSP_CODE__ -D__CPU_ARM_HAS_GENERIC_TIMER)
  export SYMCC_NKERN=nkernsmp SYMCC_PREINC="$(pwd)/src/phorepoc-arm/nkernsmp-gt $(pwd)/$NO/gt/inc"
fi
EXTRA=(-march=armv7-a -mno-unaligned-access -U__MARM_ARMV5__ -D__MARM_ARMV7__ -D__CPU_CORTEX_A8N__ -D__DEMAND_PAGING__
  "${MMDEF[@]}" -D__IN_KERNEL__ -D__EXPORT_MEMCPY__
  -I"$B/include/memmodel/epoc" -I"$B/include/memmodel/epoc/$MMN" -I"$B/include/memmodel/epoc/$MMN/arm"
  -I"$B/include/memmodel/epoc/mmubase" "${MMSRC[@]}" -I"$MM/epoc/mmubase" -I"$B/klib" -I"$B/common"
  -I"$HAL" -I"$VSPEC")
EXTRA+=(${KDEFS:-})   # KDEFS='-D_DEBUG': a debug/KTRACE kernel (same defs for nkern, ekern and the link step)
EXTRA+=("${SMPDEF[@]}")   # after the A8 define: -U__CPU_CORTEX_A8N__ must come later on the command line
[ "$BOARD" = a64 ] && EXTRA+=(-D__A64__)   # the A64 deltas in the shared virt variant (syborg.h, assp.cpp)
# the golden image (the text-shell ROM, not PHOREPOC=1): the kernel menu + keyboard on the Menu soft key (a64_bars.cpp)
GOLDEN=(); [ "${PHOREPOC:-0}" = 1 ] || GOLDEN=(-DA64_GOLDEN)
# PH_BOOT_GUARD_S=<s> / PH_LIVE_GUARD_S=<s>: the slot-B boot-completion / liveness guard of a64_glue.cpp (180 / 60 s) -- a plain env var that
# reaches the compile through ./ph pp-image (A64DEFS did not in the 2026-10-16 ALF runs: the recorded a64_glue.o.cmd had no -D for it)
[ "${NO_MON1S:-0}" = 1 ] && A64DEFS="${A64DEFS:-} -DA64_NO_MON1S"   # A/B: no 1 s cpu/RAM monitor
GUARDDEFS=(); [ -n "${PH_BOOT_GUARD_S:-}" ] && GUARDDEFS+=(-DA64_BOOT_GUARD_S="$PH_BOOT_GUARD_S"); [ -n "${PH_LIVE_GUARD_S:-}" ] && GUARDDEFS+=(-DA64_LIVE_GUARD_S="$PH_LIVE_GUARD_S")

echo "[1/3] compile the $BOARD variant (kasyborg: HwInit hooks, interrupts, assp)"
# the objects persist between builds (toolchain/cc_cached: a TU recompiles only when it, a header it includes, or its
# command line changed); FULL=1 = from scratch
[ "${FULL:-0}" = 1 ] && rm -rf .variant-build
mkdir -p .variant-build "$KO" "$NO"
# every TU below is QUEUED (ccq = symcc's arguments, ccq --raw = a plain compiler line) and compiled by ONE
# toolchain/cc_batch.py run before the link: only the stale ones, in parallel, straight into g++ -- a Git Bash start
# per TU cost ~1.4 s on this machine (2026-10-10), the per-TU wrapper + workers were the build time
CCJOBS=.variant-build/.cc_jobs; : > "$CCJOBS"
ccq() { printf '%s\0' "$@" >> "$CCJOBS"; printf '\0' >> "$CCJOBS"; }
VAROBJS=()
for tu in $VTUS; do
  ccq -k "$VSPEC/$tu" -O2 -fno-function-sections -fno-data-sections \
    "${EXTRA[@]}" -o ".variant-build/$tu.o"
  VAROBJS+=(".variant-build/$tu.o")
done

if [ "$BOARD" = a64 ]; then
  echo "[2/3] a64: ELOCD + the internal RAM drive (C:), USB link, the PinePhone display; touch (GT917S), no keys/storage yet"
  MED=(-I"$B/drivers/locmedia" -I"$B/drivers/locmedia/traces" -I"$B/include/drivers" -I"$B/include")
  ccq -k "$B/drivers/locmedia/locmedia.cpp" -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" -DInitExtension=InitExtension_locmedia \
    -DKernelModuleEntry=KernelModuleEntry_locmedia -DCreateLogicalDevice=CreateLogicalDevice_locmedia \
    -o .variant-build/locmedia.o
  ccq -k "$B/drivers/locmedia/dmasupport.cpp" -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" -o .variant-build/dmasupport.o
  # caps.iType = EMediaHardDisk (TEMPORARY, until the SD/eMMC driver, P4): the IRAM C: reports a disk. S60 SSM's
  # CheckUserDrive (customcmds ordinal 13) fails a RAM user drive with KErrNotFound -> the Fail state policy, which
  # starts the UI without WSERV (apsexe died KErrNotFound in RWsSession::Connect; the boot idled; 2026-10-07)
  sed "s/caps.iType=EMediaRam;/caps.iType=EMediaHardDisk;/" "$B/drivers/medint/iram.cpp" > .variant-build/iram_a64.cpp
  # ... and as a "disk" F32 never enlarges it: give C: a fixed 128 MB at creation (512 MB = half the 1 GB the kernel maps: S60 ran out of memory, 2026-10-07) (cenrep died KErrDiskFull)
  sed -i 's/^\(\s*\)TInt size=TInternalRamDrive::Size();/\1TInternalRamDrive::Wait(); if (TInternalRamDrive::Size() < (128<<20)) { TInt ar=TInternalRamDrive::Adjust(128<<20); Kern::Printf("[EKERN][iram] C: adjust r=%d size=%d max=%d", ar, TInternalRamDrive::Size(), TInternalRamDrive::MaxSize()); } TInternalRamDrive::Signal(); TInt size=TInternalRamDrive::Size();/' .variant-build/iram_a64.cpp
  grep -q "Adjust(128<<20)" .variant-build/iram_a64.cpp || { echo "iram.cpp: size line not found"; exit 1; }
  # ... and drop the RAM-only caps a disk must not have: the erase-block overload (= MaxSize, 512 MB: FAT format
  # aligned its data area to it -> no free clusters, cenrep died KErrDiskFull) and the variable-size attribute
  sed -i 's/caps.iEraseBlockSize=TInternalRamDrive::MaxSize();/caps.iEraseBlockSize=0;/; s/caps.iMediaAtt=KMediaAttVariableSize|KMediaAttFormattable;/caps.iMediaAtt=KMediaAttFormattable;/' .variant-build/iram_a64.cpp
  # diagnosis (2026-10-07, "memory low" notes at boot): the size each Caps call reports -- what FAT formats C: from
  sed -i 's/caps.iSize=m.Drive()->iPartitionLen;/caps.iSize=m.Drive()->iPartitionLen; Kern::Printf("[EKERN][iram] Caps iSize=%d total=%d", (TInt)caps.iSize, (TInt)TotalSizeInBytes());/' .variant-build/iram_a64.cpp
  grep -q "caps.iEraseBlockSize=0;" .variant-build/iram_a64.cpp && grep -q "caps.iMediaAtt=KMediaAttFormattable;" .variant-build/iram_a64.cpp || { echo "iram.cpp: caps lines not found"; exit 1; }
  grep -q "caps.iType=EMediaHardDisk;" .variant-build/iram_a64.cpp || { echo "iram.cpp: media type line not found"; exit 1; }
  ccq -k .variant-build/iram_a64.cpp -I"$B/drivers/medint" -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" -DInitExtension=InitExtension_medint \
    -DKernelModuleEntry=KernelModuleEntry_medint -DCreatePhysicalDevice=CreatePhysicalDevice_medint \
    -o .variant-build/iram.o
  ccq -k src/phorepoc-arm/a64/specific/a64_glue.cpp -O2 \
    -fno-function-sections -fno-data-sections "${EXTRA[@]}" ${A64DEFS:-} "${GUARDDEFS[@]}" -o .variant-build/a64_glue.o
  VAROBJS+=(.variant-build/locmedia.o .variant-build/dmasupport.o .variant-build/iram.o .variant-build/a64_glue.o)
  # C: = Syborg's REAL NVMemory media driver on SD partition 3 (src/phorepoc-arm/a64/blk/a64sdblk.cpp)
  ccq -k src/phorepoc-arm/a64/blk/a64sdblk.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" ${A64DEFS:-} -I"$QB/mednvmemory" \
    -I"$B/include/memmodel/epoc" -I"$B/include/memmodel/epoc/$MMN" \
    -DInitExtension=InitExtension_mednvme -DKernelModuleEntry=KernelModuleEntry_mednvme \
    -DCreatePhysicalDevice=CreatePhysicalDevice_mednvme -o .variant-build/a64sdblk.o
  # E: = the same driver again on SD partition 4 (Nokia's mass memory letter; grown to the end of the card)
  ccq -k src/phorepoc-arm/a64/blk/a64sdblk.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" ${A64DEFS:-} -I"$QB/mednvmemory" \
    -I"$B/include/memmodel/epoc" -I"$B/include/memmodel/epoc/$MMN" -DA64SDBLK_E \
    -DInitExtension=InitExtension_mednvmeE -DKernelModuleEntry=KernelModuleEntry_mednvmeE \
    -DCreatePhysicalDevice=CreatePhysicalDevice_mednvmeE -o .variant-build/a64sdblk_e.o
  VAROBJS+=(.variant-build/a64sdblk.o .variant-build/a64sdblk_e.o)
  # USB: the REAL USB client PIL (unmodified) + our A64 MUSB PSL + phlink
  USB=(-I"$B/drivers/usbc/traces" -I"$B/include/drivers" -I"$B/include")
  for tu in chapter9 descriptors misc ps_usbc queue; do
    ccq -k "$B/drivers/usbcc/$tu.cpp" -O2 -fno-function-sections \
      -fno-data-sections "${EXTRA[@]}" "${USB[@]}" -o ".variant-build/usbcc_$tu.o"
    VAROBJS+=(".variant-build/usbcc_$tu.o")
  done
  ccq -k src/phorepoc-arm/a64/usbcc/a64_usbcc.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${USB[@]}" -DInitExtension=InitExtension_usbcc \
    -DKernelModuleEntry=KernelModuleEntry_usbcc -o .variant-build/a64_usbcc.o
  ccq -k src/phorepoc-arm/a64/usbcc/phlink.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${GOLDEN[@]}" "${USB[@]}" -o .variant-build/phlink.o
  VAROBJS+=(.variant-build/a64_usbcc.o .variant-build/phlink.o)
  # ... and its file service: the "PhFs.A64" LDD the user-side helper (src/phfs) serves the PhoneLab Project panel through
  ccq -k src/phorepoc-arm/a64/usbcc/phfs_ldd.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${USB[@]}" -o .variant-build/phfs_ldd.o
  VAROBJS+=(.variant-build/phfs_ldd.o)
  # Display (P4): NuttX's A64 display drivers (Apache-2.0, vendored unmodified) + our glue a64_display.cpp
  DN=src/phorepoc-arm/a64/display/nuttx
  ARMGCC="$(dirname "${SYMBIAN_GXX:-/c/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin/arm-none-eabi-g++}")/arm-none-eabi-gcc"
  [ -x "$ARMGCC" ] || ARMGCC="$GCC"
  for c in "$DN"/*.c; do
    ccq --raw "$ARMGCC" -c -O2 -march=armv5te -marm -mthumb-interwork -fno-short-enums -ffreestanding -fno-strict-aliasing \
      -I"$DN/compat" -I"$DN" -include nuttx/config.h "$c" -o ".variant-build/nx_$(basename "$c" .c).o"
    VAROBJS+=(".variant-build/nx_$(basename "$c" .c).o")
  done
  ccq -k src/phorepoc-arm/a64/display/a64_display.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" ${A64DEFS:-} -o .variant-build/a64_display.o
  VAROBJS+=(.variant-build/a64_display.o)
  # The bars (top bar on every image; the golden menu + keyboard only on the text-shell ROM = not PHOREPOC=1)
  ccq -k src/phorepoc-arm/a64/display/a64_bars.cpp -O2 -fno-function-sections \
    -fno-data-sections -fno-tree-loop-distribute-patterns "${EXTRA[@]}" "${GOLDEN[@]}" -o .variant-build/a64_bars.o
  VAROBJS+=(.variant-build/a64_bars.o)
  # The DE2 video overlay (VI plane, de2-overlay-plan.md): the PheDeOv logical device; the register half is in a64_display.cpp
  ccq -k src/phorepoc-arm/a64/display/a64deov.cpp -O2 -fno-function-sections     -fno-data-sections "${EXTRA[@]}" -o .variant-build/a64deov.o
  VAROBJS+=(.variant-build/a64deov.o)
  # Touch: the GT917S over TWI0, polled (src/phorepoc-arm/a64/input/a64touch.cpp)
  ccq -k src/phorepoc-arm/a64/input/a64touch.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${GOLDEN[@]}" ${VIRT_FB:+-DVIRT_FB_WIDTH=${VIRT_FB%x*} -DVIRT_FB_HEIGHT=${VIRT_FB#*x}} \
    -o .variant-build/a64touch.o
  VAROBJS+=(.variant-build/a64touch.o)
  # GPU (NGA plan rung G3): the Mali Utgard driver (src/phorepoc-arm/mali, lima-derived under MIT) + the A64 board half
  ccq -k src/phorepoc-arm/mali/mali_ldd.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" -o .variant-build/mali_ldd.o
  ccq -k src/phorepoc-arm/a64/gpu/mali_a64.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" -o .variant-build/mali_a64.o
  VAROBJS+=(.variant-build/mali_ldd.o .variant-build/mali_a64.o)
  # Video Engine (VDU plan rung V0): the phevde driver (src/phorepoc-arm/vde) + the A64 board half
  ccq -k src/phorepoc-arm/vde/phevde_ldd.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" -o .variant-build/phevde_ldd.o
  ccq -k src/phorepoc-arm/a64/vde/phevde_a64.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" -o .variant-build/phevde_a64.o
  VAROBJS+=(.variant-build/phevde_ldd.o .variant-build/phevde_a64.o)
  # Power (plan P1/P2): the thermal sensor + the CPU OPP driver (src/phorepoc-arm/a64/power)
  ccq -k src/phorepoc-arm/a64/power/a64_ths.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" -o .variant-build/a64_ths.o
  ccq -k src/phorepoc-arm/a64/power/a64_cpufreq.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" ${A64DEFS:-} -o .variant-build/a64_cpufreq.o
  # P4: the power resource manager (rails + accelerator clocks) the screen-off sequence and the sensors use
  ccq -k src/phorepoc-arm/a64/power/a64_pwrres.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" ${A64DEFS:-} -o .variant-build/a64_pwrres.o
  VAROBJS+=(.variant-build/a64_ths.o .variant-build/a64_cpufreq.o .variant-build/a64_pwrres.o)
  # P5: idle tick suppression + core retirement on the 2010 SMP idle framework (drivers/power/smppower: idlehelper
  # + the sample DSMPIdleHandler, compiled unmodified) with our handler (power/a64_idle.cpp); SMP builds only
  if [ "${SMP:-0}" = 1 ]; then
    SPW="$B/drivers/power/smppower"
    IDLEINC=(-I"$B/include/drivers" -I"$B/include/drivers/smppower/sample_idlehandler")
    ccq -k src/phorepoc-arm/a64/power/a64_idle.cpp -O2 -fno-function-sections -fno-data-sections \
      "${EXTRA[@]}" "${IDLEINC[@]}" ${A64DEFS:-} -o .variant-build/a64_idle.o
    ccq -k "$SPW/idlehelper.cpp" -O2 -fno-function-sections -fno-data-sections "${EXTRA[@]}" "${IDLEINC[@]}" \
      -o .variant-build/smp_idlehelper.o
    ccq -k "$SPW/idlehelper.cia" -O2 -fno-function-sections -fno-data-sections "${EXTRA[@]}" "${IDLEINC[@]}" \
      -o .variant-build/smp_idlehelper_cia.o
    ccq -k "$SPW/sample_idlehandler/smpidlehandler.cpp" -O2 -fno-function-sections -fno-data-sections \
      "${EXTRA[@]}" "${IDLEINC[@]}" -o .variant-build/smp_idlehandler.o
    VAROBJS+=(.variant-build/a64_idle.o .variant-build/smp_idlehelper.o .variant-build/smp_idlehelper_cia.o
              .variant-build/smp_idlehandler.o)
  fi
  # Sound: the REAL shared-chunk sound LDD (entry renamed, as the x86 AC'97
  # build) + our A64 codec/DMA PDD "SoundSc.A64" (src/phorepoc-arm/a64/sound/a64sound.cpp; A64DEFS=-DA64_SOUND_TEST =
  # a 440 Hz boot self-test)
  ccq -k "$B/drivers/soundsc/soundldd.cpp" -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" -DCreateLogicalDevice=CreateLogicalDevice_soundsc \
    -DKernelModuleEntry=KernelModuleEntry_soundsc -o .variant-build/soundldd.o
  ccq -k src/phorepoc-arm/a64/sound/a64sound.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" ${A64DEFS:-} $([ "${MODEM:-0}" = 1 ] && echo -DPHE_MODEM) -o .variant-build/a64sound.o
  # (MODEM=1: + the voice call route, EG25-G PCM on AIF2 <-> earpiece/MIC1, switched by Comm.A64 unit 3 DTR -- telephony-plan.md 16)
  VAROBJS+=(.variant-build/soundldd.o .variant-build/a64sound.o)
  # the volume buttons on the LRADC (src/phorepoc-arm/a64/input/a64keys.cpp)
  ccq -k src/phorepoc-arm/a64/input/a64keys.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" -o .variant-build/a64keys.o
  VAROBJS+=(.variant-build/a64keys.o)
  # Bluetooth rung R1 (pinephone-port.md "Bluetooth: the decision"): the REAL serial LDD (ECOMM, d_comm.cpp, entry
  # renamed) + our UART1 PDD "Comm.A64" (src/phorepoc-arm/a64/bt/a64uart.cpp) = the RTL8723CS's HCI link
  # (one modern-GCC delta: `if (a1<0)` compares a TAny* with 0 -- an error since GCC 11; the overlay casts it)
  sed 's/^\(\s*\)if (a1<0)\(\r\?\)$/\1if ((TInt)a1<0)\2/' "$B/drivers/ecomm/d_comm.cpp" > .variant-build/d_comm_a64.cpp
  grep -q "if ((TInt)a1<0)" .variant-build/d_comm_a64.cpp || { echo "d_comm.cpp: the a1<0 line not found"; exit 1; }
  ccq -k .variant-build/d_comm_a64.cpp -I"$B/drivers/ecomm" -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" -DCreateLogicalDevice=CreateLogicalDevice_ecomm \
    -DKernelModuleEntry=KernelModuleEntry_ecomm -o .variant-build/d_comm.o
  ccq -k src/phorepoc-arm/a64/bt/a64uart.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" $([ "${MODEM:-0}" = 1 ] && echo -DPHE_MODEM) -o .variant-build/a64uart.o
  # (MODEM=1 = modem rung M1, telephony-plan.md: + Comm.A64 unit 3 = UART3 to the EG25-G and its power sequence)
  VAROBJS+=(.variant-build/d_comm.o .variant-build/a64uart.o)
  # WiFi rung 0 (pinephone-port.md "WiFi: the decision"): the SMHC1 SDIO host for the RTL8723CS's WiFi half
  # (src/phorepoc-arm/a64/wlan/a64sdio.cpp; A64DEFS=-DA64_SDIO_TEST = the identification self-test at boot)
  ccq -k src/phorepoc-arm/a64/wlan/a64sdio.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" ${A64DEFS:-} -o .variant-build/a64sdio.o
  VAROBJS+=(.variant-build/a64sdio.o)
  # WiFi, the chip core (pinephone-port.md "WiFi: the chip core"): the vendored rtw88 (Linux, C, unmodified; the
  # RTL8703B subset) against our freestanding Linux shim (wlan/shim/include), mac80211-lite + the module stubs, the
  # nkern runtime (shim_rt.cpp) and the Symbian side (a64wlan.cpp + a64wlan_glue.c). A64DEFS=-DA64_WLAN_TEST = the W2
  # demo at boot (power on, firmware, channel hop, the beacons of nearby APs logged).
  WL=src/phorepoc-arm/a64/wlan; WLI="$("$GCC" -print-file-name=include)"   # (the compiler's own freestanding headers)
  # WIFI_DRV=rtw88 (default since 2026-10-17, boots 954/957/980: OFDM rx at least as good as 8723cs) or WIFI_DRV=8723cs = the fresh vendor-derived driver (rtl8703b/); WIFI_DRV=rtw88 = the vendored rtw88 (its
  # BSD-3 alternative) + the Linux shim + a64wlan_glue.c, with the recorded vendor differences applied
  # (PHE_RTW88_VENDOR_DIFF blocks in rtw88/). Same a64wlan_api.h face either way.
  WIFI_DRV="${WIFI_DRV:-rtw88}"
  if [ "$WIFI_DRV" = rtw88 ]; then
    echo "  WiFi driver: rtw88 (WIFI_DRV=rtw88)"
    WLSRC=($WL/rtw88/{sdio,mac,efuse,rx,util,rtw8723x,rtw8703b,rtw8703b_tables,phy,fw,tx,ps,sec,coex,main,mac80211}.c
           $WL/shim/shim_mac80211.c $WL/shim/rtw88_stubs.c $WL/a64wlan_glue.c)
    # (boot 954: joins, DHCP, NTP -- after the __LITTLE_ENDIAN fix in rtw88_config.h; the test variants of boots 894-952
    #  are gone. Opt-in diagnostics via A64DEFS: -DPHE_MACCNT, -DPHE_RXREGS, -DPHE_RFWIDE)
    WLCF=(-I"$WL/shim/include" -I"$WL" -include "$WL/shim/include/rtw88_config.h" -DPHE_RTW88_VENDOR_DIFF)
    WLCXX=(-DA64_WLAN_RTW88)
  elif [ "$WIFI_DRV" = 8723cs ]; then
    WLSRC=($WL/rtl8703b/r8703b_{io,mac,phy,efuse,trx,dm,iqk,api,cam,tables}.c)
    WLCF=(-I"$WL" -Wall -Wno-unused-function)
    WLCXX=()
  else
    echo "WIFI_DRV=$WIFI_DRV: expected rtw88 or 8723cs"; exit 1
  fi
  for c in "${WLSRC[@]}"; do
    ccq --raw "$GCC" -c -O2 -march=armv7-a -marm -mno-unaligned-access -fno-short-enums -ffreestanding -fno-strict-aliasing \
      -fno-builtin -std=gnu11 -nostdinc -isystem "$WLI" "${WLCF[@]}" \
      ${A64DEFS:-} "$c" -o ".variant-build/wl_$(basename "$c" .c).o"
    VAROBJS+=(".variant-build/wl_$(basename "$c" .c).o")
  done
  for cc in a64wlan shim/shim_rt; do
    ccq -k $WL/$cc.cpp -O2 -fno-function-sections -fno-data-sections \
      "${EXTRA[@]}" "${MED[@]}" -I"$WL" "${WLCXX[@]}" ${A64DEFS:-} -o ".variant-build/wl_$(basename $cc).o"
    VAROBJS+=(".variant-build/wl_$(basename $cc).o")
  done
  # W3: the WHA PDD "wlan.phys" (a64wlanpdd.cpp) against the 2010 WLAN platform API headers (the RCL_3 tree the user
  # stack is built from; RD_WLAN_DDK = the MWlanPddIface form of the LDD/PDD boundary, as the product's bldcodeline.hrh)
  WLP=baseline/symbian-rcl3/oss.FCL.sf.os.wlan/wlan_plat
  ccq -k $WL/a64wlanpdd.cpp -O2 -fno-function-sections -fno-data-sections \
    "${EXTRA[@]}" "${MED[@]}" -DRD_WLAN_DDK -I"$WL" -I"$WLP/wlan_hal_api/inc" -I"$WLP/wlan_osa_api/inc" -I"$WLP/wlan_hpa_api/inc" \
    ${A64DEFS:-} -o .variant-build/wl_a64wlanpdd.o
  VAROBJS+=(.variant-build/wl_a64wlanpdd.o)
  #. and its host-side security (CCMP / TKIP / WEP headers, Michael, replay)
  ccq -k $WL/a64wlansec.cpp -O2 -fno-function-sections -fno-data-sections     "${EXTRA[@]}" "${MED[@]}" -DRD_WLAN_DDK -I"$WL" -I"$WLP/wlan_hal_api/inc" -I"$WLP/wlan_osa_api/inc" -I"$WLP/wlan_hpa_api/inc"     ${A64DEFS:-} -o .variant-build/wl_a64wlansec.o
  VAROBJS+=(.variant-build/wl_a64wlansec.o)
  # ... and the REAL wlanldd (the UMAC + its OSA + the LDD, unmodified RCL_3 source) as a built-in device:
  # tools/kernel/wlanldd_compile_check.sh compiles it into .variant-build/wl_ldd_*.o
  WLDD_FLAGS="${WLDD_FLAGS:--DNDEBUG}" bash tools/kernel/wlanldd_compile_check.sh >/dev/null || { echo "wlanldd compile FAILED (see .wlanldd-check.log)"; exit 1; }
  VAROBJS+=(.variant-build/wl_ldd_*.o)
  # Sensors rung S1 (pinephone-port.md "Sensors: the decision"): the TWI1 sensor bus helper
  # (src/phorepoc-arm/a64/sensors/a64twi.cpp; A64DEFS=-DA64_SENSOR_TEST = the chip-identity probe at boot)
  ccq -k src/phorepoc-arm/a64/sensors/a64twi.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" ${A64DEFS:-} -o .variant-build/a64twi.o
  VAROBJS+=(.variant-build/a64twi.o)
  # (src/phorepoc-arm/a64/sensors/a64sensors.cpp: the "Sensors.A64" LDD -- unit 0 = the MPU-6050 accelerometer)
  ccq -k src/phorepoc-arm/a64/sensors/a64sensors.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" ${A64DEFS:-} -o .variant-build/a64sensors.o
  VAROBJS+=(.variant-build/a64sensors.o)
  # camera (camera-plan.md C1-phone + C2): the 2010 shared-chunk camera LDD + the template PDD (fake sensor), unmodified,
  # entries renamed as the x86 build (make_ekern.sh), + our board glue (src/phorepoc-arm/a64/camera/a64camera.cpp: installs
  # both; the C2 rails/MCLK/CCI chip-ID probe of the OV5640 + GC2145, A64DEFS=-DA64_NO_CAMERA_PROBE = off)
  CAMD=third_party/symbian2010-camerasc
  CAMP=$CAMD/bsptemplate/asspandvariant/template_variant/camerasc
  ccq -k "$CAMD/kernel/eka/drivers/camerasc/cameraldd.cpp" -O2 -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
    -I"$CAMD/kernel/eka/include" "${MED[@]}" -DCreateLogicalDevice=CreateLogicalDevice_camerasc \
    -DKernelModuleEntry=KernelModuleEntry_camerasc -DDBufferManager=DCamBufferManager -o .variant-build/cameraldd.o
  ccq -k "$CAMP/camerasc.cpp" -O2 -fno-function-sections -fno-data-sections "${EXTRA[@]}" -I"$CAMD/kernel/eka/include" \
    "${MED[@]}" -I"$CAMP" -DCreatePhysicalDevice=CreatePhysicalDevice_camerasc \
    -DKernelModuleEntry=KernelModuleEntry_camerasc_pdd -o .variant-build/camerasc_pdd.o
  ccq -k "$CAMP/camerasc_sensor.cpp" -O2 -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
    -I"$CAMD/kernel/eka/include" "${MED[@]}" -I"$CAMP" -DDTemplateSensorIf=DTemplateFakeSensorIf -o .variant-build/camerasc_sensor.o
  ccq -k src/phorepoc-arm/a64/camera/a64camera.cpp -O2 -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
    -I"$CAMD/kernel/eka/include" "${MED[@]}" -I"$CAMP" ${PH_CAM_BARS:+-DA64_CAMERA_BARS_HOLD} ${A64DEFS:-} -o .variant-build/a64camera.o
  VAROBJS+=(.variant-build/cameraldd.o .variant-build/camerasc_pdd.o .variant-build/camerasc_sensor.o .variant-build/a64camera.o)
  # ... and the Symbian side of it: Syborg's REAL framebuffer driver on a register model (a64fb.cpp), the generic
  # RDisplayChannel LDD and the Syborg fb glue -- exactly as BOARD=virt below
  py tools/kernel/svpfb_patch.py "$QB/svpframebuffer/svpframebuffer.cpp" .variant-build/svpfb >/dev/null
  ccq -k src/phorepoc-arm/a64/display/a64fb.cpp -O2 -fno-function-sections     -fno-data-sections "${EXTRA[@]}" -I.variant-build/svpfb -I"$QB/svpframebuffer" -Isrc/phorepoc-arm/a64/display/lddinc -I"$B/include/drivers" -I"$B/include"     ${VIRT_FB:+-DVIRT_FB_WIDTH=${VIRT_FB%x*} -DVIRT_FB_HEIGHT=${VIRT_FB#*x}} -o .variant-build/a64fb.o
  ccq -k "$B/drivers/display/display.cpp" -O2 -fno-function-sections     -fno-data-sections "${EXTRA[@]}" -Isrc/phorepoc-arm/a64/display/lddinc -I"$B/include/drivers" -I"$B/include" -o .variant-build/display.o
  ccq -k tools/kernel/ekern_fb_glue.cpp -O2 -fno-function-sections     -fno-data-sections "${EXTRA[@]}" -I"$QB/svpframebuffer" -Isrc/phorepoc-arm/a64/display/lddinc -I"$B/include/drivers" -I"$B/include"     -o .variant-build/ekern_fb_glue.o
  VAROBJS+=(.variant-build/a64fb.o .variant-build/display.o .variant-build/ekern_fb_glue.o)
  # the NGA display channel PDD (vblank flips, a64dispchan.cpp) on top of the legacy driver above. A64_DISPLAY=legacy:
  # left out = the golden image's display (Syborg's PDD stays installed, as there: fine for a non-NGA ROM, which never
  # opens a display channel; an NGA=1 ROM needs this file -- Syborg's PDD hangs the phone on open)
  if [ "${A64_DISPLAY:-}" != legacy ]; then
    ccq -k src/phorepoc-arm/a64/display/a64dispchan.cpp -O2 -fno-function-sections \
      -fno-data-sections "${EXTRA[@]}" -I.variant-build/svpfb -I"$QB/svpframebuffer" -Isrc/phorepoc-arm/a64/display/lddinc -I"$B/include/drivers" -I"$B/include" \
      -o .variant-build/a64dispchan.o
    VAROBJS+=(.variant-build/a64dispchan.o)
  else
    echo "  A64_DISPLAY=legacy: the minimal framebuffer display only (no a64dispchan.cpp)"
  fi
elif [ "$BOARD" = virt ]; then
  echo "[2/3] virt: no Syborg kexts; ELOCD + the internal RAM drive (as the x86 port, X4.17)"
  MED=(-I"$B/drivers/locmedia" -I"$B/drivers/locmedia/traces" -I"$B/include/drivers" -I"$B/include")
  ccq -k "$B/drivers/locmedia/locmedia.cpp" -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" -DInitExtension=InitExtension_locmedia \
    -DKernelModuleEntry=KernelModuleEntry_locmedia -DCreateLogicalDevice=CreateLogicalDevice_locmedia \
    -o .variant-build/locmedia.o
  ccq -k "$B/drivers/locmedia/dmasupport.cpp" -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" -o .variant-build/dmasupport.o
  ccq -k "$B/drivers/medint/iram.cpp" -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" -DInitExtension=InitExtension_medint \
    -DKernelModuleEntry=KernelModuleEntry_medint -DCreatePhysicalDevice=CreatePhysicalDevice_medint \
    -o .variant-build/iram.o
  # C: = Syborg's REAL NVMemory media driver on a virtio-blk disk (src/phorepoc-arm/virt/blk/virtblk.cpp)
  ccq -k src/phorepoc-arm/virt/blk/virtblk.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" "${MED[@]}" -I"$QB/mednvmemory" \
    -I"$B/include/memmodel/epoc" -I"$B/include/memmodel/epoc/$MMN" \
    -DInitExtension=InitExtension_mednvme -DKernelModuleEntry=KernelModuleEntry_mednvme \
    -DCreatePhysicalDevice=CreatePhysicalDevice_mednvme -o .variant-build/virtblk.o
  VAROBJS+=(.variant-build/virtblk.o)
  # the display: Syborg's REAL framebuffer driver on QEMU ramfb (src/phorepoc-arm/virt/ramfb/virtfb.cpp), the generic
  # RDisplayChannel LDD and the Syborg fb glue, exactly as on syborg (step 2a)
  # (its one delta, the secure buffer's physical address: tools/kernel/svpfb_patch.py; the patched copy shadows the
  # original on the include path)
  py tools/kernel/svpfb_patch.py "$QB/svpframebuffer/svpframebuffer.cpp" .variant-build/svpfb >/dev/null
  ccq -k src/phorepoc-arm/virt/ramfb/virtfb.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" -I.variant-build/svpfb -I"$QB/svpframebuffer" -I"$B/include/drivers" -I"$B/include" \
    ${VIRT_FB:+-DVIRT_FB_WIDTH=${VIRT_FB%x*} -DVIRT_FB_HEIGHT=${VIRT_FB#*x}} -o .variant-build/virtfb.o
  ccq -k "$B/drivers/display/display.cpp" -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" -I"$B/include/drivers" -I"$B/include" -o .variant-build/display.o
  ccq -k tools/kernel/ekern_fb_glue.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" -I"$QB/svpframebuffer" -I"$B/include/drivers" -I"$B/include" \
    -o .variant-build/ekern_fb_glue.o
  VAROBJS+=(.variant-build/virtfb.o .variant-build/display.o .variant-build/ekern_fb_glue.o)
  # keyboard: virtio-input (src/phorepoc-arm/virt/input); Syborg's PC set-1 -> EStdKey table cut out of
  # syborg_keyboard.h, as the x86 port does (make_ekern.sh)
  awk '/KConvertCode\[\] *=/{on=1;next} on&&/^ *};/{print "}";exit} on{print}' \
    "$QB/keyboard/syborg_keyboard.h" > .variant-build/virt_kbdtable.inc
  [ "$(grep -c "EStdKey" .variant-build/virt_kbdtable.inc)" -gt 50 ] || { echo "keyboard table extraction failed"; exit 1; }
  ccq -k src/phorepoc-arm/virt/input/virtinput.cpp -O2 -fno-function-sections \
    -fno-data-sections "${EXTRA[@]}" -I.variant-build ${VIRT_FB:+-DVIRT_FB_WIDTH=${VIRT_FB%x*} -DVIRT_FB_HEIGHT=${VIRT_FB#*x}} \
    -o .variant-build/virtinput.o
  VAROBJS+=(.variant-build/virtinput.o)
  ccq -k src/phorepoc-arm/virt/specific/virt_glue_null.cpp -O2 \
    -fno-function-sections -fno-data-sections "${EXTRA[@]}" -o .variant-build/virt_glue_null.o
  VAROBJS+=(.variant-build/locmedia.o .variant-build/dmasupport.o .variant-build/iram.o
            .variant-build/virt_glue_null.o)
else

# objcopy --redefine-sym needs the object to EXIST: ccq only queues the compile (cc_batch below), so the
# renames are queued too and run right after cc_batch (the 2026-10-10 batch refactor ran them first: "No such file")
OCQ=(); ocq() { OCQ+=("$*"); }
echo "[2a/3] compile the REAL Syborg framebuffer driver (svpframebuffer.cpp, DECLARE_STANDARD_EXTENSION)"
# M5.5: the in-source LDD/screen driver, statically linked into ekern and invoked as a kernel
# extension (its InitExtension entry) from the seam -- replaces the ekseam_fb_test register poke.
# Needs the display LDD + generic driver includes on top of the kernel EXTRA set.
ccq -k "$QB/svpframebuffer/svpframebuffer.cpp" -O2 \
  -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
  -I"$QB/svpframebuffer" -I"$B/include/drivers" -I"$B/include" \
  -o .variant-build/svpframebuffer.o
# M6: the generic RDisplayChannel LOGICAL device (DECLARE_STANDARD_LDD). Compiles with the same
# kernel-driver include surface as the PDD; installed via Kern::InstallLogicalDevice from the glue
# so RDisplayChannel::Open (WSERV's screen driver path) finds a registered LDD+PDD pair.
ccq -k "$B/drivers/display/display.cpp" -O2 \
  -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
  -I"$B/include/drivers" -I"$B/include" \
  -o .variant-build/display.o
# M5.5 glue: load the driver as an extension + paint its buffer (replaces ekseam_fb_test).
ccq -k tools/kernel/ekern_fb_glue.cpp -O2 \
  -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
  -I"$QB/svpframebuffer" -I"$B/include/drivers" -I"$B/include" \
  -o .variant-build/ekern_fb_glue.o

echo "[2b/3] compile the REAL local-media stack (M7: locmedia/ELOCD + syborg mednvmemory)"
# Built-in kernel extensions, fb-driver pattern. Per-module entry symbols
# (InitExtension/KernelModuleEntry/Create*Device -- one DLL each in a real ROM)
# are made unique with --redefine-sym; ekern_media_glue.cpp calls them in ROM
# order and installs the LDD/PDD factories the (absent) ELOCD.LDD/med*.pdd files
# would have provided.
MEMMODEL_INC=(-I"$B/include/memmodel/epoc" -I"$B/include/memmodel/epoc/$MMN"
              -I"$B/include/memmodel/epoc/$MMN/arm" -I"$B/include/memmodel/epoc/mmubase")
ccq -k "$B/drivers/locmedia/locmedia.cpp" -O2 \
  -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
  -I"$B/drivers/locmedia" -I"$B/drivers/locmedia/traces" -I"$B/include/drivers" -I"$B/include" \
  -o .variant-build/locmedia.o
ocq \
  --redefine-sym _Z13InitExtensionv=_Z22InitExtension_locmediav \
  --redefine-sym _Z17KernelModuleEntryi=KernelModuleEntry_locmedia \
  --redefine-sym _Z19CreateLogicalDevicev=_Z28CreateLogicalDevice_locmediav \
  .variant-build/locmedia.o
ccq -k "$B/drivers/locmedia/dmasupport.cpp" -O2 \
  -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
  -I"$B/drivers/locmedia" -I"$B/drivers/locmedia/traces" -I"$B/include/drivers" -I"$B/include" \
  -o .variant-build/dmasupport.o
ccq -k "$QB/mednvmemory/syborg_mednvmemory.cpp" -O2 \
  -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
  -I"$QB/mednvmemory" -I"$QB/specific" -I"$B/include/drivers" -I"$B/include" \
  "${MEMMODEL_INC[@]}" \
  -o .variant-build/mednvmemory.o
ocq \
  --redefine-sym _Z13InitExtensionv=_Z21InitExtension_mednvmev \
  --redefine-sym _Z17KernelModuleEntryi=KernelModuleEntry_mednvme \
  --redefine-sym _Z20CreatePhysicalDevicev=_Z28CreatePhysicalDevice_mednvmev \
  .variant-build/mednvmemory.o
ccq -k tools/kernel/ekern_media_glue.cpp -O2 \
  -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
  -I"$B/include/drivers" -I"$B/include" \
  -o .variant-build/ekern_media_glue.o

echo "[2c/3] compile the REAL syborg input drivers (keyboard + pointer kexts)"
# Built-in kernel extensions (fb/media pattern); ekern_input_glue.cpp calls the
# per-driver InitExtension, made unique here with --redefine-sym.
for drv in keyboard pointer; do
  ccq -k "$QB/$drv/syborg_$drv.cpp" -O2 \
    -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
    -I"$QB/$drv" -I"$QB/specific" -I"$B/include/drivers" -I"$B/include" \
    -o .variant-build/syborg_$drv.o
done
ocq --redefine-sym _Z13InitExtensionv=_Z22InitExtension_keyboardv \
  --redefine-sym _Z17KernelModuleEntryi=KernelModuleEntry_keyboard .variant-build/syborg_keyboard.o
ocq --redefine-sym _Z13InitExtensionv=_Z21InitExtension_pointerv \
  --redefine-sym _Z17KernelModuleEntryi=KernelModuleEntry_pointer .variant-build/syborg_pointer.o
ccq -k tools/kernel/ekern_input_glue.cpp -O2 \
  -fno-function-sections -fno-data-sections "${EXTRA[@]}" \
  -I"$B/include/drivers" -I"$B/include" \
  -o .variant-build/ekern_input_glue.o
VAROBJS+=(.variant-build/svpframebuffer.o .variant-build/display.o .variant-build/ekern_fb_glue.o
  .variant-build/locmedia.o .variant-build/dmasupport.o .variant-build/mednvmemory.o .variant-build/ekern_media_glue.o
  .variant-build/syborg_keyboard.o .variant-build/syborg_pointer.o .variant-build/ekern_input_glue.o)
fi
# NGA kernel side (GPU plan rung G1b) on the boards with a display channel: a64 (the phone) + virt (QEMU, the NGA
# debug bench: synchronous serial + the QEMU monitor)
if { [ "$BOARD" = a64 ] || [ "$BOARD" = virt ]; } && [ "${NGA_KERNEL:-1}" = 1 ]; then   # NGA_KERNEL=0: leave it out (bisecting)
  # NGA kernel side (rung G1b): the surface manager (an extension + LDD) and the Graphics Resource sg extension +
  # sgdevice LDD -- REAL 2010 sources, entry points renamed (several drivers share one ekern); installed by ekern_nga_glue.cpp (ekseam_load_nga_drivers)
  SM=baseline/symbian-2010/oss.FCL.sf.adapt.NokiaGraphics/graphicscompositionref/surfacemgrcommon
  SG=baseline/symbian-2010/oss.FCL.sf.os.graphics/graphicsresourceservices
  NGAINC=(-I"$SM/inc" -I"$SG/graphicsresourceimplementation/inc" -I"$SG/graphicsresourceinterface/inc"
          -I.epoc32/include/platform -I.epoc32/include)
  ccq -k "$SM/src/surfacemanagerdriver.cpp" -O2 -fno-function-sections -fno-data-sections \
    "${EXTRA[@]}" "${NGAINC[@]}" -DInitExtension=InitExtension_surfmgr -DKernelModuleEntry=KernelModuleEntry_surfmgr \
    -DCreateLogicalDevice=CreateLogicalDevice_surfmgr -o .variant-build/surfacemanagerdriver.o
  ccq -k "$SM/src/extension.cpp" -O2 -fno-function-sections -fno-data-sections \
    "${EXTRA[@]}" "${NGAINC[@]}" -o .variant-build/surfacemanager_ext.o
  ccq -k "$SG/graphicsresourceimplementation/src/sgextension.cpp" -O2 \
    -fno-function-sections -fno-data-sections "${EXTRA[@]}" "${NGAINC[@]}" -DInitExtension=InitExtension_sgext \
    -DKernelModuleEntry=KernelModuleEntry_sgext -o .variant-build/sgextension.o
  ccq -k "$SG/graphicsresourceimplementation/src/sgresource.cpp" -O2 \
    -fno-function-sections -fno-data-sections "${EXTRA[@]}" "${NGAINC[@]}" -o .variant-build/sgresource_k.o
  ccq -k "$SG/graphicsresourceimplementation/src/sgdevice.cpp" -O2 \
    -fno-function-sections -fno-data-sections "${EXTRA[@]}" "${NGAINC[@]}" -DKernelModuleEntry=KernelModuleEntry_sgdev \
    -DCreateLogicalDevice=CreateLogicalDevice_sgdev -o .variant-build/sgdevice.o
  ccq -k "$SG/graphicsresourceimplementation/src/sgchannel.cpp" -O2 \
    -fno-function-sections -fno-data-sections "${EXTRA[@]}" "${NGAINC[@]}" -o .variant-build/sgchannel.o
  VAROBJS+=(.variant-build/surfacemanagerdriver.o .variant-build/surfacemanager_ext.o .variant-build/sgextension.o
            .variant-build/sgresource_k.o .variant-build/sgdevice.o .variant-build/sgchannel.o)
  ccq -k tools/kernel/ekern_nga_glue.cpp -O2 -fno-function-sections -fno-data-sections     "${EXTRA[@]}" -o .variant-build/ekern_nga_glue.o
  VAROBJS+=(.variant-build/ekern_nga_glue.o)
  # direct screen access: the kernel-owned screen chunk behind EDisplayHalGetDisplayMemoryHandle
  ccq -k tools/kernel/ekern_dsa_glue.cpp -O2 -fno-function-sections -fno-data-sections     "${EXTRA[@]}" -o .variant-build/ekern_dsa_glue.o
  VAROBJS+=(.variant-build/ekern_dsa_glue.o)
fi

echo "[2/3] compile the toolchain seam (aliases via --defsym below; stubs here)"
ccq -k tools/kernel/ekern_seam.cpp -O2 -fno-function-sections -fno-data-sections \
  "${EXTRA[@]}" "${SEAM_DEFS[@]}" ${DIE_TRACE:+-DEKSEAM_DIE_TRACE=$DIE_TRACE} ${DIE_TRACE_NAME:+"-DEKSEAM_DIE_TRACE_NAME=\"$DIE_TRACE_NAME\""} -o $KO/zz_seam.o

py toolchain/cc_batch.py "$CCJOBS" -j "${CC_JOBS:-12}" || exit 1
for c in "${OCQ[@]:-}"; do [ -n "$c" ] && eval "\"$BIN/arm-none-eabi-objcopy\" $c"; done
echo "[3/3] link -> $KO/ekern.elf"
do_link() {
"$LD" -T tools/kernel/ekern.lds --defsym "__code_base__=${CODE_BASE:-0x80000000}" "$@" "${BOARD_WRAPS[@]}" \
  --wrap KernelMain --wrap _ZN4Kern5AllocEi --wrap _ZN11ExecHandler11ChunkAdjustEP6DChunkiii --wrap _ZN11ExecHandler11ChunkCreateE10TOwnerTypePK6TDesC8R12TChunkCreate --wrap _ZN11ExecHandler10LeaveStartEv --wrap _ZN11ExecHandler12PopTrapFrameEv \
  --wrap _ZN1P13CreateVariantEv --wrap _ZN5NKern5Init0EPv --wrap _ZN1K21InitialiseMicrokernelEv \
  --wrap _ZN6BTrace5Init0Ev --wrap _ZN4Kern5FaultEPKci \
  --wrap _ZN11ExecHandler20CodeSegAddDependencyEPvS0_ --wrap _ZN12DEpocCodeSeg8DestructEv \
  --wrap _ZN11ExecHandler13ProcessLoadedER18TProcessCreateInfo --wrap _ZN11ExecHandler13CodeSegLoadedER18TCodeSegCreateInfo \
  --wrap _ZN1M5Init1Ev --wrap _ZN1A5Init1Ev --wrap _ZN1P10KernelInfoER18TProcessCreateInfoRPvS3_ --wrap _ZN1P10NewProcessEv \
  --wrap _ZN8DProcess9NewThreadERP7DThreadR17SThreadCreateInfoPi10TOwnerType \
  --wrap _ZN15DArmPlatProcess12GetNewThreadERP7DThreadR17SThreadCreateInfo \
  --wrap _ZN7DThread6CreateER17SThreadCreateInfo --wrap _ZN7DThread8DoCreateER17SThreadCreateInfo \
  --wrap _ZN5NKern12ThreadCreateEP7NThreadR18SNThreadCreateInfo \
  --wrap _ZN7NThread6CreateER18SNThreadCreateInfoi --wrap _ZN11NThreadBase6CreateER18SNThreadCreateInfoi \
  --wrap _ZN5NKern4InitEP7NThreadR18SNThreadCreateInfo \
  --wrap _ZN7DObjectC1Ev --wrap _ZN4TDfcC1EPFvPvES0_P7TDfcQuei --wrap _ZN11NThreadBaseC1Ev \
  --wrap _ZN10TBufCBase8C2Ev --wrap _ZN12TPriListBaseC2Ei --wrap _ZN11NThreadBaseC2Ev \
  --wrap _ZN7DThreadC2Ev \
  --wrap _ZN8DProcess6CreateEiR18TProcessCreateInfoP5HBuf8 --wrap _ZN1M5Init2Ev \
  --wrap _ZN1A5Init2Ev --wrap _ZN1K22CreateObjectContainersEv --wrap _ZN1K5FaultENS_6TFaultE \
  --wrap _ZN7DThread4ExitEv --wrap _ZN5NKern10ThreadKillEP7NThread --wrap _ZN7DThread3DieE9TExitTypeiRK6TDesC8 \
  --wrap _Z18InvalidExecHandlerv \
  --wrap _ZN7DObject7SetNameEPK6TDesC8 \
  --wrap _ZNK6TDesC83MidEii \
  --wrap _ZN7TDfcQueC1Ev --wrap _ZN8DProcess9AddThreadER7DThread \
  --wrap _ZN1P15StartExtensionsEv --wrap _ZN1M5Init4Ev --wrap _ZN1K17StartKernelServerEv \
  --wrap _Z14StartSecondaryPv --wrap _ZN11ExecHandler10DebugPrintEPvi \
  --wrap _ZN11ExecHandler9DebugMaskEv --wrap _ZN2PP20InitSuperPageFromRomEmm \
  --wrap _ZN11ExecHandler13SessionCreateERK6TDesC8iPK15TSecurityPolicyi \
  --wrap _ZN11ExecHandler23ServerCreateWithOptionsEPK6TDesC8iii \
  --wrap _ZN11ExecHandler12ServerCreateEPK6TDesC8i \
  --wrap _ZN7DThread18EpocThreadFunctionEPv --wrap ExcFault \
  --wrap _ZN3Exc8DispatchEPvP7NThread \
  --wrap _ZN16DMemModelProcess8DoCreateEiR18TProcessCreateInfo \
  --wrap _ZN8DCodeSeg6CreateER18TCodeSegCreateInfoP8DProcess \
  --wrap _ZN12DEpocCodeSeg8DoCreateER18TCodeSegCreateInfoP8DProcess \
  --wrap _ZN1K11MutexCreateERP6DMutexRK6TDesC8P7DObjectij \
  --wrap _ZN16DLcdPowerHandler6CreateEv --wrap _ZN16DLcdPowerHandler19AllocateFrameBufferEv \
  --wrap _ZN4Epoc16AllocPhysicalRamEiRmi --wrap _ZN12DPlatChunkHw3NewERPS_mij \
  --wrap _ZN16DLcdPowerHandler9DisplayOnEv --wrap _ZN13DPowerHandlerC2ERK6TDesC8 \
  --wrap _ZN4Kern10DfcQCreateERP7TDfcQueiPK6TDesC8 --wrap _ZN4Kern21InstallPhysicalDeviceEP15DPhysicalDevice \
  --wrap _ZN4Kern12ThreadCreateER17SThreadCreateInfo --wrap _ZN4Kern12ThreadResumeER7DThread \
  --wrap _ZN4Kern20InstallLogicalDeviceEP14DLogicalDevice \
  --defsym "__kern_data_base__=${KERN_DATA_BASE:-0xC8000000}" \
  --defsym 'Des8PanicBadDesType__Fv=_Z19Des8PanicBadDesTypev' \
  --defsym 'Des8PanicPosOutOfRange__Fv=_Z22Des8PanicPosOutOfRangev' \
  --defsym 'Check__14DBTraceFilter2Ul=_ZN14DBTraceFilter25CheckEm' \
  --defsym '_ZN1K9MsgInfoE=_ZN1K7MsgInfoE' \
  -o $KO/ekern.elf \
  $KO/*.o $NO/*.o \
  "${VAROBJS[@]}" \
  -L"$LIBGCC" -lgcc --allow-multiple-definition
}
if [ "${SMP:-0}" = 1 ]; then
  # nkernsmp's .cia files reference C++ through GCC 2.95-mangled names (CSM_* macros): link once, alias every such
  # unresolved name to its Itanium twin (tools/kernel/gcc2_aliases.py, only names defined in the objects), relink.
  do_link > "$KO/link1.log" 2>&1 || true
  ALIASES=($(py tools/kernel/gcc2_aliases.py "$KO/link1.log" $KO/*.o $NO/*.o "${VAROBJS[@]}"))
  do_link "${ALIASES[@]}"
else
  do_link
fi

echo "PASS: ekern.elf linked ($("$BIN/arm-none-eabi-size" $KO/ekern.elf 2>/dev/null | tail -1))"
