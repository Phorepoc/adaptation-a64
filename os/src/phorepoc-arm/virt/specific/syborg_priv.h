// OURS (PinePhone track, docs/phorepoc/pinephone-port.md): the QEMU `virt` variant, derived from
// oss.FCL.sf.adapt.Qemu/baseport/syborg/specific/syborg_priv.h. Class names stay Syborg/TSyborg for now (the ROM
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
*
* Description:
*
*/

#ifndef __SYBORG_PRIV_H__
#define __SYBORG_PRIV_H__

#include <e32const.h>
#include <arm.h>
#include <assp.h>
#include <syborg.h>

#define	_LOCK		TInt irq=NKern::DisableAllInterrupts();
#define	_UNLOCK		NKern::RestoreInterrupts(irq);

const TUint32 K1000HzTickMatchLoad = 1000;
const TInt KNumSyborgInts = 192;	// VIRT/A64: GIC ids 0-191 (SGIs, PPIs, 160 SPIs: the A64 Mali is at 129-135; = NK_MAX_IRQS + 32)

class SyborgInterrupt : public Interrupt
{
public:
#ifdef __SMP__
	static TUint32 IrqDispatch(TUint32 aVector);	// VIRT SMP: called by the nanokernel with the GIC vector
#else
	static void IrqDispatch();
#endif
	static void FiqDispatch();
	static void DisableAndClearAll();
	static void Init1();
	static void Init3();
	static void Spurious(TAny* anId);
    static void MsTimerTick(TAny* aPtr);
    static void StartTick();	// VIRT

	static SInterruptHandler Handlers[KNumSyborgInts];
};

class Syborg : public Asic
{
public:
	IMPORT_C Syborg();

public:
	// Initialisation
	IMPORT_C virtual TMachineStartupType StartupReason();
	IMPORT_C virtual void Init1();
	IMPORT_C virtual void Init3();
#ifdef __SMP__
	virtual void Init2AP();	// VIRT SMP: per secondary CPU at boot (NaviEngine: empty too)
#endif

	// Power management
	IMPORT_C virtual void Idle();

	IMPORT_C void DebugInit();
	IMPORT_C virtual void DebugOutput(TUint aChar);

	// Timing
	IMPORT_C virtual TInt MsTickPeriod();
	IMPORT_C virtual TInt SystemTimeInSecondsFrom2000(TInt& aTime);
	IMPORT_C virtual TInt SetSystemTimeInSecondsFrom2000(TInt aTime);
	IMPORT_C virtual TUint32 NanoWaitCalibration();

	// HAL
	virtual TInt VariantHal(TInt aFunction, TAny* a1, TAny* a2);

	// Machine configuration
	virtual TPtr8 MachineConfiguration();

	TUint32 iDebugPortBase;

	static Syborg* Variant;
	static TPhysAddr VideoRamPhys;
	static TPhysAddr VideoRamPhysSecure;
	NTimerQ* iTimerQ;
};

GLREF_D Syborg TheVariant;

#endif  // __SYBORG_PRIV_H__

