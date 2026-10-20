// SPDX-License-Identifier: GPL-2.0 OR MIT -- used here under the MIT option (see below)
//
// mali_ldd.cpp -- the Mali Utgard (Mali-400 MP) kernel driver for Symbian EKA2: GPU rung G3 of
// docs/phorepoc/gpu-lima-research.md. The user API is RMaliChannel (rmalichannel.h).
//
// The HARDWARE sequences -- PMU power-up, L2 flush/enable, GP/PP soft and hard reset, MMU init / VM switch /
// page-fault recovery, the GP/PP job start and completion rules, the page-table format -- are PORTED from the Linux
// lima driver (drivers/gpu/drm/lima: lima_pmu.c, lima_l2_cache.c, lima_gp.c, lima_pp.c, lima_mmu.c, lima_vm.c,
// lima_sched.c), Copyright 2017-2019 Qiang Yu <yuq825@gmail.com>, which is dual-licensed "GPL-2.0 OR MIT"; this file
// takes the MIT option:
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
//   to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above
//   copyright notice and this permission notice shall be included in all copies or substantial portions of the
//   Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
// The register header is our own (mali_utgard_regs.h; lima_regs.h is GPL-only and not used). The Symbian side --
// the logical channel, the per-channel arena (a shared chunk at a fixed GPU VA instead of DRM GEM objects), the job
// plumbing (client requests, DFCs, a timeout) -- is OURS.
//
// Board specifics (register base, interrupt ids, clocks/resets) come from the board file through TMaliBoard
// (src/phorepoc-arm/a64/gpu/mali_a64.cpp on the PinePhone).
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <kernel/arm/assp.h>
#include <kernel/cache.h>
#include "mali_utgard_regs.h"
#include "../../symgl/rmalichannel.h"   // the channel API, next to its user-side half (src/symgl/symmali.cpp)
#include "mali_board.h"

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);

static void Log(const char* aWhat) { ekseam_puts(aWhat); }
static void LogHex(const char* aWhat, TUint32 aV) { ekseam_puts(aWhat); ekseam_puthex(aV); }

// The per-channel GPU address space: an arena at a fixed VA (the channel's own buffers) and an import region
// (other drivers' shared chunks: window surfaces), both covered by contiguous page tables.
const TUint32 KMaliArenaVa = 0x10000000;
const TInt KMaliArenaPts = 32;						// x 4 MB
const TInt KMaliArenaMax = KMaliArenaPts << 22;		// 128 MB
const TUint32 KMaliImportVa = 0x20000000;
const TInt KMaliImportPts = 16;						// 64 MB
const TInt KMaliImportPages = KMaliImportPts << 10;
const TInt KMaliMaxImports = 16;
const TInt KMaliMaxPp = 4;
const TInt KMaliJobTimeoutMs = 1000;

static inline void IoBarrier() { __e32_io_completion_barrier(); }

// ---------------------------------------------------------------------------------------------------------------
// TMaliVm: a page directory + the page tables of both regions (one physically contiguous block, mapped uncached).

class TMaliVm
	{
public:
	TInt Create();
	void Destroy();
	void Map(TUint32 aVa, TPhysAddr aPa) { *Pte(aVa) = (TUint32)aPa | KMaliPteCached; }
	void Unmap(TUint32 aVa) { *Pte(aVa) = 0; }
	TPhysAddr PdPhys() const { return iPhys; }
private:
	TUint32* Pte(TUint32 aVa)
		{
		if (aVa >= KMaliImportVa)
			return iPt + (KMaliArenaPts << 10) + ((aVa - KMaliImportVa) >> 12);
		return iPt + ((aVa - KMaliArenaVa) >> 12);
		}
	TPhysAddr iPhys;
	DPlatChunkHw* iChunk;
	TUint32* iPd;
	TUint32* iPt;			// (KMaliArenaPts + KMaliImportPts) * 1024 entries, right after the PD
	};

TInt TMaliVm::Create()
	{
	const TInt pts = KMaliArenaPts + KMaliImportPts;
	const TInt size = (1 + pts) * 0x1000;
	TInt r = Epoc::AllocPhysicalRam(size, iPhys, 12);
	if (r != KErrNone)
		return r;
	// uncached: the GPU's MMU reads these tables from memory (the same type for every mapping of this RAM)
	r = DPlatChunkHw::New(iChunk, iPhys, size, EMapAttrSupRw | EMapAttrBufferedC);
	if (r != KErrNone)
		{
		Epoc::FreePhysicalRam(iPhys, size);
		return r;
		}
	iPd = (TUint32*)iChunk->LinearAddress();
	iPt = iPd + 1024;
	memclr(iPd, size);
	for (TInt i = 0; i < KMaliArenaPts; ++i)
		iPd[(KMaliArenaVa >> 22) + i] = (TUint32)(iPhys + 0x1000 * (1 + i)) | KMaliPtePresent;
	for (TInt i = 0; i < KMaliImportPts; ++i)
		iPd[(KMaliImportVa >> 22) + i] = (TUint32)(iPhys + 0x1000 * (1 + KMaliArenaPts + i)) | KMaliPtePresent;
	IoBarrier();
	return KErrNone;
	}

void TMaliVm::Destroy()
	{
	if (iChunk)
		{
		iChunk->Close(NULL);
		Epoc::FreePhysicalRam(iPhys, (1 + KMaliArenaPts + KMaliImportPts) * 0x1000);
		iChunk = NULL;
		}
	}

// an imported range of another chunk (a SurfaceManager surface): kept open while mapped for the GPU
struct TMaliImportRec
	{
	DChunk* iChunk;
	TUint32 iVa;
	TInt iPages;
	TLinAddr iKernLin;		// the kernel's mapping (cache maintenance)
	TUint32 iMapAttr;
	};

// ---------------------------------------------------------------------------------------------------------------
// DMaliDevice: the hardware (one per board) + the job pipes.

class DMaliChannel;
struct TMaliPipe
	{
	DMaliChannel* iOwner;			// the channel whose job runs (NULL = idle)
	TPhysAddr iVm;					// the PD the pipe's MMUs point at
	TInt iResult;
	volatile TInt iPending;			// PP: processors still rendering
	TDfc* iDoneDfc;
	NTimer* iTimer;
	TBool iError;
	};

class DMaliDevice : public DLogicalDevice
	{
public:
	DMaliDevice(const TMaliBoard& aBoard) : iBoard(aBoard) { iVersion = TVersion(KMaliMajorVersion, 0, 0); }
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const;
	virtual TInt Create(DLogicalChannelBase*& aChannel);
	TInt HwInit();

	inline TUint32 R(TUint aOff) const { return *(volatile TUint32*)(iBase + aOff); }
	inline void W(TUint aOff, TUint32 aV) { *(volatile TUint32*)(iBase + aOff) = aV; }
	TBool Poll(TUint aOff, TUint32 aMask, TUint32 aWant, TInt aUs);

	TInt PmuInit();
	TInt L2Flush();
	TInt L2Init();
	TInt GpSoftReset();
	TInt GpHardReset();
	TInt PpSoftReset(TInt aN);
	TInt PpHardReset(TInt aN);
	TInt MmuInit(TUint aOff);
	void MmuSwitch(TUint aOff, TPhysAddr aPd);
	void MmuFaultRecover(TUint aOff);

	TInt StartGp(DMaliChannel* aCh, const TMaliGpFrame& aFrame, TPhysAddr aPd);
	TInt StartPp(DMaliChannel* aCh, TMaliPpFrame& aFrame, TPhysAddr aPd);
	void ChannelClosing(DMaliChannel* aCh);

	static void GpIsr(TAny* aDev);
	static void PpIsr(TAny* aArg);
	static void MmuIsr(TAny* aArg);
	static void GpDoneDfc(TAny* aDev);
	static void PpDoneDfc(TAny* aDev);
	static void GpTimeout(TAny* aDev);
	static void PpTimeout(TAny* aDev);
	void PipeDone(TInt aPipe);
	void PipeFault(TInt aPipe, TInt aResult);

public:
	TMaliBoard iBoard;
	DPlatChunkHw* iRegs;
	TLinAddr iBase;
	TUint32 iGpVersion, iPpVersion;
	TPhysAddr iEmptyPd;				// an all-zero page directory (MMUs of idle pipes)
	DPlatChunkHw* iEmptyPdChunk;
	DMutex* iLock;					// job start / channel close
	TDfcQue* iDfcQ;
	TMaliPipe iPipe[2];				// 0 = GP, 1 = PP
	TInt iPpArg[KMaliMaxPp];		// PpIsr's argument: the PP index
	TUint iMmuArg[1 + KMaliMaxPp];	// MmuIsr's argument: the MMU's register offset
	TUint32 iFaultAddr, iFaultStatus;
	};

static DMaliDevice* TheMali;

// ---------------------------------------------------------------------------------------------------------------
// DMaliChannel

class DMaliChannel : public DLogicalChannelBase
	{
public:
	DMaliChannel(DMaliDevice* aDev) : iDev(aDev) {}
	virtual ~DMaliChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aReqNo, TAny* a1, TAny* a2);
	void Complete(TInt aReq, TInt aResult);
private:
	TInt Commit(TInt aOffset, TInt aSize);
	TInt Import(TMaliImport& aImp, TBool aByAddress = EFalse);
	TInt Unimport(TUint32 aVa, TBool aLocked);
	TInt CacheSync(const TMaliCacheSync& aSync);
	void ZapMmus();
	TInt AllocImportPages(TInt aPages);
	void FreeImportPages(TInt aFirst, TInt aPages);
public:
	DMaliDevice* iDev;
	DThread* iClient;
	TClientRequest* iReq[EMaliReqCount];
	TMaliVm iVm;
	DChunk* iArena;
	TLinAddr iArenaLin;
	TUint32 iArenaMapAttr;
	TMaliImportRec iImport[KMaliMaxImports];
	TUint32 iImportMap[KMaliImportPages / 32];		// 1 = page of the import region in use
	};

// ---------------------------------------------------------------------------------------------------------------
// Hardware (ported from lima)

TBool DMaliDevice::Poll(TUint aOff, TUint32 aMask, TUint32 aWant, TInt aUs)
	{
	for (TInt i = 0; i <= aUs; ++i)
		{
		if ((R(aOff) & aMask) == aWant)
			return ETrue;
		Kern::NanoWait(1000);
		}
	return EFalse;
	}

// lima_pmu_hw_init: all domains up (STATUS bit set = domain off)
TInt DMaliDevice::PmuInit()
	{
	W(KMaliOffPmu + KPmuIntMask, 0);
	W(KMaliOffPmu + KPmuSwDelay, 0xffff);		// too low = an unstable GPU at high clocks (lima)
	const TUint32 off = R(KMaliOffPmu + KPmuStatus);
	if (off)
		{
		W(KMaliOffPmu + KPmuPowerUp, off);
		if (!Poll(KMaliOffPmu + KPmuIntRawStat, KPmuIntCmd, KPmuIntCmd, 100000))
			return KErrTimedOut;
		W(KMaliOffPmu + KPmuIntClear, KPmuIntCmd);
		}
	return KErrNone;
	}

TInt DMaliDevice::L2Flush()
	{
	W(KMaliOffL2 + KL2Command, KL2CmdClearAll);
	return Poll(KMaliOffL2 + KL2Status, KL2StatusCmdBusy, 0, 1000) ? KErrNone : KErrTimedOut;
	}

TInt DMaliDevice::L2Init()
	{
	TInt r = L2Flush();
	if (r != KErrNone)
		return r;
	W(KMaliOffL2 + KL2Enable, KL2EnableAccess | KL2EnableReadAllocate);
	W(KMaliOffL2 + KL2MaxReads, 0x1c);
	return KErrNone;
	}

TInt DMaliDevice::GpSoftReset()
	{
	W(KMaliOffGp + KGpIntMask, 0);
	W(KMaliOffGp + KGpIntClear, KGpIrqResetDone);
	W(KMaliOffGp + KGpCmd, KGpCmdSoftReset);
	if (!Poll(KMaliOffGp + KGpIntRawStat, KGpIrqResetDone, KGpIrqResetDone, 100))
		return KErrTimedOut;
	W(KMaliOffGp + KGpIntClear, KGpIrqAll);
	W(KMaliOffGp + KGpIntMask, KGpIrqUsed);
	return KErrNone;
	}

// lima_gp_hard_reset: stop the bus, reset, and use PERF_CNT_0_LIMIT as the "reset done" witness
TInt DMaliDevice::GpHardReset()
	{
	W(KMaliOffGp + KGpPerfCnt0Limit, 0xC0FFE000);
	W(KMaliOffGp + KGpIntMask, 0);
	W(KMaliOffGp + KGpCmd, KGpCmdStopBus);
	if (!Poll(KMaliOffGp + KGpStatus, KGpStatusBusStopped, KGpStatusBusStopped, 1000))
		Log("[EKERN][mali] gp bus stop timeout\n");
	W(KMaliOffGp + KGpCmd, KGpCmdReset);
	TBool ok = EFalse;
	for (TInt i = 0; i < 100 && !ok; ++i)
		{
		W(KMaliOffGp + KGpPerfCnt0Limit, 0xC01A0000);
		ok = R(KMaliOffGp + KGpPerfCnt0Limit) == 0xC01A0000;
		if (!ok)
			Kern::NanoWait(10000);
		}
	W(KMaliOffGp + KGpPerfCnt0Limit, 0);
	W(KMaliOffGp + KGpIntClear, KGpIrqAll);
	W(KMaliOffGp + KGpIntMask, KGpIrqUsed);
	return ok ? KErrNone : KErrTimedOut;
	}

TInt DMaliDevice::PpSoftReset(TInt aN)
	{
	const TUint pp = MaliOffPp(aN);
	W(pp + KPpIntMask, 0);
	W(pp + KPpIntRawStat, KPpIrqAll);
	W(pp + KPpCtrl, KPpCtrlSoftReset);
	TBool ok = EFalse;
	for (TInt i = 0; i <= 100 && !ok; ++i)
		{
		ok = !(R(pp + KPpStatus) & KPpStatusRendering) && R(pp + KPpIntRawStat) == KPpIrqResetDone;
		if (!ok)
			Kern::NanoWait(1000);
		}
	W(pp + KPpIntClear, KPpIrqAll);
	W(pp + KPpIntMask, KPpIrqUsed);
	return ok ? KErrNone : KErrTimedOut;
	}

TInt DMaliDevice::PpHardReset(TInt aN)
	{
	const TUint pp = MaliOffPp(aN);
	W(pp + KPpPerfCnt0Limit, 0xC0FFE000);
	W(pp + KPpIntMask, 0);
	W(pp + KPpCtrl, KPpCtrlStopBus);
	if (!Poll(pp + KPpStatus, KPpStatusBusStopped, KPpStatusBusStopped, 1000))
		Log("[EKERN][mali] pp bus stop timeout\n");
	W(pp + KPpCtrl, KPpCtrlForceReset);
	TBool ok = EFalse;
	for (TInt i = 0; i < 100 && !ok; ++i)
		{
		W(pp + KPpPerfCnt0Limit, 0xC01A0000);
		ok = R(pp + KPpPerfCnt0Limit) == 0xC01A0000;
		if (!ok)
			Kern::NanoWait(10000);
		}
	W(pp + KPpPerfCnt0Limit, 0);
	W(pp + KPpIntClear, KPpIrqAll);
	W(pp + KPpIntMask, KPpIrqUsed);
	return ok ? KErrNone : KErrTimedOut;
	}

// lima_mmu_init + lima_mmu_hw_init: DTE write test, hard reset, fault interrupts, the empty VM, paging on
TInt DMaliDevice::MmuInit(TUint aOff)
	{
	W(aOff + KMmuDteAddr, 0xCAFEBABE);
	if (R(aOff + KMmuDteAddr) != 0xCAFEB000)
		return KErrNotFound;
	W(aOff + KMmuCommand, KMmuCmdHardReset);
	if (!Poll(aOff + KMmuDteAddr, 0xFFFFFFFF, 0, 100))
		return KErrTimedOut;
	W(aOff + KMmuIntMask, KMmuIntPageFault | KMmuIntReadBusError);
	W(aOff + KMmuDteAddr, iEmptyPd);
	W(aOff + KMmuCommand, KMmuCmdEnablePaging);
	return Poll(aOff + KMmuStatus, KMmuStatusPaging, KMmuStatusPaging, 100) ? KErrNone : KErrTimedOut;
	}

// lima_mmu_switch_vm: stall, new page directory, flush the TLB, unstall
void DMaliDevice::MmuSwitch(TUint aOff, TPhysAddr aPd)
	{
	W(aOff + KMmuCommand, KMmuCmdEnableStall);
	Poll(aOff + KMmuStatus, KMmuStatusStall, KMmuStatusStall, 100);
	W(aOff + KMmuDteAddr, aPd);
	W(aOff + KMmuCommand, KMmuCmdZapCache);
	W(aOff + KMmuCommand, KMmuCmdDisableStall);
	Poll(aOff + KMmuStatus, KMmuStatusStall, 0, 100);
	}

// lima_mmu_page_fault_resume
void DMaliDevice::MmuFaultRecover(TUint aOff)
	{
	if (!(R(aOff + KMmuStatus) & KMmuStatusPageFault))
		return;
	W(aOff + KMmuIntMask, 0);
	W(aOff + KMmuDteAddr, 0xCAFEBABE);
	W(aOff + KMmuCommand, KMmuCmdHardReset);
	Poll(aOff + KMmuDteAddr, 0xFFFFFFFF, 0, 100);
	W(aOff + KMmuIntMask, KMmuIntPageFault | KMmuIntReadBusError);
	W(aOff + KMmuDteAddr, iEmptyPd);
	W(aOff + KMmuCommand, KMmuCmdEnablePaging);
	Poll(aOff + KMmuStatus, KMmuStatusPaging, KMmuStatusPaging, 100);
	}

TInt DMaliDevice::HwInit()
	{
	TInt r = iBoard.iPowerOn ? iBoard.iPowerOn() : KErrNone;
	LogHex("[EKERN][mali] board power on r=", r);
	if (r != KErrNone)
		return r;
	NKern::ThreadEnterCS();
	r = DPlatChunkHw::New(iRegs, iBoard.iBase, KMaliRegionSize, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r == KErrNone)
		r = Epoc::AllocPhysicalRam(0x1000, iEmptyPd, 12);
	if (r == KErrNone)
		r = DPlatChunkHw::New(iEmptyPdChunk, iEmptyPd, 0x1000, EMapAttrSupRw | EMapAttrBufferedC);
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		return r;
	iBase = iRegs->LinearAddress();
	memclr((TAny*)iEmptyPdChunk->LinearAddress(), 0x1000);
	IoBarrier();

	iGpVersion = R(KMaliOffGp + KGpVersion);
	iPpVersion = R(MaliOffPp(0) + KPpVersion);
	LogHex("[EKERN][mali] GP version=", iGpVersion);
	LogHex("[EKERN][mali] PP0 version=", iPpVersion);
	if ((iGpVersion >> 16) != 0x0B07 || (iPpVersion >> 16) != 0xCD07)
		{
		Log("[EKERN][mali] not a Mali-400 -- driver not started\n");
		return KErrNotSupported;
		}
	r = PmuInit();
	LogHex("[EKERN][mali] pmu up r=", r);
	if (r != KErrNone)
		return r;
	r = L2Init();
	LogHex("[EKERN][mali] l2 size reg=", R(KMaliOffL2 + KL2Size));
	if (r != KErrNone)
		return r;
	r = GpSoftReset();
	LogHex("[EKERN][mali] gp reset r=", r);
	for (TInt i = 0; i < iBoard.iNumPp && r == KErrNone; ++i)
		{
		r = PpSoftReset(i);
		LogHex("[EKERN][mali] pp reset r=", r);
		}
	if (r == KErrNone)
		r = MmuInit(KMaliOffGpMmu);
	for (TInt i = 0; i < iBoard.iNumPp && r == KErrNone; ++i)
		r = MmuInit(MaliOffPpMmu(i));
	LogHex("[EKERN][mali] mmus up r=", r);
	if (r != KErrNone)
		return r;
	for (TInt p = 0; p < 2; ++p)
		iPipe[p].iVm = iEmptyPd;

	// interrupts
	iMmuArg[0] = KMaliOffGpMmu;
	r = Interrupt::Bind(iBoard.iIrqGp, GpIsr, this);
	if (r == KErrNone)
		r = Interrupt::Bind(iBoard.iIrqGpMmu, MmuIsr, &iMmuArg[0]);
	for (TInt i = 0; i < iBoard.iNumPp && r == KErrNone; ++i)
		{
		iPpArg[i] = i;
		iMmuArg[1 + i] = MaliOffPpMmu(i);
		r = Interrupt::Bind(iBoard.iIrqPp[i], PpIsr, &iPpArg[i]);
		if (r == KErrNone)
			r = Interrupt::Bind(iBoard.iIrqPpMmu[i], MmuIsr, &iMmuArg[1 + i]);
		}
	LogHex("[EKERN][mali] irqs bound r=", r);
	if (r != KErrNone)
		return r;
	Interrupt::Enable(iBoard.iIrqGp);
	Interrupt::Enable(iBoard.iIrqGpMmu);
	for (TInt i = 0; i < iBoard.iNumPp; ++i)
		{
		Interrupt::Enable(iBoard.iIrqPp[i]);
		Interrupt::Enable(iBoard.iIrqPpMmu[i]);
		}
	return KErrNone;
	}

// ---------------------------------------------------------------------------------------------------------------
// Jobs

// lima_sched_run_job + lima_gp_task_run (the caller holds iLock)
TInt DMaliDevice::StartGp(DMaliChannel* aCh, const TMaliGpFrame& aFrame, TPhysAddr aPd)
	{
	const TUint32* f = aFrame.iFrame;
	// lima_gp_task_validate
	if (f[0] > f[1] || f[2] > f[3] || f[4] > f[5] || (f[0] == f[1] && f[2] == f[3]))
		return KErrArgument;
	TMaliPipe& p = iPipe[0];
	if (p.iOwner)
		return KErrInUse;
	TUint32 cmd = 0;
	if (f[0] != f[1])
		cmd |= KGpCmdStartVs;
	if (f[2] != f[3])
		cmd |= KGpCmdStartPlbu;
	L2Flush();						// lima: needed or GP/PP hang / fault "after running for a while"
	if (p.iVm != aPd)
		{
		MmuSwitch(KMaliOffGpMmu, aPd);
		p.iVm = aPd;
		}
	p.iOwner = aCh;
	p.iError = EFalse;
	p.iResult = KErrNone;
	for (TInt i = 0; i < KGpFrameRegs; ++i)
		W(KMaliOffGp + KGpVsclStart + i * 4, f[i]);
	W(KMaliOffGp + KGpCmd, KGpCmdUpdatePlbuAlloc);
	p.iTimer->OneShot(NKern::TimerTicks(KMaliJobTimeoutMs));
	W(KMaliOffGp + KGpCmd, cmd);
	return KErrNone;
	}

// lima_pp_task_run, Mali-400 flavour (one frame per PP, no broadcast unit)
TInt DMaliDevice::StartPp(DMaliChannel* aCh, TMaliPpFrame& aFrame, TPhysAddr aPd)
	{
	if (aFrame.iNumPp == 0 || aFrame.iNumPp > (TUint32)iBoard.iNumPp)
		return KErrArgument;
	TMaliPipe& p = iPipe[1];
	if (p.iOwner)
		return KErrInUse;
	L2Flush();
	if (p.iVm != aPd)
		{
		for (TInt i = 0; i < iBoard.iNumPp; ++i)
			MmuSwitch(MaliOffPpMmu(i), aPd);
		p.iVm = aPd;
		}
	p.iOwner = aCh;
	p.iError = EFalse;
	p.iResult = KErrNone;
	p.iPending = aFrame.iNumPp;
	p.iTimer->OneShot(NKern::TimerTicks(KMaliJobTimeoutMs));
	for (TUint n = 0; n < aFrame.iNumPp; ++n)
		{
		const TUint pp = MaliOffPp(n);
		aFrame.iFrame[KPpFrame >> 2] = aFrame.iPlbuArrayAddress[n];
		aFrame.iFrame[KPpStack >> 2] = aFrame.iFragmentStackAddress[n];
		for (TInt i = 0; i < KPpFrameRegs; ++i)
			W(pp + KPpFrame + i * 4, aFrame.iFrame[i]);
		for (TInt w = 0; w < 3; ++w)
			for (TInt i = 0; i < KPpWbRegs; ++i)
				W(pp + MaliPpWb(w) + i * 4, aFrame.iWb[w * KPpWbRegs + i]);
		W(pp + KPpCtrl, KPpCtrlStart);
		}
	return KErrNone;
	}

// lima_gp_irq_handler
void DMaliDevice::GpIsr(TAny* aDev)
	{
	DMaliDevice& d = *(DMaliDevice*)aDev;
	const TUint32 state = d.R(KMaliOffGp + KGpIntStat);
	const TUint32 status = d.R(KMaliOffGp + KGpStatus);
	if (!state)
		return;
	TBool done;
	if (state & KGpIrqError)
		{
		d.W(KMaliOffGp + KGpIntMask, 0);		// masked until the hard reset
		d.iPipe[0].iError = ETrue;
		d.iPipe[0].iResult = -(TInt)(0x10000 | (state & 0xFFFF));	// (decodable in the log)
		done = ETrue;
		}
	else
		done = (state & (KGpIrqVsEnd | KGpIrqPlbuEnd)) && !(status & (KGpStatusVsActive | KGpStatusPlbuActive));
	d.W(KMaliOffGp + KGpIntClear, state);
	if (done)
		d.iPipe[0].iDoneDfc->Add();
	}

// lima_pp_irq_handler (one per PP)
void DMaliDevice::PpIsr(TAny* aArg)
	{
	DMaliDevice& d = *TheMali;
	const TInt n = *(TInt*)aArg;
	const TUint pp = MaliOffPp(n);
	const TUint32 state = d.R(pp + KPpIntStatus);
	if (!state)
		return;
	if (state & KPpIrqError)
		{
		d.W(pp + KPpIntMask, 0);
		d.iPipe[1].iError = ETrue;
		d.iPipe[1].iResult = -(TInt)(0x20000 | (n << 13) | (state & 0x1FFF));
		}
	d.W(pp + KPpIntClear, state);
	if (__e32_atomic_tas_ord32((TUint32*)&d.iPipe[1].iPending, 1, -1, 0) == 1)	// the last PP to finish
		d.iPipe[1].iDoneDfc->Add();
	}

// lima_mmu_irq_handler: a page fault / bus error fails the pipe's job
void DMaliDevice::MmuIsr(TAny* aArg)
	{
	DMaliDevice& d = *TheMali;
	const TUint off = *(TUint*)aArg;
	const TUint32 status = d.R(off + KMmuIntStatus);
	if (!status)
		return;
	d.iFaultAddr = d.R(off + KMmuFaultAddr);
	d.iFaultStatus = d.R(off + KMmuStatus) | (off << 16);
	d.W(off + KMmuIntMask, 0);
	d.W(off + KMmuIntClear, status);
	const TInt pipe = off == KMaliOffGpMmu ? 0 : 1;
	d.iPipe[pipe].iError = ETrue;
	d.iPipe[pipe].iResult = KErrAbort;
	if (pipe == 1)
		d.iPipe[1].iPending = 0;
	d.iPipe[pipe].iDoneDfc->Add();
	}

void DMaliDevice::GpDoneDfc(TAny* aDev) { ((DMaliDevice*)aDev)->PipeDone(0); }
void DMaliDevice::PpDoneDfc(TAny* aDev) { ((DMaliDevice*)aDev)->PipeDone(1); }

// The timeout NTimers run in interrupt context: hand over to the DFC with the error set.
void DMaliDevice::GpTimeout(TAny* aDev)
	{
	DMaliDevice& d = *(DMaliDevice*)aDev;
	d.iPipe[0].iError = ETrue;
	d.iPipe[0].iResult = KErrTimedOut;
	d.iPipe[0].iDoneDfc->Add();
	}
void DMaliDevice::PpTimeout(TAny* aDev)
	{
	DMaliDevice& d = *(DMaliDevice*)aDev;
	d.iPipe[1].iError = ETrue;
	d.iPipe[1].iResult = KErrTimedOut;
	d.iPipe[1].iPending = 0;
	d.iPipe[1].iDoneDfc->Add();
	}

// DFC thread: finish the pipe's job (lima_sched_pipe_task_done / task_error / task_fini)
void DMaliDevice::PipeDone(TInt aPipe)
	{
	TMaliPipe& p = iPipe[aPipe];
	NKern::ThreadEnterCS();
	Kern::MutexWait(*iLock);
	if (!p.iOwner)						// a late interrupt / timeout after completion
		{
		Kern::MutexSignal(*iLock);
		NKern::ThreadLeaveCS();
		return;
		}
	p.iTimer->Cancel();
	if (p.iError)
		{
		LogHex(aPipe ? "[EKERN][mali] pp job failed r=" : "[EKERN][mali] gp job failed r=", (TUint32)p.iResult);
		if (p.iResult == KErrAbort)
			{
			LogHex("[EKERN][mali]   mmu fault addr=", iFaultAddr);
			LogHex("[EKERN][mali]   mmu status (off<<16)=", iFaultStatus);
			}
		if (aPipe == 0)
			{
			GpHardReset();
			MmuFaultRecover(KMaliOffGpMmu);
			}
		else
			for (TInt i = 0; i < iBoard.iNumPp; ++i)
				{
				PpHardReset(i);
				MmuFaultRecover(MaliOffPpMmu(i));
				}
		p.iVm = iEmptyPd;				// (the MMUs were reset to the empty VM)
		}
	else if (aPipe == 0)
		GpSoftReset();					// lima resets after every job (task_fini)
	else
		for (TInt i = 0; i < iBoard.iNumPp; ++i)
			PpSoftReset(i);
	// the job's output may still sit in the Mali L2 (write-back PTEs); the CPU reads the arena uncached
	L2Flush();
	DMaliChannel* ch = p.iOwner;
	p.iOwner = NULL;
	ch->Complete(aPipe ? EMaliReqSubmitPp : EMaliReqSubmitGp, p.iError ? p.iResult : KErrNone);
	Kern::MutexSignal(*iLock);
	NKern::ThreadLeaveCS();
	}

// A closing channel's VM must not stay in a pipe (its tables are freed): back to the empty VM, after the job.
void DMaliDevice::ChannelClosing(DMaliChannel* aCh)
	{
	for (TInt p = 0; p < 2; ++p)
		{
		while (iPipe[p].iOwner == aCh)
			{
			Kern::MutexSignal(*iLock);
			NKern::Sleep(NKern::TimerTicks(10));
			Kern::MutexWait(*iLock);
			}
		if (iPipe[p].iVm == aCh->iVm.PdPhys())
			{
			if (p == 0)
				MmuSwitch(KMaliOffGpMmu, iEmptyPd);
			else
				for (TInt i = 0; i < iBoard.iNumPp; ++i)
					MmuSwitch(MaliOffPpMmu(i), iEmptyPd);
			iPipe[p].iVm = iEmptyPd;
			}
		}
	}

// ---------------------------------------------------------------------------------------------------------------
// The logical device

TInt DMaliDevice::Install()
	{
	TInt r = HwInit();
	if (r != KErrNone)
		return r;
	_LIT(KMutexName, "MaliLock");
	r = Kern::MutexCreate(iLock, KMutexName, KMutexOrdGeneral0);
	if (r != KErrNone)
		return r;
	_LIT8(KQName, "MaliDfc");
	r = Kern::DfcQCreate(iDfcQ, 27, &KQName);
	if (r != KErrNone)
		return r;
	iPipe[0].iDoneDfc = new TDfc(GpDoneDfc, this, iDfcQ, 1);
	iPipe[1].iDoneDfc = new TDfc(PpDoneDfc, this, iDfcQ, 1);
	iPipe[0].iTimer = new NTimer(GpTimeout, this);
	iPipe[1].iTimer = new NTimer(PpTimeout, this);
	if (!iPipe[0].iDoneDfc || !iPipe[1].iDoneDfc || !iPipe[0].iTimer || !iPipe[1].iTimer)
		return KErrNoMemory;
	TheMali = this;
	Log("[EKERN][mali] Mali-400 driver ready (RMaliChannel)\n");
	return SetName(&KMaliLddName);
	}

void DMaliDevice::GetCaps(TDes8& /*aDes*/) const
	{
	}

TInt DMaliDevice::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DMaliChannel(this);
	return aChannel ? KErrNone : KErrNoMemory;
	}

// ---------------------------------------------------------------------------------------------------------------
// The channel

TInt DMaliChannel::DoCreate(TInt /*aUnit*/, const TDesC8* /*aInfo*/, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(TVersion(KMaliMajorVersion, 0, 0), aVer))
		return KErrNotSupported;
	iClient = &Kern::CurrentThread();
	iClient->Open();
	for (TInt i = 0; i < EMaliReqCount; ++i)
		{
		TInt r = Kern::CreateClientRequest(iReq[i]);
		if (r != KErrNone)
			return r;
		}
	TInt r = iVm.Create();
	if (r != KErrNone)
		return r;
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = KMaliArenaMax;
	info.iMapAttr = EMapAttrBufferedC;		// uncached, write-combining (lima maps its buffers write-combined)
	info.iOwnsMemory = ETrue;
	info.iDestroyedDfc = NULL;
	return Kern::ChunkCreate(info, iArena, iArenaLin, iArenaMapAttr);
	}

DMaliChannel::~DMaliChannel()
	{
	if (iDev->iLock)
		{
		NKern::ThreadEnterCS();
		Kern::MutexWait(*iDev->iLock);
		iDev->ChannelClosing(this);
		Kern::MutexSignal(*iDev->iLock);
		NKern::ThreadLeaveCS();
		}
	for (TInt i = 0; i < KMaliMaxImports; ++i)
		if (iImport[i].iChunk)
			Unimport(iImport[i].iVa, EFalse);
	iVm.Destroy();
	if (iArena)
		Kern::ChunkClose(iArena);
	for (TInt i = 0; i < EMaliReqCount; ++i)
		Kern::DestroyClientRequest(iReq[i]);
	if (iClient)
		Kern::SafeClose((DObject*&)iClient, NULL);
	}

void DMaliChannel::Complete(TInt aReq, TInt aResult)
	{
	if (iReq[aReq]->IsReady())
		Kern::QueueRequestComplete(iClient, iReq[aReq], aResult);
	}

// Commit arena pages and enter them in the GPU page tables.
TInt DMaliChannel::Commit(TInt aOffset, TInt aSize)
	{
	if (aOffset < 0 || aSize <= 0 || (aOffset & 0xFFF) || (aSize & 0xFFF) || aOffset + aSize > KMaliArenaMax)
		return KErrArgument;
	NKern::ThreadEnterCS();
	TInt r = Kern::ChunkCommit(iArena, aOffset, aSize);
	const TInt pages = aSize >> 12;
	TPhysAddr* list = r == KErrNone ? (TPhysAddr*)Kern::Alloc(pages * sizeof(TPhysAddr)) : NULL;
	if (r == KErrNone && !list)
		r = KErrNoMemory;
	if (r == KErrNone)
		{
		TLinAddr lin;
		TUint32 attr;
		TPhysAddr phys;
		r = Kern::ChunkPhysicalAddress(iArena, aOffset, aSize, lin, attr, phys, list);
		if (r == 0)						// contiguous: the list may not be filled
			for (TInt i = 0; i < pages; ++i)
				list[i] = phys + (i << 12);
		if (r >= 0)
			{
			memclr((TAny*)lin, aSize);	// the GPU must not see stale data
			Kern::MutexWait(*iDev->iLock);
			for (TInt i = 0; i < pages; ++i)
				iVm.Map(KMaliArenaVa + aOffset + (i << 12), list[i]);
			ZapMmus();
			Kern::MutexSignal(*iDev->iLock);
			r = KErrNone;
			}
		}
	Kern::Free(list);
	NKern::ThreadLeaveCS();
	return r;
	}

// After a page-table change of a VM a pipe is using: drop the MMUs' cached translations (the caller holds iLock).
void DMaliChannel::ZapMmus()
	{
	IoBarrier();
	if (iDev->iPipe[0].iVm == iVm.PdPhys())
		iDev->W(KMaliOffGpMmu + KMmuCommand, KMmuCmdZapCache);
	if (iDev->iPipe[1].iVm == iVm.PdPhys())
		for (TInt i = 0; i < iDev->iBoard.iNumPp; ++i)
			iDev->W(MaliOffPpMmu(i) + KMmuCommand, KMmuCmdZapCache);
	}

// first-fit over the import region's page bitmap; -1 = full
TInt DMaliChannel::AllocImportPages(TInt aPages)
	{
	TInt run = 0;
	for (TInt p = 0; p < KMaliImportPages; ++p)
		{
		if (iImportMap[p >> 5] & (1u << (p & 31)))
			run = 0;
		else if (++run == aPages)
			{
			const TInt first = p + 1 - aPages;
			for (TInt i = first; i <= p; ++i)
				iImportMap[i >> 5] |= 1u << (i & 31);
			return first;
			}
		}
	return -1;
	}

void DMaliChannel::FreeImportPages(TInt aFirst, TInt aPages)
	{
	for (TInt i = aFirst; i < aFirst + aPages; ++i)
		iImportMap[i >> 5] &= ~(1u << (i & 31));
	}

// Map a page-aligned range of one of the client's shared chunks (a SurfaceManager surface) for the GPU.
TInt DMaliChannel::Import(TMaliImport& aImp, TBool aByAddress)
	{
	// aByAddress: iOffset is a user address (page aligned) in the client's process, not an offset in a chunk
	if ((!aByAddress && aImp.iOffset < 0) || aImp.iSize <= 0 || (aImp.iOffset & 0xFFF) || (aImp.iSize & 0xFFF))
		return KErrArgument;
	TInt slot = -1;
	for (TInt i = 0; i < KMaliMaxImports && slot < 0; ++i)
		if (!iImport[i].iChunk)
			slot = i;
	if (slot < 0)
		return KErrTooBig;
	const TInt pages = aImp.iSize >> 12;
	NKern::ThreadEnterCS();
	TInt chunkOffset = aImp.iOffset;
	DChunk* chunk = aByAddress ? Kern::OpenSharedChunk(iClient, (const TAny*)aImp.iOffset, ETrue, chunkOffset)
		: Kern::OpenSharedChunk(iClient, aImp.iChunkHandle, ETrue);
	TInt r = chunk ? KErrNone : KErrBadHandle;
	TPhysAddr* list = r == KErrNone ? (TPhysAddr*)Kern::Alloc(pages * sizeof(TPhysAddr)) : NULL;
	if (r == KErrNone && !list)
		r = KErrNoMemory;
	TLinAddr lin = 0;
	TUint32 attr = 0;
	if (r == KErrNone)
		{
		TPhysAddr phys;
		r = Kern::ChunkPhysicalAddress(chunk, chunkOffset, aImp.iSize, lin, attr, phys, list);
		if (r == 0)
			for (TInt i = 0; i < pages; ++i)
				list[i] = phys + (i << 12);
		if (r > 0)
			r = KErrNone;
		}
	TInt first = -1;
	if (r == KErrNone)
		{
		Kern::MutexWait(*iDev->iLock);
		first = AllocImportPages(pages);
		if (first < 0)
			r = KErrNoMemory;
		else
			{
			const TUint32 va = KMaliImportVa + ((TUint32)first << 12);
			for (TInt i = 0; i < pages; ++i)
				iVm.Map(va + (i << 12), list[i]);
			ZapMmus();
			TMaliImportRec& rec = iImport[slot];
			rec.iChunk = chunk;
			rec.iVa = va;
			rec.iPages = pages;
			rec.iKernLin = lin;
			rec.iMapAttr = attr;
			aImp.iVa = va;
			}
		Kern::MutexSignal(*iDev->iLock);
		}
	if (r != KErrNone && chunk)
		Kern::ChunkClose(chunk);
	Kern::Free(list);
	NKern::ThreadLeaveCS();
	return r;
	}

// The caller must have waited for every job that uses the range (the pages go back to the chunk's owner).
TInt DMaliChannel::Unimport(TUint32 aVa, TBool aLocked)
	{
	TInt slot = -1;
	for (TInt i = 0; i < KMaliMaxImports && slot < 0; ++i)
		if (iImport[i].iChunk && iImport[i].iVa == aVa)
			slot = i;
	if (slot < 0)
		return KErrNotFound;
	TMaliImportRec& rec = iImport[slot];
	NKern::ThreadEnterCS();
	if (aLocked)
		Kern::MutexWait(*iDev->iLock);
	for (TInt i = 0; i < rec.iPages; ++i)
		iVm.Unmap(rec.iVa + (i << 12));
	ZapMmus();
	FreeImportPages((rec.iVa - KMaliImportVa) >> 12, rec.iPages);
	if (aLocked)
		Kern::MutexSignal(*iDev->iLock);
	DChunk* chunk = rec.iChunk;
	rec.iChunk = NULL;
	Kern::ChunkClose(chunk);
	NKern::ThreadLeaveCS();
	return KErrNone;
	}

// CPU cache maintenance on an imported range (DMA rules: the GPU is the DMA engine; a cached chunk only)
TInt DMaliChannel::CacheSync(const TMaliCacheSync& aSync)
	{
	for (TInt i = 0; i < KMaliMaxImports; ++i)
		{
		const TMaliImportRec& rec = iImport[i];
		if (!rec.iChunk || aSync.iVa < rec.iVa || aSync.iSize < 0
			|| aSync.iVa + (TUint32)aSync.iSize > rec.iVa + ((TUint32)rec.iPages << 12))
			continue;
		const TLinAddr lin = rec.iKernLin + (aSync.iVa - rec.iVa);
		switch (aSync.iOp)
			{
			case EMaliCacheBeforeGpuRead:
				Cache::SyncMemoryBeforeDmaWrite(lin, aSync.iSize, rec.iMapAttr);
				break;
			case EMaliCacheBeforeGpuWrite:
				Cache::SyncMemoryBeforeDmaRead(lin, aSync.iSize, rec.iMapAttr);
				break;
			case EMaliCacheAfterGpuWrite:
				Cache::SyncMemoryAfterDmaRead(lin, aSync.iSize, rec.iMapAttr);
				break;
			default:
				return KErrArgument;
			}
		return KErrNone;
		}
	return KErrNotFound;
	}

TInt DMaliChannel::Request(TInt aReqNo, TAny* a1, TAny* a2)
	{
	if (aReqNo == KMaxTInt)				// cancel: jobs run to completion (or the timeout); nothing to do
		return KErrNone;
	if (aReqNo >= 0)
		{
		switch (aReqNo)
			{
			case EMaliCtrlGetInfo:
				{
				TMaliInfo info;
				info.iGpVersion = iDev->iGpVersion;
				info.iPpVersion = iDev->iPpVersion;
				info.iNumPp = iDev->iBoard.iNumPp;
				info.iArenaVa = KMaliArenaVa;
				info.iArenaMaxSize = KMaliArenaMax;
				TPckgC<TMaliInfo> p(info);
				return Kern::ThreadDesWrite(iClient, a1, p, 0, KChunkShiftBy0, NULL);
				}
			case EMaliCtrlOpenArena:
				{
				NKern::ThreadEnterCS();
				TInt h = Kern::MakeHandleAndOpen(NULL, iArena);
				NKern::ThreadLeaveCS();
				return h;
				}
			case EMaliCtrlCommit:
				return Commit((TInt)a1, (TInt)a2);
			case EMaliCtrlImport:
			case EMaliCtrlImportAddress:
				{
				TMaliImport imp;
				TPckg<TMaliImport> p(imp);
				TInt r = Kern::ThreadDesRead(iClient, a1, p, 0, KChunkShiftBy0);
				if (r != KErrNone)
					return r;
				r = Import(imp, aReqNo == EMaliCtrlImportAddress);
				if (r != KErrNone)
					return r;
				return Kern::ThreadDesWrite(iClient, a1, p, 0, KChunkShiftBy0, NULL);
				}
			case EMaliCtrlUnimport:
				return Unimport((TUint32)a1, ETrue);
			case EMaliCtrlCacheSync:
				{
				TMaliCacheSync s;
				TPckg<TMaliCacheSync> p(s);
				TInt r = Kern::ThreadDesRead(iClient, a1, p, 0, KChunkShiftBy0);
				return r == KErrNone ? CacheSync(s) : r;
				}
			default:
				return KErrNotSupported;
			}
		}
	// asynchronous: ~aReqNo, a1 = the TRequestStatus, a2 = the user's {arg1, arg2}
	const TInt req = ~aReqNo;
	if (req < 0 || req >= EMaliReqCount)
		return KErrNotSupported;
	TAny* args[2];
	kumemget32(args, a2, sizeof(args));
	TInt r = iReq[req]->SetStatus((TRequestStatus*)a1);
	if (r != KErrNone)
		return r;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*iDev->iLock);
	if (req == EMaliReqSubmitGp)
		{
		TMaliGpFrame f;
		kumemget32(&f, args[0], sizeof(f));
		r = iDev->StartGp(this, f, iVm.PdPhys());
		}
	else
		{
		TMaliPpFrame f;
		kumemget32(&f, args[0], sizeof(f));
		r = iDev->StartPp(this, f, iVm.PdPhys());
		}
	Kern::MutexSignal(*iDev->iLock);
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		Kern::QueueRequestComplete(iClient, iReq[req], r);
	return KErrNone;
	}

// ---------------------------------------------------------------------------------------------------------------
// Board entry: called once at boot by the board file (after its clocks are known).

extern "C" TInt MaliInstall(const TMaliBoard& aBoard)
	{
	NKern::ThreadEnterCS();
	DMaliDevice* dev = new DMaliDevice(aBoard);
	NKern::ThreadLeaveCS();
	if (!dev)
		return KErrNoMemory;
	TInt r = Kern::InstallLogicalDevice(dev);
	LogHex("[EKERN][mali] InstallLogicalDevice r=", r);
	return r;
	}
