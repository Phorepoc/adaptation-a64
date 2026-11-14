// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// enc_hw.cpp -- OURS (pheencbench): the A64 Video Engine H.264 encoder leg. The encoder itself is src/pherecenc/ve_enc.cpp
// (also the screen recorder's plug-in)
// (TVeEnc: the phevenctest2 register sequence, the "fast+tf" motion-estimation row at 402 MHz), shared with the phfs screen
// recorder since 2026-11-05. Inputs are NV12 frames staged in the uncached arena before the timed loop. The streams are
// decoded back with OUR VE decoder for the PSNR column.
#include <e32base.h>
#include <e32debug.h>
#include <e32math.h>
#include <hal.h>
#include <stdio.h>

#include "ve_rphevde.h"
#include "ve_h264enc.h"
#include "ve_enc.h"
#include "bench_common.h"
#include "enc_hw.h"

using h264enc::KVeCtrl;

namespace pb {

const TInt KFixedQp = 28;

static TInt Align(TInt a, TInt n) { return (a + n - 1) & ~(n - 1); }

// H.264 bits halve per +6 QP: qp += 6 * log2(bits / target) = 19.93 * log10(ratio)
static TInt RcAdjust(TInt aQp, TInt aBits, TInt aTarget)
	{
	if (aTarget <= 0 || aBits <= 0)
		return aQp;
	TReal r = (TReal)aBits / aTarget, l = 0;
	Math::Log(l, r);
	const TInt dq = l >= 0 ? (TInt)(l * 19.93 + 0.5) : -(TInt)(-l * 19.93 + 0.5);
	return Max(10, Min(46, aQp + Max(-4, Min(6, dq))));
	}

struct THwImpl
	{
	TVeEnc iEnc;
	};

THw::THw() : iImpl(new THwImpl), iOk(EFalse), iMhz(0)
	{
	}

THw::~THw()
	{
	Close();
	delete iImpl;
	}

int THw::Open(char* aMsg, int aMsgLen)
	{
	TBuf8<128> msg;
	const TInt r = iImpl->iEnc.Open(64, msg);
	snprintf(aMsg, aMsgLen, "%.*s", msg.Length(), (const char*)msg.Ptr());
	if (r != KErrNone)
		return r;
	iMhz = iImpl->iEnc.Mhz();
	iOk = ETrue;
	return KErrNone;
	}

void THw::Close()
	{
	if (!iOk)
		return;
	iImpl->iEnc.Close();
	iOk = EFalse;
	}

uint8_t* THw::Scratch() { return iImpl->iEnc.Scratch(); }
int THw::ScratchSize() { return TVeEnc::KScratchSize; }

void THw::Encode(int aW, int aH, int aQp, int aKbps, int aFrames, int aMaxSeconds, const uint8_t* const* aRing, uint8_t* aStream, int aStreamCap,
	int* aOff, TRunResult& aR)
	{
	Mem::FillZ(&aR, sizeof(aR));
	aR.iQpUsed = aQp;
	TVeEnc& enc = iImpl->iEnc;
	const TInt r0 = enc.Setup(aW, aH, KRing);
	if (r0 != KErrNone)
		{
		aR.iErr = r0;
		return;
		}
	const TInt aw = enc.CodedW(), ah = enc.CodedH();
	for (TInt k = 0; k < KRing; ++k)
		Mem::Copy(enc.InputY(k), aRing[k], aw * ah * 3 / 2);		// NV12, stride aw, MB-padded
	const TBool fixed = aQp > 0;
	const TInt targetP = fixed ? 0 : aKbps * 1000 / 30;
	const TInt ppsQp = fixed ? aQp : 30;
	TInt qpP = ppsQp;
	TInt err = KErrNone, total = 0;
	TInt64 maxUs = 0;
	TUint32 waitTicks = 0;			// time blocked on the VE job interrupt = CPU asleep
	aOff[0] = 0;
	TInt n = 0;
	const TUint32 tStart = ph_ticks();
	TUint32 tPrev = tStart;
	for (; n < aFrames; ++n)
		{
		const TBool idr = n == 0;
		const TInt qp = idr ? ppsQp : (fixed ? aQp : qpP);
		TInt bytes = 0;
		const TInt rr = enc.Encode(n % KRing, idr, qp, ppsQp, bytes, waitTicks);
		if (rr != KErrNone || total + bytes > aStreamCap)
			{
			err = rr == KErrNone ? 2 : 1000 + ((-rr) & 0xFFF);
			break;
			}
		Mem::Copy(aStream + total, enc.Stream(), bytes);
		total += bytes;
		aOff[n + 1] = total;
		if (idr)
			aR.iFirstBytes = bytes;
		else if (!fixed)
			qpP = RcAdjust(qp, bytes * 8, targetP);
		const TUint32 tn = ph_ticks();
		const TInt64 us = UsOf(tn - tPrev);
		if (us > maxUs)
			maxUs = us;
		tPrev = tn;
		if (n > 3 && UsOf(tn - tStart) > (TInt64)aMaxSeconds * 1000000)
			{
			++n;
			break;
			}
		}
	const TUint32 totalTicks = ph_ticks() - tStart;
	aR.iUs = UsOf(totalTicks);
	aR.iCpuPct = totalTicks ? (TInt)((TInt64)(totalTicks - waitTicks) * 100 / totalTicks) : -1;
	aR.iFrames = n;
	aR.iBytes = total;
	aR.iMaxFrameUs = maxUs;
	aR.iErr = err;
	enc.Idle();
	}

// ---------------------------------------------------------------- decode back + PSNR of every aStep-th frame
struct TDecCtx
	{
	const uint8_t* const* iRef;
	TInt iRefStride, iW, iH, iStep, iCount, iSampled, iSum, iMin;
	h264::TVeH264* iDec;
	TVeArenaMem* iMem;
	};

static void OnPicture(void* aCtx, const h264::TPicture& aPic)
	{
	TDecCtx& d = *(TDecCtx*)aCtx;
	const TInt i = d.iCount++;
	if (i % d.iStep)
		return;
	const TInt p = PsnrY(d.iMem->Cpu(aPic.iLumaBus), d.iDec->iStride, d.iRef[i % KRing], d.iRefStride, d.iW, d.iH);
	d.iSum += p;
	d.iMin = Min(d.iMin, p);
	++d.iSampled;
	}

void THw::DecodePsnr(const uint8_t* aStream, const int* aOff, int aFrames, int aW, int aH, const uint8_t* const* aRef, int aRefStride, int aStep,
	TDecodeResult& aD)
	{
	Mem::FillZ(&aD, sizeof(aD));
	aD.iMinY = 0;
	aD.iAvgY = 0;
	TVeEnc& enc = iImpl->iEnc;
	RPheVde& ve = enc.Ve();
	const TInt KEncOff = TVeEnc::KEncOff, KBitstreamSize = TVeEnc::KBitstreamSize;
	// the VE decoder takes a NEW arena buffer per picture (never reused): boot 873 ran out at 37 pictures of 720x1440 / 41 of 1280x720 (-3).
	// Decode only as many frames as fit; the P chain of a prefix is valid.
	{
	const TInt aw = Align(aW, 16), ah = Align(aH, 16);
	const TInt per = aw * ah * 3 / 2 + aw * ah / 8 + 8192;
	const TInt cap = Max(2, (enc.ArenaSize() - KEncOff) / per - 3);
	if (aFrames > cap)
		aFrames = cap;
	}
	TVeArenaMem mem(enc.Arena(), enc.Phys(), enc.ArenaSize(), KEncOff);
	h264::TParser* parser = new h264::TParser;
	TVeJobAdapter* job = new TVeJobAdapter(ve);
	TDecCtx* ctx = new TDecCtx;
	h264::TVeH264* dec = parser && job && ctx ? new h264::TVeH264(*parser, *job, mem) : NULL;
	if (!dec)
		{
		aD.iErr = KErrNoMemory;
		return;
		}
	ctx->iRef = aRef;
	ctx->iRefStride = aRefStride;
	ctx->iW = aW;
	ctx->iH = aH;
	ctx->iStep = aStep;
	ctx->iCount = ctx->iSampled = ctx->iSum = 0;
	ctx->iMin = 99999;
	ctx->iDec = dec;
	ctx->iMem = &mem;
	dec->iOnPicture = OnPicture;
	dec->iCtx = ctx;
	TInt err = 0;
	for (TInt k = 0; k < aFrames && !err; ++k)
		{
		const TUint8* s0 = aStream + aOff[k];
		const TInt len = aOff[k + 1] - aOff[k];
		size_t pos = 0, s = 0, e = 0;
		while (h264::NextNal(s0, len, pos, s, e))
			{
			const TUint8* nal = s0 + s;
			const TInt type = nal[0] & 0x1F;
			TInt rr = 0;
			if (type == h264::ENalSps)
				rr = parser->ParseSps(nal, e - s) ? 0 : -100;
			else if (type == h264::ENalPps)
				rr = parser->ParsePps(nal, e - s) ? 0 : -101;
			else if (type == h264::ENalSlice || type == h264::ENalIdr)
				{
				if ((TInt)(e - s) > KBitstreamSize - 128)
					rr = -102;
				else
					{
					Mem::Copy(enc.Arena().Base(), nal, e - s);
					Mem::FillZ(enc.Arena().Base() + (e - s), 64);
					rr = dec->DecodeSlice(nal, e - s, enc.Phys() - KDramBus);
					}
				}
			if (rr < 0)
				{
				err = rr;
				break;
				}
			}
		}
	dec->Flush();
	aD.iErr = err;
	aD.iPictures = ctx->iCount;
	aD.iSampled = ctx->iSampled;
	aD.iAvgY = ctx->iSampled ? ctx->iSum / ctx->iSampled : 0;
	aD.iMinY = ctx->iSampled ? ctx->iMin : 0;
	ve.WriteReg(KVeCtrl, 0x7);
	delete dec;
	delete ctx;
	delete job;
	delete parser;
	}

}	// namespace pb
