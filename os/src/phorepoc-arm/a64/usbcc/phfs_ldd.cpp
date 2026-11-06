// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// phfs_ldd.cpp -- OURS (2026-10-11, PhoneLab Project panel): the kernel half of the debug link's file service, the LDD
// "PhFs.A64" (contract: phfs_api.h). phlink.cpp (kernel side, no file server) hands every 'F' command to
// PhFsSubmit(); the user-side helper src/phfs (phfs.exe) holds one Get request pending, runs the operation against the
// file server and sends the answer with Reply, which goes back through PhLinkFsReplyBuf/Done into the blob stream.
// Shape: as the Sensors.A64 LDD (DLogicalChannelBase request model, TClientRequest completion).
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <nkern.h>
#include "phfs_api.h"

extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(TUint32);
extern "C" TUint8* PhLinkFsReplyBuf(TInt* aMax);		// phlink.cpp: where the reply is assembled (NULL = busy)
extern "C" void PhLinkFsReplyDone(TInt aLen);
extern "C" void PhLinkFsFail(TInt aError);				// phlink.cpp: answer the PC with a bare status
extern "C" TInt A64DisplayCaptureSource(TInt aWhich, TLinAddr* aLin, TInt* aW, TInt* aH);	// a64_display.cpp
extern "C" void A64DisplayChangeCounts(TUint32* aPosts, TUint32* aPanel);

class DPhFsChannel;
static DPhFsChannel* TheChannel;
static DMutex* TheMutex;
_LIT(KMutexName, "PhFsMutex");

class DPhFsDevice : public DLogicalDevice
	{
public:
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const;
	virtual TInt Create(DLogicalChannelBase*& aChannel);
	};

class DPhFsChannel : public DLogicalChannelBase
	{
public:
	virtual ~DPhFsChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aReqNo, TAny* a1, TAny* a2);
	TInt Submit(TInt aOp, const TUint8* aData, TInt aLen);
	TInt Capture(TAny* aDes, TBool aFull);
	void Deliver();							// mutex held
public:
	DThread* iClient;
	TClientRequest* iGetReq;
	TDes8* iGetDes;
	TUint8* iMsg;							// {op LE32, payload}
	TInt iMsgLen;							// 0 = none waiting
	TBool iOutstanding;						// a request went to the helper and its reply has not come back yet
	// a capture channel (RPhFsCapture, the screen recorder pherec.exe): not the file service
	TBool iCapture;
	TBool iPrimed;							// the client's picture holds a whole capture
	TUint32 iPosts, iPanel;					// the display's change counts at the last capture
	TLinAddr iOsLin;
	TUint32* iCapBand;						// Capture: KCapBand composed rows on their way to the client, then a row of each source
	};

const TInt KCapBand = 16;

TInt DPhFsDevice::Install()
	{
	TInt r = Kern::MutexCreate(TheMutex, KMutexName, KMutexOrdGeneral0);
	if (r != KErrNone)
		return r;
	return SetName(&KPhFsLddName);
	}

void DPhFsDevice::GetCaps(TDes8& aDes) const
	{
	Kern::InfoCopy(aDes, KNullDesC8);
	}

TInt DPhFsDevice::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DPhFsChannel;
	return aChannel ? KErrNone : KErrNoMemory;
	}

TInt DPhFsChannel::DoCreate(TInt, const TDesC8* aInfo, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(TVersion(RPhFs::EMajorVersion, 0, 0), aVer))
		return KErrNotSupported;
	if (aInfo && *aInfo == KPhFsCaptureInfo)
		{	// the screen as the user sees it: any number of channels, for a process trusted with device data
		if (!Kern::CurrentThreadHasCapability(ECapabilityReadDeviceData, __PLATSEC_DIAGNOSTIC_STRING("PhFs.A64 capture needs ReadDeviceData")))
			return KErrPermissionDenied;
		iCapture = ETrue;
		return KErrNone;
		}
	// the helper reads and writes any file on the phone: only a process with AllFiles may attach, and only one at a time
	if (!Kern::CurrentThreadHasCapability(ECapabilityAllFiles, __PLATSEC_DIAGNOSTIC_STRING("PhFs.A64 needs AllFiles")))
		return KErrPermissionDenied;
	if (TheChannel)
		return KErrInUse;
	iClient = &Kern::CurrentThread();
	iClient->Open();
	TInt r = Kern::CreateClientRequest(iGetReq);
	if (r != KErrNone)
		return r;
	iMsg = (TUint8*)Kern::Alloc(4 + KPhFsMaxRequest);
	if (!iMsg)
		return KErrNoMemory;
	TheChannel = this;
	return KErrNone;
	}

DPhFsChannel::~DPhFsChannel()
	{
	Kern::MutexWait(*TheMutex);
	if (TheChannel == this)
		TheChannel = NULL;
	const TBool lost = iOutstanding;
	iOutstanding = EFalse;
	Kern::MutexSignal(*TheMutex);
	if (lost)
		PhLinkFsFail(KErrAbort);			// the helper died with a request in hand
	if (iGetReq)
		Kern::DestroyClientRequest(iGetReq);
	if (iMsg)
		Kern::Free(iMsg);
	if (iCapBand)
		Kern::Free(iCapBand);
	if (iClient)
		Kern::SafeClose((DObject*&)iClient, NULL);
	}

// mutex held: hand the waiting message to the helper's pending Get
void DPhFsChannel::Deliver()
	{
	if (!iMsgLen || !iGetReq->IsReady() || !iGetDes)
		return;
	TPtrC8 p(iMsg, iMsgLen);
	const TInt r = Kern::ThreadDesWrite(iClient, iGetDes, p, 0, KChunkShiftBy0, iClient);
	iMsgLen = 0;
	iGetDes = NULL;
	Kern::QueueRequestComplete(iClient, iGetReq, r);
	}

TInt DPhFsChannel::Submit(TInt aOp, const TUint8* aData, TInt aLen)
	{
	if (aLen < 0 || aLen > KPhFsMaxRequest)
		return KErrTooBig;
	Kern::MutexWait(*TheMutex);
	iMsg[0] = (TUint8)aOp; iMsg[1] = iMsg[2] = iMsg[3] = 0;
	memcpy(iMsg + 4, aData, aLen);
	iMsgLen = 4 + aLen;
	iOutstanding = ETrue;
	Deliver();
	Kern::MutexSignal(*TheMutex);
	return KErrNone;
	}

TInt DPhFsChannel::Request(TInt aReqNo, TAny* a1, TAny* a2)
	{
	if (aReqNo == KMaxTInt)
		{	// cancel
		if (!iCapture && ((TUint)a1 & (1u << RPhFs::ERequestGet)) && iGetReq->IsReady())
			{
			Kern::MutexWait(*TheMutex);
			iGetDes = NULL;
			Kern::QueueRequestComplete(iClient, iGetReq, KErrCancel);
			Kern::MutexSignal(*TheMutex);
			}
		return KErrNone;
		}
	if (aReqNo < 0)
		{	// an asynchronous request: ~aReqNo, a1 = the TRequestStatus, a2 = the user's {arg1, arg2}
		if (iCapture || ~aReqNo != RPhFs::ERequestGet)
			return KErrNotSupported;
		TAny* args[2];
		kumemget32(args, a2, sizeof(args));
		TInt r = iGetReq->SetStatus((TRequestStatus*)a1);
		if (r != KErrNone)
			return r;
		Kern::MutexWait(*TheMutex);
		iGetDes = (TDes8*)args[0];
		Deliver();
		Kern::MutexSignal(*TheMutex);
		return KErrNone;
		}
	if (iCapture)
		return aReqNo == RPhFsCapture::EControlCapture ? Capture(a1, (TBool)(TInt)a2) : KErrNotSupported;
	if (aReqNo != RPhFs::EControlReply)
		return KErrNotSupported;
	TInt max = 0;
	TUint8* buf = PhLinkFsReplyBuf(&max);
	if (!buf)
		return KErrInUse;
	TPtr8 dst(buf, 0, max);
	TInt r = Kern::ThreadDesRead(iClient, a1, dst, 0, KChunkShiftBy0);
	if (r != KErrNone)
		return r;
	Kern::MutexWait(*TheMutex);
	iOutstanding = EFalse;
	Kern::MutexSignal(*TheMutex);
	PhLinkFsReplyDone(dst.Length());
	return KErrNone;
	}

// The screen as the user sees it, as phlink's composite screenshot (which 2) builds it: the panel layer (720x1440) at half
// resolution where it is opaque (the bars, the golden overlay), the OS picture (360x640, pitch 368, 80 panel rows down)
// where the panel is see-through. Written to the calling thread's descriptor KCapBand rows at a time, and only the bands
// whose source changed since this channel's last capture (the OS picture posted, the bars drawn); aFull = all of it.
// Returns 0 = nothing changed (the client's picture is still current), else 1 = the OS rows were copied | 2 = the bars.
TInt DPhFsChannel::Capture(TAny* aDes, TBool aFull)
	{
	// the counts first: a change while the pixels are copied shows at the next capture
	TUint32 posts = 0, panelGen = 0;
	A64DisplayChangeCounts(&posts, &panelGen);
	TLinAddr plin = 0, olin = 0;
	TInt pw = 0, ph = 0, ow = 0, oh = 0;
	if (A64DisplayCaptureSource(1, &plin, &pw, &ph) != KErrNone || !plin || pw != 720 || ph != 1440)
		return KErrNotReady;
	if (A64DisplayCaptureSource(0, &olin, &ow, &oh) != KErrNone || ow != 360 || oh != 640)
		olin = 0;							// the OS picture has no mapping right now: the panel layer alone
	DThread* t = &Kern::CurrentThread();
	if (Kern::ThreadGetDesMaxLength(t, aDes) < KPhCapW * KPhCapH * 4)
		return KErrOverflow;
	if (!iCapBand)
		{
		iCapBand = (TUint32*)Kern::Alloc((KCapBand * KPhCapW + 720 + KPhCapW) * 4);
		if (!iCapBand)
			return KErrNoMemory;
		}
	TBool full = aFull || !iPrimed;
#ifdef A64_GOLDEN
	full = ETrue;							// the overlay draws anywhere on the panel
#endif
	// posts == 0: the OS never posted (it draws into one fixed buffer), so its rows are copied every time
	const TBool osChanged = full || posts == 0 || posts != iPosts || olin != iOsLin;
	const TBool barsChanged = full || panelGen != iPanel;
	if (!osChanged && !barsChanged)
		return 0;
	iPosts = posts;
	iPanel = panelGen;
	iOsLin = olin;
	// the display buffers are not cached: copy whole rows in bursts first (memcpy), compose from the copies
	const TUint32* panel = (const TUint32*)plin;
	const TUint32* os = (const TUint32*)olin;
	TUint32* prow = iCapBand + KCapBand * KPhCapW;			// one panel row
	TUint32* orow = prow + 720;								// one OS picture row
	for (TInt row0 = 0; row0 < KPhCapH; row0 += KCapBand)
		{
		// the bars are capture rows 0-39 and 680-719, the OS picture 40-679: a band with neither change is skipped
		const TBool hasBars = row0 < 40 || row0 + KCapBand > 680, hasOs = row0 + KCapBand > 40 && row0 < 680;
		if (!full && !(barsChanged && hasBars) && !(osChanged && hasOs))
			continue;
		for (TInt r = 0; r < KCapBand; ++r)
			{
			const TInt row = row0 + r, oy = row - 40;
			const TBool haveOs = os && oy >= 0 && oy < 640;
			TUint32* d = iCapBand + r * KPhCapW;
#ifndef A64_GOLDEN
			if (haveOs)
				{	// between the bars the panel layer is see-through (a64_display.cpp StripEnable); only the golden
					// image's overlay draws there
				memcpy(d, os + oy * 368, KPhCapW * 4);
				continue;
				}
#endif
			memcpy(prow, panel + (row * 2) * 720, 720 * 4);
			if (haveOs)
				memcpy(orow, os + oy * 368, KPhCapW * 4);
			for (TInt x = 0; x < KPhCapW; ++x)
				{
				const TUint32 p = prow[x * 2];
				d[x] = (p >> 24) ? p : (haveOs ? orow[x] : 0);
				}
			}
		TPtrC8 band((const TUint8*)iCapBand, KCapBand * KPhCapW * 4);
		const TInt r = Kern::ThreadDesWrite(t, aDes, band, row0 * KPhCapW * 4, KChunkShiftBy0, t);
		if (r != KErrNone)
			{
			iPrimed = EFalse;
			return r;
			}
		}
	iPrimed = ETrue;
	return (osChanged ? 1 : 0) | (barsChanged ? 2 : 0);
	}

// phlink -> LDD: one request from the PC. KErrNotReady = no helper attached.
extern "C" TInt PhFsSubmit(TInt aOp, const TUint8* aData, TInt aLen)
	{
	DPhFsChannel* ch = TheChannel;			// a helper that dies right now: its destructor fails the request
	return ch ? ch->Submit(aOp, aData, aLen) : KErrNotReady;
	}

extern "C" void PhFsInstall()
	{
	DPhFsDevice* d = new DPhFsDevice;
	TInt r = d ? Kern::InstallLogicalDevice(d) : KErrNoMemory;
	ekseam_puts("[EKERN][phfs] InstallLogicalDevice(PhFs.A64) r="); ekseam_puthex(r);
	}
