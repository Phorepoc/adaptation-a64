/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

// phevde_board.h -- OURS (VDU rung V0): what a board tells the generic VE driver (phevde_ldd.cpp).
#ifndef PHEVDE_BOARD_H
#define PHEVDE_BOARD_H

struct TPheVdeBoard
	{
	TPhysAddr iBase;			// the VE's 4 KB register window
	TInt iIrq;					// interrupt id (Interrupt::Bind numbering)
	TInt (*iPowerOn)(TUint32& aPll, TUint32& aSram);	// clocks, resets, SRAM claim; reports PLL + SRAM state
	TInt (*iSetPll)(TUint32 aPllValue, TUint32& aPllNow);	// OPTIONAL (may be NULL): reprogram PLL_VE while the VE is idle (encoder speed experiments)
	TInt (*iPhysRead)(TUint32 aPhys, TUint32& aVal);	// OPTIONAL: read one word of a whitelisted diagnostic register (CCU / SYSCON / DRAM controller)
	};

// Install the "PheVde" logical device (RPheVde). Kernel boot context.
extern "C" TInt PheVdeInstall(const TPheVdeBoard& aBoard);

#endif
