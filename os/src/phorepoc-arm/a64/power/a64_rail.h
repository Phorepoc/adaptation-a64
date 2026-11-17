/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// a64_rail.h -- the PMIC rail API (power/a64_rail.cpp): named AXP803 supply rails for drivers and kernel-side channels,
// instead of raw register writes through A64PmicUpdateReg. A rail is switched on at its board voltage and counted:
// it goes off only when the last user switches it off, and rails other drivers share are never switched off here.
// Linked into ekern with the other a64 drivers (the CamHw.A64 channel uses it); an ekern.def export is added when a
// loadable kernel DLL needs it.
#ifndef A64_RAIL_H
#define A64_RAIL_H
#include <e32def.h>

enum TA64Rail
	{
	EA64RailAldo1 = 0,	// 1.8 V: the PE pin bank + camera DOVDD (shared: never switched off)
	EA64RailDldo3 = 1,	// 2.8 V: camera AVDD (only the two sensors use it: off with the last user, as Linux)
	EA64RailEldo3 = 2,	// 1.8 V: camera DVDD
	EA64RailCount
	};

// aOn = ETrue: set the board voltage and enable (counted); EFalse: release. KErrArgument for an unknown rail.
extern "C" TInt A64RailSet(TInt aRail, TBool aOn);

#endif
