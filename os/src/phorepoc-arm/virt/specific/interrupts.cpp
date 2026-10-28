// OURS (track): the QEMU `virt` variant, derived from
// oss.FCL.sf.adapt.Qemu/baseport/syborg/specific/interrupts.cpp. Class names stay Syborg/TSyborg for now (the ROM
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
* Description: Syborg interrupt control and dispatch
*
*/

#include <syborg_priv.h>

#ifdef __SMP__
// VIRT SMP (the phone SMP rung; NaviEngine naviengine_assp/interrupts.cpp
// pattern): the SMP nanokernel owns the GIC (acknowledge, EOI, routing, per-CPU state). The variant's IRQ handler
// gets the vector: SPIs go to NKern::Interrupt(vector-32) (the nanokernel's interrupt numbers = SPI index), the
// 1 ms tick (the physical timer PPI 30 -- CNTV is the timeslice) is serviced here and completed here.
SInterruptHandler SyborgInterrupt::Handlers[KNumSyborgInts];	// only the tick PPI's entry is used
extern "C" { TUint32 TheTickReload; }

static inline volatile TUint32& Reg(TUint32 aBase, TUint32 aOffset)
{
  return *(volatile TUint32*)(aBase + aOffset);
}

const TInt KFirstSpi = 32;

TUint32 SyborgInterrupt::IrqDispatch(TUint32 aVector)
{
	if (aVector == (TUint32)EIntTickTimer)
	{
		SInterruptHandler& h = Handlers[aVector];
		(*h.iIsr)(h.iPtr);
		Reg(KHwBaseGicCpu, KGiccEoir) = aVector;
		return aVector;
	}
	if (aVector < (TUint32)KFirstSpi || aVector >= (TUint32)KNumSyborgInts)
	{
		Reg(KHwBaseGicCpu, KGiccEoir) = aVector;	// not ours: complete it so the GIC is not wedged
		return aVector;
	}
	NKern::Interrupt(aVector - KFirstSpi);
	return aVector;
}

EXPORT_C void TSyborg::EnableInt(TUint anId)
{
  Reg(KHwBaseGicDist, KGicdIsEnabler + 4 * (anId >> 5)) = 1u << (anId & 31);
}

EXPORT_C void TSyborg::DisableInt(TUint anId)
{
  Reg(KHwBaseGicDist, KGicdIcEnabler + 4 * (anId >> 5)) = 1u << (anId & 31);
}

void SyborgInterrupt::DisableAndClearAll()
{
	// the nanokernel does all the GIC set-up (NKern::Init0 -> InterruptInit0)
}

void SyborgInterrupt::Init1()
{
	__KTRACE_OPT(KBOOT,Kern::Printf("SyborgInterrupt::Init1() SMP"));
	for (TUint i = 0; i < KNumSyborgInts; i++)
	{
		Handlers[i].iPtr = (TAny*)i;
		Handlers[i].iIsr = Spurious;
	}
	Arm::SetIrqHandler((TLinAddr)&SyborgInterrupt::IrqDispatch);
	Arm::SetFiqHandler((TLinAddr)&SyborgInterrupt::FiqDispatch);
	for (TInt v = KFirstSpi; v < KNumSyborgInts; ++v)
	{
		TInt r = NKern::InterruptInit(v - KFirstSpi, NKern::EIrqInit_LevelHigh, v, v);	// virt's SPIs are level
		__NK_ASSERT_ALWAYS(r == KErrNone);
	}
}

void SyborgInterrupt::Init3()
{
}

void SyborgInterrupt::Spurious(TAny* anId)
{
	Kern::Fault("SpuriousInt", (TInt)anId);
}

EXPORT_C TInt Interrupt::Bind(TInt anId, TIsr anIsr, TAny* aPtr)
{
	if (anId == EIntTickTimer)
	{
		SInterruptHandler& h = SyborgInterrupt::Handlers[anId];
		if (h.iIsr != SyborgInterrupt::Spurious)
			return KErrInUse;
		h.iPtr = aPtr;
		h.iIsr = anIsr;
		return KErrNone;
	}
	if (anId < KFirstSpi || anId >= KNumSyborgInts)
		return KErrArgument;
	// NKern::InterruptBind returns a HANDLE (>= 0x10000) on success. The UP contract -- and every driver here,
	// incl. Syborg's NVMemory (DoCreate `return r` of Bind: a positive r = "create failed" -> C: KErrNotReady)
	// and our virtinput (`if (r == KErrNone)`) -- is KErrNone; they Enable/Disable/Unbind by ID, which works below.
	TInt r = NKern::InterruptBind(anId - KFirstSpi, anIsr, aPtr, 0, 0);
	return r < 0 ? r : KErrNone;
}

EXPORT_C TInt Interrupt::Unbind(TInt anId)
{
	if (anId >= 0x10000)
		return NKern::InterruptUnbind(anId);		// a handle from Bind
	if (anId == EIntTickTimer)
	{
		TSyborg::DisableInt(anId);
		SyborgInterrupt::Handlers[anId].iIsr = SyborgInterrupt::Spurious;
		return KErrNone;
	}
	if (anId >= KFirstSpi && anId < KNumSyborgInts)
		return NKern::InterruptUnbind(anId - KFirstSpi);
	return KErrArgument;
}

EXPORT_C TInt Interrupt::Enable(TInt anId)
{
	if (anId >= 0x10000)
		return NKern::InterruptEnable(anId);
	if (anId == EIntTickTimer)
	{
		Reg(KHwBaseGicDist, KGicdIPriorityr + (anId & ~3)) = 0xa0a0a0a0u;	// PPIs 28-31 (banked, CPU 0)
		TSyborg::EnableInt(anId);
		return KErrNone;
	}
	if (anId >= KFirstSpi && anId < KNumSyborgInts)
		return NKern::InterruptEnable(anId - KFirstSpi);
	return KErrArgument;
}

EXPORT_C TInt Interrupt::Disable(TInt anId)
{
	if (anId >= 0x10000)
		return NKern::InterruptDisable(anId);
	if (anId == EIntTickTimer)
	{
		TSyborg::DisableInt(anId);
		return KErrNone;
	}
	if (anId >= KFirstSpi && anId < KNumSyborgInts)
		return NKern::InterruptDisable(anId - KFirstSpi);
	return KErrArgument;
}

EXPORT_C TInt Interrupt::Clear(TInt anId)
{
	if (anId >= 0x10000)
		return NKern::InterruptClear(anId);
	if (anId >= 0 && anId < KNumSyborgInts)
		return KErrNone;		// level-sensitive: the device's own clear drops the line
	return KErrArgument;
}

EXPORT_C TInt Interrupt::SetPriority(TInt /*anId*/, TInt /*aPriority*/)
{
	return KErrNotSupported;
}

#else	// !__SMP__ -- the uniprocessor variant: our own GIC driver

SInterruptHandler SyborgInterrupt::Handlers[KNumSyborgInts];
extern "C" { TUint32 TheTickReload; }	// VIRT: CNTFRQ/1000, set by StartTick, re-armed by MsTimerTick (interrupts.cia)

static inline volatile TUint32& Reg(TUint32 aBase, TUint32 aOffset)
{
  return *(volatile TUint32*)(aBase + aOffset);
}

// VIRT: GICv2 -- everything disabled, every interrupt at one priority and (SPIs) routed to CPU 0, both
// interfaces on. Enable/Disable below set/clear one bit; the IRQ dispatcher (interrupts.cia) acknowledges
// through GICC_IAR and completes through GICC_EOIR.
void SyborgInterrupt::DisableAndClearAll()
{
  Reg(KHwBaseGicDist, KGicdCtlr) = 0;
  for (TUint i = 0; i < KNumSyborgInts / 32; i++)
    Reg(KHwBaseGicDist, KGicdIcEnabler + 4 * i) = 0xffffffffu;
  for (TUint i = 0; i < KNumSyborgInts / 4; i++)
    Reg(KHwBaseGicDist, KGicdIPriorityr + 4 * i) = 0xa0a0a0a0u;
  for (TUint i = 32 / 4; i < KNumSyborgInts / 4; i++)
    Reg(KHwBaseGicDist, KGicdITargetsr + 4 * i) = 0x01010101u;
  Reg(KHwBaseGicDist, KGicdCtlr) = 1;
  Reg(KHwBaseGicCpu, KGiccPmr) = 0xf0;
  Reg(KHwBaseGicCpu, KGiccCtlr) = 1;
}

EXPORT_C void TSyborg::EnableInt(TUint anId)
{
  Reg(KHwBaseGicDist, KGicdIsEnabler + 4 * (anId >> 5)) = 1u << (anId & 31);
}

EXPORT_C void TSyborg::DisableInt(TUint anId)
{
  Reg(KHwBaseGicDist, KGicdIcEnabler + 4 * (anId >> 5)) = 1u << (anId & 31);
}

void SyborgInterrupt::Init1()
{
	__KTRACE_OPT(KBOOT,Kern::Printf("SyborgInterrupt::Init1()"));

	for(TUint i = 0; i < KNumSyborgInts; i++)
	{
		Handlers[i].iPtr = (TAny*)i;
		Handlers[i].iIsr = Spurious;
	}

	DisableAndClearAll();
	
	Arm::SetIrqHandler((TLinAddr)SyborgInterrupt::IrqDispatch);
	Arm::SetFiqHandler((TLinAddr)SyborgInterrupt::FiqDispatch);
}

void SyborgInterrupt::Init3()
{
	__KTRACE_OPT(KHARDWARE,Kern::Printf("SyborgInterrupt::Init3()"));
}

void SyborgInterrupt::Spurious(TAny* anId)
{
	// Handle an unexpected interrupt
	Kern::Fault("SpuriousInt", (TInt)anId);
}

EXPORT_C TInt Interrupt::Bind(TInt anId, TIsr anIsr, TAny* aPtr)
{
  __KTRACE_OPT(KHARDWARE,Kern::Printf("Interrupt::Bind(anId=%d anIsr=0x%X aPtr=0x%X)",anId,anIsr,aPtr));
	if((anId >= 0) && ((TUint)anId < KNumSyborgInts))
	{
		SInterruptHandler& h = SyborgInterrupt::Handlers[anId];
		TInt irq = NKern::DisableAllInterrupts();
        TInt r;
		if(h.iIsr != SyborgInterrupt::Spurious)
		{
            r = KErrInUse;
	    }
		else
		{
    	    h.iPtr = aPtr;
	    	h.iIsr = anIsr;
            r = KErrNone;
		}
		NKern::RestoreInterrupts(irq);
		return r;
	}
	return KErrArgument;
}

EXPORT_C TInt Interrupt::Unbind(TInt anId)
{
	__KTRACE_OPT(KHARDWARE,Kern::Printf("Interrupt::Unbind(%d)",anId));
	if((anId >= 0) && ((TUint)anId < KNumSyborgInts))
	{
		SInterruptHandler& h = SyborgInterrupt::Handlers[anId];
		TInt irq = NKern::DisableAllInterrupts();
    	TInt r;
		if(h.iIsr == SyborgInterrupt::Spurious)
		{
			r = KErrGeneral;
		}
		else
		{
            // Reset pointer to handler back to default
			h.iPtr = (TAny*)anId;
			h.iIsr = SyborgInterrupt::Spurious;	
			// Disable the interrupt
			TSyborg::DisableInt(anId);
			r = KErrNone;
        }
		NKern::RestoreInterrupts(irq);
    	return r;
	}
	return KErrArgument;
}

EXPORT_C TInt Interrupt::Enable(TInt anId)
{
	__KTRACE_OPT(KHARDWARE,Kern::Printf("Interrupt::Enable(%d)",anId));
	if((anId >= 0) && ((TUint)anId < KNumSyborgInts))
	{
		if(SyborgInterrupt::Handlers[anId].iIsr == SyborgInterrupt::Spurious)
		{
	        return KErrNotReady;
		}
		else
		{
		  TSyborg::EnableInt(anId);
		  return KErrNone;
		}
	}
	return KErrArgument;
}

EXPORT_C TInt Interrupt::Disable(TInt anId)
{
	__KTRACE_OPT(KHARDWARE,Kern::Printf("Interrupt::Disable(%d)",anId));
	if((anId >= 0) && ((TUint)anId < KNumSyborgInts))
	{
	  TSyborg::DisableInt(anId);
	  return KErrNone;
	}
	return KErrArgument;
}

EXPORT_C TInt Interrupt::Clear(TInt anId)
{
	__KTRACE_OPT(KHARDWARE,Kern::Printf("Interrupt::Clear(%d)",anId));
	if((anId >= 0) && ((TUint)anId < KNumSyborgInts))
	{
        return KErrNone;
	}
	return KErrArgument;
}

EXPORT_C TInt Interrupt::SetPriority(TInt anId, TInt aPriority)
{
	__KTRACE_OPT(KHARDWARE,Kern::Printf("Interrupt::SetPriority(anId=%d aPriority=0x%X)",anId,aPriority));
	return KErrNotSupported;
}
#endif	// !__SMP__
