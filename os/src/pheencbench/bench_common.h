/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

// bench_common.h -- OURS (pheencbench): shared declarations. Plain C++03 + libc (no Symbian types) so enc_sw.cpp / bench_common.cpp
// build against the OpenH264 headers; Symbian services come through the extern "C" functions defined in bench_main.cpp.
#ifndef PHE_BENCH_COMMON_H
#define PHE_BENCH_COMMON_H
#include <stdint.h>
#include <stddef.h>

extern "C" {
uint32_t ph_ticks(void);			// User::FastCounter()
uint32_t ph_freq(void);				// HAL EFastCounterFrequency
void ph_log(const char* aLine);		// RDebug, prefix added by the caller
void ph_cpu_mark(void);				// the encoder calls this right after its timed loop, before tearing worker threads down
}

namespace pb {

const int KRing = 6;				// distinct source frames (frame k uses k % KRing)

inline uint32_t Ticks() { return ph_ticks(); }
inline int64_t UsOf(uint32_t aTicks) { return (int64_t)aTicks * 1000000 / ph_freq(); }

// XRGB8888 (0x00RRGGBB) UI-like frame: flat background, header bar, list rows with text-ish glyph strokes, a moving gradient box
void GenXrgb(int aW, int aH, int aK, uint32_t* aDst);
// the worst case of the first VE benchmark (phevenctest2): a full-frame gradient + a moving checker square (every macroblock has residual)
void GenStress(int aW, int aH, int aK, uint32_t* aDst);
void ToRgb565(const uint32_t* aSrc, int aW, int aH, uint16_t* aDst);

enum TSrc { EXrgb, ERgb565 };
// Colour conversion (BT.601 limited range). aU2 == NULL: aU is the interleaved UV plane (NV12), else planar (I420 U, V).
// aNeon: the NEON kernel for whole 16-pixel groups (the tail columns run scalar).
void Csc(TSrc aFmt, const void* aSrc, int aSrcStrideBytes, int aW, int aH, uint8_t* aY, int aYStride, uint8_t* aU, uint8_t* aV, int aCStride,
	bool aNeon);
// replicate the last column / row of an NV12 picture into the MB-aligned padding
void PadNv12(uint8_t* aY, uint8_t* aUv, int aW, int aH, int aAW, int aAH);

// PSNR (hundredths of a dB, 9999 = identical) of the aW x aH luma region
int PsnrY(const uint8_t* aA, int aStrideA, const uint8_t* aB, int aStrideB, int aW, int aH);

// result of one encode run
struct TRunResult
	{
	int iFrames;					// frames encoded
	int iBytes;						// total stream bytes
	int iFirstBytes;				// bytes of the first (IDR) frame
	int64_t iUs;					// wall time of the encode loop (microseconds)
	int64_t iMaxFrameUs;			// slowest frame
	int iErr;						// 0 = ok
	int iQpUsed;					// fixed QP, or 0 for rate control
	int iCpuPct;					// CPU busy x100 of one core when the encoder knows it (HW: time outside the job wait), else -1
	};

}	// namespace pb
#endif
