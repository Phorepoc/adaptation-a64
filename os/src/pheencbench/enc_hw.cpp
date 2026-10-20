// enc_hw.cpp -- OURS (pheencbench): the A64 Video Engine H.264 encoder leg. The register sequence is src/phevenctest2 (rung 2, boot 686+): Bootlin
// mode enable, headers through the hardware bit writer, one job per frame (IDR then P, the "fast+tf" motion-estimation row = the parked best
// setting of ve-encoder-plan.md section 15) at 402 MHz. Inputs are NV12 frames staged in the uncached arena before the timed loop (a capture
// path would write them there directly). The streams are decoded back with OUR VE decoder for the PSNR column.
#include <e32base.h>
#include <e32debug.h>
#include <e32math.h>
#include <hal.h>
#include <stdio.h>

#include "ve_rphevde.h"
#include "ve_h264enc.h"
#include "bench_common.h"
#include "enc_hw.h"

using namespace h264enc;

namespace pb {

const TInt KBitstreamSize = 1 << 20;		// arena offset 0: the encoder's output window (and the decoder's NAL copy)
const TInt KScratchOff = KBitstreamSize;	// CSC destination test region (Scratch())
const TInt KScratchSize = 3 << 20;
const TInt KEncOff = KScratchOff + KScratchSize;	// encoder / decoder allocations start here
const TInt KFixedQp = 28;

// ME_PARA bits (Bootlin cedrus_regs.h 803-822): the "fast" row; PARA1 bit 19 = temporal-filter history output disable
const TUint32 KMeWbMvDis = 1u << 16, KMeSplitMbDis = 1u << 15, KMeIntra4Dis = 1u << 14, KMeQpixSplitOff = 1u << 21, KMeQpixSmartOff = 1u << 20,
	KMeImeTimePrio = 1u << 5, KMeQsmtssDis = 1u << 3, KMeQsmtssRefineDis = 1u << 4;
const TUint32 KMeFast = KMeWbMvDis | KMeSplitMbDis | KMeIntra4Dis | KMeQpixSplitOff | KMeQpixSmartOff | KMeImeTimePrio | KMeQsmtssDis | KMeQsmtssRefineDis;
const TUint32 KPara1TfHisOutDis = 1u << 19;

const TUint32 KModeBase = 0x00130000u, KModeEnc = 1u << 7, KModeIsp = 1u << 6, KResetEncoder = 1u << 24;
const TInt KVeReset = 0x004;

static TInt Align(TInt a, TInt n) { return (a + n - 1) & ~(n - 1); }

class THwBits : public MBitSink
	{
public:
	THwBits(RPheVde& aVe) : iVe(aVe) {}
	void Put(uint32_t aValue, int aBits)
		{
		TUint32 st = 0;
		for (TInt i = 0; i < 1000; ++i)			// wait for STATUS bit 9 (PUT_BITS_READY)
			{
			iVe.ReadReg(KAvcStatus, st);
			if (st & (1u << 9))
				break;
			}
		iVe.WriteReg(KAvcBasicBits, aValue);
		iVe.WriteReg(KAvcTrigger, 0x1 | (aBits << 8));
		}
	void NoEmulationPrevention(bool aOff) { iVe.WriteReg(KAvcParam, aOff ? 0x80000000u : 0u); }
	RPheVde& iVe;
	};

static void EnterEncoderMode(RPheVde& aVe)
	{
	TUint32 v = 0;
	aVe.ReadReg(KVeCtrl, v);
	aVe.WriteReg(KVeCtrl, (((v & ~0x00370000u) | KModeBase) & ~(KModeEnc | KModeIsp)) | 7);
	aVe.ReadReg(KVeReset, v);
	aVe.WriteReg(KVeReset, v | KResetEncoder);
	aVe.ReadReg(KVeReset, v);
	aVe.WriteReg(KVeReset, v & ~KResetEncoder);
	aVe.ReadReg(KVeCtrl, v);
	aVe.WriteReg(KVeCtrl, v | KModeEnc | KModeIsp | 7);
	}

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
	RPheVde iVe;
	RChunk iArena;
	TUint32 iPhys;
	TInt iArenaSize;
	TUint32 iOrigPll;
	};

THw::THw() : iImpl(new THwImpl), iOk(EFalse), iMhz(0)
	{
	iImpl->iPhys = 0;
	iImpl->iArenaSize = 0;
	iImpl->iOrigPll = 0;
	}

THw::~THw()
	{
	Close();
	delete iImpl;
	}

static TUint32 PllFor(TUint32 aOrig, TInt aN, TInt aM)
	{
	return (aOrig & ~((1u << 31) | (1u << 28) | 0x7F0Fu)) | ((TUint32)(aN - 1) << 8) | (TUint32)(aM - 1);
	}

int THw::Open(char* aMsg, int aMsgLen)
	{
	TInt r = iImpl->iVe.Open();
	if (r != KErrNone)
		{
		snprintf(aMsg, aMsgLen, "VE open r=%d", r);
		return r;
		}
	TPheVdeInfo info;
	iImpl->iVe.GetInfo(info);
	iImpl->iOrigPll = info.iPllVe;
	r = iImpl->iVe.OpenArena(iImpl->iArena);
	TInt rc = r;
	if (r == KErrNone)
		{
		rc = KErrNoMemory;
		for (TInt mb = 64; mb >= 32 && rc != KErrNone; mb -= 16)
			{
			iImpl->iArenaSize = mb << 20;
			rc = iImpl->iVe.Commit(0, iImpl->iArenaSize, iImpl->iPhys);
			}
		}
	if (rc != KErrNone)
		{
		snprintf(aMsg, aMsgLen, "VE arena r=%d commit r=%d", r, rc);
		iImpl->iVe.Close();
		return rc ? rc : -1;
		}
	iMhz = 24 * (((iImpl->iOrigPll >> 8) & 0x7F) + 1) / ((iImpl->iOrigPll & 0xF) + 1);
	TUint32 now = 0;
	const TInt sr = iImpl->iVe.SetPll(PllFor(iImpl->iOrigPll, 67, 4) | (1u << 31), now);		// 24 * 67 / 4 = 402 MHz (Linux's rate)
	if (sr == KErrNone)
		iMhz = 402;
	snprintf(aMsg, aMsgLen, "VE ok version=%08x arena %d MiB, clock %d MHz (SetPll r=%d)", (unsigned)info.iVersion, iImpl->iArenaSize >> 20, iMhz, sr);
	iOk = ETrue;
	return KErrNone;
	}

void THw::Close()
	{
	if (!iOk)
		return;
	if (iImpl->iOrigPll)
		{
		TUint32 now = 0;
		iImpl->iVe.SetPll(iImpl->iOrigPll | (1u << 31), now);
		}
	iImpl->iArena.Close();
	iImpl->iVe.Close();
	iOk = EFalse;
	}

uint8_t* THw::Scratch() { return iImpl->iArena.Base() + KScratchOff; }
int THw::ScratchSize() { return KScratchSize; }

void THw::Encode(int aW, int aH, int aQp, int aKbps, int aFrames, int aMaxSeconds, const uint8_t* const* aRing, uint8_t* aStream, int aStreamCap,
	int* aOff, TRunResult& aR)
	{
	Mem::FillZ(&aR, sizeof(aR));
	aR.iQpUsed = aQp;
	RPheVde& ve = iImpl->iVe;
	const TInt aw = Align(aW, 16), ah = Align(aH, 16), mbw = aw / 16, mbh = ah / 16;
	TVeArenaMem mem(iImpl->iArena, iImpl->iPhys, iImpl->iArenaSize, KEncOff);
	const TInt recLuma = Align(mbw, 2) * 16 * Align(mbh + 1, 4) * 16;
	const TInt recChroma = Align(mbw, 2) * 16 * Align((mbh + 1) / 2, 4) * 16;
	const TInt recSize = Align(recLuma + recChroma, 4096);
	const TInt subW = ((mbw + 47) * 2 / 3 & ~31) + Align(mbw, 32) * 2, subH = (mbh * 16 + 72) / 8;
	const TInt subSize = Align(Max(subW * subH, recLuma / 4), 4096);
	const TInt mbInfoSize = ((mbw + 31) / 32) * 4096;
	const TInt frameBufSize = Align(Align(mbw, 4) * mbh * 8, 4096);
	TInt idx;
	TUint32 rec[2], sub[2], in[KRing];
	for (TInt i = 0; i < 2; ++i)
		{
		rec[i] = mem.Alloc(recSize, 4096, idx);
		sub[i] = mem.Alloc(subSize, 4096, idx);
		}
	const TUint32 lineBus = mem.Alloc(mbInfoSize, 4096, idx);
	const TUint32 frameBus = mem.Alloc(frameBufSize, 4096, idx);
	if (!rec[0] || !rec[1] || !sub[0] || !sub[1] || !lineBus || !frameBus)
		{
		aR.iErr = KErrNoMemory;
		return;
		}
	for (TInt i = 0; i < 2; ++i)
		{
		Mem::FillZ(mem.Cpu(rec[i]), recSize);
		Mem::FillZ(mem.Cpu(sub[i]), subSize);
		}
	Mem::FillZ(mem.Cpu(lineBus), mbInfoSize);
	Mem::FillZ(mem.Cpu(frameBus), frameBufSize);
	for (TInt k = 0; k < KRing; ++k)
		{
		in[k] = mem.Alloc(aw * ah * 3 / 2, 4096, idx);
		if (!in[k])
			{
			aR.iErr = KErrNoMemory;
			return;
			}
		Mem::Copy(mem.Cpu(in[k]), aRing[k], aw * ah * 3 / 2);		// NV12, stride aw, MB-padded
		}
	Mem::FillZ(iImpl->iArena.Base(), KBitstreamSize);

	EnterEncoderMode(ve);
	static const TUint16 KZero[] = { 0xB0C, 0xB28, 0xB2C, 0xB30, 0xB34, 0xB38, 0xB3C, 0xB48, 0xB4C, 0xB50, 0xB5C, 0xBC4 };
	TVeJobAdapter job(ve);
	THwBits sink(ve);
	const TInt div48 = (mbw + 47) / 48;
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
		const TInt cur = n & 1, ref = cur ^ 1;
		ve.WriteReg(KAvcStatus, 0x7);
		ve.WriteReg(KAvcTrigger, 0);
		ve.WriteReg(0xB90, 0);
		ve.WriteReg(0xB54, 0);
		ve.WriteReg(0xB58, 0);
		ve.WriteReg(KAvcVleOffset, 0);
		ve.WriteReg(KAvcVleAddr, iImpl->iPhys - KDramBus);
		ve.WriteReg(KAvcVleEnd, iImpl->iPhys - KDramBus + KBitstreamSize - 1);
		ve.WriteReg(KAvcVleMax, (TUint32)KBitstreamSize * 8);
		THeaders hdr(sink, mbw, mbh, ppsQp);
		if (idr)
			{
			hdr.Sps();
			hdr.Pps();
			}
		hdr.SliceHeader(idr, n, qp - ppsQp, 0);
		ve.WriteReg(KIspStride, (TUint32)mbw << 16);
		ve.WriteReg(KIspSize, ((TUint32)mbw << 16) | mbh);
		ve.WriteReg(KIspCtrl, 0);
		ve.WriteReg(KIspLuma, in[n % KRing]);
		ve.WriteReg(KIspChroma, in[n % KRing] + aw * ah);
		ve.WriteReg(KAvcMbInfo, lineBus);
		ve.WriteReg(KAvcUnkBuf, frameBus);
		for (TUint i = 0; i < sizeof(KZero) / sizeof(KZero[0]); ++i)
			ve.WriteReg(KZero[i], 0);
		ve.WriteReg(KAvcRecLuma, rec[cur]);
		ve.WriteReg(KAvcRecChroma, rec[cur] + recLuma);
		const TInt refSet = idr ? cur : ref;
		ve.WriteReg(KAvcRefLuma, rec[refSet]);
		ve.WriteReg(KAvcRefChroma, rec[refSet] + recLuma);
		ve.WriteReg(KAvcRefSluma, sub[refSet]);
		ve.WriteReg(KAvcRecSluma, sub[cur]);
		TUint32 avcCtrl = 0;
		ve.ReadReg(KAvcCtrl, avcCtrl);
		job.Begin();
		job.Write(KAvcCtrl, avcCtrl | 0xF);
		job.Write(KAvcStatus, 0x7);
		job.Write(KAvcParam, idr ? 0u : 0x10u);
		job.Write(KAvcQp, (4u << 16) | ((TUint32)div48 << 10) | (TUint32)qp | KPara1TfHisOutDis);
		job.Write(KAvcMotionEst, KMeFast);
		job.Write(KAvcTrigger, 0x8);
		const TUint32 tw0 = ph_ticks();
		const TInt rr = job.Run(KAvcStatus, 0x7, KAvcCtrl, 0xF);
		waitTicks += ph_ticks() - tw0;
		TUint32 vle = 0;
		ve.ReadReg(KAvcVleLength, vle);
		const TInt bytes = (TInt)((vle + 7) / 8);
		if (!(rr >= 0 && (rr & 3) == 1) || bytes <= 0 || total + bytes > aStreamCap)
			{
			err = bytes > 0 && total + bytes > aStreamCap ? 2 : 1000 + (rr & 0xFFF);
			break;
			}
		Mem::Copy(aStream + total, iImpl->iArena.Base(), bytes);
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
	ve.WriteReg(KVeCtrl, 0x7);
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
	RPheVde& ve = iImpl->iVe;
	// the VE decoder takes a NEW arena buffer per picture (never reused): boot 873 ran out at 37 pictures of 720x1440 / 41 of 1280x720 (-3).
	// Decode only as many frames as fit; the P chain of a prefix is valid.
	{
	const TInt aw = Align(aW, 16), ah = Align(aH, 16);
	const TInt per = aw * ah * 3 / 2 + aw * ah / 8 + 8192;
	const TInt cap = Max(2, (iImpl->iArenaSize - KEncOff) / per - 3);
	if (aFrames > cap)
		aFrames = cap;
	}
	TVeArenaMem mem(iImpl->iArena, iImpl->iPhys, iImpl->iArenaSize, KEncOff);
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
					Mem::Copy(iImpl->iArena.Base(), nal, e - s);
					Mem::FillZ(iImpl->iArena.Base() + (e - s), 64);
					rr = dec->DecodeSlice(nal, e - s, iImpl->iPhys - KDramBus);
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
