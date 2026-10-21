// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// ve_h264.cpp -- OURS (VDU rung V3): see ve_h264.h. Section numbers: ITU-T H.264 (08/2021); register facts:
#include "ve_h264.h"
#include <string.h>

namespace h264
{

// VE registers (offsets in the 4 KB window)
enum
	{
	KVeMode = 0x000, KVeBufCtrl = 0x050, KVeChromaBufLen = 0x0C4, KVeStride = 0x0C8, KVeOutFmt = 0x0EC,
	KSps = 0x200, KPps = 0x204, KShs = 0x208, KShs2 = 0x20C, KShsWp = 0x210, KShsQp = 0x21C, KCtrl = 0x220,
	KTrigger = 0x224, KStatus = 0x228, KVldAddr = 0x230, KVldOffset = 0x234, KVldLen = 0x238, KVldEnd = 0x23C,
	KSdRot = 0x240, KOutIdx = 0x24C, KExtra1 = 0x250, KExtra2 = 0x254, KSramOff = 0x2E0, KSramData = 0x2E4
	};
enum { KSramWeights = 0x000, KSramFrames = 0x100, KSramList0 = 0x190, KSramList1 = 0x199, KSram8x8a = 0x200,
	KSram8x8b = 0x210, KSram4x4 = 0x220 };

TVeH264::TVeH264(TParser& aParser, MVeJob& aJob, MVeMemory& aMem)
	: iOnPicture(NULL), iCtx(NULL), iSlices(0), iPictures(0), iJobErrors(0), iLastStatus(0), iWidth(0), iHeight(0),
	iStride(0), iAlignedHeight(0), iParser(aParser), iJob(aJob), iMem(aMem), iDpbCount(0), iInPicture(false), iHavePend(false),
	iPrevPocMsb(0), iPrevPocLsb(0), iPrevFrameNum(0), iPrevFrameNumOffset(0), iIdrPeriod(0), iPicInfoBus(0),
	iNeighborBus(0)
	{
	memset(iDpb, 0, sizeof(iDpb));
	memset(&iCur, 0, sizeof(iCur));
	memset(&iPend, 0, sizeof(iPend));
	}

int TVeH264::FreeSlot() const
	{
	for (int s = 0; s < KSlots; ++s)
		{
		bool used = false;
		for (uint32_t i = 0; i < iDpbCount; ++i)
			used = used || iDpb[i].iSlot == s;
		if (!used)
			return s;
		}
	return -1;
	}

static inline int Par(const TSliceHeader& aSh) { return aSh.iFieldPic ? (aSh.iBottomField ? 1 : 0) : 2; }

static inline int32_t WrapOf(const TPicture& aP, uint32_t aFrameNum, uint32_t aMaxFn)
	{
	return aP.iFrameNum > aFrameNum ? (int32_t)aP.iFrameNum - (int32_t)aMaxFn : (int32_t)aP.iFrameNum;
	}

// iRef / iLongTerm follow the per-field marking
static void Sync(TPicture& aP)
	{
	aP.iRef = aP.iFieldRef[0] || aP.iFieldRef[1];
	aP.iLongTerm = aP.iFieldRef[0] == 2 || aP.iFieldRef[1] == 2;
	}

// PicOrderCnt of the picture as far as it is decoded (8.2.1: a frame or complementary pair = the smaller, a field = its own)
static void RecalcPoc(TPicture& aP)
	{
	aP.iPoc = aP.iFields == 1 ? aP.iTopPoc : aP.iFields == 2 ? aP.iBottomPoc :
		(aP.iTopPoc < aP.iBottomPoc ? aP.iTopPoc : aP.iBottomPoc);
	}

int TVeH264::FindEntry(uint32_t aLumaBus) const
	{
	for (uint32_t i = 0; i < iDpbCount; ++i)
		if (iDpb[i].iLumaBus == aLumaBus)
			return (int)i;
	return -1;
	}

// 8.2.1.1 (type 0) and 8.2.1.3 (type 2); frames and fields
void TVeH264::Poc(const TSliceHeader& aSh, const TSps& aSps)
	{
	const bool idr = aSh.iNalType == ENalIdr;
	const int par = Par(aSh);
	if (aSps.iPicOrderCntType == 0)
		{
		if (idr)
			iPrevPocMsb = 0, iPrevPocLsb = 0;
		const int32_t max = 1 << aSps.iLog2MaxPocLsb;
		const int32_t lsb = (int32_t)aSh.iPicOrderCntLsb, prev = (int32_t)iPrevPocLsb;
		int32_t msb = iPrevPocMsb;
		if (lsb < prev && prev - lsb >= max / 2)
			msb += max;
		else if (lsb > prev && lsb - prev > max / 2)
			msb -= max;
		if (par == 2)
			{
			iCur.iTopPoc = msb + lsb;
			iCur.iBottomPoc = iCur.iTopPoc + aSh.iDeltaPicOrderCntBottom;
			}
		else if (par == 0)
			iCur.iTopPoc = msb + lsb;
		else
			iCur.iBottomPoc = msb + lsb;
		if (aSh.iNalRefIdc)
			iPrevPocMsb = msb, iPrevPocLsb = (uint32_t)lsb;
		}
	else
		{
		// type 2 (type 1 is treated the same way here: x264 and most encoders never emit it)
		const uint32_t maxFn = 1u << aSps.iLog2MaxFrameNum;
		uint32_t offset = 0;
		if (!idr)
			offset = iPrevFrameNum > aSh.iFrameNum ? iPrevFrameNumOffset + maxFn : iPrevFrameNumOffset;
		int32_t t = idr ? 0 : (int32_t)(2 * (offset + aSh.iFrameNum)) - (aSh.iNalRefIdc ? 0 : 1);
		if (par != 1)
			iCur.iTopPoc = t;
		if (par != 0)
			iCur.iBottomPoc = t;
		iPrevFrameNumOffset = offset;
		}
	iPrevFrameNum = aSh.iFrameNum;
	RecalcPoc(iCur);
	}

static bool HasMmco5(const TSliceHeader& aSh)
	{
	if (aSh.iNalRefIdc == 0 || aSh.iNalType == ENalIdr || !aSh.iMarking.iAdaptive)
		return false;
	for (uint32_t k = 0; k < aSh.iMarking.iCount; ++k)
		if (aSh.iMarking.iOps[k].iOp == 5)
			return true;
	return false;
	}

int TVeH264::StartPicture(const TSliceHeader& aSh, const TSps& aSps, const TPps& aPps)
	{
	(void)aPps;
	const uint32_t w = aSps.Width(), h = aSps.Height();
	if (aSps.iChromaFormatIdc != 1 || aSps.iBitDepthLuma != 8 || aSps.iBitDepthChroma != 8)
		return -2;							// 4:2:0 8-bit only
	if (!iPicInfoBus || w != iWidth || h != iHeight)
		{
		iWidth = w, iHeight = h;
		iStride = (w + 15) & ~15u;
		iAlignedHeight = (h + 15) & ~15u;
		int ix;
		uint32_t picInfo = 18 * 0x1000 + iAlignedHeight * 2 * 64;
		if (picInfo < 130 * 1024)
			picInfo = 130 * 1024;
		iPicInfoBus = iMem.Alloc(picInfo, 4096, ix);
		iNeighborBus = iMem.Alloc(32 * 1024, 16 * 1024, ix);
		if (!iPicInfoBus || !iNeighborBus)
			return -3;
		}
	const int par = Par(aSh);
	// the second field of a complementary pair (3.30, 3.31): follows its first field, opposite parity, same frame_num,
	// both reference or both non-reference, not an IDR, no MMCO 5
	bool second = false;
	if (iHavePend)
		{
		second = par < 2 && iPend.iFields == (par ? 1 : 2) && aSh.iFrameNum == iPend.iFrameNum &&
			aSh.iNalType != ENalIdr && iPend.iRef == (aSh.iNalRefIdc != 0) && !HasMmco5(aSh) &&
			iPend.iLumaBus != 0 && w == iWidth && h == iHeight;
		if (!second)
			{
			++iPictures;					// the first field stays a single field picture
			if (iOnPicture)
				iOnPicture(iCtx, iPend);
			}
		iHavePend = false;
		}
	if (second)
		{
		iCur = iPend;
		iCur.iFieldRef[0] = iCur.iFieldRef[1] = 0;	// the DPB entry holds the first field's marking
		}
	else
		{
		memset(&iCur, 0, sizeof(iCur));
		const uint32_t luma = iStride * iAlignedHeight;
		const uint32_t bus = iMem.Alloc(luma * 3 / 2, 4096, iCur.iBuffer);
		uint32_t mbs = ((w + 15) / 16) * ((h + 15) / 16) * 16;
		if (!aSps.iDirect8x8Inference)
			mbs *= 2;
		if (!aSps.iFrameMbsOnly)
			mbs *= 2;
		iCur.iMvColSize = mbs * 2;
		iCur.iMvColBus = iMem.Alloc(iCur.iMvColSize, 4096, iCur.iSlot);	// (index unused)
		if (!bus || !iCur.iMvColBus)
			return -3;
		iCur.iLumaBus = bus;
		iCur.iChromaBus = bus + luma;
		if (aSh.iNalType == ENalIdr)
			{
			++iIdrPeriod;
			for (uint32_t i = 0; i < iDpbCount; ++i)		// an IDR empties the DPB (8.2.5.1)
				iDpb[i].iRef = false, iDpb[i].iFieldRef[0] = iDpb[i].iFieldRef[1] = 0;
			iDpbCount = 0;
			}
		iCur.iIdrPeriod = iIdrPeriod;
		iCur.iPicType = par < 2 ? 1 : (aSps.iMbAdaptiveFrameField ? 2 : 0);
		if (par < 2)						// the other field's order count is unknown until it is decoded
			iCur.iTopPoc = iCur.iBottomPoc = 0x7FFFFFFF;
		}
	iCur.iFrameNum = aSh.iFrameNum;
	iCur.iRef = aSh.iNalRefIdc != 0;
	iCur.iFields |= par == 2 ? 3 : (1 << par);
	Poc(aSh, aSps);
	if (!second)
		{
		iCur.iSlot = FreeSlot();
		if (iCur.iSlot < 0)
			return -4;
		}
	iCurSh = aSh;
	iInPicture = true;
	return 0;
	}

// 8.2.5: after the picture -- sliding window or MMCO, then the current picture joins the DPB if it is a reference
void TVeH264::Marking(const TSliceHeader& aSh, const TSps& aSps)
	{
	if (!iCur.iRef)
		return;
	const uint32_t maxFn = 1u << aSps.iLog2MaxFrameNum;
	const int par = Par(aSh);
	const bool field = par < 2;
	const uint8_t cmask = field ? (uint8_t)(1 << par) : 3;
	const int32_t curPicNum = field ? 2 * (int32_t)aSh.iFrameNum + 1 : (int32_t)aSh.iFrameNum;
	int own = FindEntry(iCur.iLumaBus);			// the first field of this frame, if it is a reference
	uint8_t curState = 1;						// 1 short-term, 2 long-term
	if (aSh.iNalType == ENalIdr)
		{
		curState = aSh.iMarking.iLongTermReference ? 2 : 1;
		iCur.iLongTermFrameIdx = 0;
		}
	else if (aSh.iMarking.iAdaptive)
		{
		for (uint32_t k = 0; k < aSh.iMarking.iCount; ++k)
			{
			const uint32_t op = aSh.iMarking.iOps[k].iOp;
			if (op == 1)						// unmark a short-term picture (frame or field)
				{
				const int32_t pn = curPicNum - (int32_t)(aSh.iMarking.iOps[k].iDiffPicNumsMinus1 + 1);
				for (uint32_t i = 0; i < iDpbCount; ++i)
					{
					TPicture& e = iDpb[i];
					const int32_t wrap = WrapOf(e, aSh.iFrameNum, maxFn);
					if (field)
						{
						for (int q = 0; q < 2; ++q)
							if (e.iFieldRef[q] == 1 && (q == par ? 2 * wrap + 1 : 2 * wrap) == pn)
								e.iFieldRef[q] = 0;
						}
					else if (e.iFieldRef[0] == 1 && e.iFieldRef[1] == 1 && wrap == pn)
						e.iFieldRef[0] = e.iFieldRef[1] = 0;
					}
				}
			else if (op == 2)					// unmark a long-term picture
				{
				const uint32_t lpn = aSh.iMarking.iOps[k].iLongTermPicNum;
				for (uint32_t i = 0; i < iDpbCount; ++i)
					{
					TPicture& e = iDpb[i];
					if (field)
						{
						for (int q = 0; q < 2; ++q)
							if (e.iFieldRef[q] == 2 && (q == par ? 2 * e.iLongTermFrameIdx + 1 : 2 * e.iLongTermFrameIdx) == lpn)
								e.iFieldRef[q] = 0;
						}
					else if (e.iFieldRef[0] == 2 && e.iFieldRef[1] == 2 && e.iLongTermFrameIdx == lpn)
						e.iFieldRef[0] = e.iFieldRef[1] = 0;
					}
				}
			else if (op == 3)					// short-term -> long-term
				{
				const int32_t pn = curPicNum - (int32_t)(aSh.iMarking.iOps[k].iDiffPicNumsMinus1 + 1);
				const uint32_t idx = aSh.iMarking.iOps[k].iLongTermFrameIdx;
				int te = -1, tq = 0;
				for (uint32_t i = 0; i < iDpbCount && te < 0; ++i)
					{
					const TPicture& e = iDpb[i];
					const int32_t wrap = WrapOf(e, aSh.iFrameNum, maxFn);
					if (field)
						{
						for (int q = 0; q < 2 && te < 0; ++q)
							if (e.iFieldRef[q] == 1 && (q == par ? 2 * wrap + 1 : 2 * wrap) == pn)
								te = (int)i, tq = q;
						}
					else if (e.iFieldRef[0] == 1 && e.iFieldRef[1] == 1 && wrap == pn)
						te = (int)i;
					}
				// the index is freed first (another frame / field holding it is unmarked; the target's own sibling stays)
				for (uint32_t i = 0; i < iDpbCount; ++i)
					if ((int)i != te && iDpb[i].iLongTermFrameIdx == idx)
						for (int q = 0; q < 2; ++q)
							if (iDpb[i].iFieldRef[q] == 2)
								iDpb[i].iFieldRef[q] = 0;
				if (te >= 0)
					{
					if (field)
						iDpb[te].iFieldRef[tq] = 2;
					else
						iDpb[te].iFieldRef[0] = iDpb[te].iFieldRef[1] = 2;
					iDpb[te].iLongTermFrameIdx = idx;
					}
				}
			else if (op == 4)					// the long-term index limit
				{
				for (uint32_t i = 0; i < iDpbCount; ++i)
					for (int q = 0; q < 2; ++q)
						if (iDpb[i].iFieldRef[q] == 2 &&
							iDpb[i].iLongTermFrameIdx + 1 > aSh.iMarking.iOps[k].iMaxLongTermFrameIdxPlus1)
							iDpb[i].iFieldRef[q] = 0;
				}
			else if (op == 5)					// everything unmarked; the current picture becomes POC 0 / frame_num 0
				{
				for (uint32_t i = 0; i < iDpbCount; ++i)
					iDpb[i].iFieldRef[0] = iDpb[i].iFieldRef[1] = 0;
				iCur.iMmco5 = true;
				}
			else if (op == 6)					// the current picture becomes long-term
				{
				const uint32_t idx = aSh.iMarking.iOps[k].iLongTermFrameIdx;
				for (uint32_t i = 0; i < iDpbCount; ++i)
					if ((int)i != own && iDpb[i].iLongTermFrameIdx == idx)
						for (int q = 0; q < 2; ++q)
							if (iDpb[i].iFieldRef[q] == 2)
								iDpb[i].iFieldRef[q] = 0;
				curState = 2;
				iCur.iLongTermFrameIdx = idx;
				}
			}
		}
	else
		{
		// sliding window (8.2.5.3); not for the second field of a pair whose first field is a short-term reference
		const bool skip = field && iCur.iFields == 3 && own >= 0 && iDpb[own].iFieldRef[1 - par] == 1;
		const uint32_t maxRefs = aSps.iMaxNumRefFrames ? aSps.iMaxNumRefFrames : 1;
		while (!skip)
			{
			uint32_t numRefs = 0;
			for (uint32_t i = 0; i < iDpbCount; ++i)
				numRefs += (iDpb[i].iFieldRef[0] || iDpb[i].iFieldRef[1]) ? 1 : 0;
			if (numRefs < maxRefs)
				break;
			int oldest = -1;
			int32_t oldestWrap = 0;
			for (uint32_t i = 0; i < iDpbCount; ++i)
				if (iDpb[i].iFieldRef[0] == 1 || iDpb[i].iFieldRef[1] == 1)
					{
					const int32_t wrap = WrapOf(iDpb[i], aSh.iFrameNum, maxFn);
					if (oldest < 0 || wrap < oldestWrap)
						oldest = (int)i, oldestWrap = wrap;
					}
			if (oldest < 0)
				break;
			for (int q = 0; q < 2; ++q)
				if (iDpb[oldest].iFieldRef[q] == 1)
					iDpb[oldest].iFieldRef[q] = 0;
			}
		}
	for (int q = 0; q < 2; ++q)
		if (cmask & (1 << q))
			iCur.iFieldRef[q] = curState;
	iCur.iLongTerm = curState == 2;
	// compact the DPB, then add (or complete) the current picture
	uint32_t n = 0;
	for (uint32_t i = 0; i < iDpbCount; ++i)
		{
		Sync(iDpb[i]);
		if (iDpb[i].iRef)
			iDpb[n++] = iDpb[i];
		}
	iDpbCount = n;
	if (iCur.iMmco5)
		{
		// 8.2.1: after memory_management_control_operation 5 the picture counts as POC 0 for what follows
		if (field)
			{
			if (par == 0)
				iCur.iTopPoc = 0;
			else
				iCur.iBottomPoc = 0;
			iPrevPocLsb = 0;
			}
		else
			{
			iCur.iTopPoc -= iCur.iPoc;
			iCur.iBottomPoc -= iCur.iPoc;
			iPrevPocLsb = (uint32_t)iCur.iTopPoc;
			}
		iPrevPocMsb = 0;
		iPrevFrameNum = 0;
		iPrevFrameNumOffset = 0;
		iCur.iFrameNum = 0;
		RecalcPoc(iCur);
		}
	own = FindEntry(iCur.iLumaBus);
	if (own >= 0)
		{
		TPicture& e = iDpb[own];
		const uint8_t keep[2] = { e.iFieldRef[0], e.iFieldRef[1] };
		const uint32_t keepIdx = e.iLongTermFrameIdx;
		e = iCur;
		for (int q = 0; q < 2; ++q)
			if (!(cmask & (1 << q)))
				e.iFieldRef[q] = keep[q];
		if (curState != 2)
			e.iLongTermFrameIdx = keepIdx;
		Sync(e);
		}
	else if (iDpbCount < KMaxDpb)
		{
		Sync(iCur);
		iDpb[iDpbCount++] = iCur;
		}
	}

void TVeH264::FinishPicture()
	{
	if (!iInPicture)
		return;
	iInPicture = false;
	const TPps* pps = iParser.Pps(iCurSh.iPpsId);
	const TSps* sps = pps ? iParser.Sps(pps->iSpsId) : NULL;
	TPicture out = iCur;
	if (sps)
		Marking(iCurSh, *sps);
	if (iCurSh.iFieldPic && iCur.iFields != 3)
		{
		iPend = iCur;						// a first field: reported with its second field (or alone, later)
		iHavePend = true;
		return;
		}
	++iPictures;
	if (iOnPicture)
		iOnPicture(iCtx, out);
	}

void TVeH264::Flush()
	{
	FinishPicture();
	if (iHavePend)
		{
		iHavePend = false;
		++iPictures;
		if (iOnPicture)
			iOnPicture(iCtx, iPend);
		}
	}

bool TVeH264::InUse(uint32_t aBus) const
	{
	if (aBus == iPicInfoBus || aBus == iNeighborBus)
		return true;
	if (iInPicture && (aBus == iCur.iLumaBus || aBus == iCur.iMvColBus))
		return true;
	if (iHavePend && (aBus == iPend.iLumaBus || aBus == iPend.iMvColBus))
		return true;
	for (uint32_t i = 0; i < iDpbCount; ++i)
		if (iDpb[i].iRef && (aBus == iDpb[i].iLumaBus || aBus == iDpb[i].iMvColBus))
			return true;
	return false;
	}

// 8.2.4.3 modification of a list (frames: 8.2.4.3.1/.2 with frame numbers; fields: field picture numbers, 8.2.4.1)
int TVeH264::Modify(const TSliceHeader& aSh, const TSps& aSps, int aList, TRef* aRefs, uint32_t aCount)
	{
	const int par = Par(aSh);
	const bool field = par < 2;
	const int32_t maxFn = 1 << aSps.iLog2MaxFrameNum;
	const int32_t maxPicNum = field ? 2 * maxFn : maxFn;
	const int32_t curPicNum = field ? 2 * (int32_t)aSh.iFrameNum + 1 : (int32_t)aSh.iFrameNum;
	int32_t pred = curPicNum;
	uint32_t refIdx = 0;
	for (uint32_t m = 0; m < aSh.iModCount[aList]; ++m)
		{
		const TRefPicListMod& mod = aSh.iMod[aList][m];
		TRef ref;
		ref.iPic = NULL;
		ref.iParity = 2;
		if (mod.iIdc < 2)
			{
			int32_t noWrap;
			const int32_t abs = (int32_t)mod.iValue + 1;
			if (mod.iIdc == 0)
				noWrap = pred - abs < 0 ? pred - abs + maxPicNum : pred - abs;
			else
				noWrap = pred + abs >= maxPicNum ? pred + abs - maxPicNum : pred + abs;
			pred = noWrap;
			const int32_t picNum = noWrap > curPicNum ? noWrap - maxPicNum : noWrap;
			for (uint32_t i = 0; i < iDpbCount && !ref.iPic; ++i)
				{
				TPicture& e = iDpb[i];
				if (field)
					{
					for (int q = 0; q < 2 && !ref.iPic; ++q)
						if (e.iFieldRef[q] == 1 && (q == par ? 2 * e.iFrameNumWrap + 1 : 2 * e.iFrameNumWrap) == picNum)
							ref.iPic = &e, ref.iParity = (uint8_t)q;
					}
				else if (e.iFieldRef[0] == 1 && e.iFieldRef[1] == 1 && e.iFrameNumWrap == picNum)
					ref.iPic = &e;
				}
			}
		else if (mod.iIdc == 2)
			{
			for (uint32_t i = 0; i < iDpbCount && !ref.iPic; ++i)
				{
				TPicture& e = iDpb[i];
				if (field)
					{
					for (int q = 0; q < 2 && !ref.iPic; ++q)
						if (e.iFieldRef[q] == 2 && (q == par ? 2 * e.iLongTermFrameIdx + 1 : 2 * e.iLongTermFrameIdx) == mod.iValue)
							ref.iPic = &e, ref.iParity = (uint8_t)q;
					}
				else if (e.iFieldRef[0] == 2 && e.iFieldRef[1] == 2 && e.iLongTermFrameIdx == mod.iValue)
					ref.iPic = &e;
				}
			}
		if (!ref.iPic)
			return -5;
		// insert at refIdx, drop the later duplicate (8-37 / 8-38)
		for (uint32_t c = aCount; c > refIdx; --c)
			aRefs[c] = aRefs[c - 1];
		aRefs[refIdx++] = ref;
		uint32_t n = refIdx;
		for (uint32_t c = refIdx; c <= aCount; ++c)
			if (!(aRefs[c] == ref))
				aRefs[n++] = aRefs[c];
		}
	return 0;
	}

// 8.2.4.2.5: the fields of an ordered frame list, alternating parity starting with the current field's, a missing
// field skipped, the rest of the other parity appended once one parity runs out
static void Alternate(TPicture** aF, uint32_t aN, uint8_t aState, int aPar, TRef* aOut, uint32_t& aLen)
	{
	uint32_t ip[2] = { 0, 0 };
	int want = aPar;
	for (;;)
		{
		while (ip[want] < aN && aF[ip[want]]->iFieldRef[want] != aState)
			++ip[want];
		if (ip[want] < aN)
			{
			if (aLen < KMaxRefIdx)
				{
				TRef r;
				r.iPic = aF[ip[want]], r.iParity = (uint8_t)want;
				aOut[aLen++] = r;
				}
			++ip[want];
			want ^= 1;
			}
		else
			{
			want ^= 1;
			for (; ip[want] < aN; ++ip[want])
				if (aF[ip[want]]->iFieldRef[want] == aState && aLen < KMaxRefIdx)
					{
					TRef r;
					r.iPic = aF[ip[want]], r.iParity = (uint8_t)want;
					aOut[aLen++] = r;
					}
			break;
			}
		}
	}

static TRef FrameRef(TPicture* aP)
	{
	TRef r;
	r.iPic = aP, r.iParity = 2;
	return r;
	}

// 8.2.4.1 picture numbers + 8.2.4.2 initialisation + 8.2.4.3 modification
int TVeH264::BuildLists(const TSliceHeader& aSh, const TSps& aSps)
	{
	const uint32_t maxFn = 1u << aSps.iLog2MaxFrameNum;
	const int par = Par(aSh);
	const bool field = par < 2;
	for (uint32_t i = 0; i < iDpbCount; ++i)
		iDpb[i].iFrameNumWrap = WrapOf(iDpb[i], aSh.iFrameNum, maxFn);
	// frames (frame decoding: both fields marked alike) or entries with any field marked (field decoding)
	TPicture* shortT[KMaxDpb];
	TPicture* longT[KMaxDpb];
	uint32_t ns = 0, nl = 0;
	for (uint32_t i = 0; i < iDpbCount; ++i)
		{
		const TPicture& e = iDpb[i];
		if (field)
			{
			if (e.iFieldRef[0] == 1 || e.iFieldRef[1] == 1)
				shortT[ns++] = &iDpb[i];
			if (e.iFieldRef[0] == 2 || e.iFieldRef[1] == 2)
				longT[nl++] = &iDpb[i];
			}
		else if (e.iFieldRef[0] == 1 && e.iFieldRef[1] == 1)
			shortT[ns++] = &iDpb[i];
		else if (e.iFieldRef[0] == 2 && e.iFieldRef[1] == 2)
			longT[nl++] = &iDpb[i];
		}
	// long-term by LongTermPicNum / LongTermFrameIdx ascending (both list kinds)
	for (uint32_t a = 0; a < nl; ++a)
		for (uint32_t b = a + 1; b < nl; ++b)
			if (longT[b]->iLongTermFrameIdx < longT[a]->iLongTermFrameIdx)
				{ TPicture* t = longT[a]; longT[a] = longT[b]; longT[b] = t; }
	iListLen[0] = iListLen[1] = 0;
	TRef init[2][KMaxRefIdx + 1];
	uint32_t initLen[2] = { 0, 0 };
	if (aSh.iSliceType == ESliceP || aSh.iSliceType == ESliceSP)
		{
		for (uint32_t a = 0; a < ns; ++a)				// PicNum / FrameNumWrap descending
			for (uint32_t b = a + 1; b < ns; ++b)
				if (shortT[b]->iFrameNumWrap > shortT[a]->iFrameNumWrap)
					{ TPicture* t = shortT[a]; shortT[a] = shortT[b]; shortT[b] = t; }
		if (field)
			{
			Alternate(shortT, ns, 1, par, init[0], initLen[0]);
			Alternate(longT, nl, 2, par, init[0], initLen[0]);
			}
		else
			{
			for (uint32_t a = 0; a < ns && initLen[0] < KMaxRefIdx; ++a)
				init[0][initLen[0]++] = FrameRef(shortT[a]);
			for (uint32_t a = 0; a < nl && initLen[0] < KMaxRefIdx; ++a)
				init[0][initLen[0]++] = FrameRef(longT[a]);
			}
		}
	else if (aSh.iSliceType == ESliceB)
		{
		// frames: POC < current, fields: POC <= current (8.2.4.2.3 / 8.2.4.2.4); a frame's POC = the smaller decoded field's
		const int32_t cur = field ? (par ? iCur.iBottomPoc : iCur.iTopPoc) : iCur.iPoc;
		TPicture* before[KMaxDpb];
		TPicture* after[KMaxDpb];
		uint32_t nb = 0, na = 0;
		for (uint32_t a = 0; a < ns; ++a)
			((field ? shortT[a]->iPoc <= cur : shortT[a]->iPoc < cur) ? before[nb++] : after[na++]) = shortT[a];
		for (uint32_t a = 0; a < nb; ++a)				// descending POC
			for (uint32_t b = a + 1; b < nb; ++b)
				if (before[b]->iPoc > before[a]->iPoc)
					{ TPicture* t = before[a]; before[a] = before[b]; before[b] = t; }
		for (uint32_t a = 0; a < na; ++a)				// ascending POC
			for (uint32_t b = a + 1; b < na; ++b)
				if (after[b]->iPoc < after[a]->iPoc)
					{ TPicture* t = after[a]; after[a] = after[b]; after[b] = t; }
		TPicture* f0[KMaxDpb];
		TPicture* f1[KMaxDpb];
		uint32_t n0 = 0, n1 = 0;
		for (uint32_t a = 0; a < nb; ++a) f0[n0++] = before[a];
		for (uint32_t a = 0; a < na; ++a) f0[n0++] = after[a];
		for (uint32_t a = 0; a < na; ++a) f1[n1++] = after[a];
		for (uint32_t a = 0; a < nb; ++a) f1[n1++] = before[a];
		if (field)
			{
			Alternate(f0, n0, 1, par, init[0], initLen[0]);
			Alternate(longT, nl, 2, par, init[0], initLen[0]);
			Alternate(f1, n1, 1, par, init[1], initLen[1]);
			Alternate(longT, nl, 2, par, init[1], initLen[1]);
			}
		else
			{
			for (uint32_t a = 0; a < n0; ++a) init[0][initLen[0]++] = FrameRef(f0[a]);
			for (uint32_t a = 0; a < nl; ++a) init[0][initLen[0]++] = FrameRef(longT[a]);
			for (uint32_t a = 0; a < n1; ++a) init[1][initLen[1]++] = FrameRef(f1[a]);
			for (uint32_t a = 0; a < nl; ++a) init[1][initLen[1]++] = FrameRef(longT[a]);
			}
		if (initLen[1] > 1 && initLen[0] == initLen[1])
			{
			bool same = true;
			for (uint32_t a = 0; a < initLen[0]; ++a)
				same = same && init[0][a] == init[1][a];
			if (same)
				{ TRef t = init[1][0]; init[1][0] = init[1][1]; init[1][1] = t; }
			}
		}
	const int lists = aSh.iSliceType == ESliceB ? 2 : (aSh.iSliceType == ESliceP || aSh.iSliceType == ESliceSP ? 1 : 0);
	for (int l = 0; l < lists; ++l)
		{
		const uint32_t active = l ? aSh.iNumRefIdxL1Active : aSh.iNumRefIdxL0Active;
		TRef refs[KMaxRefIdx + 1];
		for (uint32_t i = 0; i <= KMaxRefIdx; ++i)
			refs[i] = i < initLen[l] && i < active ? init[l][i] : FrameRef(NULL);
		const int r = Modify(aSh, aSps, l, refs, active);
		if (r < 0)
			return r;
		for (uint32_t i = 0; i < active && i < KMaxRefIdx; ++i)
			iList[l][i] = refs[i];
		iListLen[l] = active;
		}
	return 0;
	}

static uint32_t Le4(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// One slice = one VE job (ve-h264-regs.md section 6, cedrus order)
int TVeH264::Job(const uint8_t* aNal, size_t aLen, uint32_t aBus, const TSliceHeader& aSh, const TSps& aSps,
	const TPps& aPps)
	{
	MVeJob& j = iJob;
	j.Begin();
	j.Write(KVeOutFmt, 0x40);								// linear NV12 (verified with MPEG-2)
	j.Write(KVeChromaBufLen, iStride * iAlignedHeight / 4);
	j.Write(KVeStride, ((iStride / 2) << 16) | iStride);
	j.Write(KVeMode, 0x00130001 | (iWidth > 2048 ? 1u << 21 : 0));
	j.Write(KSdRot, 0);
	j.Write(KExtra1, iPicInfoBus);
	j.Write(KExtra2, iNeighborBus);
	// scaling lists: the PPS copy holds the effective lists (SPS fall-back already applied by the parser)
	const bool scaling = aPps.iPicScalingMatrixPresent || aSps.iSeqScalingMatrixPresent;
	if (scaling)
		{
		// the VE takes the lists in RASTER order (as the V4L2 control delivers them: after the inverse scan, 8.5.6);
		// the bitstream codes them in zigzag (frame) scan order
		static const uint8_t KZz4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
		static const uint8_t KZz8[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
			41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59,
			52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
		uint8_t r8[2][64], r4[6][16];
		for (int l = 0; l < 2; ++l)
			for (int i = 0; i < 64; ++i)
				r8[l][KZz8[i]] = aPps.iScaling8x8[l][i];
		for (int l = 0; l < 6; ++l)
			for (int i = 0; i < 16; ++i)
				r4[l][KZz4[i]] = aPps.iScaling4x4[l][i];
		j.Write(KSramOff, KSram8x8a << 2);
		for (int w = 0; w < 16; ++w) j.Write(KSramData, Le4(r8[0] + 4 * w));
		j.Write(KSramOff, KSram8x8b << 2);
		for (int w = 0; w < 16; ++w) j.Write(KSramData, Le4(r8[1] + 4 * w));
		j.Write(KSramOff, KSram4x4 << 2);
		for (int l = 0; l < 6; ++l)
			for (int w = 0; w < 4; ++w) j.Write(KSramData, Le4(r4[l] + 4 * w));
		}
	// the frame list: the current picture + every reference in the DPB; the rest zero
	uint32_t entries[KSlots][8];
	memset(entries, 0, sizeof(entries));
	for (uint32_t i = 0; i <= iDpbCount; ++i)
		{
		const TPicture& p = i < iDpbCount ? iDpb[i] : iCur;
		if (p.iSlot < 0 || p.iSlot >= KSlots)
			continue;
		uint32_t* e = entries[p.iSlot];
		// a field not decoded yet has no order count: V4L2 userspace (ffmpeg/vaapi convention: INT_MAX -> 0) gives cedrus 0
		e[0] = p.iTopPoc == 0x7FFFFFFF ? 0u : (uint32_t)p.iTopPoc;
		e[1] = p.iBottomPoc == 0x7FFFFFFF ? 0u : (uint32_t)p.iBottomPoc;
		e[2] = (uint32_t)p.iPicType << 8;
		e[3] = p.iLumaBus;
		e[4] = p.iChromaBus;
		e[5] = p.iMvColBus;
		e[6] = p.iMvColBus + p.iMvColSize / 2;
		}
	j.Write(KSramOff, KSramFrames << 2);
	for (int s = 0; s < KSlots; ++s)
		for (int w = 0; w < 8; ++w)
			j.Write(KSramData, entries[s][w]);
	j.Write(KOutIdx, (uint32_t)iCur.iSlot);
	// the bitstream: the whole NAL (header byte included), the header skipped by flushes
	j.Write(KVldLen, (uint32_t)aLen * 8);
	j.Write(KVldOffset, 0);
	j.Write(KVldEnd, aBus + (uint32_t)aLen);
	j.Write(KVldAddr, (aBus & 0x0FFFFFF0) | (aBus >> 28) | 0x70000000);
	j.Write(KVeBufCtrl, 0);
	j.Write(KTrigger, 7);
	uint32_t skip = (uint32_t)aSh.iHeaderBitsEscaped;			// from the NAL's first byte, escaped stream
	while (skip)
		{
		const uint32_t n = skip > 32 ? 32 : skip;
		j.Write(KTrigger, 3 | (n << 8));
		j.PollClear(KStatus, 1u << 8);
		skip -= n;
		}
	(void)aNal;
	// prediction weights
	const uint32_t t = aSh.iSliceType;
	if ((aPps.iWeightedPred && (t == ESliceP || t == ESliceSP)) || (aPps.iWeightedBipredIdc == 1 && t == ESliceB))
		{
		j.Write(KShsWp, (aSh.iPwt.iChromaLog2Denom << 4) | aSh.iPwt.iLumaLog2Denom);
		j.Write(KSramOff, KSramWeights << 2);
		for (int l = 0; l < 2; ++l)
			{
			for (int i = 0; i < 32; ++i)
				j.Write(KSramData, ((uint32_t)(aSh.iPwt.iL[l][i].iLumaOffset & 0x1FF) << 16) |
					(uint32_t)(aSh.iPwt.iL[l][i].iLumaWeight & 0x1FF));
			for (int i = 0; i < 32; ++i)
				for (int c = 0; c < 2; ++c)
					j.Write(KSramData, ((uint32_t)(aSh.iPwt.iL[l][i].iChromaOffset[c] & 0x1FF) << 16) |
						(uint32_t)(aSh.iPwt.iL[l][i].iChromaWeight[c] & 0x1FF));
			}
		}
	// reference lists: byte i = slot << 1, bit 0 set for a bottom field (frames and top fields: clear)
	for (int l = 0; l < 2; ++l)
		{
		if (!iListLen[l])
			continue;
		j.Write(KSramOff, (l ? KSramList1 : KSramList0) << 2);
		const uint32_t words = (iListLen[l] + 3) / 4;
		for (uint32_t w = 0; w < words; ++w)
			{
			uint32_t v = 0;
			for (uint32_t b = 0; b < 4; ++b)
				{
				const uint32_t i = w * 4 + b;
				const TRef* p = i < iListLen[l] ? &iList[l][i] : NULL;
				v |= (uint32_t)(p && p->iPic ? (p->iPic->iSlot << 1) | (p->iParity == 1 ? 1 : 0) : 0) << (8 * b);
				}
			j.Write(KSramData, v);
			}
		}
	const uint32_t l0 = aSh.iNumRefIdxL0Active ? aSh.iNumRefIdxL0Active - 1 : 0;
	const uint32_t l1 = aSh.iNumRefIdxL1Active ? aSh.iNumRefIdxL1Active - 1 : 0;
	j.Write(KPps, ((aPps.iEntropyCodingMode ? 1u : 0) << 15) | (l0 << 10) | (l1 << 5) | ((aPps.iWeightedPred ? 1u : 0) << 4)
		| (aPps.iWeightedBipredIdc << 2) | ((aPps.iConstrainedIntraPred ? 1u : 0) << 1) | (aPps.iTransform8x8Mode ? 1u : 0));
	j.Write(KSps, (aSps.iChromaFormatIdc << 19) | ((aSps.iFrameMbsOnly ? 1u : 0) << 18) |
		((aSps.iMbAdaptiveFrameField ? 1u : 0) << 17) | ((aSps.iDirect8x8Inference ? 1u : 0) << 16) |
		((aSps.iPicWidthInMbs - 1) << 8) | (aSps.iPicHeightInMapUnits - 1));
	const uint32_t wMbs = aSps.iPicWidthInMbs;
	const uint32_t mbY = (aSh.iFirstMbInSlice / wMbs) * (aSps.iMbAdaptiveFrameField && !aSh.iFieldPic ? 2 : 1);
	// SHS bit 5 = "first slice of the frame". cedrus sets it from !dst->copied_timestamp, i.e. only on the first slice
	// that writes a capture buffer: userspace holds the capture buffer across the two fields (HOLD_CAPTURE_BUF), so the
	// second field of a frame is NOT a first slice (V3b guess "bit 5 on every field" = the suspect; ve-h264-regs.md)
	const bool secondField = aSh.iFieldPic && iCur.iFields == 3;
	const bool first = aSh.iFirstMbInSlice == 0 && !secondField;
	j.Write(KShs, ((aSh.iFirstMbInSlice % wMbs) << 24) | ((mbY & 0xFF) << 16) | ((aSh.iNalRefIdc ? 1u : 0) << 12) |
		(t << 8) | ((first ? 1u : 0) << 5) | ((aSh.iFieldPic ? 1u : 0) << 4) | ((aSh.iBottomField ? 1u : 0) << 3) |
		((aSh.iDirectSpatialMvPred ? 1u : 0) << 2) | (aSh.iCabacInitIdc & 3));
	j.Write(KShs2, (l0 << 24) | (l1 << 16) | (1u << 12) | (aSh.iDisableDeblockingFilterIdc << 8) |
		(((uint32_t)aSh.iSliceAlphaC0OffsetDiv2 & 0xF) << 4) | ((uint32_t)aSh.iSliceBetaOffsetDiv2 & 0xF));
	const int32_t qp = aPps.iPicInitQp + aSh.iSliceQpDelta;
	j.Write(KShsQp, ((scaling ? 0u : 1u) << 24) | (((uint32_t)aPps.iSecondChromaQpIndexOffset & 0x3F) << 16) |
		(((uint32_t)aPps.iChromaQpIndexOffset & 0x3F) << 8) | ((uint32_t)qp & 0x3F));
	j.Write(KStatus, 7);
	j.Write(KCtrl, 7);
	j.Write(KTrigger, 8);
	const int st = j.Run(KStatus, 7, KCtrl, 7);
	iLastStatus = (uint32_t)st;
	if (st < 0 || (st & 6) || !(st & 1))
		{
		++iJobErrors;
		return -6;
		}
	return 0;
	}

int TVeH264::DecodeSlice(const uint8_t* aNal, size_t aLen, uint32_t aBus)
	{
	TSliceHeader sh;
	if (!iParser.ParseSliceHeader(aNal, aLen, sh))
		return -1;
	const TPps* pps = iParser.Pps(sh.iPpsId);
	const TSps* sps = pps ? iParser.Sps(pps->iSpsId) : NULL;
	if (!sps)
		return -1;
	if (sh.iFirstMbInSlice == 0)
		{
		FinishPicture();
		const int r = StartPicture(sh, *sps, *pps);
		if (r < 0)
			return r;
		}
	else if (!iInPicture)
		return -7;
	const int r = BuildLists(sh, *sps);
	if (r < 0)
		return r;
	++iSlices;
	return Job(aNal, aLen, aBus, sh, *sps, *pps);
	}

} // namespace h264
