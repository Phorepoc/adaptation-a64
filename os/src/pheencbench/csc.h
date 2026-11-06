/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// csc.h -- OURS: RGB -> YUV 4:2:0 colour conversion (csc.cpp), shared by pheencbench and the phfs screen recorder.
#ifndef PHE_CSC_H
#define PHE_CSC_H
#include <stdint.h>

namespace pb {

enum TSrc { EXrgb, ERgb565 };
// Colour conversion (BT.601 limited range). aU2 == NULL: aU is the interleaved UV plane (NV12), else planar (I420 U, V).
// aNeon: the NEON kernel for whole 16-pixel groups (the tail columns run scalar).
void Csc(TSrc aFmt, const void* aSrc, int aSrcStrideBytes, int aW, int aH, uint8_t* aY, int aYStride, uint8_t* aU, uint8_t* aV, int aCStride,
	bool aNeon);
// replicate the last column / row of an NV12 picture into the MB-aligned padding
void PadNv12(uint8_t* aY, uint8_t* aUv, int aW, int aH, int aAW, int aAH);

}	// namespace pb
#endif
