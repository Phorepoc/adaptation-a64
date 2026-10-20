// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// mali_a64.cpp -- OURS (GPU rung G3, docs/phorepoc/gpu-lima-research.md): the PinePhone's (Allwinner A64) half of
// the Mali driver: where the Mali-400 MP2 sits, its interrupts (sun50i-a64.dtsi `mali: gpu@1c40000`), and its clock
// bring-up -- the same CCU writes G3-K0 verified by hand with PhoneLab (2026-10-09). The GPU rail is always on.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include "../../mali/mali_board.h"

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);

const TPhysAddr KCcuPhys = 0x01C20000;
enum
	{
	KCcuPllGpu	= 0x038,		// PLL_GPU_CTRL: [31] enable, [28] lock, [14:8] N-1, [3:0] M-1
	KCcuGpuClk	= 0x1A0,		// GPU_CLK: [31] gating, source PLL_GPU, divider 1
	KCcuBusGate2	= 0x064,	// BUS_CLK_GATING_REG1: [20] GPU
	KCcuBusRst1	= 0x2C4		// BUS_SOFT_RST_REG1: [20] GPU (1 = released)
	};

// PLL_GPU = 24 MHz * N / M. U-Boot leaves N = 99, M = 8 (297 MHz, the reset value; the first frames ran on it);
// Linux runs the A64's GPU at 432 MHz (its only OPP: sun50i-a64.dtsi) = N 18, M 1 (fields hold N-1, M-1), integer
// mode. Rung G9: set before the PLL is enabled, then wait for the lock.
const TUint32 KPllGpu432 = (17u << 8) | 0u;
static TInt A64MaliPowerOn()
	{
	DPlatChunkHw* ccuChunk;
	NKern::ThreadEnterCS();
	TInt r = DPlatChunkHw::New(ccuChunk, KCcuPhys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		return r;
	volatile TUint32* ccu = (volatile TUint32*)ccuChunk->LinearAddress();
	ccu[KCcuPllGpu / 4] = KPllGpu432;		// (PLL off while the factors change)
	Kern::NanoWait(10000);
	ccu[KCcuPllGpu / 4] = KPllGpu432 | (1u << 31);
	for (TInt i = 0; i < 1000 && !(ccu[KCcuPllGpu / 4] & (1u << 28)); ++i)
		Kern::NanoWait(10000);
	const TBool locked = (ccu[KCcuPllGpu / 4] & (1u << 28)) != 0;
	ccu[KCcuGpuClk / 4] = 1u << 31;
	ccu[KCcuBusGate2 / 4] |= 1u << 20;
	ccu[KCcuBusRst1 / 4] |= 1u << 20;
	Kern::NanoWait(100000);
	ekseam_puts("[EKERN][mali] A64 PLL_GPU="); ekseam_puthex(ccu[KCcuPllGpu / 4]);
	NKern::ThreadEnterCS();
	ccuChunk->Close(NULL);
	NKern::ThreadLeaveCS();
	return locked ? KErrNone : KErrTimedOut;
	}

// Called from the board's driver-loading seam (specific/a64_glue.cpp).
extern "C" void A64MaliInstall()
	{
	TMaliBoard b;
	b.iBase = 0x01C40000;
	b.iNumPp = 2;
	b.iIrqGp = 32 + 97;
	b.iIrqGpMmu = 32 + 98;
	b.iIrqPp[0] = 32 + 99;
	b.iIrqPpMmu[0] = 32 + 100;
	b.iIrqPp[1] = 32 + 102;
	b.iIrqPpMmu[1] = 32 + 103;
	b.iIrqPp[2] = b.iIrqPp[3] = b.iIrqPpMmu[2] = b.iIrqPpMmu[3] = -1;
	b.iIrqPmu = 32 + 101;
	b.iPowerOn = A64MaliPowerOn;
	MaliInstall(b);
	}
