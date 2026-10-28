/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// mali_utgard_regs.h -- OURS (GPU rung G3): the ARM Mali Utgard (Mali-400 MP)
// register map our Mali driver uses. Written for this project (Linux's lima_regs.h is GPL-2.0-only and is NOT copied):
// offsets and bit meanings are hardware facts, cross-checked against the open lima driver's behaviour and verified on
// the phone (A64, Mali-400 MP2 r1p1) with PhoneLab peeks (G3-K0, 2026-10-09).
#ifndef MALI_UTGARD_REGS_H
#define MALI_UTGARD_REGS_H

// Block offsets from the GPU base (Mali-400 MP layout)
enum
	{
	KMaliOffGp		= 0x00000,
	KMaliOffL2		= 0x01000,
	KMaliOffPmu		= 0x02000,
	KMaliOffGpMmu	= 0x03000,
	KMaliOffPpMmu0	= 0x04000,		// PP n's MMU at 0x04000 + n * 0x1000 (n < 4)
	KMaliOffPp0		= 0x08000,		// PP n at 0x08000 + n * 0x2000
	KMaliRegionSize	= 0x10000
	};
inline TUint MaliOffPp(TInt aN) { return KMaliOffPp0 + aN * 0x2000; }
inline TUint MaliOffPpMmu(TInt aN) { return KMaliOffPpMmu0 + aN * 0x1000; }

// PMU (power domains; STATUS bit = 1 means the domain is OFF)
enum
	{
	KPmuPowerUp		= 0x00,
	KPmuPowerDown	= 0x04,
	KPmuStatus		= 0x08,
	KPmuIntMask		= 0x0C,
	KPmuIntRawStat	= 0x10,
	KPmuIntClear	= 0x18,
	KPmuSwDelay		= 0x1C,
	KPmuIntCmd		= 1 << 0
	};

// L2 cache
enum
	{
	KL2Size			= 0x04,		// [31:24] log2 bus width, [23:16] log2 size, [15:8] log2 ways, [7:0] log2 line
	KL2Status		= 0x08,
	KL2StatusCmdBusy	= 1 << 0,
	KL2Command		= 0x10,
	KL2CmdClearAll	= 1 << 0,
	KL2MaxReads		= 0x18,
	KL2Enable		= 0x1C,
	KL2EnableAccess	= 1 << 0,
	KL2EnableReadAllocate = 1 << 1
	};

// GP (geometry processor: vertex shader + PLBU tiler)
enum
	{
	KGpVsclStart	= 0x00,		// the 6 "frame" registers, written in order from here
	KGpVsclEnd		= 0x04,
	KGpPlbuclStart	= 0x08,
	KGpPlbuclEnd	= 0x0C,
	KGpPlbuAllocStart = 0x10,
	KGpPlbuAllocEnd	= 0x14,
	KGpFrameRegs	= 6,
	KGpCmd			= 0x20,
	KGpCmdStartVs	= 1 << 0,
	KGpCmdStartPlbu	= 1 << 1,
	KGpCmdUpdatePlbuAlloc = 1 << 4,
	KGpCmdReset		= 1 << 5,
	KGpCmdStopBus	= 1 << 9,
	KGpCmdSoftReset	= 1 << 10,
	KGpIntRawStat	= 0x24,
	KGpIntClear		= 0x28,
	KGpIntMask		= 0x2C,
	KGpIntStat		= 0x30,
	KGpPerfCnt0Limit = 0x54,
	KGpStatus		= 0x68,
	KGpStatusVsActive	= 1 << 1,
	KGpStatusBusStopped	= 1 << 2,
	KGpStatusPlbuActive	= 1 << 3,
	KGpVersion		= 0x6C
	};
// GP interrupt bits
enum
	{
	KGpIrqVsEnd			= 1 << 0,
	KGpIrqPlbuEnd		= 1 << 1,
	KGpIrqPlbuOutOfMem	= 1 << 2,
	KGpIrqVsSem			= 1 << 3,
	KGpIrqPlbuSem		= 1 << 4,
	KGpIrqHang			= 1 << 5,
	KGpIrqForceHang		= 1 << 6,
	KGpIrqPerf0			= 1 << 7,
	KGpIrqPerf1			= 1 << 8,
	KGpIrqWriteBound	= 1 << 9,
	KGpIrqSyncError		= 1 << 10,
	KGpIrqAxiBusError	= 1 << 11,
	KGpIrqAxiBusStopped	= 1 << 12,
	KGpIrqVsInvalidCmd	= 1 << 13,
	KGpIrqPlbInvalidCmd	= 1 << 14,
	KGpIrqResetDone		= 1 << 19,
	KGpIrqSemUnderflow	= 1 << 20,
	KGpIrqSemOverflow	= 1 << 21,
	KGpIrqPtrArrayOob	= 1 << 22,
	KGpIrqAll = KGpIrqVsEnd | KGpIrqPlbuEnd | KGpIrqPlbuOutOfMem | KGpIrqVsSem | KGpIrqPlbuSem | KGpIrqHang
		| KGpIrqForceHang | KGpIrqPerf0 | KGpIrqPerf1 | KGpIrqWriteBound | KGpIrqSyncError | KGpIrqAxiBusError
		| KGpIrqAxiBusStopped | KGpIrqVsInvalidCmd | KGpIrqPlbInvalidCmd | KGpIrqResetDone | KGpIrqSemUnderflow
		| KGpIrqSemOverflow | KGpIrqPtrArrayOob,
	KGpIrqError = KGpIrqPlbuOutOfMem | KGpIrqForceHang | KGpIrqWriteBound | KGpIrqSyncError | KGpIrqAxiBusError
		| KGpIrqVsInvalidCmd | KGpIrqPlbInvalidCmd | KGpIrqSemUnderflow | KGpIrqSemOverflow | KGpIrqPtrArrayOob,
	KGpIrqUsed = KGpIrqVsEnd | KGpIrqPlbuEnd | KGpIrqError
	};

// PP (pixel processor)
enum
	{
	KPpFrame		= 0x0000,	// 23 frame registers from here (0x00 = PLBU array / tile list address)
	KPpStack		= 0x0030,	// (frame register 12: the fragment stack address)
	KPpFrameRegs	= 23,
	KPpWbRegs		= 12,		// write-back unit n (0..2) at 0x100 * (n + 1), 12 registers each
	KPpVersion		= 0x1000,
	KPpStatus		= 0x1008,
	KPpStatusRendering	= 1 << 0,
	KPpStatusBusStopped	= 1 << 4,
	KPpCtrl			= 0x100C,
	KPpCtrlStopBus	= 1 << 0,
	KPpCtrlForceReset	= 1 << 5,
	KPpCtrlStart	= 1 << 6,
	KPpCtrlSoftReset	= 1 << 7,
	KPpIntRawStat	= 0x1020,
	KPpIntClear		= 0x1024,
	KPpIntMask		= 0x1028,
	KPpIntStatus	= 0x102C,
	KPpPerfCnt0Limit = 0x1088
	};
inline TUint MaliPpWb(TInt aN) { return 0x100 * (aN + 1); }
// PP interrupt bits
enum
	{
	KPpIrqEndOfFrame	= 1 << 0,
	KPpIrqEndOfTile		= 1 << 1,
	KPpIrqHang			= 1 << 2,
	KPpIrqForceHang		= 1 << 3,
	KPpIrqBusError		= 1 << 4,
	KPpIrqBusStop		= 1 << 5,
	KPpIrqCnt0			= 1 << 6,
	KPpIrqCnt1			= 1 << 7,
	KPpIrqWriteBound	= 1 << 8,
	KPpIrqInvalidPlist	= 1 << 9,
	KPpIrqStackUnderflow = 1 << 10,
	KPpIrqStackOverflow	= 1 << 11,
	KPpIrqResetDone		= 1 << 12,
	KPpIrqAll = KPpIrqEndOfFrame | KPpIrqEndOfTile | KPpIrqHang | KPpIrqForceHang | KPpIrqBusError | KPpIrqBusStop
		| KPpIrqCnt0 | KPpIrqCnt1 | KPpIrqWriteBound | KPpIrqInvalidPlist | KPpIrqStackUnderflow | KPpIrqStackOverflow
		| KPpIrqResetDone,
	KPpIrqError = KPpIrqForceHang | KPpIrqBusError | KPpIrqWriteBound | KPpIrqInvalidPlist | KPpIrqStackUnderflow
		| KPpIrqStackOverflow,
	KPpIrqUsed = KPpIrqEndOfFrame | KPpIrqError
	};

// MMU (one per GP / PP)
enum
	{
	KMmuDteAddr		= 0x00,		// the page directory's physical address
	KMmuStatus		= 0x04,
	KMmuStatusPaging	= 1 << 0,
	KMmuStatusPageFault	= 1 << 1,
	KMmuStatusStall		= 1 << 2,
	KMmuStatusIdle		= 1 << 3,
	KMmuStatusFaultWrite = 1 << 5,
	KMmuCommand		= 0x08,
	KMmuCmdEnablePaging	= 0,
	KMmuCmdDisablePaging = 1,
	KMmuCmdEnableStall	= 2,
	KMmuCmdDisableStall	= 3,
	KMmuCmdZapCache		= 4,
	KMmuCmdPageFaultDone = 5,
	KMmuCmdHardReset	= 6,
	KMmuFaultAddr	= 0x0C,
	KMmuIntRawStat	= 0x14,
	KMmuIntClear	= 0x18,
	KMmuIntMask		= 0x1C,
	KMmuIntPageFault	= 1 << 0,
	KMmuIntReadBusError	= 1 << 1,
	KMmuIntStatus	= 0x20
	};
inline TUint MaliMmuBusId(TUint32 aStatus) { return (aStatus >> 6) & 0x1F; }

// Page table entries (two levels, 4 KB pages: PDE -> 4 MB of PTEs)
enum
	{
	KMaliPtePresent		= 1 << 0,
	KMaliPteRead		= 1 << 1,
	KMaliPteWrite		= 1 << 2,
	KMaliPteOverrideCache	= 1 << 3,
	KMaliPteWriteCacheable	= 1 << 4,
	KMaliPteWriteAllocate	= 1 << 5,
	KMaliPteWriteBufferable	= 1 << 6,
	KMaliPteReadCacheable	= 1 << 7,
	KMaliPteReadAllocate	= 1 << 8,
	// GPU-side caching through the Mali L2 (flushed before every job); the CPU maps the same pages uncached
	KMaliPteCached = KMaliPtePresent | KMaliPteRead | KMaliPteWrite | KMaliPteOverrideCache | KMaliPteWriteCacheable
		| KMaliPteWriteBufferable | KMaliPteReadCacheable | KMaliPteReadAllocate
	};

#endif
