// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
//
// a64_rail.cpp -- the PMIC rail API of a64_rail.h over A64PmicUpdateReg (display/a64_display.cpp, RSB to the AXP803).
// Voltages: the board device tree's regulator values; registers: the AXP803 datasheet (voltage register, enable
// bit in the output-control register).
#include <kernel/kernel.h>
#include "a64_rail.h"

extern "C" TInt A64PmicUpdateReg(TUint8 aReg, TUint8 aClr, TUint8 aSet);

struct TRailDef { TUint8 iVReg, iVMask, iVSel, iEnReg, iEnBit; TBool iShared; };
static const TRailDef KRails[EA64RailCount] =
	{
	{ 0x28, 0x1F, 11, 0x13, 5, ETrue },	// ALDO1: 0.7 V + 11 x 0.1 V = 1.8 V
	{ 0x17, 0x1F, 21, 0x12, 5, ETrue },	// DLDO3: 0.7 V + 21 x 0.1 V = 2.8 V
	{ 0x1B, 0x1F, 22, 0x12, 2, EFalse },	// ELDO3: 0.7 V + 22 x 0.05 V = 1.8 V
	};
static TInt Users[EA64RailCount];
static NFastMutex RailLock;

extern "C" TInt A64RailSet(TInt aRail, TBool aOn)
	{
	if (aRail < 0 || aRail >= EA64RailCount)
		return KErrArgument;
	const TRailDef& d = KRails[aRail];
	TInt r = KErrNone;
	NKern::FMWait(&RailLock);
	if (aOn)
		{
		if (Users[aRail]++ == 0)
			{
			NKern::FMSignal(&RailLock);		// the RSB access sleeps: not under a fast mutex
			r = A64PmicUpdateReg(d.iVReg, d.iVMask, d.iVSel);
			if (r == KErrNone)
				r = A64PmicUpdateReg(d.iEnReg, 0, (TUint8)(1u << d.iEnBit));
			NKern::FMWait(&RailLock);
			if (r != KErrNone)
				Users[aRail]--;
			}
		}
	else if (Users[aRail] > 0 && --Users[aRail] == 0 && !d.iShared)
		{
		NKern::FMSignal(&RailLock);
		r = A64PmicUpdateReg(d.iEnReg, (TUint8)(1u << d.iEnBit), 0);
		return r;
		}
	NKern::FMSignal(&RailLock);
	return r;
	}
