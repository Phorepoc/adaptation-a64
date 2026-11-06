// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// pherec.cpp -- OURS (2026-11-06, PhoneLab "record"): the screen recorder server (pherec.h). Started on demand by phfs.exe.
// A recorder thread captures at the requested rate; the kernel copies only what changed since the last capture (the OS
// picture posted, the bars drawn) and says "unchanged" otherwise, so an idle screen costs next to nothing. A changed
// picture that hashes the same as the last one is not encoded either. Each encoded picture becomes a record (phfs_api.h)
// with its capture time, mean luma and the share of near-white pixels, so PhoneLab lines frames up with the log.
// The encoder is a plug-in (pherecenc.h) from the screen recorder package in Catalogue: not in the image. [pherec] lines (RDebug).
#include <e32base.h>
#include <e32debug.h>
#include <hal.h>
#include "phfs_api.h"
#include "csc.h"
#include "pherec.h"
#include "pherecenc.h"

const TInt KRingSize = 4 << 20;				// a power of two
const TInt KHeadLen = 32;					// the record header (phfs_api.h)
const TInt KGopSeconds = 2;					// an IDR every 2 s: a reader that lost bytes decodes again from there
const TInt64 KFullEveryUs = 500000;			// a whole capture at least this often, whatever the change counts say
const TUint32 KFlagIdr = 1, KFlagEnd = 2;	// KFlagEnd: the last record of a recording (no picture; its time ends the last frame)
const TInt KCapBytes = KPhCapW * KPhCapH * 4;

static void Put32(TUint8* aP, TUint32 aV) { aP[0] = (TUint8)aV; aP[1] = (TUint8)(aV >> 8); aP[2] = (TUint8)(aV >> 16); aP[3] = (TUint8)(aV >> 24); }
static void Put16(TUint8* aP, TUint32 aV) { aP[0] = (TUint8)aV; aP[1] = (TUint8)(aV >> 8); }

static void Head(TUint8* aH, TUint32 aFrame, TInt64 aUs, TUint32 aFlags, TUint32 aLuma, TUint32 aWhite, TUint32 aLen)
	{
	aH[0] = 'P'; aH[1] = 'R'; aH[2] = 'E'; aH[3] = 'C';
	Put32(aH + 4, aFrame);
	Put32(aH + 8, (TUint32)aUs);
	Put32(aH + 12, (TUint32)(aUs >> 32));
	Put32(aH + 16, aFlags);
	Put16(aH + 20, aLuma);
	Put16(aH + 22, aWhite);
	Put32(aH + 24, aLen);
	Put32(aH + 28, 0);						// reserved
	}

// a 64-bit microsecond clock from the fast counter (sampled at least every few seconds: the 32-bit counter wraps in ~3 min)
class TClock
	{
public:
	TClock() : iUs(0), iRem(0)
		{
		TInt f = 0;
		iFreq = HAL::Get(HAL::EFastCounterFrequency, f) == KErrNone && f > 0 ? f : 1000000;
		iLast = User::FastCounter();
		}
	TInt64 Now()
		{
		const TUint32 c = User::FastCounter();
		const TInt64 ticks = (TInt64)(TUint32)(c - iLast) * 1000000 + iRem;
		iLast = c;
		iUs += ticks / iFreq;
		iRem = ticks % iFreq;
		return iUs;
		}
private:
	TInt64 iUs, iRem;
	TInt iFreq;
	TUint32 iLast;
	};

// ---------------------------------------------------------------- the recorder
class CPheRec : public CBase
	{
public:
	~CPheRec();
	TInt Start(TInt aFps, TInt aQp);
	void Read(TUint32 aFrom, TDes8& aOut);
	void Stop(TDes8& aText);
private:
	static TInt ThreadMain(TAny* aSelf);
	TInt Loop();
	void Append(const TUint8* aHead, TInt aHeadLen, const TUint8* aData, TInt aLen);
	void WriteRing(TUint32 aSeq, const TUint8* aP, TInt aLen);
	void Join();
private:
	enum { KStarts = 8192 };				// record starts kept for a reader that fell behind
	RMutex iLock;
	TBool iLockOpen;
	RChunk iRingChunk;
	TUint8* iRing;
	TInt iRingSize;
	TUint32 iWrite;							// the seq of the next byte (bytes ever written)
	TUint32 iStart[KStarts];				// seq of each record start, a ring of the last KStarts
	TInt iStartHead, iStartCount;
	RLibrary iLib;
	TBool iLibOpen;
	TPheRecEncoderNew iNew;
	RThread iThread;
	TBool iThreadOpen;
	volatile TBool iStopReq;
	TInt iState;							// 0 idle, 1 recording, 2 stopped, < 0 the error it stopped with
	TInt iFps, iQp;
	// statistics
	TInt iFrames, iDups, iUnchanged, iDrops, iIdrs, iCaptures;
	TInt64 iBytes, iCapUs, iHashUs, iCscUs, iEncUs, iBusyUs, iWallUs, iIdleUs;
	TInt iMaxFrameUs;
	TBuf8<160> iEncMsg;
	};

CPheRec::~CPheRec()
	{
	Join();
	if (iLibOpen)
		iLib.Close();
	iRingChunk.Close();
	if (iLockOpen)
		iLock.Close();
	}

void CPheRec::Join()
	{
	if (!iThreadOpen)
		return;
	iStopReq = ETrue;
	TRequestStatus st;
	iThread.Logon(st);
	User::WaitForRequest(st);
	iThread.Close();
	iThreadOpen = EFalse;
	}

TInt CPheRec::Start(TInt aFps, TInt aQp)
	{
	if (iState == 1)
		return KErrInUse;
	Join();									// the previous recording's thread has ended (state 2 / error): reap it
	if (!iLibOpen)
		{	// the encoder plug-in: src/pherecenc, the screen recorder package from Catalogue (not in the image)
		TInt r = iLib.Load(KPheRecEncoderDll);
		if (r == KErrNone && iLib.Type()[2] != KPheRecEncoderUid)
			{
			iLib.Close();
			r = KErrNotSupported;
			}
		if (r != KErrNone)
			{
			RDebug::Printf("[pherec] no encoder plug-in (%d): install the screen recorder package from Catalogue", r);
			return r == KErrNotSupported ? r : KErrNotFound;
			}
		iLibOpen = ETrue;
		iNew = (TPheRecEncoderNew)iLib.Lookup(1);
		}
	if (!iLockOpen)
		{
		const TInt r = iLock.CreateLocal();
		if (r != KErrNone)
			return r;
		iLockOpen = ETrue;
		}
	if (!iRing)
		{
		const TInt r = iRingChunk.CreateLocal(KRingSize, KRingSize);
		if (r != KErrNone)
			return r;
		iRing = iRingChunk.Base();
		iRingSize = KRingSize;
		}
	iFps = aFps < 1 ? 30 : Min(aFps, 60);
	iQp = aQp < 10 || aQp > 46 ? 28 : aQp;
	iLock.Wait();
	iWrite = 0;
	iStartHead = iStartCount = 0;
	iLock.Signal();
	iFrames = iDups = iUnchanged = iDrops = iIdrs = iCaptures = 0;
	iBytes = iCapUs = iHashUs = iCscUs = iEncUs = iBusyUs = iWallUs = iIdleUs = 0;
	iMaxFrameUs = 0;
	iEncMsg.Zero();
	iStopReq = EFalse;
	iState = 1;
	const TInt r = iThread.Create(_L("pherec-capture"), ThreadMain, 0x4000, 0x10000, 2 * KCapBytes, this);
	if (r != KErrNone)
		{
		iState = r;
		return r;
		}
	iThreadOpen = ETrue;
	iThread.Resume();
	RDebug::Printf("[pherec] recording %d fps qp %d", iFps, iQp);
	return KErrNone;
	}

TInt CPheRec::ThreadMain(TAny* aSelf)
	{
	CPheRec* self = (CPheRec*)aSelf;
	const TInt r = self->Loop();
	self->iLock.Wait();
	self->iState = r == KErrNone ? 2 : r;
	self->iLock.Signal();
	RDebug::Printf("[pherec] stopped r=%d frames=%d dups=%d unchanged=%d drops=%d", r, self->iFrames, self->iDups,
		self->iUnchanged, self->iDrops);
	return r;
	}

// ring bytes [aSeq, aSeq + aLen) <- aP (lock held)
void CPheRec::WriteRing(TUint32 aSeq, const TUint8* aP, TInt aLen)
	{
	const TInt at = (TInt)(aSeq & (TUint32)(iRingSize - 1));
	const TInt first = Min(aLen, iRingSize - at);
	Mem::Copy(iRing + at, aP, first);
	if (aLen > first)
		Mem::Copy(iRing, aP + first, aLen - first);
	}

void CPheRec::Append(const TUint8* aHead, TInt aHeadLen, const TUint8* aData, TInt aLen)
	{
	const TInt total = aHeadLen + aLen;
	if (total > iRingSize / 2)
		{
		++iDrops;
		return;
		}
	iLock.Wait();
	// forget the records this one overwrites: a reader behind them jumps to the oldest whole record
	while (iStartCount)
		{
		const TInt tail = (iStartHead - iStartCount + KStarts) % KStarts;
		if ((TInt)(iWrite + total - iStart[tail]) <= iRingSize)
			break;
		--iStartCount;
		}
	if (iStartCount == KStarts)
		--iStartCount;						// (the oldest start falls out of the index: its record is still readable from a later one)
	iStart[iStartHead] = iWrite;
	iStartHead = (iStartHead + 1) % KStarts;
	++iStartCount;
	WriteRing(iWrite, aHead, aHeadLen);
	if (aLen)
		WriteRing(iWrite + aHeadLen, aData, aLen);
	iWrite += total;
	iBytes += total;
	iLock.Signal();
	}

void CPheRec::Read(TUint32 aFrom, TDes8& aOut)
	{
	TUint8 head[8];
	if (!iLockOpen || !iRing)
		{
		Put32(head, (TUint32)iState);
		Put32(head + 4, 0);
		aOut.Append(head, 8);
		return;
		}
	iLock.Wait();
	const TUint32 oldest = iStartCount ? iStart[(iStartHead - iStartCount + KStarts) % KStarts] : iWrite;
	TUint32 from = aFrom;
	if ((TInt)(from - oldest) < 0)
		from = oldest;						// fell behind the ring (or a new recording started): the oldest whole record
	if ((TInt)(iWrite - from) < 0)
		from = iWrite;
	Put32(head, (TUint32)iState);
	Put32(head + 4, from);
	aOut.Append(head, 8);
	const TInt n = Max(0, Min((TInt)(iWrite - from), aOut.MaxLength() - aOut.Length()));
	const TInt at = (TInt)(from & (TUint32)(iRingSize - 1));
	const TInt first = Min(n, iRingSize - at);
	aOut.Append(iRing + at, first);
	if (n > first)
		aOut.Append(iRing, n - first);
	iLock.Signal();
	}

void CPheRec::Stop(TDes8& aText)
	{
	Join();
	const TInt64 n = iFrames ? iFrames : 1;
	const TInt64 idle = iDups + iUnchanged ? iDups + iUnchanged : 1;
	aText.AppendFormat(_L8("state=%d frames=%d idrs=%d dups=%d unchanged=%d drops=%d captures=%d bytes=%Ld wall_ms=%Ld "),
		iState, iFrames, iIdrs, iDups, iUnchanged, iDrops, iCaptures, iBytes, iWallUs / 1000);
	aText.AppendFormat(_L8("cap_us=%Ld hash_stats_us=%Ld csc_us=%Ld enc_us=%Ld enc_busy_us=%Ld max_frame_us=%d (per encoded frame) "),
		iCapUs / n, iHashUs / n, iCscUs / n, iEncUs / n, iBusyUs / n, iMaxFrameUs);
	aText.AppendFormat(_L8("idle_us=%Ld (per picture not encoded) fps=%d qp=%d "), iIdleUs / idle, iFps, iQp);
	aText.Append(iEncMsg.Left(aText.MaxLength() - aText.Length()));
	}

TInt CPheRec::Loop()
	{
	MPheRecEncoder* enc = iNew ? iNew() : NULL;
	if (!enc)
		return KErrNoMemory;
	TInt r = enc->Open(iEncMsg);
	RDebug::Printf("[pherec] %.*s", iEncMsg.Length(), iEncMsg.Ptr());
	if (r == KErrNone)
		r = enc->Setup(KPhCapW, KPhCapH);
	RPhFsCapture cap;
	if (r == KErrNone)
		r = cap.Open();
	HBufC8* buf = r == KErrNone ? HBufC8::New(KCapBytes) : NULL;
	if (r == KErrNone && !buf)
		r = KErrNoMemory;
	if (r != KErrNone)
		{
		cap.Close();
		enc->Close();
		delete enc;
		return r;
		}
	TPtr8 pic = buf->Des();
	const TInt stride = enc->Stride(), codedH = enc->CodedHeight();
	const TInt gop = KGopSeconds * iFps;
	const TInt64 period = 1000000 / iFps;
	TClock clock;
	TUint32 prevHash = 0;
	TBool havePrev = EFalse;
	TInt sinceIdr = gop;
	TInt64 next = clock.Now();
	const TInt64 t0 = next;
	TInt64 lastFull = t0 - KFullEveryUs;
	TUint8 head[KHeadLen];
	while (!iStopReq)
		{
		const TInt64 tCap = clock.Now();
		const TBool full = tCap - lastFull >= KFullEveryUs;
		const TInt changed = cap.Capture(pic, full);
		pic.SetLength(KCapBytes);			// (a partial copy leaves the descriptor shorter; the rest is the last picture)
		const TInt64 tCapDone = clock.Now();
		++iCaptures;
		if (full && changed >= 0)
			lastFull = tCap;
		if (changed < 0)
			{
			if (changed != KErrNotReady)
				{
				r = changed;
				break;
				}							// no display yet / mid mode change: try again next period
			}
		else if (changed == 0 && sinceIdr < gop)
			{								// the kernel saw no post and no bar drawing: the picture is current
			++iUnchanged;
			iIdleUs += tCapDone - tCap;
			}
		else
			{
			// a post can repeat the picture: identical to the previous one = no frame (the next record's time ends it)
			const TUint32* px = (const TUint32*)pic.Ptr();
			TUint32 h = 2166136261u;
			for (TInt i = 0; i < KPhCapW * KPhCapH; ++i)
				h = (h ^ px[i]) * 16777619u;
			if (havePrev && h == prevHash && sinceIdr < gop)
				{
				++iDups;
				iIdleUs += clock.Now() - tCap;
				}
			else
				{
				// mean luma and the share of near-white pixels, over every 2nd pixel of every 2nd row
				TUint32 lumaSum = 0, white = 0;
				TInt samples = 0;
				for (TInt y = 0; y < KPhCapH; y += 2)
					for (TInt x = 0; x < KPhCapW; x += 2)
						{
						const TUint32 p = px[y * KPhCapW + x];
						const TInt luma = (66 * (TInt)((p >> 16) & 255) + 129 * (TInt)((p >> 8) & 255) + 25 * (TInt)(p & 255) + 4224) >> 8;
						lumaSum += luma;
						white += luma >= 230;
						++samples;
						}
				const TInt64 tStats = clock.Now();
				TUint8* y = enc->InputY();
				TUint8* uv = enc->InputUv();
				pb::Csc(pb::EXrgb, px, KPhCapW * 4, KPhCapW, KPhCapH, y, stride, uv, NULL, stride, true);
				pb::PadNv12(y, uv, KPhCapW, KPhCapH, stride, codedH);
				const TInt64 tCsc = clock.Now();
				const TBool idr = sinceIdr >= gop;
				TInt bytes = 0;
				r = enc->Encode(idr, iQp, bytes, iBusyUs);
				const TInt64 tEnc = clock.Now();
				if (r != KErrNone)
					break;
				Head(head, (TUint32)iFrames, tCap - t0, idr ? KFlagIdr : 0, lumaSum / samples, white * 1000 / samples, (TUint32)bytes);
				Append(head, KHeadLen, enc->Stream(), bytes);
				sinceIdr = idr ? 1 : sinceIdr + 1;
				iIdrs += idr;
				++iFrames;
				iCapUs += tCapDone - tCap;
				iHashUs += tStats - tCapDone;
				iCscUs += tCsc - tStats;
				iEncUs += tEnc - tCsc;
				iMaxFrameUs = Max(iMaxFrameUs, (TInt)(tEnc - tCap));
				prevHash = h;
				havePrev = ETrue;
				}
			}
		// the next period; a picture that took longer than one costs the periods it overran (drops)
		next += period;
		const TInt64 now = clock.Now();
		if (now >= next)
			{
			iDrops += (TInt)((now - next) / period);
			next = now + period - (now - next) % period;
			}
		User::AfterHighRes((TInt)(next - now));
		}
	const TInt64 tEnd = clock.Now();
	iWallUs = tEnd - t0;
	Head(head, (TUint32)iFrames, tEnd - t0, KFlagEnd, 0, 0, 0);
	Append(head, KHeadLen, NULL, 0);
	cap.Close();
	enc->Close();
	delete enc;
	delete buf;
	return r;
	}

// ---------------------------------------------------------------- the server
class CPheRecServer : public CServer2
	{
public:
	CPheRecServer() : CServer2(EPriorityStandard) {}
	~CPheRecServer() { delete iRec; }
	void ConstructL() { iRec = new (ELeave) CPheRec; StartL(KPheRecServerName); }
	CSession2* NewSessionL(const TVersion& aVersion, const RMessage2& aMessage) const;
	CPheRec* iRec;
	};

class CPheRecSession : public CSession2
	{
public:
	void ServiceL(const RMessage2& aMessage);
private:
	CPheRec& Rec() { return *((CPheRecServer*)Server())->iRec; }
	};

CSession2* CPheRecServer::NewSessionL(const TVersion& aVersion, const RMessage2& aMessage) const
	{
	if (!User::QueryVersionSupported(TVersion(1, 0, 0), aVersion))
		User::Leave(KErrNotSupported);
	if (aMessage.SecureId() != TSecureId(KPheRecClientSid))	// the screen's contents: the debug link's file service only
		User::Leave(KErrPermissionDenied);
	return new (ELeave) CPheRecSession;
	}

void CPheRecSession::ServiceL(const RMessage2& aMessage)
	{
	switch (aMessage.Function())
		{
	case EPrStart:
		aMessage.Complete(Rec().Start(aMessage.Int0(), aMessage.Int1()));
		return;
	case EPrRead:
	case EPrStop:
		{
		const TInt arg = aMessage.Function() == EPrRead ? 1 : 0;
		const TInt max = aMessage.GetDesMaxLengthL(arg);
		const TInt size = Min(Max(max, 8), KPhFsMaxReply);
		HBufC8* out = HBufC8::NewLC(size);
		TPtr8 p((TUint8*)out->Ptr(), 0, size);	// exactly the client's room: Des() may be the heap cell's rounded-up size
												// (a full ring burst then overflowed the client's descriptor: KErrOverflow)
		if (aMessage.Function() == EPrRead)
			Rec().Read((TUint32)aMessage.Int0(), p);
		else
			Rec().Stop(p);
		aMessage.WriteL(arg, p);
		CleanupStack::PopAndDestroy(out);
		aMessage.Complete(KErrNone);
		return;
		}
		}
	aMessage.Complete(KErrNotSupported);
	}

static void RunServerL()
	{
	CActiveScheduler* s = new (ELeave) CActiveScheduler;
	CleanupStack::PushL(s);
	CActiveScheduler::Install(s);
	CPheRecServer* server = new (ELeave) CPheRecServer;
	CleanupStack::PushL(server);
	server->ConstructL();
	User::RenameThread(KPheRecServerName);
	RProcess::Rendezvous(KErrNone);
	CActiveScheduler::Start();
	CleanupStack::PopAndDestroy(2, s);
	}

TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	TInt r = KErrNoMemory;
	if (cleanup)
		{
		TRAP(r, RunServerL());
		if (r != KErrNone)
			RProcess::Rendezvous(r);
		delete cleanup;
		}
	return r;
	}
