/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// mali_board.h -- OURS (GPU rung G3): what a board tells the generic Mali Utgard driver (mali_ldd.cpp).
#ifndef MALI_BOARD_H
#define MALI_BOARD_H

struct TMaliBoard
	{
	TPhysAddr iBase;			// the GPU's 64 KB register region
	TInt iNumPp;				// pixel processors (1..4)
	TInt iIrqGp, iIrqGpMmu;		// interrupt ids (Interrupt::Bind numbering)
	TInt iIrqPp[4], iIrqPpMmu[4];
	TInt iIrqPmu;
	TInt (*iPowerOn)();			// clocks + resets (+ rails) on; NULL = already on
	};

// Install the "Mali" logical device (RMaliChannel). Kernel boot context.
extern "C" TInt MaliInstall(const TMaliBoard& aBoard);

#endif
