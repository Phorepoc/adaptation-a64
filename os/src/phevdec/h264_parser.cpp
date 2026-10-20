// h264_parser.cpp -- OURS (VDU rung V3): see h264_parser.h. Section numbers are ITU-T H.264 (08/2021).
#include "h264_parser.h"
#include <string.h>

namespace h264
{

// ---------------------------------------------------------------------------------------------------------------
// bits (7.2, 9.1)

uint32_t TBitReader::U(int aBits)
	{
	uint32_t v = 0;
	for (int i = 0; i < aBits; ++i, ++iPos)
		{
		if (iPos >= iLen * 8)
			{
			iError = true;
			return 0;
			}
		v = (v << 1) | ((iData[iPos >> 3] >> (7 - (iPos & 7))) & 1);
		}
	return v;
	}

uint32_t TBitReader::Ue()
	{
	int zeros = 0;
	while (!iError && U(1) == 0)
		if (++zeros > 31)
			{
			iError = true;
			return 0;
			}
	if (zeros == 0)
		return 0;
	return ((1u << zeros) - 1) + U(zeros);
	}

int32_t TBitReader::Se()
	{
	const uint32_t k = Ue();
	return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
	}

// 7.2: more_rbsp_data -- anything before the rbsp_stop_one_bit (the last 1 bit of the RBSP)
bool TBitReader::MoreRbspData() const
	{
	size_t last = iLen;
	while (last > 0 && iData[last - 1] == 0)
		--last;
	if (last == 0)
		return false;
	const uint8_t b = iData[last - 1];
	int tz = 0;
	while (!((b >> tz) & 1))
		++tz;
	const size_t stopBit = (last - 1) * 8 + (7 - tz);
	return iPos < stopBit;
	}

// ---------------------------------------------------------------------------------------------------------------
// NAL units (Annex B, 7.3.1)

bool NextNal(const uint8_t* aBuf, size_t aLen, size_t& aPos, size_t& aNalStart, size_t& aNalEnd)
	{
	size_t i = aPos;
	while (i + 3 <= aLen && !(aBuf[i] == 0 && aBuf[i + 1] == 0 && aBuf[i + 2] == 1))
		++i;
	if (i + 3 > aLen)
		return false;
	aNalStart = i + 3;
	size_t j = aNalStart;
	while (j + 3 <= aLen && !(aBuf[j] == 0 && aBuf[j + 1] == 0 && (aBuf[j + 2] == 1 || aBuf[j + 2] == 0)))
		++j;
	aNalEnd = j + 3 <= aLen ? j : aLen;
	while (aNalEnd > aNalStart && aBuf[aNalEnd - 1] == 0)		// trailing_zero_8bits
		--aNalEnd;
	aPos = j;
	return aNalEnd > aNalStart;
	}

size_t Unescape(const uint8_t* aNal, size_t aLen, uint8_t* aOut)
	{
	size_t n = 0;
	int zeros = 0;
	for (size_t i = 0; i < aLen; ++i)
		{
		if (zeros >= 2 && aNal[i] == 3)
			{
			zeros = 0;
			continue;
			}
		zeros = aNal[i] == 0 ? zeros + 1 : 0;
		aOut[n++] = aNal[i];
		}
	return n;
	}

size_t EscapedBitPos(const uint8_t* aNal, size_t aLen, size_t aRbspBitPos)
	{
	size_t rbspBytes = 0;
	int zeros = 0;
	size_t i = 0;
	for (; i < aLen && rbspBytes < aRbspBitPos / 8; ++i)
		{
		if (zeros >= 2 && aNal[i] == 3)
			{
			zeros = 0;
			continue;
			}
		zeros = aNal[i] == 0 ? zeros + 1 : 0;
		++rbspBytes;
		}
	if (i < aLen && zeros >= 2 && aNal[i] == 3)	// an escape right at the byte the position lands in
		++i;
	return i * 8 + (aRbspBitPos & 7);
	}

// ---------------------------------------------------------------------------------------------------------------
// scaling lists (7.3.2.1.1.1, Tables 7-2 .. 7-4), zigzag order

static const uint8_t KDefault4x4Intra[16] = { 6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42 };
static const uint8_t KDefault4x4Inter[16] = { 10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34 };
static const uint8_t KDefault8x8Intra[64] = { 6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23, 23, 23, 23,
	23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
	31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42 };
static const uint8_t KDefault8x8Inter[64] = { 9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21, 21, 21, 21,
	21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
	27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35 };

// returns true when useDefaultScalingMatrixFlag is set
static bool ScalingList(TBitReader& aB, uint8_t* aList, int aSize)
	{
	int last = 8, next = 8;
	bool useDefault = false;
	for (int j = 0; j < aSize; ++j)
		{
		if (next != 0)
			{
			const int32_t delta = aB.Se();
			next = (last + delta + 256) % 256;
			useDefault = j == 0 && next == 0;
			}
		aList[j] = (uint8_t)(next == 0 ? last : next);
		last = aList[j];
		}
	return useDefault;
	}

// aCount lists (6 4x4 + 2 or 6 8x8); fall-back rule A (SPS: aFallback4x4/8x8 NULL) or B (PPS: the SPS's lists)
static void ScalingMatrix(TBitReader& aB, int aCount, uint8_t a4x4[6][16], uint8_t a8x8[6][64],
	const uint8_t (*aFb4x4)[16], const uint8_t (*aFb8x8)[64])
	{
	for (int i = 0; i < aCount; ++i)
		{
		const bool present = aB.Flag();
		if (i < 6)
			{
			if (present)
				{
				if (ScalingList(aB, a4x4[i], 16))
					memcpy(a4x4[i], i < 3 ? KDefault4x4Intra : KDefault4x4Inter, 16);
				}
			else if (i == 0 || i == 3)
				memcpy(a4x4[i], aFb4x4 ? aFb4x4[i] : (i == 0 ? KDefault4x4Intra : KDefault4x4Inter), 16);
			else
				memcpy(a4x4[i], a4x4[i - 1], 16);
			}
		else
			{
			const int k = i - 6;
			if (present)
				{
				if (ScalingList(aB, a8x8[k], 64))
					memcpy(a8x8[k], (k & 1) ? KDefault8x8Inter : KDefault8x8Intra, 64);
				}
			else if (k < 2)
				memcpy(a8x8[k], aFb8x8 ? aFb8x8[k] : ((k & 1) ? KDefault8x8Inter : KDefault8x8Intra), 64);
			else
				memcpy(a8x8[k], a8x8[k - 2], 64);
			}
		}
	}

static void Flat(uint8_t a4x4[6][16], uint8_t a8x8[6][64])
	{
	memset(a4x4, 16, 6 * 16);
	memset(a8x8, 16, 6 * 64);
	}

// ---------------------------------------------------------------------------------------------------------------

TParser::TParser()
	{
	memset(iSps, 0, sizeof(iSps));
	memset(iPps, 0, sizeof(iPps));
	}

// 7.3.2.1.1
bool TParser::ParseSps(const uint8_t* aNal, size_t aLen)
	{
	const size_t n = Unescape(aNal + 1, aLen - 1 < sizeof(iRbsp) ? aLen - 1 : sizeof(iRbsp), iRbsp);
	TBitReader b(iRbsp, n);
	TSps s;
	memset(&s, 0, sizeof(s));
	s.iProfileIdc = (uint8_t)b.U(8);
	s.iConstraintFlags = (uint8_t)b.U(8);
	s.iLevelIdc = (uint8_t)b.U(8);
	s.iSpsId = b.Ue();
	if (s.iSpsId >= KMaxSps)
		return false;
	s.iChromaFormatIdc = 1;
	s.iBitDepthLuma = s.iBitDepthChroma = 8;
	Flat(s.iScaling4x4, s.iScaling8x8);
	const uint8_t p = s.iProfileIdc;
	if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 ||
		p == 138 || p == 139 || p == 134 || p == 135)
		{
		s.iChromaFormatIdc = b.Ue();
		if (s.iChromaFormatIdc == 3)
			s.iSeparateColourPlane = b.Flag();
		s.iBitDepthLuma = b.Ue() + 8;
		s.iBitDepthChroma = b.Ue() + 8;
		s.iQpprimeYZeroTransformBypass = b.Flag();
		s.iSeqScalingMatrixPresent = b.Flag();
		if (s.iSeqScalingMatrixPresent)
			ScalingMatrix(b, s.iChromaFormatIdc != 3 ? 8 : 12, s.iScaling4x4, s.iScaling8x8, NULL, NULL);
		}
	s.iLog2MaxFrameNum = b.Ue() + 4;
	s.iPicOrderCntType = b.Ue();
	if (s.iPicOrderCntType == 0)
		s.iLog2MaxPocLsb = b.Ue() + 4;
	else if (s.iPicOrderCntType == 1)
		{
		s.iDeltaPicOrderAlwaysZero = b.Flag();
		s.iOffsetForNonRefPic = b.Se();
		s.iOffsetForTopToBottomField = b.Se();
		s.iNumRefFramesInPocCycle = b.Ue();
		if (s.iNumRefFramesInPocCycle > 255)
			return false;
		for (uint32_t i = 0; i < s.iNumRefFramesInPocCycle; ++i)
			s.iOffsetForRefFrame[i] = b.Se();
		}
	s.iMaxNumRefFrames = b.Ue();
	s.iGapsInFrameNumAllowed = b.Flag();
	s.iPicWidthInMbs = b.Ue() + 1;
	s.iPicHeightInMapUnits = b.Ue() + 1;
	s.iFrameMbsOnly = b.Flag();
	if (!s.iFrameMbsOnly)
		s.iMbAdaptiveFrameField = b.Flag();
	s.iDirect8x8Inference = b.Flag();
	s.iFrameCropping = b.Flag();
	if (s.iFrameCropping)
		{
		s.iCropLeft = b.Ue();
		s.iCropRight = b.Ue();
		s.iCropTop = b.Ue();
		s.iCropBottom = b.Ue();
		}
	s.iVuiPresent = b.Flag();				// VUI not needed by the decoder
	if (b.Error() || s.iLog2MaxFrameNum > 16 || s.iLog2MaxPocLsb > 16 || s.iPicOrderCntType > 2)
		return false;
	s.iValid = true;
	iSps[s.iSpsId] = s;
	return true;
	}

// 7.3.2.2
bool TParser::ParsePps(const uint8_t* aNal, size_t aLen)
	{
	const size_t n = Unescape(aNal + 1, aLen - 1 < sizeof(iRbsp) ? aLen - 1 : sizeof(iRbsp), iRbsp);
	TBitReader b(iRbsp, n);
	TPps q;
	memset(&q, 0, sizeof(q));
	q.iPpsId = b.Ue();
	q.iSpsId = b.Ue();
	const TSps* s = Sps(q.iSpsId);
	if (q.iPpsId >= KMaxPps || !s)
		return false;
	q.iEntropyCodingMode = b.Flag();
	q.iBottomFieldPicOrderInFramePresent = b.Flag();
	q.iNumSliceGroups = b.Ue() + 1;
	if (q.iNumSliceGroups > 1)
		return false;						// FMO (Baseline extended feature): not supported by the VE path
	q.iNumRefIdxL0DefaultActive = b.Ue() + 1;
	q.iNumRefIdxL1DefaultActive = b.Ue() + 1;
	q.iWeightedPred = b.Flag();
	q.iWeightedBipredIdc = b.U(2);
	q.iPicInitQp = 26 + b.Se();
	q.iPicInitQs = 26 + b.Se();
	q.iChromaQpIndexOffset = b.Se();
	q.iDeblockingFilterControlPresent = b.Flag();
	q.iConstrainedIntraPred = b.Flag();
	q.iRedundantPicCntPresent = b.Flag();
	// without a PPS matrix the SPS's (Flat_16 if the SPS has none) applies
	memcpy(q.iScaling4x4, s->iScaling4x4, sizeof(q.iScaling4x4));
	memcpy(q.iScaling8x8, s->iScaling8x8, sizeof(q.iScaling8x8));
	q.iSecondChromaQpIndexOffset = q.iChromaQpIndexOffset;
	if (b.MoreRbspData())
		{
		q.iTransform8x8Mode = b.Flag();
		q.iPicScalingMatrixPresent = b.Flag();
		if (q.iPicScalingMatrixPresent)
			{
			// fall-back rule B: the sequence-level lists if the SPS has a matrix, else rule A (the defaults)
			const bool seq = s->iSeqScalingMatrixPresent;
			ScalingMatrix(b, 6 + ((s->iChromaFormatIdc != 3) ? 2 : 6) * (q.iTransform8x8Mode ? 1 : 0),
				q.iScaling4x4, q.iScaling8x8, seq ? s->iScaling4x4 : NULL, seq ? s->iScaling8x8 : NULL);
			}
		q.iSecondChromaQpIndexOffset = b.Se();
		}
	if (b.Error())
		return false;
	q.iValid = true;
	iPps[q.iPpsId] = q;
	return true;
	}

// 7.3.3.1
static bool RefPicListMod(TBitReader& aB, TSliceHeader& aSh, int aList)
	{
	aSh.iModCount[aList] = 0;
	if (!aB.Flag())
		return true;
	for (;;)
		{
		if (aSh.iModCount[aList] > KMaxRefIdx)
			return false;
		TRefPicListMod& m = aSh.iMod[aList][aSh.iModCount[aList]];
		m.iIdc = aB.Ue();
		m.iValue = 0;
		if (m.iIdc == 3)
			return true;
		if (m.iIdc > 5 || aB.Error())
			return false;
		m.iValue = aB.Ue();
		++aSh.iModCount[aList];
		}
	}

// 7.3.3.2
static bool PredWeightTable(TBitReader& aB, TSliceHeader& aSh, bool aChroma)
	{
	TPredWeightTable& w = aSh.iPwt;
	w.iLumaLog2Denom = aB.Ue();
	w.iChromaLog2Denom = aChroma ? aB.Ue() : 0;
	const int lists = aSh.iSliceType == ESliceB ? 2 : 1;
	for (int l = 0; l < lists; ++l)
		{
		const uint32_t n = l ? aSh.iNumRefIdxL1Active : aSh.iNumRefIdxL0Active;
		for (uint32_t i = 0; i < n && i < KMaxRefIdx; ++i)
			{
			w.iL[l][i].iLumaWeight = (int16_t)(1 << w.iLumaLog2Denom);
			w.iL[l][i].iLumaOffset = 0;
			w.iL[l][i].iLumaFlag = aB.Flag();
			if (w.iL[l][i].iLumaFlag)
				{
				w.iL[l][i].iLumaWeight = (int16_t)aB.Se();
				w.iL[l][i].iLumaOffset = (int16_t)aB.Se();
				}
			for (int c = 0; c < 2; ++c)
				{
				w.iL[l][i].iChromaWeight[c] = (int16_t)(1 << w.iChromaLog2Denom);
				w.iL[l][i].iChromaOffset[c] = 0;
				}
			if (aChroma)
				{
				w.iL[l][i].iChromaFlag = aB.Flag();
				if (w.iL[l][i].iChromaFlag)
					for (int c = 0; c < 2; ++c)
						{
						w.iL[l][i].iChromaWeight[c] = (int16_t)aB.Se();
						w.iL[l][i].iChromaOffset[c] = (int16_t)aB.Se();
						}
				}
			}
		}
	return !aB.Error();
	}

// 7.3.3.3
static bool DecRefPicMarking(TBitReader& aB, TSliceHeader& aSh)
	{
	TDecRefPicMarking& m = aSh.iMarking;
	m.iCount = 0;
	if (aSh.iNalType == ENalIdr)
		{
		m.iNoOutputOfPriorPics = aB.Flag();
		m.iLongTermReference = aB.Flag();
		return !aB.Error();
		}
	m.iAdaptive = aB.Flag();
	if (!m.iAdaptive)
		return true;
	for (;;)
		{
		if (m.iCount >= 32)
			return false;
		const uint32_t op = aB.Ue();
		if (op == 0)
			return !aB.Error();
		if (op > 6 || aB.Error())
			return false;
		m.iOps[m.iCount].iOp = op;
		if (op == 1 || op == 3)
			m.iOps[m.iCount].iDiffPicNumsMinus1 = aB.Ue();
		if (op == 2)
			m.iOps[m.iCount].iLongTermPicNum = aB.Ue();
		if (op == 3 || op == 6)
			m.iOps[m.iCount].iLongTermFrameIdx = aB.Ue();
		if (op == 4)
			m.iOps[m.iCount].iMaxLongTermFrameIdxPlus1 = aB.Ue();
		++m.iCount;
		}
	}

bool TParser::ParseSliceHeader(const uint8_t* aNal, size_t aLen, TSliceHeader& aSh)
	{
	memset(&aSh, 0, sizeof(aSh));
	aSh.iNalRefIdc = (aNal[0] >> 5) & 3;
	aSh.iNalType = aNal[0] & 0x1F;
	if (aSh.iNalType != ENalSlice && aSh.iNalType != ENalIdr)
		return false;
	const size_t n = Unescape(aNal + 1, aLen - 1 < sizeof(iRbsp) ? aLen - 1 : sizeof(iRbsp), iRbsp);
	TBitReader b(iRbsp, n);
	aSh.iFirstMbInSlice = b.Ue();
	aSh.iSliceType = b.Ue() % 5;
	aSh.iPpsId = b.Ue();
	const TPps* q = Pps(aSh.iPpsId);
	const TSps* s = q ? Sps(q->iSpsId) : NULL;
	if (!s)
		return false;
	if (s->iSeparateColourPlane)
		aSh.iColourPlaneId = b.U(2);
	aSh.iFrameNum = b.U(s->iLog2MaxFrameNum);
	if (!s->iFrameMbsOnly)
		{
		aSh.iFieldPic = b.Flag();
		if (aSh.iFieldPic)
			aSh.iBottomField = b.Flag();
		}
	if (aSh.iNalType == ENalIdr)
		aSh.iIdrPicId = b.Ue();
	if (s->iPicOrderCntType == 0)
		{
		aSh.iPicOrderCntLsb = b.U(s->iLog2MaxPocLsb);
		if (q->iBottomFieldPicOrderInFramePresent && !aSh.iFieldPic)
			aSh.iDeltaPicOrderCntBottom = b.Se();
		}
	if (s->iPicOrderCntType == 1 && !s->iDeltaPicOrderAlwaysZero)
		{
		aSh.iDeltaPicOrderCnt[0] = b.Se();
		if (q->iBottomFieldPicOrderInFramePresent && !aSh.iFieldPic)
			aSh.iDeltaPicOrderCnt[1] = b.Se();
		}
	if (q->iRedundantPicCntPresent)
		aSh.iRedundantPicCnt = b.Ue();
	const uint32_t t = aSh.iSliceType;
	if (t == ESliceB)
		aSh.iDirectSpatialMvPred = b.Flag();
	aSh.iNumRefIdxL0Active = q->iNumRefIdxL0DefaultActive;
	aSh.iNumRefIdxL1Active = q->iNumRefIdxL1DefaultActive;
	if (t == ESliceP || t == ESliceSP || t == ESliceB)
		{
		aSh.iNumRefIdxActiveOverride = b.Flag();
		if (aSh.iNumRefIdxActiveOverride)
			{
			aSh.iNumRefIdxL0Active = b.Ue() + 1;
			if (t == ESliceB)
				aSh.iNumRefIdxL1Active = b.Ue() + 1;
			}
		}
	if (aSh.iNumRefIdxL0Active > KMaxRefIdx || aSh.iNumRefIdxL1Active > KMaxRefIdx)
		return false;
	if (t != ESliceI && t != ESliceSI)
		{
		if (!RefPicListMod(b, aSh, 0))
			return false;
		if (t == ESliceB && !RefPicListMod(b, aSh, 1))
			return false;
		}
	const bool chroma = s->iChromaFormatIdc != 0 && !s->iSeparateColourPlane;
	if ((q->iWeightedPred && (t == ESliceP || t == ESliceSP)) || (q->iWeightedBipredIdc == 1 && t == ESliceB))
		if (!PredWeightTable(b, aSh, chroma))
			return false;
	if (aSh.iNalRefIdc != 0 && !DecRefPicMarking(b, aSh))
		return false;
	if (q->iEntropyCodingMode && t != ESliceI && t != ESliceSI)
		aSh.iCabacInitIdc = b.Ue();
	aSh.iSliceQpDelta = b.Se();
	if (t == ESliceSP || t == ESliceSI)
		{
		if (t == ESliceSP)
			aSh.iSpForSwitch = b.Flag();
		aSh.iSliceQsDelta = b.Se();
		}
	if (q->iDeblockingFilterControlPresent)
		{
		aSh.iDisableDeblockingFilterIdc = b.Ue();
		if (aSh.iDisableDeblockingFilterIdc != 1)
			{
			aSh.iSliceAlphaC0OffsetDiv2 = b.Se();
			aSh.iSliceBetaOffsetDiv2 = b.Se();
			}
		}
	if (b.Error())
		return false;
	aSh.iHeaderBitsRbsp = b.BitPos();
	// +8: the NAL header byte, which the escaped position counts from
	aSh.iHeaderBitsEscaped = 8 + EscapedBitPos(aNal + 1, aLen - 1, aSh.iHeaderBitsRbsp);
	return true;
	}

} // namespace h264
