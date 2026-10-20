// a64deov.cpp -- OURS (DE2 overlay plan, docs/phorepoc/de2-overlay-plan.md): "PheDeOv", the kernel driver of the A64 Display
// Engine 2 video (VI) plane. User API: rphedeov.h. This file is the validation + lifetime half: it never trusts a physical
// address from user mode -- the caller names its picture by the ADDRESS of the Y / UV planes inside a shared chunk (the VE
// arena), the driver resolves that chunk (Kern::OpenSharedChunk by address), checks the planes are physically contiguous,
// and keeps the chunk open for as long as the DE can scan it (until the vblank after it was replaced). The register work is
// in a64_display.cpp (A64DeOv*: it owns the DE, the vblank interrupt and the lock all UI flips use).
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <kernel/arm/assp.h>
#include "rphedeov.h"

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);

extern "C" void A64DeOvSetDfc(TDfc* aDfc);
extern "C" void A64DeOvInfo(TInt* aW, TInt* aH, TInt* aOn, TUint32* aVblanks, TUint32* aFlips, TUint32* aDone, TInt* aMode);
extern "C" TBool A64DeOvApply(TBool aGeom, TUint32 aY, TUint32 aUV, TInt aPitch, TInt aSrcW, TInt aSrcH,
	TInt aDstX, TInt aDstY, TInt aDstW, TInt aDstH, TInt aFormat, TInt aColor, TInt aFull, TInt aAlpha);
extern "C" void A64DeOvDisable();
extern "C" TInt A64DeOvReadReg(TUint32 aOff, TUint32* aVal);

static void LogHex(const char* aWhat, TUint32 aV) { ekseam_puts(aWhat); ekseam_puthex(aV); }

const TInt KPanelW = 720, KPanelH = 1440;		// (a64_display.cpp)
const TInt KMaxSrc = 4096;						// the A64 VI scanline for YUV (sun50i_a64_mixer0_cfg.scanline_yuv)
const TUint32 KDramBase = 0x40000000;			// the DE reads bus = physical; below this is no RAM
const TInt KRetireMax = 8;

class DPheDeOvChannel;

class DPheDeOvDevice : public DLogicalDevice
	{
public:
	DPheDeOvDevice() : iOwner(NULL) { iVersion = TVersion(KPheDeOvMajorVersion, 0, 0); }
	virtual TInt Install();
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DLogicalChannelBase*& aChannel);
	static void Done(TAny* aDev);
public:
	DMutex* iLock;
	TDfcQue* iDfcQ;
	TDfc* iDfc;
	DPheDeOvChannel* volatile iOwner;
	};

class DPheDeOvChannel : public DLogicalChannelBase
	{
public:
	DPheDeOvChannel(DPheDeOvDevice* aDev) : iDev(aDev), iClient(NULL), iReq(NULL), iPending(EFalse), iHaveGeom(EFalse), iCurY(NULL), iCurUV(NULL), iNRetire(0) {}
	virtual ~DPheDeOvChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aReqNo, TAny* a1, TAny* a2);
	void VblankDone();			// DFC thread, iLock held
private:
	TInt Resolve(const TUint8* aUser, TInt aPitch, TInt aRows, TInt aRowBytes, DChunk*& aChunk, TUint32& aPhys);
	TInt DoSet(const TPheDeOvSet& aS, TBool aFlipOnly, const TPheDeOvFlip* aF);
	void Retire(DChunk* aC);
	void CloseRetired();
	void DisableAll();
	TInt Flip(TAny* a1, TAny* a2);
public:
	DPheDeOvDevice* iDev;
	DThread* iClient;
	TClientRequest* iReq;
	TBool iPending;			// a Flip request is armed
	TBool iHaveGeom;
	TPheDeOvSet iGeom;
	DChunk* iCurY;
	DChunk* iCurUV;
	DChunk* iRetire[KRetireMax];
	TInt iNRetire;
	};

// ---------------------------------------------------------------------------------------------------------------
// device

TInt DPheDeOvDevice::Install()
	{
	_LIT(KMutexName, "PheDeOvLock");
	TInt r = Kern::MutexCreate(iLock, KMutexName, KMutexOrdGeneral0);
	if (r != KErrNone)
		return r;
	_LIT8(KQName, "PheDeOvDfc");
	r = Kern::DfcQCreate(iDfcQ, 27, &KQName);
	if (r != KErrNone)
		return r;
	iDfc = new TDfc(Done, this, iDfcQ, 1);
	if (!iDfc)
		return KErrNoMemory;
	A64DeOvSetDfc(iDfc);
	ekseam_puts("[EKERN][deov] DE2 video overlay driver ready (RPheDeOv)\n");
	return SetName(&KPheDeOvLddName);
	}

TInt DPheDeOvDevice::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DPheDeOvChannel(this);
	return aChannel ? KErrNone : KErrNoMemory;
	}

// the vblank after a Set/Flip: the previous pictures are no longer scanned, the flip request completes
void DPheDeOvDevice::Done(TAny* aDev)
	{
	DPheDeOvDevice& d = *(DPheDeOvDevice*)aDev;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*d.iLock);
	DPheDeOvChannel* ch = d.iOwner;
	if (ch)
		ch->VblankDone();
	Kern::MutexSignal(*d.iLock);
	NKern::ThreadLeaveCS();
	}

// ---------------------------------------------------------------------------------------------------------------
// channel

TInt DPheDeOvChannel::DoCreate(TInt /*aUnit*/, const TDesC8* /*aInfo*/, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(TVersion(KPheDeOvMajorVersion, 0, 0), aVer))
		return KErrNotSupported;
	// the plane scans RAM named by the caller: the same trust as the VE and the display channel (multimedia drivers)
	if (!Kern::CurrentThreadHasCapability(ECapabilityMultimediaDD, __PLATSEC_DIAGNOSTIC_STRING("Checked by PheDeOv")))
		return KErrPermissionDenied;
	iClient = &Kern::CurrentThread();
	iClient->Open();
	TInt r = Kern::CreateClientRequest(iReq);
	if (r != KErrNone)
		return r;
	Kern::MutexWait(*iDev->iLock);
	if (iDev->iOwner)
		r = KErrInUse;						// one overlay plane, one owner
	else
		iDev->iOwner = this;
	Kern::MutexSignal(*iDev->iLock);
	return r;
	}

DPheDeOvChannel::~DPheDeOvChannel()
	{
	NKern::ThreadEnterCS();
	if (iDev->iLock)
		{
		Kern::MutexWait(*iDev->iLock);
		TBool mine = iDev->iOwner == this;
		if (mine)
			{
			DisableAll();
			iDev->iOwner = NULL;
			}
		Kern::MutexSignal(*iDev->iLock);
		}
	if (iReq)
		Kern::DestroyClientRequest(iReq);
	if (iClient)
		Kern::SafeClose((DObject*&)iClient, NULL);
	NKern::ThreadLeaveCS();
	}

// iLock held: the plane off, then the vblank that latches it, then every chunk it could still scan goes
void DPheDeOvChannel::DisableAll()
	{
	A64DeOvDisable();
	iHaveGeom = EFalse;
	if (iPending)
		{
		iPending = EFalse;
		Kern::QueueRequestComplete(iClient, iReq, KErrCancel);
		}
	Retire(iCurY);
	Retire(iCurUV);
	iCurY = iCurUV = NULL;
	if (iNRetire)
		{
		NKern::Sleep(NKern::TimerTicks(50));	// >= 2 frames: the off is latched at the next frame start
		CloseRetired();
		}
	}

void DPheDeOvChannel::Retire(DChunk* aC)
	{
	if (!aC)
		return;
	if (iNRetire >= KRetireMax)
		{
		NKern::Sleep(NKern::TimerTicks(50));
		CloseRetired();
		}
	iRetire[iNRetire++] = aC;
	}

void DPheDeOvChannel::CloseRetired()
	{
	for (TInt i = 0; i < iNRetire; ++i)
		Kern::ChunkClose(iRetire[i]);
	iNRetire = 0;
	}

// DFC thread, iLock held: a vblank passed since the last Apply
void DPheDeOvChannel::VblankDone()
	{
	CloseRetired();
	if (iPending)
		{
		iPending = EFalse;
		Kern::QueueRequestComplete(iClient, iReq, KErrNone);
		}
	}

// aUser: a byte in the caller's chunk. Resolves the chunk, checks [aUser, aUser + pitch*(rows-1) + rowBytes) is physically
// contiguous inside it, returns the bus address of aUser. aChunk is returned opened (the caller closes or keeps it).
TInt DPheDeOvChannel::Resolve(const TUint8* aUser, TInt aPitch, TInt aRows, TInt aRowBytes, DChunk*& aChunk, TUint32& aPhys)
	{
	TInt off = 0;
	DChunk* c = Kern::OpenSharedChunk(iClient, aUser, EFalse, off);
	if (!c)
		return KErrBadHandle;					// not inside a shared chunk of the caller
	const TInt size = aPitch * (aRows - 1) + aRowBytes;
	TLinAddr lin = 0;
	TUint32 attr = 0, phys = 0;
	TInt r = Kern::ChunkPhysicalAddress(c, off, size, lin, attr, phys);
	if (r != 0)									// 0 = one contiguous run; > 0 = scattered pages; < 0 = error
		{
		Kern::ChunkClose(c);
		return r < 0 ? r : KErrNotSupported;
		}
	if (phys < KDramBase || phys + (TUint32)size < phys)
		{
		Kern::ChunkClose(c);
		return KErrArgument;
		}
	aChunk = c;
	aPhys = phys;
	return KErrNone;
	}

// aFlipOnly: new addresses of the geometry given by iGeom (aF), else a whole new setup (aS).
TInt DPheDeOvChannel::DoSet(const TPheDeOvSet& aS, TBool aFlipOnly, const TPheDeOvFlip* aF)
	{
	const TPheDeOvSet& g = aFlipOnly ? iGeom : aS;
	const TUint8* y = aFlipOnly ? aF->iY : aS.iY;
	const TUint8* uv = aFlipOnly ? aF->iUV : aS.iUV;
	DChunk *cy = NULL, *cuv = NULL;
	TUint32 py = 0, puv = 0;
	TInt r = Resolve(y, g.iPitch, g.iSrcH, g.iSrcW, cy, py);
	if (r == KErrNone)
		r = Resolve(uv, g.iPitch, g.iSrcH / 2, g.iSrcW, cuv, puv);
	if (r != KErrNone)
		{
		if (cy)
			Kern::ChunkClose(cy);
		return r;
		}
	// bus addresses: Y and UV at the cropped origin (the caller did the pointer arithmetic)
	Retire(iCurY);
	Retire(iCurUV);
	iCurY = cy;
	iCurUV = cuv;
	if (!aFlipOnly)
		{
		iGeom = aS;
		iHaveGeom = ETrue;
		}
	const TBool armed = A64DeOvApply(!aFlipOnly, py, puv, g.iPitch, g.iSrcW, g.iSrcH, g.iDstX, g.iDstY, g.iDstW, g.iDstH,
		g.iFormat, g.iColor == EPheDeOvAuto ? 2 : g.iColor, g.iFullRange, g.iAlpha);
	if (!armed)
		{
		CloseRetired();										// (display off: nothing was scanning)
		}
	return armed ? 1 : 0;									// 1 = completes at a vblank
	}

static TInt Check(const TPheDeOvSet& s)
	{
	if (s.iFormat != EPheDeOvNV12 && s.iFormat != EPheDeOvNV21)
		return KErrNotSupported;
	if (s.iRotation != EPheDeOvRot0)
		return KErrNotSupported;						// a G2D rotation pass is rung V6b
	if (s.iColor < EPheDeOvBT601 || s.iColor > EPheDeOvAuto || s.iAlpha < 0 || s.iAlpha > 255)
		return KErrArgument;
	if (s.iSrcW < 2 || s.iSrcH < 2 || s.iSrcW > KMaxSrc || s.iSrcH > KMaxSrc || ((s.iSrcW | s.iSrcH) & 1))
		return KErrArgument;
	if (s.iPitch < s.iSrcW || (s.iPitch & 1))
		return KErrArgument;
	if (s.iDstW < 2 || s.iDstH < 2 || s.iDstX < 0 || s.iDstY < 0 || s.iDstX + s.iDstW > KPanelW || s.iDstY + s.iDstH > KPanelH)
		return KErrArgument;
	// the VSU step is 20-bit fixed point (Linux SUN8I_VI_SCALER_SCALE_MAX): up to ~16x down. Upscaling has no hard limit.
	if (s.iSrcW > s.iDstW * 16 || s.iSrcH > s.iDstH * 16 * 4)	// (vertical may be coarse-decimated: x4 more)
		return KErrArgument;
	return KErrNone;
	}

TInt DPheDeOvChannel::Flip(TAny* a1, TAny* a2)
	{
	TAny* args[2];
	kumemget32(args, a2, sizeof(args));
	TInt r = iReq->SetStatus((TRequestStatus*)a1);
	if (r != KErrNone)
		return r;
	TPheDeOvFlip f;
	kumemget32(&f, args[0], sizeof(f));
	NKern::ThreadEnterCS();
	Kern::MutexWait(*iDev->iLock);
	if (!iHaveGeom)
		r = KErrNotReady;
	else
		{
		iPending = ETrue;									// before the hardware is told: the vblank DFC may run at once
		r = DoSet(iGeom, ETrue, &f);
		if (r == 1)
			r = KErrNone;									// armed: completes at the vblank
		else
			iPending = EFalse;								// failed (r < 0), or taken now (0: display off)
		}
	const TBool completeNow = !iPending;
	Kern::MutexSignal(*iDev->iLock);
	NKern::ThreadLeaveCS();
	if (completeNow)
		Kern::QueueRequestComplete(iClient, iReq, r);
	return KErrNone;
	}

TInt DPheDeOvChannel::Request(TInt aReqNo, TAny* a1, TAny* a2)
	{
	if (aReqNo == KMaxTInt)							// cancel
		{
		NKern::ThreadEnterCS();
		Kern::MutexWait(*iDev->iLock);
		if (iPending)
			{
			iPending = EFalse;
			Kern::QueueRequestComplete(iClient, iReq, KErrCancel);
			}
		Kern::MutexSignal(*iDev->iLock);
		NKern::ThreadLeaveCS();
		return KErrNone;
		}
	if (aReqNo < 0)									// asynchronous: ~aReqNo
		{
		if (~aReqNo != EPheDeOvReqFlip)
			return KErrNotSupported;
		return Flip(a1, a2);
		}
	TInt r = KErrNotSupported;
	switch (aReqNo)
		{
		case EPheDeOvCtrlGetInfo:
			{
			TPheDeOvInfo info;
			A64DeOvInfo(&info.iPanelW, &info.iPanelH, &info.iEnabled, &info.iVblanks, &info.iFlips, &info.iFlipsDone, &info.iScaleMode);
			info.iMaxSrcW = info.iMaxSrcH = KMaxSrc;
			TPckgC<TPheDeOvInfo> p(info);
			return Kern::ThreadDesWrite(iClient, a1, p, 0, KChunkShiftBy0, NULL);
			}
		case EPheDeOvCtrlSet:
			{
			TPheDeOvSet s;
			TPckg<TPheDeOvSet> p(s);
			r = Kern::ThreadDesRead(iClient, a1, p, 0, KChunkShiftBy0);
			if (r != KErrNone)
				return r;
			r = Check(s);
			if (r != KErrNone)
				return r;
			NKern::ThreadEnterCS();
			Kern::MutexWait(*iDev->iLock);
			if (iPending)									// a flip not yet latched: this Set replaces it
				{
				iPending = EFalse;
				Kern::QueueRequestComplete(iClient, iReq, KErrCancel);
				}
			r = DoSet(s, EFalse, NULL);
			Kern::MutexSignal(*iDev->iLock);
			NKern::ThreadLeaveCS();
			return r < 0 ? r : KErrNone;
			}
		case EPheDeOvCtrlDisable:
			NKern::ThreadEnterCS();
			Kern::MutexWait(*iDev->iLock);
			DisableAll();
			Kern::MutexSignal(*iDev->iLock);
			NKern::ThreadLeaveCS();
			return KErrNone;
		case EPheDeOvCtrlReadReg:
			{
			const TUint32 off = (TUint32)a1;
			const TBool ok = !(off & 3) && (off < 0x6000 || (off >= KPheDeOvRegVsu && off < KPheDeOvRegVsu + 0x1000)
				|| (off >= 0xAA000 && off < 0xAA100));
			if (!ok)
				return KErrArgument;
			TUint32 v = 0;
			r = A64DeOvReadReg(off, &v);
			if (r != KErrNone)
				return r;
			TPckgC<TUint32> p(v);
			return Kern::ThreadDesWrite(iClient, a2, p, 0, KChunkShiftBy0, NULL);
			}
		default:
			return KErrNotSupported;
		}
	}

// ---------------------------------------------------------------------------------------------------------------
// board entry: called once at boot by the board glue (specific/a64_glue.cpp)

extern "C" void A64DeOvInstall()
	{
	NKern::ThreadEnterCS();
	DPheDeOvDevice* dev = new DPheDeOvDevice;
	NKern::ThreadLeaveCS();
	if (!dev)
		return;
	TInt r = Kern::InstallLogicalDevice(dev);
	LogHex("[EKERN][deov] InstallLogicalDevice r=", (TUint32)r);
	}
