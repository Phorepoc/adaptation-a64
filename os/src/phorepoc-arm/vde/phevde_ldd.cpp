// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// phevde_ldd.cpp -- OURS (VDU rung V0): the kernel driver of the Allwinner Video Engine.
// The user API is RPheVde (rphevde.h). The engine is stateless, so the driver is small: register access, a
// physically contiguous buffer arena, and one job at a time (register writes, then wait for the interrupt).
// Board specifics (base, interrupt, clocks, SRAM) come through TPheVdeBoard (src/phorepoc-arm/a64/vde/phevde_a64.cpp).
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <kernel/arm/assp.h>
#include "rphevde.h"
#include "phevde_board.h"

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);

static void LogHex(const char* aWhat, TUint32 aV) { ekseam_puts(aWhat); ekseam_puthex(aV); }

const TInt KVeWindow = 0x1000;
const TUint KVeVersion = 0x0F0;
const TInt KVeArenaMax = 128 << 20;
const TInt KVeJobTimeoutMs = 1000;

class DPheVdeChannel;

class DPheVdeDevice : public DLogicalDevice
	{
public:
	DPheVdeDevice(const TPheVdeBoard& aBoard) : iBoard(aBoard), iFinished(1) { iVersion = TVersion(KPheVdeMajorVersion, 0, 0); }
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const;
	virtual TInt Create(DLogicalChannelBase*& aChannel);

	inline TUint32 R(TUint aOff) const { return *(volatile TUint32*)(iBase + aOff); }
	inline void W(TUint aOff, TUint32 aV) { *(volatile TUint32*)(iBase + aOff) = aV; }

	TInt Start(DPheVdeChannel* aCh, const TPheVdeJob& aJob);
	void ChannelClosing(DPheVdeChannel* aCh);
	static void Isr(TAny* aDev);
	static void Timeout(TAny* aDev);
	static void DoneDfc(TAny* aDev);

public:
	TPheVdeBoard iBoard;
	DPlatChunkHw* iRegs;
	TLinAddr iBase;
	TUint32 iVeVersion, iPllVe, iSramCtrl;
	volatile TUint32 iIrqCount;
	TUint32 iPollTimeouts;
	DMutex* iLock;
	TDfcQue* iDfcQ;
	TDfc* iDone;
	NTimer* iTimer;
	// the job in flight (iOwner NULL = idle)
	DPheVdeChannel* iOwner;
	TUint32 iStatusOff, iStatusMask, iIntCtrlOff, iIntCtrlMask;
	volatile TInt iResult;
	volatile TInt iFinished;		// set once, by the ISR or the timeout
	};

class DPheVdeChannel : public DLogicalChannelBase
	{
public:
	DPheVdeChannel(DPheVdeDevice* aDev) : iDev(aDev) {}
	virtual ~DPheVdeChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aReqNo, TAny* a1, TAny* a2);
	void Complete(TInt aResult);
private:
	TInt Commit(TPheVdeCommit& aC);
public:
	DPheVdeDevice* iDev;
	DThread* iClient;
	TClientRequest* iReq;
	DChunk* iArena;
	TLinAddr iArenaLin;
	TUint32 iArenaMapAttr;
	};

// ---------------------------------------------------------------------------------------------------------------
// The device

TInt DPheVdeDevice::Install()
	{
	TInt r = iBoard.iPowerOn ? iBoard.iPowerOn(iPllVe, iSramCtrl) : KErrNone;
	LogHex("[EKERN][ve] power on r=", (TUint32)r);
	if (r != KErrNone)
		return r;
	r = DPlatChunkHw::New(iRegs, iBoard.iBase, KVeWindow, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return r;
	iBase = iRegs->LinearAddress();
	iVeVersion = R(KVeVersion);
	LogHex("[EKERN][ve] VE_VERSION=", iVeVersion);
	LogHex("[EKERN][ve] PLL_VE=", iPllVe);
	LogHex("[EKERN][ve] SRAM ctrl=", iSramCtrl);
	_LIT(KMutexName, "PheVdeLock");
	r = Kern::MutexCreate(iLock, KMutexName, KMutexOrdGeneral0);
	if (r != KErrNone)
		return r;
	_LIT8(KQName, "PheVdeDfc");
	r = Kern::DfcQCreate(iDfcQ, 27, &KQName);
	if (r != KErrNone)
		return r;
	iDone = new TDfc(DoneDfc, this, iDfcQ, 1);
	iTimer = new NTimer(Timeout, this);
	if (!iDone || !iTimer)
		return KErrNoMemory;
	r = Interrupt::Bind(iBoard.iIrq, Isr, this);
	LogHex("[EKERN][ve] Interrupt::Bind r=", (TUint32)r);
	if (r < 0)
		return r;
	Interrupt::Enable(iBoard.iIrq);
	ekseam_puts("[EKERN][ve] VE driver ready (RPheVde)\n");
	return SetName(&KPheVdeLddName);
	}

void DPheVdeDevice::GetCaps(TDes8& /*aDes*/) const
	{
	}

TInt DPheVdeDevice::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DPheVdeChannel(this);
	return aChannel ? KErrNone : KErrNoMemory;
	}

// Thread context, iLock held: program the job; its last write starts the engine.
TInt DPheVdeDevice::Start(DPheVdeChannel* aCh, const TPheVdeJob& aJob)
	{
	if (iOwner)
		return KErrInUse;
	if (aJob.iNumWrites < 1 || aJob.iNumWrites > KPheVdeMaxWrites || aJob.iStatusOff >= (TUint)KVeWindow
		|| aJob.iIntCtrlOff >= (TUint)KVeWindow || (aJob.iStatusOff & 3) || (aJob.iIntCtrlOff & 3))
		return KErrArgument;
	for (TInt i = 0; i < aJob.iNumWrites; ++i)
		if ((aJob.iWrites[i].iOff & ~KPheVdePoll) >= (TUint)KVeWindow || (aJob.iWrites[i].iOff & 3))
			return KErrArgument;
	iOwner = aCh;
	iStatusOff = aJob.iStatusOff;
	iStatusMask = aJob.iStatusMask;
	iIntCtrlOff = aJob.iIntCtrlOff;
	iIntCtrlMask = aJob.iIntCtrlMask;
	iResult = KErrTimedOut;
	iFinished = 0;
	iTimer->OneShot(NKern::TimerTicks(KVeJobTimeoutMs));
	for (TInt i = 0; i < aJob.iNumWrites; ++i)
		{
		const TUint off = aJob.iWrites[i].iOff;
		if (off & KPheVdePoll)			// wait (<= 1 ms) until (reg & mask) == 0, e.g. the H.264 VLD busy bit
			{
			TInt n = 0;
			while ((R(off & ~KPheVdePoll) & aJob.iWrites[i].iVal) && ++n < 1000)
				Kern::NanoWait(1000);
			if (n >= 1000)
				++iPollTimeouts;
			}
		else
			W(off, aJob.iWrites[i].iVal);
		}
	__e32_io_completion_barrier();
	return KErrNone;
	}

// Interrupt context: clear the engine's status + interrupt enables, hand the status to the DFC.
void DPheVdeDevice::Isr(TAny* aDev)
	{
	DPheVdeDevice& d = *(DPheVdeDevice*)aDev;
	++d.iIrqCount;
	if (d.iFinished)					// no job in flight (a stray or late interrupt): leave the engine alone
		return;
	const TUint32 s = d.R(d.iStatusOff);
	d.W(d.iIntCtrlOff, d.R(d.iIntCtrlOff) & ~d.iIntCtrlMask);
	d.W(d.iStatusOff, s & d.iStatusMask);
	__e32_io_completion_barrier();
	if (__e32_atomic_swp_ord32(&d.iFinished, 1) == 0)
		{
		d.iResult = (TInt)(s & 0x7FFFFFFF);
		d.iDone->Add();
		}
	}

// The NTimer runs in interrupt context too.
void DPheVdeDevice::Timeout(TAny* aDev)
	{
	DPheVdeDevice& d = *(DPheVdeDevice*)aDev;
	if (__e32_atomic_swp_ord32(&d.iFinished, 1) == 0)
		{
		d.iResult = KErrTimedOut;
		d.iDone->Add();
		}
	}

void DPheVdeDevice::DoneDfc(TAny* aDev)
	{
	DPheVdeDevice& d = *(DPheVdeDevice*)aDev;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*d.iLock);
	d.iTimer->Cancel();
	DPheVdeChannel* ch = d.iOwner;
	d.iOwner = NULL;
	if (d.iResult == KErrTimedOut)
		{
		LogHex("[EKERN][ve] job timed out, status=", d.R(d.iStatusOff));
		d.W(d.iIntCtrlOff, d.R(d.iIntCtrlOff) & ~d.iIntCtrlMask);
		}
	if (ch)
		ch->Complete(d.iResult);
	Kern::MutexSignal(*d.iLock);
	NKern::ThreadLeaveCS();
	}

// iLock held: a closing channel's job keeps running to its interrupt / timeout, nobody is completed.
void DPheVdeDevice::ChannelClosing(DPheVdeChannel* aCh)
	{
	if (iOwner == aCh)
		iOwner = NULL;
	}

// ---------------------------------------------------------------------------------------------------------------
// The channel

TInt DPheVdeChannel::DoCreate(TInt /*aUnit*/, const TDesC8* /*aInfo*/, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(TVersion(KPheVdeMajorVersion, 0, 0), aVer))
		return KErrNotSupported;
	iClient = &Kern::CurrentThread();
	iClient->Open();
	TInt r = Kern::CreateClientRequest(iReq);
	if (r != KErrNone)
		return r;
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = KVeArenaMax;
	info.iMapAttr = EMapAttrBufferedC;		// uncached, write-combining: the VE reads and writes it behind the CPU
	info.iOwnsMemory = ETrue;
	info.iDestroyedDfc = NULL;
	return Kern::ChunkCreate(info, iArena, iArenaLin, iArenaMapAttr);
	}

DPheVdeChannel::~DPheVdeChannel()
	{
	if (iDev->iLock)
		{
		NKern::ThreadEnterCS();
		Kern::MutexWait(*iDev->iLock);
		iDev->ChannelClosing(this);
		Kern::MutexSignal(*iDev->iLock);
		NKern::ThreadLeaveCS();
		}
	if (iArena)
		Kern::ChunkClose(iArena);
	if (iReq)
		Kern::DestroyClientRequest(iReq);
	if (iClient)
		Kern::SafeClose((DObject*&)iClient, NULL);
	}

void DPheVdeChannel::Complete(TInt aResult)
	{
	if (iReq->IsReady())
		Kern::QueueRequestComplete(iClient, iReq, aResult);
	}

TInt DPheVdeChannel::Commit(TPheVdeCommit& aC)
	{
	if ((aC.iOffset | aC.iSize) & 0xFFF || aC.iSize <= 0 || aC.iOffset < 0 || aC.iOffset + aC.iSize > KVeArenaMax)
		return KErrArgument;
	TUint32 phys = 0;
	NKern::ThreadEnterCS();
	TInt r = Kern::ChunkCommitContiguous(iArena, aC.iOffset, aC.iSize, phys);
	NKern::ThreadLeaveCS();
	aC.iPhys = phys;
	return r;
	}

TInt DPheVdeChannel::Request(TInt aReqNo, TAny* a1, TAny* a2)
	{
	if (aReqNo == KMaxTInt)				// cancel: the job runs to its interrupt or the timeout
		return KErrNone;
	if (aReqNo >= 0)
		{
		switch (aReqNo)
			{
			case EPheVdeCtrlGetInfo:
				{
				TPheVdeInfo info;
				info.iVersion = iDev->iVeVersion;
				info.iIrqCount = iDev->iIrqCount;
				info.iPllVe = iDev->iPllVe;
				info.iSramCtrl = iDev->iSramCtrl;
				info.iArenaMaxSize = KVeArenaMax;
				info.iPollTimeouts = iDev->iPollTimeouts;
				TPckgC<TPheVdeInfo> p(info);
				return Kern::ThreadDesWrite(iClient, a1, p, 0, KChunkShiftBy0, NULL);
				}
			case EPheVdeCtrlReadReg:
				{
				const TUint off = (TUint)a1;
				if (off >= (TUint)KVeWindow || (off & 3))
					return KErrArgument;
				TUint32 v = iDev->R(off);
				TPckgC<TUint32> p(v);
				return Kern::ThreadDesWrite(iClient, a2, p, 0, KChunkShiftBy0, NULL);
				}
			case EPheVdeCtrlWriteReg:
				{
				const TUint off = (TUint)a1;
				if (off >= (TUint)KVeWindow || (off & 3))
					return KErrArgument;
				iDev->W(off, (TUint32)a2);
				return KErrNone;
				}
			case EPheVdeCtrlSetPll:
				{
				if (!iDev->iBoard.iSetPll || !iDev->iFinished)
					return KErrNotSupported;
				TUint32 now = 0;
				const TInt r = iDev->iBoard.iSetPll((TUint32)a1, now);
				iDev->iPllVe = now;
				TPckgC<TUint32> p(now);
				const TInt w = Kern::ThreadDesWrite(iClient, a2, p, 0, KChunkShiftBy0, NULL);
				return r != KErrNone ? r : w;
				}
			case EPheVdeCtrlPhysRead:
				{
				if (!iDev->iBoard.iPhysRead)
					return KErrNotSupported;
				TUint32 v = 0;
				const TInt r = iDev->iBoard.iPhysRead((TUint32)a1, v);
				if (r != KErrNone)
					return r;
				TPckgC<TUint32> p(v);
				return Kern::ThreadDesWrite(iClient, a2, p, 0, KChunkShiftBy0, NULL);
				}
			case EPheVdeCtrlOpenArena:
				{
				NKern::ThreadEnterCS();
				TInt h = Kern::MakeHandleAndOpen(NULL, iArena);
				NKern::ThreadLeaveCS();
				return h;
				}
			case EPheVdeCtrlCommit:
				{
				TPheVdeCommit c;
				TPckg<TPheVdeCommit> p(c);
				TInt r = Kern::ThreadDesRead(iClient, a1, p, 0, KChunkShiftBy0);
				if (r != KErrNone)
					return r;
				r = Commit(c);
				if (r != KErrNone)
					return r;
				return Kern::ThreadDesWrite(iClient, a1, p, 0, KChunkShiftBy0, NULL);
				}
			default:
				return KErrNotSupported;
			}
		}
	// asynchronous: ~aReqNo, a1 = the TRequestStatus, a2 = the user's {arg1, arg2}
	if (~aReqNo != EPheVdeReqRun)
		return KErrNotSupported;
	TAny* args[2];
	kumemget32(args, a2, sizeof(args));
	TInt r = iReq->SetStatus((TRequestStatus*)a1);
	if (r != KErrNone)
		return r;
	NKern::ThreadEnterCS();
	TPheVdeJob* job = new TPheVdeJob;
	if (!job)
		r = KErrNoMemory;
	else
		{
		kumemget32(job, args[0], sizeof(TPheVdeJob));
		Kern::MutexWait(*iDev->iLock);
		r = iDev->Start(this, *job);
		Kern::MutexSignal(*iDev->iLock);
		delete job;
		}
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		Kern::QueueRequestComplete(iClient, iReq, r);
	return KErrNone;
	}

// ---------------------------------------------------------------------------------------------------------------
// Board entry: called once at boot by the board file.

extern "C" TInt PheVdeInstall(const TPheVdeBoard& aBoard)
	{
	NKern::ThreadEnterCS();
	DPheVdeDevice* dev = new DPheVdeDevice(aBoard);
	NKern::ThreadLeaveCS();
	if (!dev)
		return KErrNoMemory;
	TInt r = Kern::InstallLogicalDevice(dev);
	LogHex("[EKERN][ve] InstallLogicalDevice r=", (TUint32)r);
	return r;
	}
