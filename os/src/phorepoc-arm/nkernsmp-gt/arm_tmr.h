// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// OURS (PinePhone SMP, docs/phorepoc/pinephone-port.md "generic-timer stand-in"): <arm_tmr.h> for an ARMv7
// generic-timer CPU (Cortex-A7/A15, and the A53 in AArch32) in place of the ARM11MP/Cortex-A9 private timer.
// On this include path FIRST, ahead of kernel/eka/include/nkernsmp/arm. The 2010 SMP nanokernel's C++ code uses
// the per-CPU timer only through ArmLocalTimer members (T.iTimerCount = n, reads of it, iTimerCtrl,
// iTimerIntStatus, the watchdog lines); here each member is a 4-byte PROXY whose assignment/read is a cp15
// access to the virtual timer, so those sources compile UNCHANGED. Layout (offsets, size 0x100) is kept for
// the asm users; the two asm sites that touch the registers are patched separately (nkernsmp_gt_patch.py).
//
// Mapping (both are 32-bit down-counters that keep counting through 0 after the event):
//   iTimerCount = n   -> CNTV_TVAL = n (next deadline), unmask if the timer is enabled
//   (TUint32)iTimerCount -> CNTV_TVAL (CVAL - CNT: goes "negative" after it fires, as the A9 count wraps)
//   iTimerCtrl = f    -> CNTV_CTL: ENABLE = f & Enable, IMASK = !(f & IntEn) (reload is implicit)
//   iTimerIntStatus = Event -> CNTV_CTL.IMASK = 1 (the PPI is level-triggered: mask until re-armed)
//   iTimerLoad, iWatchdog* -> ignored (no reload register; no per-CPU watchdog)
// The timeslice interrupt is the virtual timer PPI (ID 27), see TIMESLICE_VECTOR (nk_plat.h patch).

#ifndef	__ARM_TMR_H__
#define	__ARM_TMR_H__
#include <e32def.h>

#ifdef	__STANDALONE_NANOKERNEL__
#undef	__IN_KERNEL__
#define	__IN_KERNEL__
#endif

#ifndef __CIA__
inline TUint32 ArmGtCtl()				{ TUint32 v; asm volatile("mrc p15, 0, %0, c14, c3, 1" : "=r"(v)); return v; }
inline void ArmGtSetCtl(TUint32 aV)		{ asm volatile("mcr p15, 0, %0, c14, c3, 1" : : "r"(aV) : "memory"); asm volatile("isb" ::: "memory"); }
inline TUint32 ArmGtTval()				{ TUint32 v; asm volatile("mrc p15, 0, %0, c14, c3, 0" : "=r"(v)); return v; }
inline void ArmGtSetTval(TUint32 aV)	{ asm volatile("mcr p15, 0, %0, c14, c3, 0" : : "r"(aV) : "memory"); asm volatile("isb" ::: "memory"); }

enum { EArmGtCtlEnable = 1u, EArmGtCtlIMask = 2u, EArmGtCtlIStatus = 4u };

struct TArmGtCount		// iTimerCount
	{
	operator TUint32() const			{ return ArmGtTval(); }
	TArmGtCount& operator=(TUint32 aV)	{ ArmGtSetTval(aV); TUint32 c = ArmGtCtl(); if (c & EArmGtCtlEnable) ArmGtSetCtl(EArmGtCtlEnable); return *this; }
	TUint32 iPad;
	};
struct TArmGtCtrl		// iTimerCtrl
	{
	operator TUint32() const			{ return ArmGtCtl() & EArmGtCtlEnable; }
	TArmGtCtrl& operator=(TUint32 aV)	{ ArmGtSetCtl(((aV & 1u) ? EArmGtCtlEnable : 0u) | ((aV & 4u) ? 0u : EArmGtCtlIMask)); return *this; }
	TUint32 iPad;
	};
struct TArmGtStatus		// iTimerIntStatus
	{
	operator TUint32() const			{ return (ArmGtCtl() & EArmGtCtlIStatus) ? 1u : 0u; }
	TArmGtStatus& operator=(TUint32 aV)	{ if (aV & 1u) ArmGtSetCtl(ArmGtCtl() | EArmGtCtlIMask); return *this; }
	TUint32 iPad;
	};
struct TArmGtSink		// iTimerLoad, the watchdog registers
	{
	operator TUint32() const			{ return 0; }
	TArmGtSink& operator=(TUint32)		{ return *this; }
	TUint32 iPad;
	};

struct ArmLocalTimer
	{
	TArmGtSink			iTimerLoad;				// 00
	TArmGtCount			iTimerCount;			// 04
	TArmGtCtrl			iTimerCtrl;				// 08
	TArmGtStatus		iTimerIntStatus;		// 0C
	TUint32				i_Spare1[4];			// 10
	TArmGtSink			iWatchdogLoad;			// 20
	TArmGtSink			iWatchdogCount;			// 24
	TArmGtSink			iWatchdogCtrl;			// 28
	TArmGtSink			iWatchdogIntStatus;		// 2C
	TArmGtSink			iWatchdogResetSent;		// 30
	TArmGtSink			iWatchdogDisable;		// 34
	TUint32				i_Spare2[50];			// 38
	};
__ASSERT_COMPILE(sizeof(ArmLocalTimer)==0x100);
#else
struct ArmLocalTimer	// the .cia files only take _FOFF() of these
	{
	TUint32 iTimerLoad, iTimerCount, iTimerCtrl, iTimerIntStatus, i_Spare1[4];
	TUint32 iWatchdogLoad, iWatchdogCount, iWatchdogCtrl, iWatchdogIntStatus, iWatchdogResetSent, iWatchdogDisable;
	TUint32 i_Spare2[50];
	};
#endif

// These bits apply to both timer and watchdog control registers (the 2010 values; the proxies decode them)
enum TArmTimerCtrl
	{
	E_ArmTmrCtrl_Enable			=1u,
	E_ArmTmrCtrl_Reload			=2u,
	E_ArmTmrCtrl_IntEn			=4u,
	E_ArmTmrCtrl_WD				=8u,
	E_ArmTmrCtrl_PrescaleShift	=8u,
	E_ArmTmrCtrl_PrescaleMask	=0xff00u,
	E_ArmTmrCtrl_Prescale64		=0x3f00u,
	};

enum TArmTimerIntStatus
	{
	E_ArmTmrIntStatus_Event		=1u
	};

enum TArmTimerWRS
	{
	E_ArmTmrWRS_ResetSent		=1u
	};

enum TArmTimerWDDisable
	{
	E_ArmTmrWDD_1				=0x12345678u,
	E_ArmTmrWDD_2				=0x87654321u,
	};

#endif	// 	__ARM_TMR_H__
