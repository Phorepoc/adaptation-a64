// ve_mpeg2.cpp -- OURS: see ve_mpeg2.h. Moved (logic unchanged) from phevdetest.cpp (VDU V1/V2).
#include "ve_mpeg2.h"
#include <string.h>

namespace mpeg2
{

namespace
{
class TBits
	{
public:
	TBits(const uint8_t* aP, int aLen) : iP(aP), iLen(aLen), iPos(0) {}
	uint32_t Get(int aN)
		{
		uint32_t v = 0;
		for (int i = 0; i < aN; ++i, ++iPos)
			v = (v << 1) | (iPos / 8 < iLen ? (iP[iPos / 8] >> (7 - iPos % 8)) & 1 : 0);
		return v;
		}
private:
	const uint8_t* iP;
	int iLen, iPos;
	};

const uint8_t KZigzag[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31,
	39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
// the default intra matrix (6.3.11), raster order
const uint8_t KDefIntra[64] = { 8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
	34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58, 26, 27, 29, 34,
	38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83 };
}

TParser::TParser(const uint8_t* aBuf, size_t aLen) : iS(aBuf), iLen((int)aLen), iPos(0), iGopBase(0), iInGop(0)
	{
	memset(&iSeq, 0, sizeof(iSeq));
	DefaultMatrices();
	}

int TParser::NextStart(int aFrom) const
	{
	for (int i = aFrom; i + 3 < iLen; ++i)
		if (iS[i] == 0 && iS[i + 1] == 0 && iS[i + 2] == 1)
			return i;
	return -1;
	}

void TParser::DefaultMatrices()
	{
	for (int i = 0; i < 64; ++i)
		{
		iSeq.iIntraQ[i] = KDefIntra[KZigzag[i]];
		iSeq.iNonIntraQ[i] = 16;
		}
	}

void TParser::Header(int aAt, TPic& aP, bool aInPicture)
	{
	const uint8_t code = iS[aAt + 3];
	TBits b(iS + aAt + 4, iLen - aAt - 4);
	if (code == 0xB3)
		{
		iSeq.iWidth = b.Get(12);
		iSeq.iHeight = b.Get(12);
		b.Get(4 + 4 + 18 + 1 + 10 + 1);
		DefaultMatrices();				// a sequence header without matrices resets them to the defaults
		if (b.Get(1))
			for (int i = 0; i < 64; ++i)
				iSeq.iIntraQ[i] = (uint8_t)b.Get(8);
		if (b.Get(1))
			for (int i = 0; i < 64; ++i)
				iSeq.iNonIntraQ[i] = (uint8_t)b.Get(8);
		}
	else if (code == 0xB8)				// GOP: temporal references restart
		{
		iGopBase += iInGop;
		iInGop = 0;
		}
	else if (code == 0xB5)
		{
		const int id = b.Get(4);
		if (id == 1)					// sequence extension: the size extension bits
			{
			b.Get(8 + 1 + 2);
			iSeq.iWidth |= b.Get(2) << 12;
			iSeq.iHeight |= b.Get(2) << 12;
			}
		else if (id == 3)				// quant matrix extension
			{
			TPic& t = aInPicture ? aP : iSeq;
			if (b.Get(1))
				for (int i = 0; i < 64; ++i)
					t.iIntraQ[i] = (uint8_t)b.Get(8);
			if (b.Get(1))
				for (int i = 0; i < 64; ++i)
					t.iNonIntraQ[i] = (uint8_t)b.Get(8);
			}
		else if (id == 8 && aInPicture)	// picture coding extension
			{
			aP.iFCode[0][0] = b.Get(4), aP.iFCode[0][1] = b.Get(4), aP.iFCode[1][0] = b.Get(4), aP.iFCode[1][1] = b.Get(4);
			aP.iDcPrec = b.Get(2);
			aP.iStructure = b.Get(2);
			aP.iTff = b.Get(1);
			aP.iFpfd = b.Get(1);
			aP.iConceal = b.Get(1);
			aP.iQScale = b.Get(1);
			aP.iIntraVlc = b.Get(1);
			aP.iAltScan = b.Get(1);
			}
		}
	}

bool TParser::Next(TPic& aP)
	{
	int at = NextStart(iPos);
	for (; at >= 0 && iS[at + 3] != 0x00; at = NextStart(at + 3))	// up to the picture start code
		Header(at, aP, false);
	if (at < 0)
		return false;
	aP = iSeq;
	TBits b(iS + at + 4, iLen - at - 4);
	aP.iTemporalRef = b.Get(10);
	aP.iType = b.Get(3);
	aP.iDisplay = iGopBase + aP.iTemporalRef;
	++iInGop;
	aP.iFCode[0][0] = aP.iFCode[0][1] = aP.iFCode[1][0] = aP.iFCode[1][1] = 15;
	aP.iDcPrec = 0, aP.iStructure = 3, aP.iTff = 0, aP.iFpfd = 1, aP.iConceal = 0, aP.iQScale = 0, aP.iIntraVlc = 0,
		aP.iAltScan = 0;
	aP.iPicAt = at;
	aP.iSliceAt = aP.iSliceEnd = -1;
	aP.iTerminated = false;
	for (at = NextStart(at + 3); at >= 0; at = NextStart(at + 3))
		{
		const uint8_t code = iS[at + 3];
		if (code >= 0x01 && code <= 0xAF)
			{
			if (aP.iSliceAt < 0)
				aP.iSliceAt = at;
			continue;
			}
		if (aP.iSliceAt >= 0)			// the first non-slice start code after the slices ends the picture
			break;
		Header(at, aP, true);
		}
	aP.iSliceEnd = at >= 0 ? at : iLen;
	aP.iTerminated = at >= 0;
	iPos = aP.iSliceEnd;
	return aP.iSliceAt >= 0 && aP.iWidth && aP.iHeight;
	}

// ---------------------------------------------------------------------------------------------------------------

TVeMpeg2::TVeMpeg2(h264::MVeJob& aJob, h264::MVeMemory& aMem, int aFrames)
	: iHeld(NULL), iHeldCtx(NULL), iJob(aJob), iMem(aMem), iStride(0), iH16(0), iOlder(-1), iNewer(-1),
	iFrames(aFrames < 3 ? 3 : aFrames > KMaxFrames ? (int)KMaxFrames : aFrames), iAllocW(0), iAllocH(0)
	{
	for (int i = 0; i < KMaxFrames; ++i)
		iBus[i] = 0, iMemIdx[i] = 0;
	}

int TVeMpeg2::Decode(const TPic& p, uint32_t aEsBus, int& aStatus)
	{
	iStride = (p.iWidth + 15) & ~15;
	iH16 = (p.iHeight + 15) & ~15;
	const int esLen = p.iSliceEnd - p.iSliceAt;
	if (esLen <= 0 || p.iStructure != 3 || p.iType < EPicI || p.iType > EPicB)
		return -1;
	if (iAllocW != p.iWidth || iAllocH != p.iHeight)		// (re)allocate the frame buffers for a new size
		{
		for (int i = 0; i < iFrames; ++i)
			{
			iBus[i] = iMem.Alloc((uint32_t)FrameSize(), 4096, iMemIdx[i]);
			if (!iBus[i])
				return -3;
			}
		iAllocW = p.iWidth, iAllocH = p.iHeight;
		iOlder = iNewer = -1;
		}
	int out = 0;							// a buffer that is neither a reference nor held by the caller
	while (out < iFrames && (out == iOlder || out == iNewer || (iHeld && iHeld(iHeldCtx, out))))
		++out;
	if (out >= iFrames)
		return -4;
	int fwd = -1, bwd = -1;
	if (p.iType == EPicP)
		fwd = iNewer;
	else if (p.iType == EPicB)
		fwd = iOlder, bwd = iNewer;
	if ((p.iType != EPicI && fwd < 0) || (p.iType == EPicB && bwd < 0))
		return -2;							// an open GOP's leading B frames: no references yet
	const uint32_t outBus = iBus[out];
	const uint32_t luma = (uint32_t)LumaSize();

	h264::MVeJob& j = iJob;
	j.Begin();
	j.Write(0x000, 0x00130000);					// VE_MODE: MPEG engine, DDR BW_128, REC 2 MB
	j.Write(0x0EC, 0x40);						// primary output: linear NV12
	j.Write(0x0C8, ((iStride / 2) << 16) | iStride);
	j.Write(0x0C4, luma / 4);
	for (int i = 0; i < 64; ++i)
		j.Write(0x180, (1u << 14) | (i << 8) | p.iIntraQ[i]);
	for (int i = 0; i < 64; ++i)
		j.Write(0x180, (i << 8) | p.iNonIntraQ[i]);
	j.Write(0x100, (p.iType << 28) | (p.iFCode[0][0] << 24) | (p.iFCode[0][1] << 20) | (p.iFCode[1][0] << 16)
		| (p.iFCode[1][1] << 12) | (p.iDcPrec << 10) | (p.iStructure << 8) | (p.iTff << 7) | (p.iFpfd << 6)
		| (p.iConceal << 5) | (p.iQScale << 4) | (p.iIntraVlc << 3) | (p.iAltScan << 2));
	j.Write(0x108, (((p.iWidth + 15) / 16) << 8) | ((p.iHeight + 15) / 16));
	j.Write(0x10C, (iStride << 16) | iH16);
	j.Write(0x150, fwd < 0 ? 0 : iBus[fwd]);
	j.Write(0x154, fwd < 0 ? 0 : iBus[fwd] + luma);
	j.Write(0x158, bwd < 0 ? 0 : iBus[bwd]);
	j.Write(0x15C, bwd < 0 ? 0 : iBus[bwd] + luma);
	j.Write(0x148, outBus);
	j.Write(0x14C, outBus + luma);
	j.Write(0x12C, 0);							// VLD bit offset: the buffer starts at the slice start code
	j.Write(0x130, (uint32_t)esLen * 8);
	j.Write(0x128, (aEsBus & 0x0FFFFFF0) | (aEsBus >> 28) | 0x70000000);	// first | last | valid
	j.Write(0x134, aEsBus + (uint32_t)esLen);
	j.Write(0x110, 0);
	j.Write(0x1C4, 0);
	j.Write(0x1C8, 0);
	j.Write(0x114, 0x800000B8);					// finish + error + data-request IRQs, MC cache
	j.Write(0x118, 0x8200000F);					// trigger: MPEG-2 VLD decode
	aStatus = j.Run(0x11C, 0x7, 0x114, 0x38);
	if (p.iType != EPicB)
		{
		iOlder = iNewer;
		iNewer = out;
		}
	return out;
	}

} // namespace mpeg2
