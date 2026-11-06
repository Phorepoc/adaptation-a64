/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// ve_enc.h -- OURS: the A64 Video Engine H.264 encoder, one picture per call. The register sequence is src/phevenctest2's
// as pheencbench measured it (360x640 in 1.2 ms at 402 MHz): Bootlin
// mode enable, headers through the hardware bit writer, IDR / P pictures with the "fast+tf" motion-estimation row,
// CAVLC, one reference frame. Users: pheencbench (enc_hw.cpp) and the screen recorder plug-in (pherecenc.cpp).
// Inputs are NV12 pictures in the arena (stride = width rounded up to 16, macroblock-padded height).
#ifndef PHE_VE_ENC_H
#define PHE_VE_ENC_H
#include <e32std.h>
#include "rphevde.h"

class TVeEnc
	{
public:
	enum { KBitstreamSize = 1 << 20, KScratchOff = KBitstreamSize, KScratchSize = 3 << 20, KEncOff = KScratchOff + KScratchSize };
	TVeEnc();
	// the VE driver, an arena of the largest of aMaxMb .. 32 MiB that commits, the VE clock at 402 MHz (restored by Close)
	TInt Open(TInt aMaxMb, TDes8& aMsg);
	void Close();
	TBool IsOpen() const { return iOpen; }
	TInt Mhz() const { return iMhz; }
	RPheVde& Ve() { return iVe; }
	RChunk& Arena() { return iArena; }
	TUint32 Phys() const { return iPhys; }
	TInt ArenaSize() const { return iArenaSize; }
	TUint8* Scratch() { return iArena.Base() + KScratchOff; }		// uncached, contiguous, VE-readable (not used by the encoder)

	// the picture size (any even size; coded in 16x16 macroblocks, cropped back in the SPS) and aInputs NV12 input slots
	TInt Setup(TInt aW, TInt aH, TInt aInputs);
	TInt CodedW() const { return iAw; }
	TInt CodedH() const { return iAh; }
	TUint8* InputY(TInt aSlot) { return iArena.Base() + (iIn[aSlot] + KDramBusOff - iPhys); }
	TUint8* InputUv(TInt aSlot) { return InputY(aSlot) + iAw * iAh; }
	// encode input aSlot: an IDR (with SPS + PPS in front) or a P picture predicted from the previous one. aQp = the
	// slice QP (10..46); aPpsQp = the PPS pic_init_qp (pass the same value for every picture of a stream). The access
	// unit (Annex B) is at Stream(), aBytes long; aWaitTicks += the fast-counter ticks blocked on the engine.
	TInt Encode(TInt aSlot, TBool aIdr, TInt aQp, TInt aPpsQp, TInt& aBytes, TUint32& aWaitTicks);
	const TUint8* Stream() const { return iArena.Base(); }
	void Idle();							// the engine back to its idle mode (after the last picture)
private:
	static const TUint32 KDramBusOff = 0x40000000;
	enum { KMaxInputs = 8 };
	RPheVde iVe;
	RChunk iArena;
	TUint32 iPhys;
	TInt iArenaSize;
	TUint32 iOrigPll;
	TInt iMhz;
	TBool iOpen;
	TInt iW, iH, iAw, iAh, iMbW, iMbH, iRecLuma;
	TUint32 iRec[2], iSub[2], iLineBus, iFrameBus, iIn[KMaxInputs];
	TInt iCur, iFrameNum;
	TBool iModeSet;
	};

#endif
