// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// ve_enc.cpp -- OURS: the A64 VE H.264 encoder, one picture per call (ve_enc.h). Moved out of pheencbench/enc_hw.cpp
// (2026-11-05) unchanged in register order so the screen recorder runs what the benchmark measured.
#include <e32base.h>
#include "ve_rphevde.h"
#include "ve_h264enc.h"
#include "ve_enc.h"

using namespace h264enc;

// ME_PARA bits (Bootlin cedrus_regs.h 803-822): the "fast" row; PARA1 bit 19 = temporal-filter history output disable
static const TUint32 KMeWbMvDis = 1u << 16, KMeSplitMbDis = 1u << 15, KMeIntra4Dis = 1u << 14, KMeQpixSplitOff = 1u << 21,
	KMeQpixSmartOff = 1u << 20, KMeImeTimePrio = 1u << 5, KMeQsmtssDis = 1u << 3, KMeQsmtssRefineDis = 1u << 4;
static const TUint32 KMeFast = KMeWbMvDis | KMeSplitMbDis | KMeIntra4Dis | KMeQpixSplitOff | KMeQpixSmartOff | KMeImeTimePrio
	| KMeQsmtssDis | KMeQsmtssRefineDis;
static const TUint32 KPara1TfHisOutDis = 1u << 19;

static const TUint32 KModeBase = 0x00130000u, KModeEnc = 1u << 7, KModeIsp = 1u << 6, KResetEncoder = 1u << 24;
static const TInt KVeReset = 0x004;

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

static TUint32 PllFor(TUint32 aOrig, TInt aN, TInt aM)
	{
	return (aOrig & ~((1u << 31) | (1u << 28) | 0x7F0Fu)) | ((TUint32)(aN - 1) << 8) | (TUint32)(aM - 1);
	}

TVeEnc::TVeEnc()
	: iPhys(0), iArenaSize(0), iOrigPll(0), iMhz(0), iOpen(EFalse), iW(0), iH(0), iAw(0), iAh(0), iMbW(0), iMbH(0), iRecLuma(0),
	  iLineBus(0), iFrameBus(0), iCur(0), iFrameNum(0), iModeSet(EFalse)
	{
	}

TInt TVeEnc::Open(TInt aMaxMb, TDes8& aMsg)
	{
	TInt r = iVe.Open();
	if (r != KErrNone)
		{
		aMsg.Format(_L8("VE open r=%d"), r);
		return r;
		}
	TPheVdeInfo info;
	iVe.GetInfo(info);
	iOrigPll = info.iPllVe;
	r = iVe.OpenArena(iArena);
	TInt rc = r;
	if (r == KErrNone)
		{
		rc = KErrNoMemory;
		for (TInt mb = aMaxMb; mb >= 8 && rc != KErrNone; mb -= 8)
			{
			iArenaSize = mb << 20;
			rc = iVe.Commit(0, iArenaSize, iPhys);
			}
		}
	if (rc != KErrNone)
		{
		aMsg.Format(_L8("VE arena r=%d commit r=%d"), r, rc);
		iArena.Close();
		iVe.Close();
		return rc ? rc : KErrGeneral;
		}
	iMhz = 24 * (((iOrigPll >> 8) & 0x7F) + 1) / ((iOrigPll & 0xF) + 1);
	TUint32 now = 0;
	const TInt sr = iVe.SetPll(PllFor(iOrigPll, 67, 4) | (1u << 31), now);		// 24 * 67 / 4 = 402 MHz (Linux's rate)
	if (sr == KErrNone)
		iMhz = 402;
	aMsg.Format(_L8("VE ok version=%08x arena %d MiB, clock %d MHz (SetPll r=%d)"), info.iVersion, iArenaSize >> 20, iMhz, sr);
	iOpen = ETrue;
	iModeSet = EFalse;
	return KErrNone;
	}

void TVeEnc::Close()
	{
	if (!iOpen)
		return;
	if (iOrigPll)
		{
		TUint32 now = 0;
		iVe.SetPll(iOrigPll | (1u << 31), now);
		}
	iArena.Close();
	iVe.Close();
	iOpen = EFalse;
	}

TInt TVeEnc::Setup(TInt aW, TInt aH, TInt aInputs)
	{
	if (!iOpen || aInputs < 1 || aInputs > KMaxInputs || aW <= 0 || aH <= 0 || (aW & 1) || (aH & 1))
		return KErrArgument;
	iW = aW;
	iH = aH;
	iAw = Align(aW, 16);
	iAh = Align(aH, 16);
	iMbW = iAw / 16;
	iMbH = iAh / 16;
	TVeArenaMem mem(iArena, iPhys, iArenaSize, KEncOff);
	iRecLuma = Align(iMbW, 2) * 16 * Align(iMbH + 1, 4) * 16;
	const TInt recChroma = Align(iMbW, 2) * 16 * Align((iMbH + 1) / 2, 4) * 16;
	const TInt recSize = Align(iRecLuma + recChroma, 4096);
	const TInt subW = ((iMbW + 47) * 2 / 3 & ~31) + Align(iMbW, 32) * 2, subH = (iMbH * 16 + 72) / 8;
	const TInt subSize = Align(Max(subW * subH, iRecLuma / 4), 4096);
	const TInt mbInfoSize = ((iMbW + 31) / 32) * 4096;
	const TInt frameBufSize = Align(Align(iMbW, 4) * iMbH * 8, 4096);
	TInt idx;
	for (TInt i = 0; i < 2; ++i)
		{
		iRec[i] = mem.Alloc(recSize, 4096, idx);
		iSub[i] = mem.Alloc(subSize, 4096, idx);
		}
	iLineBus = mem.Alloc(mbInfoSize, 4096, idx);
	iFrameBus = mem.Alloc(frameBufSize, 4096, idx);
	if (!iRec[0] || !iRec[1] || !iSub[0] || !iSub[1] || !iLineBus || !iFrameBus)
		return KErrNoMemory;
	for (TInt i = 0; i < 2; ++i)
		{
		Mem::FillZ(mem.Cpu(iRec[i]), recSize);
		Mem::FillZ(mem.Cpu(iSub[i]), subSize);
		}
	Mem::FillZ(mem.Cpu(iLineBus), mbInfoSize);
	Mem::FillZ(mem.Cpu(iFrameBus), frameBufSize);
	for (TInt k = 0; k < aInputs; ++k)
		{
		iIn[k] = mem.Alloc(iAw * iAh * 3 / 2, 4096, idx);
		if (!iIn[k])
			return KErrNoMemory;
		}
	Mem::FillZ(iArena.Base(), KBitstreamSize);
	iCur = 0;
	iFrameNum = 0;
	EnterEncoderMode(iVe);
	iModeSet = ETrue;
	return KErrNone;
	}

TInt TVeEnc::Encode(TInt aSlot, TBool aIdr, TInt aQp, TInt aPpsQp, TInt& aBytes, TUint32& aWaitTicks)
	{
	aBytes = 0;
	if (!iModeSet)
		return KErrNotReady;
	static const TUint16 KZero[] = { 0xB0C, 0xB28, 0xB2C, 0xB30, 0xB34, 0xB38, 0xB3C, 0xB48, 0xB4C, 0xB50, 0xB5C, 0xBC4 };
	RPheVde& ve = iVe;
	TVeJobAdapter job(ve);
	THwBits sink(ve);
	const TInt div48 = (iMbW + 47) / 48;
	if (aIdr)
		iFrameNum = 0;
	const TInt cur = iCur, ref = cur ^ 1;
	ve.WriteReg(KAvcStatus, 0x7);
	ve.WriteReg(KAvcTrigger, 0);
	ve.WriteReg(0xB90, 0);
	ve.WriteReg(0xB54, 0);
	ve.WriteReg(0xB58, 0);
	ve.WriteReg(KAvcVleOffset, 0);
	ve.WriteReg(KAvcVleAddr, iPhys - KDramBus);
	ve.WriteReg(KAvcVleEnd, iPhys - KDramBus + KBitstreamSize - 1);
	ve.WriteReg(KAvcVleMax, (TUint32)KBitstreamSize * 8);
	THeaders hdr(sink, iMbW, iMbH, aPpsQp);
	hdr.SetCrop(iAw - iW, iAh - iH);
	if (aIdr)
		{
		hdr.Sps();
		hdr.Pps();
		}
	hdr.SliceHeader(aIdr, iFrameNum, aQp - aPpsQp, 0);
	ve.WriteReg(KIspStride, (TUint32)iMbW << 16);
	ve.WriteReg(KIspSize, ((TUint32)iMbW << 16) | iMbH);
	ve.WriteReg(KIspCtrl, 0);
	ve.WriteReg(KIspLuma, iIn[aSlot]);
	ve.WriteReg(KIspChroma, iIn[aSlot] + iAw * iAh);
	ve.WriteReg(KAvcMbInfo, iLineBus);
	ve.WriteReg(KAvcUnkBuf, iFrameBus);
	for (TUint i = 0; i < sizeof(KZero) / sizeof(KZero[0]); ++i)
		ve.WriteReg(KZero[i], 0);
	ve.WriteReg(KAvcRecLuma, iRec[cur]);
	ve.WriteReg(KAvcRecChroma, iRec[cur] + iRecLuma);
	const TInt refSet = aIdr ? cur : ref;
	ve.WriteReg(KAvcRefLuma, iRec[refSet]);
	ve.WriteReg(KAvcRefChroma, iRec[refSet] + iRecLuma);
	ve.WriteReg(KAvcRefSluma, iSub[refSet]);
	ve.WriteReg(KAvcRecSluma, iSub[cur]);
	TUint32 avcCtrl = 0;
	ve.ReadReg(KAvcCtrl, avcCtrl);
	job.Begin();
	job.Write(KAvcCtrl, avcCtrl | 0xF);
	job.Write(KAvcStatus, 0x7);
	job.Write(KAvcParam, aIdr ? 0u : 0x10u);
	job.Write(KAvcQp, (4u << 16) | ((TUint32)div48 << 10) | (TUint32)aQp | KPara1TfHisOutDis);
	job.Write(KAvcMotionEst, KMeFast);
	job.Write(KAvcTrigger, 0x8);
	const TUint32 tw0 = User::FastCounter();
	const TInt rr = job.Run(KAvcStatus, 0x7, KAvcCtrl, 0xF);
	aWaitTicks += User::FastCounter() - tw0;
	TUint32 vle = 0;
	ve.ReadReg(KAvcVleLength, vle);
	const TInt bytes = (TInt)((vle + 7) / 8);
	if (!(rr >= 0 && (rr & 3) == 1) || bytes <= 0)
		return rr < 0 ? rr : KErrGeneral;
	if (bytes > KBitstreamSize)
		return KErrOverflow;
	aBytes = bytes;
	iCur ^= 1;
	++iFrameNum;
	return KErrNone;
	}

void TVeEnc::Idle()
	{
	if (iOpen)
		iVe.WriteReg(KVeCtrl, 0x7);
	iModeSet = EFalse;
	}
