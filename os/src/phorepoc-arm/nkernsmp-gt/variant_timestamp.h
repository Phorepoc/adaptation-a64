// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// OURS (phone SMP rung): the BSP timestamp for the SMP nanokernel built with
// __NKERN_TIMESTAMP_USE_INLINE_BSP_CODE__ (nkernsmp/arm/ncutils.cpp + ncutilf.cia #include this; the NaviEngine BSP's
// naviengine_assp/variant_timestamp.h is the template). The source is the ARM generic timer's 64-bit PHYSICAL count
// (CNTPCT): one system-wide counter, constant rate, readable from any CPU -- exactly what NKern::Timestamp needs.
// Found ahead of the 2010 include paths through symcc's SYMCC_PREINC (the generic-timer overlay directory).

// The counter frequency in Hz = CNTFRQ, which the firmware programs (QEMU `virt`: 62.5 MHz; the A64:
// 24 MHz, set by ARM Trusted Firmware). Read at run time, so one nanokernel build serves every board. Its only
// user is NKern::TimestampFrequency() (nkernsmp/arm/ncutils.cpp).
static inline TUint32 VariantCntFrq()
	{
	TUint32 f;
	asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(f));
	return f;
	}
#define KTimestampFrequency VariantCntFrq()

#ifdef __DEFINE_NKERN_TIMESTAMP_ASM__
EXPORT_C __NAKED__ TUint64 NKern::Timestamp()
	{
	asm("isb ");							// no speculative early read of the counter
	asm("mrrc p15, 0, r0, r1, c14 ");		// CNTPCT -> r1:r0
	__JUMP(,lr);
	}
#endif

#ifdef __DEFINE_NKERN_TIMESTAMP_CPP__
#endif
