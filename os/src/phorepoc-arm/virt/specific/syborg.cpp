// OURS (track): the QEMU `virt` variant, derived from
// oss.FCL.sf.adapt.Qemu/baseport/syborg/specific/syborg.cpp. Class names stay Syborg/TSyborg for now (the ROM
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

#include <syborg_priv.h>

//#define DEBUG

#ifdef DEBUG
#define __DEBUG_PRINT(format...)    Kern::Printf(format)
#else
#define __DEBUG_PRINT(format...)    __KTRACE_OPT(KBOOT,Kern::Printf(format))
#endif

void TSyborg::Init3()
{
//  SetTimerMode(KHwBaseCounterTimer, ETimerModePeriodic);
//  EnableTimer(KHwBaseCounterTimer, EEnable);
}

// VIRT: no Syborg counter timer -- the tick is the ARM generic timer (interrupts.cia)

// Return base address of debug UART
// (as selected in obey file or with eshell debugport command)
EXPORT_C TUint32 TSyborg::DebugPortAddr()
{
	// Defaults to UART 0
    switch (Kern::SuperPage().iDebugPort)
    {
        case KNullDebugPort:        // debug supressed
            return (TUint32)KNullDebugPort;
        case 1:
            return KHwBaseUart1;
        case 2:
            return KHwBaseUart2;
        case 3:
            return KHwBaseUart3;
        case 0:
        default:
            return KHwBaseUart0;
    }
}

EXPORT_C void TSyborg::MarkDebugPortOff()
{
	TheVariant.iDebugPortBase = 0;
}

EXPORT_C TInt TSyborg::VideoRamSize()
{

  return 4*854*854; // Now allow for 854 x 854 display, instead of 480 x 640

}

// !@!
EXPORT_C TPhysAddr TSyborg::VideoRamPhys()
{
#if 0
  __KTRACE_OPT(KEXTENSION,Kern::Printf("TSyborg::VideoRamPhys: VideoRamPhys=0x%x", Syborg::VideoRamPhys));
#endif
  return 0;

}

EXPORT_C TPhysAddr TSyborg::VideoRamPhysSecure()
{
#if 0
  return Syborg::VideoRamPhysSecure;
#endif
  return 0;
}

