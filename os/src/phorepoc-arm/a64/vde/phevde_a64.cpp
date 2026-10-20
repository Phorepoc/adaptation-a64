// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// phevde_a64.cpp -- OURS (VDU rung V0): the PinePhone's (Allwinner A64) half of the VE
// driver: where the Video Engine sits (0x01C0E000, GIC SPI 58), its clock bring-up and the SRAM C1 claim -- the CCU
// writes checked by hand with PhoneLab on 2026-10-15 (VE_VERSION answered 0x16890040 after them).
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include "../../vde/phevde_board.h"

const TPhysAddr KCcuPhys = 0x01C20000;
const TPhysAddr KSysconPhys = 0x01C00000;
enum
	{
	KCcuPllVe		= 0x018,	// PLL_VE_CTRL: [31] enable, [28] lock, [24] integer mode, [14:8] N-1, [3:0] M-1
	KCcuBusGate1	= 0x064,	// BUS_CLK_GATING_REG1: [0] VE
	KCcuDramGate	= 0x100,	// DRAM_CFG / gating: [0] VE's DRAM access
	KCcuVeClk		= 0x13C,	// VE_CLK: [31] gating, source PLL_VE, divider 1
	KCcuBusRst1		= 0x2C4,	// BUS_SOFT_RST_REG1: [0] VE (1 = released)
	KSysconSram0	= 0x000		// SRAM control 0: the C1 area's owner (0 = CPU, 0x7FFFFFFF = VE)
	};

// U-Boot leaves PLL_VE off at its reset factors 0x03006207 = 24 MHz * 99 / 8 = 297 MHz (integer mode).
const TUint32 KPllVe297 = 0x03006207;
const TUint32 KSramC1ToVe = 0x7FFFFFFF;

static TInt A64VePowerOn(TUint32& aPll, TUint32& aSram)
	{
	DPlatChunkHw* ccuChunk;
	DPlatChunkHw* sysChunk;
	NKern::ThreadEnterCS();
	TInt r = DPlatChunkHw::New(ccuChunk, KCcuPhys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r == KErrNone)
		{
		r = DPlatChunkHw::New(sysChunk, KSysconPhys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
		if (r != KErrNone)
			ccuChunk->Close(NULL);
		}
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		return r;
	volatile TUint32* ccu = (volatile TUint32*)ccuChunk->LinearAddress();
	volatile TUint32* sys = (volatile TUint32*)sysChunk->LinearAddress();
	if (!(ccu[KCcuPllVe / 4] & (1u << 31)))
		{
		ccu[KCcuPllVe / 4] = KPllVe297;
		Kern::NanoWait(10000);
		ccu[KCcuPllVe / 4] = KPllVe297 | (1u << 31);
		}
	for (TInt i = 0; i < 1000 && !(ccu[KCcuPllVe / 4] & (1u << 28)); ++i)
		Kern::NanoWait(10000);
	aPll = ccu[KCcuPllVe / 4];
	const TBool locked = (aPll & (1u << 28)) != 0;
	ccu[KCcuVeClk / 4] = 1u << 31;
	ccu[KCcuDramGate / 4] |= 1u << 0;
	ccu[KCcuBusGate1 / 4] |= 1u << 0;
	ccu[KCcuBusRst1 / 4] |= 1u << 0;
	sys[KSysconSram0 / 4] = KSramC1ToVe;
	aSram = sys[KSysconSram0 / 4];
	Kern::NanoWait(100000);
	NKern::ThreadEnterCS();
	ccuChunk->Close(NULL);
	sysChunk->Close(NULL);
	NKern::ThreadLeaveCS();
	return locked ? KErrNone : KErrTimedOut;
	}

// Reprogram PLL_VE while the VE is idle (rung-2 speed experiments, ve-encoder-plan.md section 13): gate VE_CLK, stop the PLL, write the new
// factors, start it, wait for lock, ungate. The VE clock is PLL_VE / 1 (VE_CLK divider field 0). Mainline cedrus runs the A64 VE at 402 MHz
// (clock_mod_rate, cedrus.c variant sun50i_a64) = 24 MHz * 67 / 4 = PLL 0x03004203 | enable.
static TInt A64VeSetPll(TUint32 aPllValue, TUint32& aPllNow)
	{
	DPlatChunkHw* ccuChunk;
	NKern::ThreadEnterCS();
	TInt r = DPlatChunkHw::New(ccuChunk, KCcuPhys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		return r;
	volatile TUint32* ccu = (volatile TUint32*)ccuChunk->LinearAddress();
	ccu[KCcuVeClk / 4] = 0;							// gate the module clock
	ccu[KCcuPllVe / 4] &= ~(1u << 31);				// stop the PLL
	Kern::NanoWait(10000);
	ccu[KCcuPllVe / 4] = aPllValue & ~(1u << 31);
	Kern::NanoWait(10000);
	ccu[KCcuPllVe / 4] = aPllValue | (1u << 31);
	for (TInt i = 0; i < 1000 && !(ccu[KCcuPllVe / 4] & (1u << 28)); ++i)
		Kern::NanoWait(10000);
	aPllNow = ccu[KCcuPllVe / 4];
	const TBool locked = (aPllNow & (1u << 28)) != 0;
	ccu[KCcuVeClk / 4] = 1u << 31;
	Kern::NanoWait(100000);
	NKern::ThreadEnterCS();
	ccuChunk->Close(NULL);
	NKern::ThreadLeaveCS();
	return locked ? KErrNone : KErrTimedOut;
	}

// Read-only diagnostics (rung-2 speed hunt, ve-encoder-plan.md section 14): one word of the CCU, SYSCON or DRAM-controller (MBUS) registers.
static TInt A64PhysRead(TUint32 aPhys, TUint32& aVal)
	{
	const TBool ok = !(aPhys & 3) && ((aPhys >= 0x01C20000 && aPhys < 0x01C20400) || (aPhys >= 0x01C00000 && aPhys < 0x01C00100) ||
		(aPhys >= 0x01C62000 && aPhys < 0x01C63000));
	if (!ok)
		return KErrArgument;
	DPlatChunkHw* chunk;
	NKern::ThreadEnterCS();
	TInt r = DPlatChunkHw::New(chunk, aPhys & ~0xFFFu, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		return r;
	aVal = *(volatile TUint32*)(chunk->LinearAddress() + (aPhys & 0xFFF));
	NKern::ThreadEnterCS();
	chunk->Close(NULL);
	NKern::ThreadLeaveCS();
	return KErrNone;
	}

// Called from the board's driver-loading seam (specific/a64_glue.cpp).
extern "C" void A64VdeInstall()
	{
	TPheVdeBoard b;
	b.iBase = 0x01C0E000;
	b.iIrq = 32 + 58;
	b.iPowerOn = A64VePowerOn;
	b.iSetPll = A64VeSetPll;
	b.iPhysRead = A64PhysRead;
	PheVdeInstall(b);
	}
