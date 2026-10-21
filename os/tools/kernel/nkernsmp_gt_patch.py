"""nkernsmp_gt_patch.py -- the generic-timer deltas of the 2010 ARM SMP nanokernel (PinePhone SMP).

Writes patched copies of the three baseline files that touch the A9/ARM11MP private timer from ASM (the C++
uses compile unchanged against src/phorepoc-arm/nkernsmp-gt/arm_tmr.h). Each delta is one site, asserted to
match exactly once, marked "GT:" in the output:
  include/nkernsmp/arm/nk_plat.h  TIMESLICE_VECTOR -> 27 (the virtual timer PPI) under __CPU_ARM_HAS_GENERIC_TIMER
  include/nkernsmp/arm/entry.h    timeslice IRQ: "clear timer event flag" str -> CNTV_CTL = ENABLE|IMASK
  nkernsmp/arm/ncsched.cia        SaveTimesliceTimer: the count load -> mrc CNTV_TVAL
Usage: py tools/kernel/nkernsmp_gt_patch.py <kernel/eka dir> <out dir>
  -> <out>/inc/{nk_plat.h,entry.h} (put <out>/inc first on the include path) + <out>/ncsched.cia
"""
import os
import sys

EKA, OUT = sys.argv[1], sys.argv[2]

DELTAS = {
    "include/nkernsmp/arm/nk_plat.h": ("inc/nk_plat.h", [(
        "#if defined(__CPU_ARM11MP__)\n#define\tTIMESLICE_VECTOR",
        "#if defined(__CPU_ARM_HAS_GENERIC_TIMER)\t// GT: the virtual timer PPI (ID 27) is the per-CPU timeslice timer\n"
        "#define\tTIMESLICE_VECTOR\t\t\t27\n#elif defined(__CPU_ARM11MP__)\n#define\tTIMESLICE_VECTOR")]),
    "include/nkernsmp/arm/entry.h": ("inc/entry.h", [(
        '\tasm("ldr\tr2, [r4, #%a0]" : : "i" _FOFF(TSubScheduler, iSSX.iLocalTimerAddr));\n'
        '\tasm("mov\tr1, #1 ");\n'
        '\tasm("str\tr1, [r2, #%a0]" : : "i" _FOFF(ArmLocalTimer, iTimerIntStatus));\t// clear timer event flag\n',
        '\tasm("mov\tr1, #3 ");\t\t\t\t\t// GT: CNTV_CTL = ENABLE|IMASK (level PPI: mask until the count is re-armed)\n'
        '\tasm("mcr\tp15, 0, r1, c14, c3, 1 ");\n'
        '\tasm("isb ");\n')]),
    "nkernsmp/arm/ncsched.cia": ("ncsched.cia", [(
        '\tasm("ldr\tr3, [r2, #%a0]" : : "i" _FOFF(ArmLocalTimer,iTimerCount));\n',
        '\tasm("mrc\tp15, 0, r3, c14, c3, 0 ");\t// GT: r3 = CNTV_TVAL (the remaining count)\n'), (
        # ThumbEE (TEEHBR, p14 opc1 6) does not exist in ARMv8 AArch32: UNDEF on the A53. Symbian never uses it.
        '\tasm("mrc\tp14, 6, r7, c1, c0, 0 ");\t// r7 = TEEHBR\n',
        '\tasm("mov\tr7, #0 ");\t\t\t\t// GT: no TEEHBR (no ThumbEE in ARMv8 AArch32)\n'), (
        '\tasm("mcr\tp14, 6, r7, c1, c0, 0 ");\t// r7 = TEEHBR\n',
        '\t// GT: no TEEHBR restore (no ThumbEE in ARMv8 AArch32)\n')]),
    "nkernsmp/arm/ncutils.cia": ("ncutils.cia", [(
        '\tasm("mrc\tp14, 6, r3, c1, c0, 0 ");\n',
        '\tasm("mov\tr3, #0 ");\t\t\t\t// GT: Arm::SaveState TEEHBR = 0 (no ThumbEE in ARMv8 AArch32)\n')]),
}

for src, (dst, reps) in DELTAS.items():
    with open(os.path.join(EKA, src), encoding="latin-1", newline="") as f:
        text = f.read().replace("\r\n", "\n")
    for old, new in reps:
        n = text.count(old)
        if n != 1:
            sys.exit(f"nkernsmp_gt_patch: {src}: expected 1 match, found {n}: {old[:60]!r}")
        text = text.replace(old, new)
    path = os.path.join(OUT, dst)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="latin-1", newline="\n") as f:
        f.write(text)
    print(f"nkernsmp_gt_patch: {src} -> {path}")
