/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// ve_h264.h -- OURS (VDU rung V3): H.264 decoding on the stateless A64 Video Engine.
// The CPU side the VE needs: picture order count, the
// decoded picture buffer with reference marking (8.2.5: sliding window + MMCO), reference picture lists (8.2.4:
// initialisation + modification), and one VE job per slice (register writes, SRAM tables, the header skip, the trigger).
// Frame pictures (progressive, MBAFF flag passed through) and field pictures (PAFF, field_pic_flag = 1): both fields of a
// frame share one frame buffer + frame-list slot; per-field POC (8.2.1), per-field reference marking (8.2.5), field
// picture numbers + field list initialisation (8.2.4.2.2/.4/.5) and modification (8.2.4.3).
// Plain C++ with no Symbian dependency: the VE and the memory come through the two interfaces below.
#ifndef VE_H264_H
#define VE_H264_H

#include "h264_parser.h"

namespace h264
{

// One VE job: register writes and polls in order; Run() starts it (the last write is the trigger) and returns the
// engine's status register value (or a negative error).
class MVeJob
	{
public:
	virtual void Begin() = 0;
	virtual void Write(uint32_t aOff, uint32_t aVal) = 0;
	virtual void PollClear(uint32_t aOff, uint32_t aMask) = 0;		// wait until (reg & mask) == 0
	virtual int Run(uint32_t aStatusOff, uint32_t aStatusMask, uint32_t aIntCtrlOff, uint32_t aIntCtrlMask) = 0;
	};

// VE-visible memory (bus addresses = physical - 0x40000000 on the A64).
class MVeMemory
	{
public:
	// a physically contiguous block; returns its bus address, 0 on failure; aIndex = an id for the caller's mapping
	virtual uint32_t Alloc(uint32_t aSize, uint32_t aAlign, int& aIndex) = 0;
	};

struct TPicture
	{
	int iBuffer;					// the MVeMemory index of its NV12 buffer
	uint32_t iLumaBus, iChromaBus, iMvColBus, iMvColSize;
	int32_t iTopPoc, iBottomPoc, iPoc;
	uint32_t iFrameNum;
	int32_t iFrameNumWrap;
	bool iRef, iLongTerm;			// iRef: any field marked as a reference; iLongTerm: some field is long-term
	uint8_t iFieldRef[2];			// per field (0 top, 1 bottom): 0 unused for reference, 1 short-term, 2 long-term
	uint8_t iFields;				// fields decoded into this buffer: bit0 top, bit1 bottom (3 = a frame / a complete pair)
	uint8_t iPicType;				// frame-list word 2 >> 8: 0 frame, 1 field pictures, 2 MBAFF frame
	uint32_t iLongTermFrameIdx;
	int iSlot;						// frame-list slot 0..17 while in the DPB
	uint32_t iIdrPeriod;			// counts IDRs: display order = (period, POC)
	bool iMmco5;
	};

// a reference picture list entry: a frame (iParity 2) or one field of a frame (0 top, 1 bottom)
struct TRef
	{
	TPicture* iPic;
	uint8_t iParity;
	bool operator==(const TRef& a) const { return iPic == a.iPic && iParity == a.iParity; }
	};

class TVeH264
	{
public:
	enum { KMaxDpb = 16, KSlots = 18 };
	TVeH264(TParser& aParser, MVeJob& aJob, MVeMemory& aMem);
	// a NAL unit (escaped, no start code); for a slice, aBus = the bus address of that NAL copied into VE memory
	// (16-byte aligned). Returns <0 on an error. A decoded picture is reported by the callback when the NEXT
	// picture starts (or at Flush).
	int DecodeSlice(const uint8_t* aNal, size_t aLen, uint32_t aBus);
	void Flush();
	// true while a buffer (a frame's luma bus, its co-located motion buffer, the shared tables) is still needed by the
	// decoder: the picture in flight, the reference pictures, the per-stream tables. A caller that recycles memory may
	// reuse an allocation for which this is false and which it no longer holds itself (V4: the DevVideo plug-in).
	bool InUse(uint32_t aBus) const;
	// picture output, in DECODE order; display order is (iIdrPeriod, iPoc)
	void (*iOnPicture)(void* aCtx, const TPicture& aPic);
	void* iCtx;
	// statistics
	uint32_t iSlices, iPictures, iJobErrors, iLastStatus;
	uint32_t iWidth, iHeight, iStride, iAlignedHeight;
private:
	int StartPicture(const TSliceHeader& aSh, const TSps& aSps, const TPps& aPps);
	void FinishPicture();
	void Poc(const TSliceHeader& aSh, const TSps& aSps);
	void Marking(const TSliceHeader& aSh, const TSps& aSps);
	int BuildLists(const TSliceHeader& aSh, const TSps& aSps);
	int Modify(const TSliceHeader& aSh, const TSps& aSps, int aList, TRef* aRefs, uint32_t aCount);
	int FindEntry(uint32_t aLumaBus) const;
	int Job(const uint8_t* aNal, size_t aLen, uint32_t aBus, const TSliceHeader& aSh, const TSps& aSps,
		const TPps& aPps);
	int FreeSlot() const;
	TParser& iParser;
	MVeJob& iJob;
	MVeMemory& iMem;
	TPicture iDpb[KMaxDpb];			// reference pictures (iRef)
	uint32_t iDpbCount;
	TPicture iCur;
	bool iInPicture;
	TSliceHeader iCurSh;			// the first slice's header of the current picture
	TPicture iPend;					// a first field waiting for its second field (iHavePend)
	bool iHavePend;
	TRef iList[2][KMaxRefIdx];
	uint32_t iListLen[2];
	// POC state (8.2.1)
	int32_t iPrevPocMsb;
	uint32_t iPrevPocLsb;
	uint32_t iPrevFrameNum, iPrevFrameNumOffset;
	uint32_t iIdrPeriod;
	uint32_t iPicInfoBus, iNeighborBus;
	};

} // namespace h264

#endif
