// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

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
	void Deliver();							// mutex held
public:
	DThread* iClient;
	TClientRequest* iGetReq;
	TDes8* iGetDes;
	TUint8* iMsg;							// {op LE32, payload}
	TInt iMsgLen;							// 0 = none waiting
	TBool iOutstanding;						// a request went to the helper and its reply has not come back yet
	};

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

TInt DPhFsChannel::DoCreate(TInt, const TDesC8*, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(TVersion(RPhFs::EMajorVersion, 0, 0), aVer))
		return KErrNotSupported;
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
		if (((TUint)a1 & (1u << RPhFs::ERequestGet)) && iGetReq->IsReady())
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
		if (~aReqNo != RPhFs::ERequestGet)
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
