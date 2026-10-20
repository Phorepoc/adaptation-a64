/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// rphevde.h -- OURS (VDU rung V0): the user-side API of phevde, our kernel driver for
// the Allwinner A64 Video Engine (built into ekern, like the Mali driver). The VE is stateless: the CPU parses the
// bitstream and programs every register of a frame; the driver only owns what user mode cannot do safely:
//   GetInfo   -> the VE version, the interrupt count, the clock / SRAM state seen at install
//   ReadReg / WriteReg -> one VE register (offset within the 4 KB VE window; development and probing)
//   OpenArena / Commit -> ONE per-channel shared chunk; Commit backs a range with PHYSICALLY CONTIGUOUS RAM (the A64
//                         has no IOMMU in front of the VE) and returns its physical address
//   Run       -> a job: a list of register writes (the last one starts the engine), then wait for the VE interrupt;
//                completes with the engine's status register value (or KErrTimedOut)
// Open never needs User::LoadLogicalDevice (the LDD is part of the kernel).
#ifndef RPHEVDE_H
#define RPHEVDE_H

#include <e32cmn.h>
#ifndef __KERNEL_MODE__
#include <e32std.h>
#endif

_LIT(KPheVdeLddName, "PheVde");

struct TPheVdeInfo
	{
	TUint32 iVersion;		// VE_VERSION (0x0F0): A64 = 0x1689xxxx
	TUint32 iIrqCount;		// VE interrupts taken since boot
	TUint32 iPllVe;			// CCU PLL_VE_CTRL after the power-up
	TUint32 iSramCtrl;		// syscon SRAM control 0 after the claim (C1 -> VE)
	TInt iArenaMaxSize;		// bytes
	TUint32 iPollTimeouts;	// poll steps that gave up after 1 ms
	};

const TInt KPheVdeMaxWrites = 1024;		// an H.264 slice: frame list 144 + weights 192 + scaling 56 + header skip + ~40
const TUint32 KPheVdePoll = 0x80000000;	// iOff | KPheVdePoll: wait (<= 1 ms) until (reg & iVal) == 0, no write
struct TPheVdeWrite
	{
	TUint32 iOff;
	TUint32 iVal;
	};
// a job: iWrites[0..iNumWrites) in order (the last starts the decode); on the interrupt the driver reads
// iStatusOff, writes back (status & iStatusMask) to clear it, clears iIntCtrlMask in iIntCtrlOff (the engine's
// interrupt enables) and completes the request with the status value read (>= 0 as a TInt for every 31-bit status)
struct TPheVdeJob
	{
	TUint32 iStatusOff;
	TUint32 iStatusMask;
	TUint32 iIntCtrlOff;
	TUint32 iIntCtrlMask;
	TInt iNumWrites;
	TPheVdeWrite iWrites[KPheVdeMaxWrites];
	};

struct TPheVdeCommit
	{
	TInt iOffset;
	TInt iSize;
	TUint32 iPhys;			// out
	};

enum TPheVdeControl { EPheVdeCtrlGetInfo, EPheVdeCtrlReadReg, EPheVdeCtrlWriteReg, EPheVdeCtrlOpenArena,
	EPheVdeCtrlCommit, EPheVdeCtrlSetPll, EPheVdeCtrlPhysRead };
enum TPheVdeRequest { EPheVdeReqRun, EPheVdeReqCount };
const TInt KPheVdeMajorVersion = 1;

#ifndef __KERNEL_MODE__
class RPheVde : public RBusLogicalChannel
	{
public:
	inline static TVersion VersionRequired() { return TVersion(KPheVdeMajorVersion, 0, 0); }
	inline TInt Open() { return DoCreate(KPheVdeLddName, VersionRequired(), KNullUnit, NULL, NULL, EOwnerThread); }
	inline TInt GetInfo(TPheVdeInfo& aInfo)
		{ TPckg<TPheVdeInfo> p(aInfo); return DoControl(EPheVdeCtrlGetInfo, &p); }
	// the value, or a negative error (a register reading 0x8xxxxxxx comes back through aVal)
	inline TInt ReadReg(TUint32 aOff, TUint32& aVal)
		{ TPckg<TUint32> p(aVal); return DoControl(EPheVdeCtrlReadReg, (TAny*)aOff, &p); }
	inline TInt WriteReg(TUint32 aOff, TUint32 aVal)
		{ return DoControl(EPheVdeCtrlWriteReg, (TAny*)aOff, (TAny*)aVal); }
	// a handle to the arena chunk (Base() = offset 0); uncached
	inline TInt OpenArena(RChunk& aChunk)
		{ TInt h = DoControl(EPheVdeCtrlOpenArena); return h < 0 ? h : aChunk.SetReturnedHandle(h); }
	// commit [aOffset, aOffset + aSize) (page aligned) with contiguous RAM; aPhys = its physical address
	inline TInt Commit(TInt aOffset, TInt aSize, TUint32& aPhys)
		{
		TPheVdeCommit c;
		c.iOffset = aOffset;
		c.iSize = aSize;
		c.iPhys = 0;
		TPckg<TPheVdeCommit> p(c);
		TInt r = DoControl(EPheVdeCtrlCommit, &p);
		aPhys = c.iPhys;
		return r;
		}
	// reprogram PLL_VE_CTRL (the full register value, enable bit included; the VE must be idle). aPllNow = the register read back
	// (bit 28 = locked). KErrNotSupported on a board without the hook. VE clock = PLL_VE (VE_CLK divider 1).
	inline TInt SetPll(TUint32 aPll, TUint32& aPllNow)
		{
		TPckg<TUint32> p(aPllNow);
		return DoControl(EPheVdeCtrlSetPll, (TAny*)aPll, &p);
		}
	// diagnostics: one word of a whitelisted SoC register (CCU 0x01C20000, SYSCON 0x01C00000, DRAM controller 0x01C62000; read only)
	inline TInt PhysRead(TUint32 aPhys, TUint32& aVal)
		{
		TPckg<TUint32> p(aVal);
		return DoControl(EPheVdeCtrlPhysRead, (TAny*)aPhys, &p);
		}
	inline void Run(const TPheVdeJob& aJob, TRequestStatus& aStatus)
		{ DoRequest(EPheVdeReqRun, aStatus, (TAny*)&aJob); }
	inline void CancelAll() { DoCancel(0xFFFFFFFF); }
	};
#endif

#endif
