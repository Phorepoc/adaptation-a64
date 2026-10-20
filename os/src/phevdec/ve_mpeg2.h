/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// ve_mpeg2.h -- OURS (VDU rung V1/V2, moved out of phevdetest for V4): MPEG-2 video
// (ISO/IEC 13818-2) on the stateless A64 Video Engine. The elementary-stream walker (sequence / GOP / picture headers)
// and the VE job (one per frame picture, two reference frames). Register facts: research/ve-mpeg2-regs.md.
// Plain C++ with no Symbian dependency: the VE and the memory come through h264::MVeJob / h264::MVeMemory
// (ve_h264.h), the same seams the H.264 decoder uses.
#ifndef VE_MPEG2_H
#define VE_MPEG2_H

#include "ve_h264.h"

namespace mpeg2
{

enum { EPicI = 1, EPicP = 2, EPicB = 3 };

struct TPic
	{
	int iWidth, iHeight;
	uint8_t iIntraQ[64], iNonIntraQ[64];	// bitstream (zigzag) order
	int iType;
	int iTemporalRef;
	int iDisplay;							// display index in the stream (GOP base + temporal reference)
	int iFCode[2][2];
	int iDcPrec, iStructure, iTff, iFpfd, iConceal, iQScale, iIntraVlc, iAltScan;
	int iPicAt;								// the picture start code
	int iSliceAt, iSliceEnd;				// [first slice start code, the next non-slice start code)
	bool iTerminated;						// iSliceEnd is a start code (false: the buffer ended inside this picture)
	};

// Walks an elementary stream picture by picture; sequence state (size, matrices) carries over. The buffer must
// outlive the parser.
class TParser
	{
public:
	TParser(const uint8_t* aBuf, size_t aLen);
	bool Next(TPic& aP);
	// continue at aPos in a (re)allocated or grown buffer; the sequence state (size, matrices, GOP) carries over
	void SetBuffer(const uint8_t* aBuf, size_t aLen, size_t aPos) { iS = aBuf; iLen = (int)aLen; iPos = (int)aPos; }
	size_t Pos() const { return (size_t)iPos; }
private:
	int NextStart(int aFrom) const;
	void DefaultMatrices();
	void Header(int aAt, TPic& aP, bool aInPicture);
	const uint8_t* iS;
	int iLen;
	int iPos;
	TPic iSeq;
	int iGopBase, iInGop;
	};

// One VE job per picture. The caller copies [iSliceAt, iSliceEnd) of the stream (plus 64 zero bytes) to VE memory and
// passes that block's bus address (16-byte aligned); the decoder allocates its four frame buffers from MVeMemory.
class TVeMpeg2
	{
public:
	enum { KFrames = 4, KMaxFrames = 12 };	// default: two references + B + spare
	TVeMpeg2(h264::MVeJob& aJob, h264::MVeMemory& aMem, int aFrames = KFrames);
	// Optional: a frame buffer the caller still holds (a delivered picture) is not reused for the next decode
	bool (*iHeld)(void* aCtx, int aIndex);
	void* iHeldCtx;
	// Returns the frame buffer index (>= 0), or <0: -1 unsupported, -2 no references yet (an open GOP's leading B
	// frames), -3 no memory, -4 every free-able buffer is held by the caller. aStatus = the VE status register value.
	int Decode(const TPic& aP, uint32_t aEsBus, int& aStatus);
	int Frames() const { return iFrames; }
	uint32_t FrameLumaBus(int aIndex) const { return iBus[aIndex]; }
	uint32_t FrameChromaBus(int aIndex) const { return iBus[aIndex] + (uint32_t)LumaSize(); }
	int FrameMemIndex(int aIndex) const { return iMemIdx[aIndex]; }		// the MVeMemory index of the frame
	int Stride() const { return iStride; }
	int LumaSize() const { return iStride * iH16; }
	int FrameSize() const { return (LumaSize() * 3 / 2 + 0xFFF) & ~0xFFF; }
	void Reset() { iOlder = iNewer = -1; }
private:
	h264::MVeJob& iJob;
	h264::MVeMemory& iMem;
	int iStride, iH16;
	int iOlder, iNewer;						// reference frame buffers (forward / backward for B)
	uint32_t iBus[KMaxFrames];
	int iMemIdx[KMaxFrames];
	int iFrames;
	int iAllocW, iAllocH;
	};

} // namespace mpeg2

#endif
