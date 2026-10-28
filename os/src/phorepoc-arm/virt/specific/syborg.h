// OURS (track): the QEMU `virt` variant, derived from
// oss.FCL.sf.adapt.Qemu/baseport/syborg/specific/syborg.h. Class names stay Syborg/TSyborg for now (the ROM
// scripts export them by name); deltas are marked VIRT:.
/*
* Copyright (c) 2009 Nokia Corporation and/or its subsidiary(-ies).
* All rights reserved.
* This component and the accompanying materials are made available
* under the terms of the License "Eclipse Public License v1.0"
* which accompanies this distribution, and is available
* at the URL "http://www.eclipse.org/legal/epl-v10.html".
*
* Initial Contributors:
* Nokia Corporation - initial contribution.
*
* Contributors:
* NTT DOCOMO, INC : BUG 1296
* NTT DOCOMO, INC : BUG 3598
* NTT DOCOMO, INC - Fix for bug 1291 "E32test t_tock.exe failed to load Logical Device"
*
* Description:
*
*/

#ifndef __SYBORG_H__
#define __SYBORG_H__

#include <e32const.h>
#include <platform.h>
#include <e32hal.h>
#include <assp.h>
#include <kern_priv.h>
#include <mmboot.h>     // KPrimaryIOBase

#ifndef __SYBORG__
#define __SYBORG__
#endif

const TUint KHwBasePeripherals  = KPrimaryIOBase;       
const TUint KHwLinSeparation	= 0x1000;
const TInt KMsTickPeriod	= 1000;	// VIRT/A64: the tick is the generic timer at 1 ms (CNTFRQ/1000); Syborg's 10000 = its
							// 100 Hz timer, kept by mistake = every NTimer 10x early (phone clock ran 6.5x fast, 2026-10-06)

#ifdef __A64__
// A64 (BOARD=a64, src/phorepoc-arm/a64): the same variant on the Allwinner A64. Linear addresses of
// its bootstrap's HW banks (a64/bootstrap/a64.inc GetHwBanks order), KPrimaryIOBase + n * 4K
const TUint KHwBaseGicDist			= KHwBasePeripherals + 0x00*KHwLinSeparation;	// GIC-400
const TUint KHwBaseGicCpu			= KHwBasePeripherals + 0x01*KHwLinSeparation;	// 2 pages
const TUint KHwBaseUart0			= KHwBasePeripherals + 0x03*KHwLinSeparation;	// 16550
const TUint KHwBaseCcuPio			= KHwBasePeripherals + 0x04*KHwLinSeparation;	// CCU +0, PIO +0x800
const TUint KHwBaseRPio				= KHwBasePeripherals + 0x05*KHwLinSeparation;
const TUint KHwBaseUart1			= KHwBaseUart0;
const TUint KHwBaseUart2			= KHwBaseUart0;
const TUint KHwBaseUart3			= KHwBaseUart0;
const TUint KUartLsr = 0x14, KUartLsrThre = 0x20;		// 16550, 32-bit register stride
#else
// VIRT: linear addresses of the bootstrap's HW banks (bootstrap/virt.inc), KPrimaryIOBase + n * 4K
const TUint KHwBaseGicDist			= KHwBasePeripherals + 0x00*KHwLinSeparation;
const TUint KHwBaseGicCpu			= KHwBasePeripherals + 0x01*KHwLinSeparation;
const TUint KHwBaseUart0			= KHwBasePeripherals + 0x02*KHwLinSeparation;	// PL011
const TUint KHwBaseRtc				= KHwBasePeripherals + 0x03*KHwLinSeparation;	// PL031
const TUint KHwBaseFwCfg			= KHwBasePeripherals + 0x04*KHwLinSeparation;
const TUint KHwBaseGpio				= KHwBasePeripherals + 0x05*KHwLinSeparation;	// PL061
const TUint KHwBaseVirtio			= KHwBasePeripherals + 0x06*KHwLinSeparation;	// 4 pages
const TUint KHwBaseUart1			= KHwBaseUart0;
const TUint KHwBaseUart2			= KHwBaseUart0;
const TUint KHwBaseUart3			= KHwBaseUart0;
#endif

// VIRT: GICv2 register offsets (A64: the GIC-400 is a GICv2)
const TUint KGicdCtlr = 0x000, KGicdIsEnabler = 0x100, KGicdIcEnabler = 0x180, KGicdIPriorityr = 0x400,
            KGicdITargetsr = 0x800;
const TUint KGiccCtlr = 0x000, KGiccPmr = 0x004, KGiccIar = 0x00C, KGiccEoir = 0x010;

enum TSyborgInterruptId
{
  // VIRT: GIC interrupt ids (PPIs 16-31, SPIs from 32; QEMU hw/arm/virt.c)
  EIntVirtTimer = 27,	/* the generic timer's virtual timer PPI = the 1 ms tick (UP) */
  EIntPhysTimer = 30,	/* the non-secure physical timer PPI = the 1 ms tick under __SMP__ (CNTV = the timeslice) */
#ifdef __SMP__
  EIntTickTimer = EIntPhysTimer,
#else
  EIntTickTimer = EIntVirtTimer,
#endif
#ifdef __A64__
  EIntSerial0 = 32		/* A64 UART0, SPI 0 (sun50i-a64.dtsi) */
#else
  EIntSerial0 = 33,		/* PL011, SPI 1 */
  EIntRtc = 34,			/* PL031, SPI 2 */
  EIntVirtio0 = 48		/* virtio-mmio transport 0, SPI 16 (32 of them) */
#endif
};

// Timer Mode
const TUint KPeriodic			= 0x0;
const TUint KOneShot		    = 0x1;

class TSyborg
{
public:
	// generic enums
	enum TState
	{
		EEnable,
		EDisable
	};
	enum TLock
	{
		ELocked=0,
		EUnlocked=0xA05F
	};
    enum TTimerMode
	{
	  ETimerModePeriodic=KPeriodic,
	  ETimerModeOneShot=KOneShot,
	};
public:
    // Initialisation of class
	static void Init3();

    // Accessor methods for timers 
	IMPORT_C static void EnableTimerInterrupt(TUint aTimerBase);                    // Enable the timer interrupt
	IMPORT_C static void DisableTimerInterrupt(TUint aTimerBase);                   // Disable the timer interrupt
    IMPORT_C static void SetTimerLoad(TUint aTimerBase, TUint32 aValue);			// Set the starting count value for a timer
    IMPORT_C static TUint TimerLoad(TUint aTimerBase);							    // Read the load register (starting count value)
    IMPORT_C static TUint TimerValue(TUint aTimerBase);							    // Read the actual timer value
    IMPORT_C static void ClearTimerInt(TUint aTimerBase); 						    // Clear the timer interrupt
    IMPORT_C static TBool IsTimerEnabled(TUint aTimerBase);						    // Enquire as to whether the timer is enabled
    IMPORT_C static void EnableTimer(TUint aTimerBase, TState aState);			    // Enable/disable the timer (start/stop it running)
    IMPORT_C static TTimerMode TimerMode(TUint aTimerBase);						    // Find out what mode the timer is running in
    IMPORT_C static void SetTimerMode(TUint aTimerBase, TTimerMode aValue);		    // Set the timer mode (periodic or free running)
	
    // Accessor methods for interrupts
    IMPORT_C static void  EnableInt(TUint anId);					
    IMPORT_C static void  DisableInt(TUint anId);					
        
	IMPORT_C static TInt VideoRamSize();
	IMPORT_C static TPhysAddr VideoRamPhys();
	IMPORT_C static TPhysAddr VideoRamPhysSecure();

	// Debug Port Specific
	IMPORT_C static TUint32 DebugPortAddr();
	IMPORT_C static void MarkDebugPortOff();
};

static inline TUint32 ReadReg(TUint32 base, TUint32 aReg)
{
  return *(volatile TUint32 *)(base + (aReg << 2));
}

static inline void WriteReg(TUint32 base, TUint32 aReg, TUint32 aVal)
{
  *(volatile TUint32*)(base + (aReg<<2)) = aVal;
}

#endif  // __SYBORG_H__
