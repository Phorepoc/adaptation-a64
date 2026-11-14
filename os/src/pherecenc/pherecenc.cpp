// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// pherecenc.cpp -- OURS (2026-11-06): the screen recorder's H.264 encoder plug-in (contract: src/pherec/pherecenc.h),
// loaded by the image's pherec.exe when the screen recorder package is installed from Catalogue (package.conf; never in
// an image: H.264 is patent-encumbered). A64 boards: the Video
// Engine's hardware encoder (ve_enc.cpp, TVeEnc), the register sequence pheencbench measured: 360x720 in ~1.4 ms at
// 402 MHz, the CPU asleep while the engine runs. One NV12 input slot; fixed QP (no rate control).
#include <e32base.h>
#include <hal.h>
#include "pherecenc.h"
#include "ve_enc.h"

class CVeRecEncoder : public CBase, public MPheRecEncoder
	{
public:
	TInt Open(TDes8& aMsg) { return iEnc.Open(16, aMsg); }
	TInt Setup(TInt aW, TInt aH) { return iEnc.Setup(aW, aH, 1); }
	TUint8* InputY() { return iEnc.InputY(0); }
	TUint8* InputUv() { return iEnc.InputUv(0); }
	TInt Stride() { return iEnc.CodedW(); }
	TInt CodedHeight() { return iEnc.CodedH(); }
	TInt Encode(TBool aIdr, TInt aQp, TInt& aBytes, TInt64& aBusyUs)
		{
		TUint32 wait = 0;
		const TInt r = iEnc.Encode(0, aIdr, aQp, aQp, aBytes, wait);
		aBusyUs += FastCounterUs(wait);
		return r;
		}
	const TUint8* Stream() { return iEnc.Stream(); }
	void Close()
		{
		iEnc.Idle();
		iEnc.Close();
		}
private:
	static TInt64 FastCounterUs(TUint32 aTicks)
		{
		TInt f = 0;
		if (HAL::Get(HAL::EFastCounterFrequency, f) != KErrNone || f <= 0)
			f = 1000000;
		return (TInt64)aTicks * 1000000 / f;
		}
	TVeEnc iEnc;
	};

extern "C" EXPORT_C MPheRecEncoder* PheRecEncoderNew()		// ordinal 1 (pherecenc.def)
	{
	return new CVeRecEncoder;
	}
