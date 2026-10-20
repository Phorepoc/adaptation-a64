/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// h264_parser.h -- OURS (VDU rung V3): the CPU half of H.264 decoding for the stateless A64
// Video Engine -- NAL unit splitting, emulation-prevention removal, and the SPS / PPS / slice header fields the
// hardware needs (ITU-T H.264 (08/2021) sections 7.3.2.1.1, 7.3.2.2, 7.3.3). Written from the specification.
// Plain C++ with no Symbian dependency, so the host test (tests/) can compile it unmodified.
#ifndef H264_PARSER_H
#define H264_PARSER_H

#include <stdint.h>
#include <stddef.h>

namespace h264
{

enum { KMaxSps = 32, KMaxPps = 256, KMaxRefIdx = 32, KMaxSliceGroups = 8 };

enum TNalType
	{
	ENalSlice = 1, ENalSliceA = 2, ENalIdr = 5, ENalSei = 6, ENalSps = 7, ENalPps = 8, ENalAud = 9, ENalEndSeq = 10,
	ENalEndStream = 11, ENalFiller = 12
	};

enum TSliceType { ESliceP = 0, ESliceB = 1, ESliceI = 2, ESliceSP = 3, ESliceSI = 4 };

// Exp-Golomb bit reader over an RBSP (emulation prevention already removed).
class TBitReader
	{
public:
	TBitReader(const uint8_t* aData, size_t aLen) : iData(aData), iLen(aLen), iPos(0), iError(false) {}
	uint32_t U(int aBits);
	bool Flag() { return U(1) != 0; }
	uint32_t Ue();
	int32_t Se();
	size_t BitPos() const { return iPos; }
	size_t BitsLeft() const { return iPos < iLen * 8 ? iLen * 8 - iPos : 0; }
	bool Error() const { return iError; }
	bool MoreRbspData() const;
private:
	const uint8_t* iData;
	size_t iLen, iPos;
	bool iError;
	};

struct TSps
	{
	bool iValid;
	uint8_t iProfileIdc, iLevelIdc, iConstraintFlags;
	uint32_t iSpsId;
	uint32_t iChromaFormatIdc;			// 1 = 4:2:0
	bool iSeparateColourPlane;
	uint32_t iBitDepthLuma, iBitDepthChroma;	// minus 8 already added back: 8 for 8-bit
	bool iQpprimeYZeroTransformBypass;
	bool iSeqScalingMatrixPresent;
	uint8_t iScaling4x4[6][16];			// zigzag order as coded (defaults filled in)
	uint8_t iScaling8x8[6][64];
	uint32_t iLog2MaxFrameNum;
	uint32_t iPicOrderCntType;
	uint32_t iLog2MaxPocLsb;
	bool iDeltaPicOrderAlwaysZero;
	int32_t iOffsetForNonRefPic, iOffsetForTopToBottomField;
	uint32_t iNumRefFramesInPocCycle;
	int32_t iOffsetForRefFrame[256];
	uint32_t iMaxNumRefFrames;
	bool iGapsInFrameNumAllowed;
	uint32_t iPicWidthInMbs, iPicHeightInMapUnits;
	bool iFrameMbsOnly, iMbAdaptiveFrameField, iDirect8x8Inference;
	bool iFrameCropping;
	uint32_t iCropLeft, iCropRight, iCropTop, iCropBottom;
	bool iVuiPresent;
	// derived
	uint32_t FrameHeightInMbs() const { return (2 - (iFrameMbsOnly ? 1 : 0)) * iPicHeightInMapUnits; }
	uint32_t Width() const { return iPicWidthInMbs * 16; }
	uint32_t Height() const { return FrameHeightInMbs() * 16; }
	};

struct TPps
	{
	bool iValid;
	uint32_t iPpsId, iSpsId;
	bool iEntropyCodingMode;			// CABAC
	bool iBottomFieldPicOrderInFramePresent;
	uint32_t iNumSliceGroups;
	uint32_t iNumRefIdxL0DefaultActive, iNumRefIdxL1DefaultActive;	// minus1 added back
	bool iWeightedPred;
	uint32_t iWeightedBipredIdc;
	int32_t iPicInitQp, iPicInitQs, iChromaQpIndexOffset;
	bool iDeblockingFilterControlPresent, iConstrainedIntraPred, iRedundantPicCntPresent;
	bool iTransform8x8Mode;
	bool iPicScalingMatrixPresent;
	uint8_t iScaling4x4[6][16];
	uint8_t iScaling8x8[6][64];
	int32_t iSecondChromaQpIndexOffset;
	};

struct TRefPicListMod
	{
	uint32_t iIdc;						// modification_of_pic_nums_idc (3 = end)
	uint32_t iValue;					// abs_diff_pic_num_minus1 or long_term_pic_num
	};

struct TDecRefPicMarking
	{
	bool iNoOutputOfPriorPics, iLongTermReference;	// IDR
	bool iAdaptive;
	uint32_t iCount;
	struct { uint32_t iOp, iDiffPicNumsMinus1, iLongTermPicNum, iLongTermFrameIdx, iMaxLongTermFrameIdxPlus1; } iOps[32];
	};

struct TPredWeightTable
	{
	uint32_t iLumaLog2Denom, iChromaLog2Denom;
	struct { bool iLumaFlag, iChromaFlag; int16_t iLumaWeight, iLumaOffset, iChromaWeight[2], iChromaOffset[2]; }
		iL[2][KMaxRefIdx];
	};

struct TSliceHeader
	{
	uint32_t iNalType, iNalRefIdc;
	uint32_t iFirstMbInSlice;
	uint32_t iSliceType;				// 0..4 (types 5..9 folded in)
	uint32_t iPpsId;
	uint32_t iColourPlaneId;
	uint32_t iFrameNum;
	bool iFieldPic, iBottomField;
	uint32_t iIdrPicId;
	uint32_t iPicOrderCntLsb;
	int32_t iDeltaPicOrderCntBottom;
	int32_t iDeltaPicOrderCnt[2];
	uint32_t iRedundantPicCnt;
	bool iDirectSpatialMvPred;
	bool iNumRefIdxActiveOverride;
	uint32_t iNumRefIdxL0Active, iNumRefIdxL1Active;	// minus1 added back
	uint32_t iModCount[2];
	TRefPicListMod iMod[2][KMaxRefIdx + 1];
	TPredWeightTable iPwt;
	TDecRefPicMarking iMarking;
	uint32_t iCabacInitIdc;
	int32_t iSliceQpDelta;
	bool iSpForSwitch;
	int32_t iSliceQsDelta;
	uint32_t iDisableDeblockingFilterIdc;
	int32_t iSliceAlphaC0OffsetDiv2, iSliceBetaOffsetDiv2;
	// where the slice data (macroblock layer) starts, in bits from the start of the NAL unit payload as it is in the
	// ESCAPED byte stream (the VE reads the escaped stream itself) -- and the header size in RBSP bits
	size_t iHeaderBitsRbsp;
	size_t iHeaderBitsEscaped;
	};

// Split a byte stream (Annex B start codes) into NAL units; returns the next unit's [start, end) after the start code,
// or false at the end. aPos is updated.
bool NextNal(const uint8_t* aBuf, size_t aLen, size_t& aPos, size_t& aNalStart, size_t& aNalEnd);

// Remove emulation prevention bytes (00 00 03 -> 00 00); returns the RBSP length. aOut must hold aLen bytes.
size_t Unescape(const uint8_t* aNal, size_t aLen, uint8_t* aOut);

// Escaped-stream bit position for an RBSP bit position (counts the 03 bytes removed before it).
size_t EscapedBitPos(const uint8_t* aNal, size_t aLen, size_t aRbspBitPos);

class TParser
	{
public:
	TParser();
	// aNal = one NAL unit including its 1-byte header, escaped; returns false on a malformed unit
	bool ParseSps(const uint8_t* aNal, size_t aLen);
	bool ParsePps(const uint8_t* aNal, size_t aLen);
	bool ParseSliceHeader(const uint8_t* aNal, size_t aLen, TSliceHeader& aSh);
	const TSps* Sps(uint32_t aId) const { return aId < KMaxSps && iSps[aId].iValid ? &iSps[aId] : NULL; }
	const TPps* Pps(uint32_t aId) const { return aId < KMaxPps && iPps[aId].iValid ? &iPps[aId] : NULL; }
private:
	TSps iSps[KMaxSps];
	TPps iPps[KMaxPps];
	uint8_t iRbsp[4096];				// headers only: an SPS/PPS/slice header fits easily
	};

} // namespace h264

#endif
