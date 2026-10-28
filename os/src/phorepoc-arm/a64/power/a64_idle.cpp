// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// a64_idle.cpp -- OURS (power plan P5): idle tick suppression + core retirement for the
// A64, on the 2010 SMP idle framework (drivers/power/smppower: DSMPIdleHandler + TIdleSupport, the model
// NaviEngine's ne1_tb/specific/power.cpp uses).
//
//  1. IDLE TICK SUPPRESSION (ITS). Once every engaged core is idle, the last one asks NTimerQ::IdleTime() how many
//     ticks are empty (GetLowPowerMode). The tick is the generic PHYSICAL timer of CPU 0 (PPI 30, interrupts.cia:
//     CNTP_CVAL += 24000 each ms); it is banked per core, so CPU 0 itself (EnterLowPowerMode) moves its compare value
//     to the tick that has work, then every core does WFI. After the wake-up (PostWakeup, still inside the idle
//     handler, before ANY core leaves it -- the framework's last sync point) CPU 0 works out how many whole ticks went by,
//     NTimerQ::Advance()s the timer queue by them and puts CVAL back on the original 1 ms phase. The timestamp
//     (CNTVCT) never stops, so unlike NaviEngine's timers nothing else needs restarting.
//     Wake time W = T0 + n * reload (T0 = the next tick's deadline when the sleep started): the timer's own interrupt
//     is the tick that has work; ticks skipped = min(ticks elapsed, n), so a late wake can only make the next tick fire
//     at once, never lose one.
//
//  2. CORE RETIREMENT (the kernel's core control, NKern::SetNumberOfActiveCpus; the policy is in a64_cpufreq.cpp: screen
//     off + low load -> 1 core). The kernel migrates threads away and, when a core is idle and no longer accepts
//     threads, calls the idle handler with SCpuIdleHandler::ERetire. The retire sequence (RetireCore):
//       MarkCoreRetired (the sync points ignore it) -> (no idle IPI: SGI 7 on a running core = CRASH_IPI) -> Arm::SaveState (ALL ARM registers into a
//       per-core SFullArmRegSet, like NKern::BootAP does for a new AP) + the GIC CPU-interface and banked PPI state +
//       the virtual timer -> clean it to memory -> ++iDetachCount -> PSCI CPU_OFF (TF-A flushes this core's L1 and
//       drops it out of the coherency domain, then cuts its power).
//     Power-up (iCpuPowerUpFn, called by the kernel from CCInitiatePowerUp): PSCI CPU_ON(cpu, the shim, a resume
//     record). The shim (a53shim/shim.S) sees the record in x0, ERETs straight to the kernel's AP boot code
//     (APBootCodeEntry, bootcpu.s; the bootstrap stored its address in SSuperPageBase iReserved[0]) with r0 = the
//     regset: it restores MMU/TTBR/DACR/... and every mode's registers and resumes right after Arm::SaveState in
//     RetireCore, which restores the GIC state, ++iAttachCount and returns into NKern::DoIdle as if from a long WFI.
//     CPU 0 never retires (it owns the tick and the SPIs: nkernsmp routes every SPI to CPU 0): if the kernel asks
//     anyway the request is refused (the core just continues; counted, the policy backs off).
//
//  A64DEFS=-DA64_NO_ITS (no tick suppression), -DA64_NO_RETIRE (no core retirement), -DA64_NO_IDLEHANDLER (neither: the
//  old `wfi` per core, DA64PowerController::CpuIdle).
//
// Facts: PSCI 0.2 (SMC32 ids 0x84000002 CPU_OFF, 0x84000003 CPU_ON); TF-A plat/allwinner/sun50i_a64 implements both
// (hotplug) -- research/power-linux-modern.md section 3; the 2010 kernel sources named above.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <kernel/arm/arm.h>
#include <platform.h>
#include "syborg_priv.h"
#include <smppower/sample_idlehandler/smpidlehandler.h>

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);
extern "C" TUint32 TheTickReload;			// interrupts.cpp: CNTFRQ/1000, the 1 ms tick's compare increment
extern TLinAddr RomHeaderAddress;			// plat_priv.h: the bootstrap's / ROM header's linear address
// CORE RETIREMENT IS OPT-IN (boots 627-644: the retire path stalls core 1 silently): off unless A64DEFS=-DA64_RETIRE
// (the policy of a64_cpufreq.cpp, screen off + idle -> 1 core) or -DA64_CC_TEST (the unattended test); -DA64_NO_RETIRE
// still forces it off. Without it iCpuPowerUpFn = 0, the kernel's core control is "not supported" and
// SetNumberOfActiveCpus returns at once; idle tick suppression is unaffected.
#if (defined(A64_RETIRE) || defined(A64_CC_TEST)) && !defined(A64_NO_RETIRE)
#define A64_RETIRE_ON
#endif
extern "C" void A64SdFlushNow(TInt aStage);	// a64_glue.cpp: flush the SD log (THREAD context only)
extern "C" void A64SdFlushIfDirty(TInt aStage);

// PheSaveCore(regs): Arm::SaveState with the C calling convention. Arm::SaveState is a __NAKED__ routine that ZEROES
// r4-r11 (and trashes r0-r3, r12): called from C++ directly (as the first version did) every callee-saved register
// the caller keeps its variables in is zero afterwards -> a data abort on the very first return (the hang of boots
// 627/629). This wrapper pushes r4-r12,lr on the stack first. It returns twice (like setjmp): once now, and once
// after PSCI CPU_ON + APBootCodeEntry restored the register set -- r13_svc in the set is the wrapper's sp, so the
// second return pops the very same frame (its memory survived: the retired core flushed its L1 to DRAM and only
// the core was powered off). ARM state throughout (APBootCodeEntry's final `LDM {pc}^` does not interwork).
extern "C" void PheSaveCore(SFullArmRegSet& aRegs) __attribute__((returns_twice));
asm(".pushsection .text.PheSaveCore,\"ax\",%progbits\n"
	".arm\n"
	".global PheSaveCore\n"
	".type PheSaveCore,%function\n"
	".balign 4\n"
	"PheSaveCore:\n"
	"	push	{r4-r12,lr}\n"
	"	bl	_ZN3Arm9SaveStateER14SFullArmRegSet\n"
	"	pop	{r4-r12,pc}\n"
	".popsection\n");

#ifdef __SMP__

namespace
{
// ---------------------------------------------------------------------------------------------------------------
// generic-timer + PSCI primitives
inline TUint64 CntPct()
	{
	TUint32 lo, hi;
	asm volatile("isb; mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi) : : "memory");
	return ((TUint64)hi << 32) | lo;
	}
inline TUint64 CntpCval()
	{
	TUint32 lo, hi;
	asm volatile("mrrc p15, 2, %0, %1, c14" : "=r"(lo), "=r"(hi));
	return ((TUint64)hi << 32) | lo;
	}
inline void SetCntpCval(TUint64 aV)
	{
	asm volatile("mcrr p15, 2, %0, %1, c14\n\tisb" : : "r"((TUint32)aV), "r"((TUint32)(aV >> 32)) : "memory");
	}
inline TUint32 CntpCtl()
	{
	TUint32 v;
	asm volatile("mrc p15, 0, %0, c14, c2, 1" : "=r"(v));
	return v;
	}

const TUint32 KPsciCpuOff = 0x84000002u, KPsciCpuOn = 0x84000003u, KPsciAffinityInfo = 0x84000004u;
const TInt KPsciAlreadyOn = -4;
const TUint32 KShimBase = 0x40200000u;		// a64.inc KHwShimBase: the AArch64 entry shim (PSCI CPU_ON's entry)

// SMC32 PSCI call (the bootstrap's PSCI CPU_ON, a64.s FinalInitialise, is the same instruction: smc #0, encoded
// because the assembler is told armv7-a without the security extension)
inline TInt Psci(TUint32 aFn, TUint32 a1, TUint32 a2, TUint32 a3)
	{
	register TUint32 r0 asm("r0") = aFn;
	register TUint32 r1 asm("r1") = a1;
	register TUint32 r2 asm("r2") = a2;
	register TUint32 r3 asm("r3") = a3;
	asm volatile(".word 0xE1600070" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "r4", "r5", "r6", "r7");
	return (TInt)r0;
	}

// clean [aP, aP+aN) to the point of coherency (line size <= 32 assumed: A53 lines are 64, so every line is hit)
void CleanRange(const void* aP, TUint aN)
	{
	TUint32 a = (TUint32)aP & ~31u;
	const TUint32 e = (TUint32)aP + aN;
	for (; a < e; a += 32)
		asm volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(a) : "memory");		// DCCMVAC
	asm volatile("dsb" : : : "memory");
	}

// BREADCRUMBS: where each core is in the retire path, as plain memory (the 'ekseam' log is a char-at-a-time ring shared
// by all cores without a lock and its quiet filter keeps ONE global drop flag: a retiring core's pieces get lost while
// CPU 0 floods probes -- boots 642/644 showed no 'saving state' line although the core may well have got there). The
// CC test's flusher prints them ('crumbs=') and, under A64_CC_TEST, every step is also stored in the RTC's battery-backed
// general-purpose words 4..7 (0x01F00110 + 4*cpu), readable with a PhoneLab peek after the watchdog reset (slot A).
// step: 1 ERetire 2 delayed 3 sync reset 4 marked retired 5 RetireOrRefuse 6 refused 7 GIC state read 8 before SaveCore
//       9 SaveCore first return 10 before CPU_OFF 11 CPU_OFF returned | 20 resumed 21 GIC restored 22 attach counted
volatile TUint32 Crumb[4];
volatile TUint32* RtcGp;
void CrumbInit()
	{
#ifdef A64_CC_TEST
	DPlatChunkHw* ch = 0;
	NKern::ThreadEnterCS();
	TInt r = DPlatChunkHw::New(ch, 0x01F00000, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	NKern::ThreadLeaveCS();
	if (r == KErrNone)
		{
		RtcGp = (volatile TUint32*)(ch->LinearAddress() + 0x110);
		for (TInt i = 0; i < 4; ++i)
			RtcGp[i] = 0;
		}
#endif
	}
inline void Cr(TInt aCpu, TUint32 aStep)
	{
	Crumb[aCpu & 3] = aStep;
#ifdef A64_CC_TEST
	if (RtcGp)
		RtcGp[aCpu & 3] = 0xC0DE0000u | aStep;
#endif
	}

// CC test only: hold the core ~aMs so the 2 ms SD flusher on another core gets the last print onto the card before
// the next step (which might be the one that freezes the machine)
inline void DiagDelay(TUint aMs)
	{
#ifdef A64_CC_TEST
	const TUint64 e = CntPct() + (TUint64)aMs * 24000;
	while (CntPct() < e) {}
#else
	(void)aMs;
#endif
	}

// ---------------------------------------------------------------------------------------------------------------
// per-core retire state
const TUint32 KResumeMagic = 0x53524850u;	// 'PHRS' (the shim checks it)
const TInt KCpus = 4;

// The shim's resume record is the FIRST 16 bytes (physical address = PSCI CPU_ON's context_id, 16-byte aligned).
struct SRetire
	{
	TUint32 iMagic;						// 'PHRS'
	TUint32 iEntry;						// physical address of APBootCodeEntry (the shim ERETs here)
	TUint32 iRegsPhys;					// -> r0: physical address of this core's SFullArmRegSet
	TUint32 iPad;
	volatile TUint32 iState;			// 0 running, 1 = past Arm::SaveState (so the next return is the resume)
	TUint32 iRecPhys;					// physical address of this record
	volatile TUint64 iOnReqAt;			// CNTPCT when the kernel asked for the power-up
	TUint64 iOffAt;						// CNTPCT just before CPU_OFF
	// GIC state banked per core (distributor SGI/PPI words + the CPU interface) -- restored on the way back
	TUint32 iIsEnabler0, iCfg1, iPri[8], iGiccCtlr, iGiccPmr, iGiccBpr;
	// virtual timer (the nanokernel's timeslice) + the timer access control
	TUint32 iCntkctl, iCntvCtl;
	TUint64 iCntvCval;
	} __attribute__((aligned(64)));

SRetire Ret[KCpus];
// 1 KB aligned: never straddles a page, so one physical address covers the whole set (the AP boot code reads it with
// the MMU off); static kernel data, cached -> cleaned before the power goes
TUint8 RegSet[KCpus][1024] __attribute__((aligned(1024)));

TUint32 BootCodePhys;					// APBootCodeEntry (from SSuperPageBase iReserved[0])
TUint32 PageDirPhys;					// the AP boot page directory
TUint32 CodeOffset;						// linear - physical of the AP boot code (iR2)
TBool RetireReady;
volatile TInt Refusals;
TInt RetireCount, ResumeCount, OffFailures;
TUint32 LatencyUsLast, LatencyUsMax;

// ITS accounting (CPU 0 writes)
volatile TInt ItsTicks;					// set by GetLowPowerMode: ticks to skip (0 = plain WFI until the tick)
TBool ItsOn;							// CPU 0 moved CVAL for this sleep
TInt ItsN;
TUint64 ItsT0;
TUint32 ItsSleeps, ItsSkipped, ItsLate;

const TInt KMinIdleTicks = 2;
const TInt KMaxIdleTicks = 2000;		// at most 2 s per sleep
const TUint32 KTooCloseToTick = 4800;	// 200 us: the tick is too close to move

static_assert(sizeof(SFullArmRegSet) <= 1024, "RegSet[] slot too small");

void RetireOrRefuse(TInt aCpu, SPerCpuUncached* aU) __attribute__((noinline));
}

// ---------------------------------------------------------------------------------------------------------------
class DA64IdleHandler : public DSMPIdleHandler
	{
public:
	DA64IdleHandler() : DSMPIdleHandler() {}
protected:
	TBool DoEnterIdle(TInt aCpuMask, TInt aStage, volatile TAny* aU);
	TBool GetLowPowerMode(TInt aIdleTime, TInt& aLowPowerMode);
	TBool EnterLowPowerMode(TInt aMode, TInt aCpuMask, TBool aLastCpu);
	void PostWakeup(TInt aCpuMask, TBool aLastCpu);
	};

static DA64IdleHandler* TheHandler;

TBool DA64IdleHandler::GetLowPowerMode(TInt aIdleTime, TInt& aLowPowerMode)
	{
	// called by the last core to go idle, interrupts off, everyone else waiting: aIdleTime = empty ticks ahead
	aLowPowerMode = 0;
#ifdef A64_NO_ITS
	ItsTicks = 0;
#else
	ItsTicks = aIdleTime < KMinIdleTicks ? 0 : (aIdleTime > KMaxIdleTicks ? KMaxIdleTicks : aIdleTime);
#endif
	return ETrue;			// always wait (a nearby tick wakes us in <= 1 ms); EFalse would spin the idle loop
	}

TBool DA64IdleHandler::EnterLowPowerMode(TInt /*aMode*/, TInt aCpuMask, TBool /*aLastCpu*/)
	{
	// every engaged core gets here after the sync point; CPU 0 (the tick's owner) moves its own tick first
	if ((aCpuMask & 1) && ItsTicks > 0)
		{
		const TUint64 t0 = CntpCval();		// the next tick's deadline (interrupts.cia re-arms CVAL += reload each tick)
		const TUint64 now = CntPct();
		if ((CntpCtl() & 3) == 1 && t0 > now + KTooCloseToTick)
			{
			ItsN = ItsTicks;
			ItsT0 = t0;
			SetCntpCval(t0 + (TUint64)ItsN * TheTickReload);
			ItsOn = ETrue;
			}
		}
	TIdleSupport::DoWFI();
	return ETrue;			// rouse the others after the wake-up (every core leaves together, one sync point later)
	}

void DA64IdleHandler::PostWakeup(TInt aCpuMask, TBool /*aLastCpu*/)
	{
	// still inside the idle handler (interrupts off); no core leaves before CPU 0 has been here
	if ((aCpuMask & 1) && ItsOn)
		{
		ItsOn = EFalse;
		const TUint64 now = CntPct();
		TInt s = 0;
		if (now >= ItsT0)
			{
			const TUint64 q = (now - ItsT0) / TheTickReload + 1;		// tick boundaries passed since T0
			s = q > (TUint64)ItsN ? ItsN : (TInt)q;
			}
		SetCntpCval(ItsT0 + (TUint64)s * TheTickReload);			// back on the 1 ms phase
		if (s > 0)
			NTimerQ::Advance(s);
		++ItsSleeps;
		ItsSkipped += (TUint32)s;
		if (now >= ItsT0 + (TUint64)ItsN * TheTickReload)
			++ItsLate;						// woken at/after the tick that has work (the normal timer wake)
		}
	}

TBool DA64IdleHandler::DoEnterIdle(TInt aCpuMask, TInt aStage, volatile TAny* aU)
	{
	if (aStage & SCpuIdleHandler::ERetire)
		{
		// the kernel has already stopped this core accepting threads and IPIs: take it down (or refuse)
		const TInt cpu = NKern::CurrentCpu();
		ekseam_puts("[EKERN][power] ERetire on cpu "); ekseam_puthex((unsigned)cpu);
		ekseam_puts(" stage="); ekseam_puthex((unsigned)aStage); ekseam_puts("\n");
		Cr(cpu, 1);
		DiagDelay(6);
		Cr(cpu, 2);
		// the sync points remember which cores arrived: a stale 'all arrived' under the old engaged mask would
		// complete the next round early (the idlehelper's ResetSyncPoints contract). Only while no core is inside
		// the idle protocol (a reset under a running round would strand its waiters).
		if ((TIdleSupport::GetCpusIdleMask() & TIdleSupport::AllCpusMask()) == 0)
			ResetSyncPoints();
		Cr(cpu, 3);
		TIdleSupport::MarkCoreRetired(aCpuMask);
		Cr(cpu, 4);					// the sync points ignore it from now on
		// NO idle IPI here (boot 642 died on it): SGI 7 is only ever sent when EVERY engaged core sits in the idle
		// handler's WFI. A core running a thread (CPU 0) or one leaving idle without ClearIdleIPI takes it in the
		// kernel's IRQ entry, which treats every unknown vector <= 15 as CRASH_IPI -> HandleCrashIPI. The idlers
		// re-evaluate at their next interrupt (the 1 ms tick) anyway.
		RetireOrRefuse(cpu, (SPerCpuUncached*)aU);
		if ((TIdleSupport::GetCpusIdleMask() & TIdleSupport::AllCpusMask()) == 0)
			ResetSyncPoints();
		TIdleSupport::MarkCoreEngaged(aCpuMask);					// back (powered up, or never went)
		return EFalse;			// leave the idle handler; NKern::DoIdle treats a retire as returned from power down
		}
	return ETrue;
	}

// ---------------------------------------------------------------------------------------------------------------
// the retire sequence
namespace
{
void RetireOrRefuse(TInt aCpu, SPerCpuUncached* aU)
	{
	SRetire& R = Ret[aCpu];
	Cr(aCpu, 5);
	if (aCpu == 0 || aCpu >= KCpus || !RetireReady || !R.iRecPhys)
		{
		++Refusals;
		Cr(aCpu, 6);
		if (Refusals <= 3)
			{
			ekseam_puts("[EKERN][power] retire cpu "); ekseam_puthex((unsigned)aCpu); ekseam_puts(" REFUSED\n");
			}
		return;
		}
	SFullArmRegSet& rs = *(SFullArmRegSet*)RegSet[aCpu];
	ekseam_puts("[EKERN][power] retire cpu "); ekseam_puthex((unsigned)aCpu); ekseam_puts(": saving state\n");
	DiagDelay(6);
#ifdef __CPU_HAS_VFP
	// the lazy VFP owner's registers are lost with the power: a thread that comes back to this core must reload
	// them (HandleVFPOperation skips the restore when VfpThread[cpu] == the thread). Its context was already saved
	// to iExtraContext when it was descheduled (NThread::CompleteContextSave); the idle thread runs now.
	Arm::VfpThread[aCpu] = 0;
#endif

	// the per-core hardware state that is not in the register set
	volatile TUint32* gd = (volatile TUint32*)KHwBaseGicDist;
	volatile TUint32* gc = (volatile TUint32*)KHwBaseGicCpu;
	R.iIsEnabler0 = gd[KGicdIsEnabler / 4];
	R.iCfg1 = gd[0xC04 / 4];
	for (TInt i = 0; i < 8; ++i)
		R.iPri[i] = gd[KGicdIPriorityr / 4 + i];
	R.iGiccCtlr = gc[KGiccCtlr / 4];
	R.iGiccPmr = gc[KGiccPmr / 4];
	R.iGiccBpr = gc[0x008 / 4];
	asm volatile("mrc p15, 0, %0, c14, c1, 0" : "=r"(R.iCntkctl));
	asm volatile("mrc p15, 0, %0, c14, c3, 1" : "=r"(R.iCntvCtl));
	{
	TUint32 lo, hi;
	asm volatile("mrrc p15, 3, %0, %1, c14" : "=r"(lo), "=r"(hi));
	R.iCntvCval = ((TUint64)hi << 32) | lo;
	}
	Cr(aCpu, 7);
	R.iState = 0;
	R.iMagic = KResumeMagic;
	R.iEntry = BootCodePhys;

	Cr(aCpu, 8);
	PheSaveCore(rs);					// <-- returns here twice (callee-saved registers preserved)
	if (R.iState == 0)
		{
		// first return: finish the set (what NKern::BootAP puts in a new AP's set), then go
		Cr(aCpu, 9);
		R.iState = 1;
		rs.iN.iR1 = PageDirPhys;		// APBootCodeEntry: AP boot page directory (physical)
		rs.iN.iR2 = CodeOffset;			//   offset linear - physical of the AP boot code
		rs.iN.iR3 = (TUint32)&rs;		//   linear address of this set
		CleanRange(&rs, sizeof(rs));
		ekseam_puts("[EKERN][power] retire cpu "); ekseam_puthex((unsigned)aCpu); ekseam_puts(" -> PSCI CPU_OFF, pc=");
		ekseam_puthex(rs.iN.iR15); ekseam_puts(" sp="); ekseam_puthex(rs.iN.iR13Svc);
		ekseam_puts(" ttbr0="); ekseam_puthex(rs.iB[0].iTTBR0); ekseam_puts(" sctlr="); ekseam_puthex(rs.iB[0].iSCTLR);
		ekseam_puts(" pd="); ekseam_puthex(rs.iN.iR1); ekseam_puts(" off="); ekseam_puthex(rs.iN.iR2); ekseam_puts("\n");
		++RetireCount;
		DiagDelay(10);
		R.iOffAt = CntPct();
		CleanRange(&R, sizeof(R));
		aU->iDetachCount = aU->iDetachCount + 1;		// the kernel's "core has detached" (others may retire next)
		asm volatile("dsb" : : : "memory");
		Cr(aCpu, 10);
		const TInt r = Psci(KPsciCpuOff, 0, 0, 0);
		Cr(aCpu, 11);		// never returns when it works
		// CPU_OFF came back: the core is still here. Carry on as if it had been powered up.
		++OffFailures;
		ekseam_puts("[EKERN][power] CPU_OFF returned r="); ekseam_puthex((unsigned)r); ekseam_puts("\n");
		}
	else
		{
		// second return: we were powered up again; caches and MMU are on again (APBootCodeEntry), nothing else is
		Cr(aCpu, 20);
		const TUint64 now = CntPct();
		const TUint64 req = R.iOnReqAt;
		const TUint32 us = req && now > req ? (TUint32)((now - req) / 24) : 0;		// 24 MHz counter
		LatencyUsLast = us;
		if (us > LatencyUsMax)
			LatencyUsMax = us;
		++ResumeCount;
		}
	// both paths: re-establish the per-core state (a no-op rewrite when CPU_OFF failed)
	gd[0xC04 / 4] = R.iCfg1;
	for (TInt i = 0; i < 8; ++i)
		gd[KGicdIPriorityr / 4 + i] = R.iPri[i];
	gd[KGicdIsEnabler / 4] = R.iIsEnabler0;
	gc[KGiccPmr / 4] = R.iGiccPmr;
	gc[0x008 / 4] = R.iGiccBpr;
	asm volatile("dsb" : : : "memory");
	gc[KGiccCtlr / 4] = R.iGiccCtlr;
	asm volatile("mcr p15, 0, %0, c14, c1, 0" : : "r"(R.iCntkctl));
	asm volatile("mcr p15, 0, %0, c14, c3, 1" : : "r"(R.iCntvCtl));
	asm volatile("mcrr p15, 3, %0, %1, c14" : : "r"((TUint32)R.iCntvCval), "r"((TUint32)(R.iCntvCval >> 32)));
	asm volatile("isb" : : : "memory");
	Cr(aCpu, 21);
	R.iState = 0;
	R.iOnReqAt = 0;
	aU->iAttachCount = aU->iAttachCount + 1;			// the kernel's "core has reattached"
	Cr(aCpu, 22);
	asm volatile("dsb" : : : "memory");
	ekseam_puts("[EKERN][power] cpu "); ekseam_puthex((unsigned)aCpu); ekseam_puts(" back, wake us="); ekseam_puthex(LatencyUsLast);
	ekseam_puts("\n");
	}
}

// ---------------------------------------------------------------------------------------------------------------
// the kernel's power-up request (SVariantInterfaceBlock::iCpuPowerUpFn): CCInitiatePowerUp, thread context
static void A64CpuPowerUp(TInt aCpu, SPerCpuUncached* aU)
	{
	if (aCpu <= 0 || aCpu >= KCpus)
		return;
	if (aU->iDetachCount == aU->iAttachCount)
		return;							// never detached (a retire that was revived in time): nothing to power up
	SRetire& R = Ret[aCpu];
	R.iOnReqAt = CntPct();
	CleanRange(&R, sizeof(R));
	TInt r = KErrGeneral;
	ekseam_puts("[EKERN][power] power up cpu "); ekseam_puthex((unsigned)aCpu); ekseam_puts(": waiting for CPU_OFF to complete\n");
	{
	// PSCI AFFINITY_INFO (as Linux's psci_cpu_kill): CPU_ON is only safe once the core has reached OFF; TF-A flags it
	// OFF just before it cuts the power, and a CPU_ON in that window can be lost (the kernel would then spin forever
	// in CCInitiatePowerUp). 1 = OFF, 0 = ON, 2 = ON_PENDING, negative = unsupported (then the ALREADY_ON retry below)
	const TUint64 affGiveUp = CntPct() + 24000000ull / 2;		// 0.5 s
	TInt a;
	do
		{
		a = Psci(KPsciAffinityInfo, (TUint32)aCpu, 0, 0);
		if (a != 0 && a != 2)
			break;
		const TUint64 t = CntPct() + 2400;					// 100 us
		while (CntPct() < t) {}
		} while (CntPct() < affGiveUp);
	ekseam_puts("[EKERN][power] affinity info cpu "); ekseam_puthex((unsigned)aCpu); ekseam_puts(" = "); ekseam_puthex((unsigned)a); ekseam_puts("\n");
	}
	// CPU_ON answers ALREADY_ON until the core's CPU_OFF has really completed (a few ms at most)
	const TUint64 giveUp = CntPct() + 24000000ull * 2;			// 2 s
	do
		{
		if (aU->iDetachCount == aU->iAttachCount)
			return;						// it came back by itself (CPU_OFF failed)
		r = Psci(KPsciCpuOn, (TUint32)aCpu, KShimBase, R.iRecPhys);
		if (r != KPsciAlreadyOn)
			break;
		const TUint64 t = CntPct() + 24000;			// ~1 ms
		while (CntPct() < t) {}
		} while (CntPct() < giveUp);
	ekseam_puts("[EKERN][power] CPU_ON cpu "); ekseam_puthex((unsigned)aCpu); ekseam_puts(" r="); ekseam_puthex((unsigned)r);
	ekseam_puts(r == 0 ? " ok\n" : " FAILED\n");
	}

extern "C" TCpuPowerUpFn A64CpuPowerUpFn()
	{
#ifndef A64_RETIRE_ON
	return 0;
#else
	return &A64CpuPowerUp;
#endif
	}

#ifdef A64_CC_TEST
// Unattended test of core retirement (A64DEFS=-DA64_CC_TEST; the governor's screen-off policy is off then): 20 s after
// boot, 20 times: ask for ONE active core, 3 s later for all of them, 3 s later again. After each request the kernel's
// own masks (threads / IPIs accepted, cores coming up, per-core detach/attach counts) are logged one second later:
// level 1 must show ta=1 and cores 1..3 detached (retire > resume by the same count), level 4 ta=f and every
// detach == attach. A line "cc test: done" ends it. The 30 s "idle:" line carries the counters and the wake latency.
static TDfcQue* CcQ;
static TDfc* CcDfc;
static NTimer* CcTimer;
static TInt CcStep, CcLevel;
static void CcTimerFn(TAny*) { CcDfc->Add(); }
// The prints of the retire / resume path run in the idle thread (no SD access there): while the test runs a 100 ms
// timer -> DFC flushes the SD log, so the last lines before a hang reach the card (a hang that leaves CPU 0 alive).
static TDfc* CcFlushDfc;
static NTimer* CcFlushTimer;
static TBool CcFlushing;
static void CcLog();
static TInt CcWatch;				// 2 ms flusher calls left in the 'watch' window after a request (0 = off)
static void CcFlushDfcFn(TAny*)
	{
	if (CcWatch > 0)
		{
		if (CcWatch % 10 == 0)		// every 20 ms after a request: the kernel's core-control state, to see where it stalls
			CcLog();
		--CcWatch;
		}
	{
	static TUint32 last[4];
	if (Crumb[1] != last[1] || Crumb[2] != last[2] || Crumb[3] != last[3])
		{
		ekseam_puts("[EKERN][power] crumbs cpu1/2/3=");
		for (TInt i = 1; i < 4; ++i)
			{
			last[i] = Crumb[i];
			ekseam_puthex(last[i]);
			}
		ekseam_puts("\n");
		}
	}
	A64SdFlushIfDirty(0x300);
	if (CcFlushing)
		CcFlushTimer->OneShot(NKern::TimerTicks(2));		// 2 ms: the retire path's prints must be on the card before a hang
	}
static void CcFlushTimerFn(TAny*) { CcFlushDfc->Add(); }
static void CcLog()
	{
	TScheduler& s = TheScheduler;
	ekseam_puts("[EKERN][power] cc test: level="); ekseam_puthex((unsigned)CcLevel);
	ekseam_puts(" ta="); ekseam_puthex(s.iThreadAcceptCpus);
	ekseam_puts(" ipi="); ekseam_puthex(s.iIpiAcceptCpus);
	ekseam_puts(" up="); ekseam_puthex(s.iCpusComingUp);
	ekseam_puts(" react="); ekseam_puthex(s.iCCReactivateCpus);
	for (TInt i = 0; i < NKern::NumberOfCpus() && i < KCpus; ++i)
		{
		const SPerCpuUncached& u = ((UPerCpuUncached*)TheSubSchedulers[i].iUncached)->iU;
		ekseam_puts(" c"); ekseam_puthex((unsigned)i); ekseam_puts("="); ekseam_puthex(u.iDetachCount);
		ekseam_puts("/"); ekseam_puthex(u.iAttachCount);
		}
	ekseam_puts(" retires="); ekseam_puthex((unsigned)RetireCount);
	ekseam_puts(" resumes="); ekseam_puthex((unsigned)ResumeCount);
	ekseam_puts("\n");
	}
static void CcRun(TAny*)
	{
	if (CcStep & 1)
		{
		CcLog();					// one second after the request
		if (CcStep >= 39)
			{
			ekseam_puts("[EKERN][power] cc test: done\n");
			CcFlushing = EFalse;
			A64SdFlushNow(0x301);
			return;
			}
		CcTimer->OneShot(NKern::TimerTicks(2000));
		++CcStep;
		return;
		}
	CcLevel = (CcStep & 2) ? NKern::NumberOfCpus() : 1;
	ekseam_puts("[EKERN][power] cc test: SetNumberOfActiveCpus("); ekseam_puthex((unsigned)CcLevel); ekseam_puts(")\n");
	A64SdFlushNow(0x302);				// this line must be on the card before the kernel starts retiring cores
	CcWatch = 250;
	NKern::SetNumberOfActiveCpus(CcLevel);
	ekseam_puts("[EKERN][power] cc test: SetNumberOfActiveCpus returned\n");
	A64SdFlushNow(0x303);
	++CcStep;
	CcTimer->OneShot(NKern::TimerTicks(1000));
	}
static void CcStart()
	{
	_LIT8(KQName, "CcTest");
	if (Kern::DfcQCreate(CcQ, 26, &KQName) != KErrNone)
		return;
	CcDfc = new TDfc(CcRun, NULL, CcQ, 1);
	CcTimer = new NTimer(CcTimerFn, NULL);
	CcFlushDfc = new TDfc(CcFlushDfcFn, NULL, CcQ, 1);
	CcFlushTimer = new NTimer(CcFlushTimerFn, NULL);
	if (CcDfc && CcTimer)
		CcTimer->OneShot(NKern::TimerTicks(20000));
	if (CcFlushDfc && CcFlushTimer)
		{
		CcFlushing = ETrue;
		CcFlushTimer->OneShot(NKern::TimerTicks(19000));		// from 1 s before the first request
		}
	ekseam_puts("[EKERN][power] cc test armed (20 s)\n");
	}
#endif

// ---------------------------------------------------------------------------------------------------------------
// start (a64_glue.cpp, thread context after the extensions): physical addresses, then the handler
extern "C" TInt A64IdleStart()
	{
#ifdef A64_NO_IDLEHANDLER
	return KErrNone;
#else
	TInt r = KErrNone;
	TSuperPage& spg = TheSuperPage();
#ifdef A64_RETIRE_ON
	CrumbInit();
	BootCodePhys =((TUint32*)&spg.iReserved[0])[0];		// stored by the bootstrap (a64.s FinalInitialise)
	PageDirPhys = spg.iAPBootPageDirPhys;
	CodeOffset = (TUint32)RomHeaderAddress - spg.iRomHeaderPhys;
	TBool ok = BootCodePhys != 0 && PageDirPhys != 0;
	for (TInt i = 1; i < KCpus && ok; ++i)
		{
		const TPhysAddr pr = Epoc::LinearToPhysical((TLinAddr)&Ret[i]);
		const TPhysAddr ps = Epoc::LinearToPhysical((TLinAddr)RegSet[i]);
		if (pr == KPhysAddrInvalid || ps == KPhysAddrInvalid || (pr & 15))
			{
			ok = EFalse;
			break;
			}
		Ret[i].iRecPhys = pr;
		Ret[i].iRegsPhys = ps;
		Ret[i].iEntry = BootCodePhys;
		Ret[i].iMagic = KResumeMagic;
		CleanRange(&Ret[i], sizeof(SRetire));
		}
	RetireReady = ok;
	ekseam_puts("[EKERN][power] retire: AP boot code phys="); ekseam_puthex(BootCodePhys);
	ekseam_puts(" pagedir="); ekseam_puthex(PageDirPhys);
	ekseam_puts(" offset="); ekseam_puthex(CodeOffset);
	ekseam_puts(ok ? " ready\n" : " NOT READY (core retirement unavailable)\n");
#endif
	NKern::ThreadEnterCS();
	TheHandler = new DA64IdleHandler;
	if (TheHandler)
		TheHandler->Initialise(KHwBaseGicDist, KHwBaseGicCpu);		// binds the idle handler (Arm::SetIdleHandler)
	else
		r = KErrNoMemory;
	NKern::ThreadLeaveCS();
#ifdef A64_CC_TEST
	CcStart();
#endif
	ekseam_puts("[EKERN][power] idle handler (tick suppression");
#ifndef A64_NO_ITS
	ekseam_puts(" on");
#else
	ekseam_puts(" off");
#endif
	ekseam_puts(", core retirement ");
	ekseam_puts(RetireReady ? "ready" : "off");
	ekseam_puts(") r="); ekseam_puthex((unsigned)r); ekseam_puts("\n");
	return r;
#endif
	}

// the 30 s line (a64_cpufreq.cpp governor summary)
extern "C" void A64IdleReport()
	{
	ekseam_puts("[EKERN][power] idle: its sleeps="); ekseam_puthex(ItsSleeps);
	ekseam_puts(" ticks skipped="); ekseam_puthex(ItsSkipped);
	ekseam_puts(" retires="); ekseam_puthex((unsigned)RetireCount);
	ekseam_puts(" resumes="); ekseam_puthex((unsigned)ResumeCount);
	ekseam_puts(" refused="); ekseam_puthex((unsigned)Refusals);
	ekseam_puts(" cpu_off fails="); ekseam_puthex((unsigned)OffFailures);
	ekseam_puts(" wake us last/max="); ekseam_puthex(LatencyUsLast); ekseam_puts("/"); ekseam_puthex(LatencyUsMax);
	ekseam_puts("\n");
	}

extern "C" TInt A64CoreRetireRefusals() { return Refusals; }
extern "C" TBool A64CoreRetireReady() { return RetireReady; }

#else	// !__SMP__
extern "C" TInt A64IdleStart() { return KErrNone; }
extern "C" void A64IdleReport() {}
extern "C" TInt A64CoreRetireRefusals() { return 0; }
extern "C" TBool A64CoreRetireReady() { return EFalse; }
#endif
