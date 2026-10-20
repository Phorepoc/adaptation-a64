// ve_rphevde.h -- OURS (VDU V4): the Symbian-side adapters from the plain-C++ VE decoder cores (ve_h264, ve_mpeg2) to the
// phevde driver: MVeJob -> one RPheVde::Run job, MVeMemory -> a bump allocator over the committed arena chunk.
// Shared by phevdetest, phevdectest and the phevdec DevVideo plugin.
#ifndef VE_RPHEVDE_H
#define VE_RPHEVDE_H

#include <e32std.h>
#include "rphevde.h"
#include "ve_h264.h"

static const TUint32 KDramBus = 0x40000000;		// the VE addresses DRAM from 0 (sunxi mbus offset)

class TVeJobAdapter : public h264::MVeJob
	{
public:
	TVeJobAdapter(RPheVde& aVe) : iMs(0), iMaxWrites(0), iVe(aVe), iJob(new TPheVdeJob) {}
	~TVeJobAdapter() { delete iJob; }
	void Begin() { iJob->iNumWrites = 0; }
	void Write(uint32_t aOff, uint32_t aVal) { Add(aOff, aVal); }
	void PollClear(uint32_t aOff, uint32_t aMask) { Add(aOff | KPheVdePoll, aMask); }
	int Run(uint32_t aStatusOff, uint32_t aStatusMask, uint32_t aIntCtrlOff, uint32_t aIntCtrlMask)
		{
		if (iJob->iNumWrites > KPheVdeMaxWrites)
			return KErrOverflow;
		iJob->iStatusOff = aStatusOff;
		iJob->iStatusMask = aStatusMask;
		iJob->iIntCtrlOff = aIntCtrlOff;
		iJob->iIntCtrlMask = aIntCtrlMask;
		TRequestStatus st;
		const TUint32 t0 = User::NTickCount();
		iVe.Run(*iJob, st);
		User::WaitForRequest(st);
		iMs += User::NTickCount() - t0;
		iMaxWrites = Max(iMaxWrites, iJob->iNumWrites);
		return st.Int();
		}
	TUint32 iMs;
	TInt iMaxWrites;
private:
	void Add(TUint32 aOff, TUint32 aVal)
		{
		if (iJob->iNumWrites < KPheVdeMaxWrites)
			{
			iJob->iWrites[iJob->iNumWrites].iOff = aOff;
			iJob->iWrites[iJob->iNumWrites].iVal = aVal;
			}
		++iJob->iNumWrites;
		}
	RPheVde& iVe;
	TPheVdeJob* iJob;
	};

// a bump allocator over the arena (offset 0 .. aReserve = the coded-data buffer, 1 MB by default; aIndex = the arena offset)
class TVeArenaMem : public h264::MVeMemory
	{
public:
	TVeArenaMem(RChunk& aArena, TUint32 aPhys, TInt aSize, TInt aReserve = 1 << 20) : iArena(aArena), iPhys(aPhys), iSize(aSize), iNext(aReserve) {}
	uint32_t Alloc(uint32_t aSize, uint32_t aAlign, int& aIndex)
		{
		const TInt at = (iNext + (TInt)aAlign - 1) & ~((TInt)aAlign - 1);
		if (at + (TInt)aSize > iSize)
			return 0;
		iNext = at + (TInt)aSize;
		aIndex = at;
		return iPhys + at - KDramBus;
		}
	TUint8* Cpu(TUint32 aBus) const { return iArena.Base() + (aBus + KDramBus - iPhys); }
	RChunk& iArena;
	TUint32 iPhys;
	TInt iSize, iNext;
	};

#endif
