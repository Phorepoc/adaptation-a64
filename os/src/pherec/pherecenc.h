/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

// pherecenc.h -- OURS (2026-11-06): the screen recorder's encoder plug-in contract. pherec.exe (in the image) captures and
// converts the pictures; the H.264 encoder is NOT in the image (H.264 is patent-encumbered): it comes with this repository's
// screen recorder package from Catalogue as pherecenc.dll (src/pherecenc, the VE encoder; package.conf). pherec.exe
// loads it by name; without it a recording fails with KErrNotFound.
// The DLL exports one function at ordinal 1: MPheRecEncoder* PheRecEncoderNew(), and has the UID3 KPheRecEncoderUid.
#ifndef PHERECENC_H
#define PHERECENC_H
#include <e32std.h>

_LIT(KPheRecEncoderDll, "pherecenc.dll");
const TUid KPheRecEncoderUid = { 0xE7015813 };

class MPheRecEncoder
	{
public:
	virtual ~MPheRecEncoder() {}
	// the encoder (hardware, clocks, memory); aMsg = what it found, for the statistics line
	virtual TInt Open(TDes8& aMsg) = 0;
	// the picture size (even; coded in macroblocks, cropped back in the SPS); NV12 input from then on
	virtual TInt Setup(TInt aW, TInt aH) = 0;
	virtual TUint8* InputY() = 0;			// aStride x CodedHeight luma, then the interleaved UV plane at InputUv
	virtual TUint8* InputUv() = 0;
	virtual TInt Stride() = 0;				// bytes per row of both planes (>= the width, 16-aligned)
	virtual TInt CodedHeight() = 0;			// rows to fill (padding below the picture repeats its last row)
	// encode the input: an IDR (SPS + PPS in front) or a P picture; the Annex B access unit is at Stream(), aBytes long.
	// aBusyUs += the time the encoder was busy (not the caller's CPU when it is hardware)
	virtual TInt Encode(TBool aIdr, TInt aQp, TInt& aBytes, TInt64& aBusyUs) = 0;
	virtual const TUint8* Stream() = 0;
	virtual void Close() = 0;
	};

typedef MPheRecEncoder* (*TPheRecEncoderNew)();

#endif
