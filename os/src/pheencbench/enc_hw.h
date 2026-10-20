/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// enc_hw.h -- OURS (pheencbench): the hardware (A64 VE) and software encoder legs' interfaces.
#ifndef PHE_ENC_HW_H
#define PHE_ENC_HW_H
#include <stdint.h>
#include "bench_common.h"

namespace pb {

struct TDecodeResult
	{
	int iErr, iPictures, iSampled, iAvgY, iMinY;		// PSNR in hundredths of a dB
	};

struct THwImpl;

class THw
	{
public:
	THw();
	~THw();
	int Open(char* aMsg, int aMsgLen);			// VE driver, 64 MiB arena, 402 MHz (restored in Close)
	void Close();
	bool Ok() const { return iOk; }
	int Mhz() const { return iMhz; }
	uint8_t* Scratch();							// uncached, physically contiguous DRAM the VE can read: colour conversion destination
	int ScratchSize();
	// aQp > 0: fixed QP; else rate control to aKbps at 30 fps. aRing: KRing NV12 frames, stride = width rounded up to 16, MB-padded height.
	void Encode(int aW, int aH, int aQp, int aKbps, int aFrames, int aMaxSeconds, const uint8_t* const* aRing, uint8_t* aStream, int aStreamCap,
		int* aOff, TRunResult& aR);
	// decode a stream (any encoder) with the VE decoder; PSNR Y of every aStep-th picture vs aRef[i % KRing] (stride aRefStride)
	void DecodePsnr(const uint8_t* aStream, const int* aOff, int aFrames, int aW, int aH, const uint8_t* const* aRef, int aRefStride, int aStep,
		TDecodeResult& aD);
private:
	THwImpl* iImpl;
	bool iOk;
	int iMhz;
	};

// the OpenH264 leg (enc_sw.cpp); aRing = KRing I420 frames (Y, U, V contiguous, stride = aW)
void SwShutdown();
void SwEncode(int aW, int aH, int aQp, int aKbps, int aThreads, int aFrames, int aMaxSeconds, const uint8_t* const* aRing, uint8_t* aStream,
	int aStreamCap, int* aOff, TRunResult& aR);

}	// namespace pb
#endif
