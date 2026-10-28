// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// OURS (track P4): the phone's framebuffer driver = Syborg's
// REAL framebuffer driver (oss.FCL.sf.adapt.Qemu/baseport/syborg/svpframebuffer/svpframebuffer.cpp, compiled
// unmodified below) on the A64 display pipeline that a64_display.cpp brings up (DE2 -> TCON0 -> MIPI DSI -> ST7703).
// Same shape as the QEMU `virt` port's virtfb.cpp: the driver only knows the Syborg framebuffer device's registers,
// so its ReadReg/WriteReg are redirected to a model of that device. FB_WIDTH/FB_HEIGHT = the nHD screen the ROM is
// built for (360x640 unless VIRT_FB says otherwise; the DE scales it x2 onto the 720x1440 panel), FB_BASE hands the
// driver's video RAM (XRGB8888, EColor16MU) to the DE as the scanned source, FB_ENABLED starts the panel bring-up.

#include <videodriver.h>
#include "platform.h"
#include <nkern.h>
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <kernel/kpower.h>
#include <syborg_priv.h>
#include "svpframebuffer.h"

#ifndef VIRT_FB_WIDTH
#define VIRT_FB_WIDTH 360
#endif
#ifndef VIRT_FB_HEIGHT
#define VIRT_FB_HEIGHT 640
#endif

extern "C" void A64DisplayStart();
extern "C" void A64DisplaySetSource(TUint32 aPhys, TInt aW, TInt aH);
extern "C" void A64DisplaySetBrightness(TInt aPercent);
extern "C" TInt A64DisplayBrightness();
extern "C" void A64DisplayBlank(TBool aBlank);
extern "C" void A64DisplayBacklightOff(TBool aOff);
extern "C" TBool A64DisplayBacklightIsOff();
extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);
// the NGA direct-screen-access chunk (tools/kernel/ekern_dsa_glue.cpp); weak so A64_DISPLAY=legacy / NGA_KERNEL=0 still link
extern "C" TPhysAddr ekseam_dsa_backing_phys;
extern "C" TInt ekseam_display_memory_handle(TInt aScreen, TAny* a1, TUint aBytes) __attribute__((weak));

const TUint KHwBaseClcd = 0;			// the driver's iPortAddr: nothing behind it, the model below is the device

static TUint32 A64FbBase;
static TBool A64FbEnabled;
static TBool A64FbStarted;

static TUint32 A64FbReadReg(TUint32 /*aBase*/, TUint32 aReg)
	{
	switch (aReg)
		{
		case DLcdPowerHandler::FB_WIDTH:	return VIRT_FB_WIDTH;
		case DLcdPowerHandler::FB_HEIGHT:	return VIRT_FB_HEIGHT;
		case DLcdPowerHandler::FB_BASE:		return A64FbBase;
		case DLcdPowerHandler::FB_ENABLED:	return A64FbEnabled;
		default:							return 0;
		}
	}

static void A64FbWriteReg(TUint32 /*aBase*/, TUint32 aReg, TUint32 aValue)
	{
	switch (aReg)
		{
		case DLcdPowerHandler::FB_BASE:
			A64FbBase = aValue;
			A64DisplaySetSource(A64FbBase, VIRT_FB_WIDTH, VIRT_FB_HEIGHT);
			break;
		case DLcdPowerHandler::FB_ENABLED:
			A64FbEnabled = aValue != 0;
			if (A64FbEnabled && !A64FbStarted)
				{
				A64FbStarted = ETrue;
				A64DisplayStart();		// the panel bring-up, on its own thread a few seconds from now
				}
			break;
		case DLcdPowerHandler::FB_BLANK:		// PowerDownLcd / PowerUpLcd (EDisplayHalSetState, power handler)
			A64DisplayBlank(aValue != 0);
			break;
		default:	// pixel order, interrupts: not modelled (yet)
			break;
		}
	}

// Brightness: the Syborg driver has no backlight, so its HAL handler answers KErrNotSupported. Register ours in
// front of it (the macro below swaps the function its Kern::AddHalEntry names) and pass everything else through.
static TInt DoHalFunction(TAny* aPtr, TInt aFunction, TAny* a1, TAny* a2);
static TInt A64FbHal(TAny* aPtr, TInt aFunction, TAny* a1, TAny* a2)
	{
	TInt v;
	switch (aFunction)
		{
		case EDisplayHalSetDisplayBrightness:
			A64DisplaySetBrightness((TInt)a1);
			return KErrNone;
		case EDisplayHalDisplayBrightness:
			v = A64DisplayBrightness();
			kumemput32(a1, &v, sizeof(v));
			return KErrNone;
		case EDisplayHalMaxDisplayBrightness:
			v = 100;
			kumemput32(a1, &v, sizeof(v));
			return KErrNone;
		case EDisplayHalSetBacklightOn:		// HAL EBacklightState (the screensaver's display off, SysAp delta)
			A64DisplayBacklightOff(!(TBool)a1);
			return KErrNone;
		case EDisplayHalBacklightOn:
			v = !A64DisplayBacklightIsOff();
			kumemput32(a1, &v, sizeof(v));
			return KErrNone;
		case EDisplayHalGetDisplayMemoryHandle:		// DSA / the screen-surface stream
			if (!ekseam_display_memory_handle)
				return KErrNotSupported;
			ekseam_dsa_backing_phys = TSyborg::VideoRamPhys();		// the DSA surface = the legacy video RAM (ekern_dsa_glue.cpp)
			return ekseam_display_memory_handle(0, a1, ((VIRT_FB_WIDTH * 4 + 63) & ~63) * VIRT_FB_HEIGHT);
		case EDisplayHalSetState:
			ekseam_puts((TBool)a1 ? "[EKERN][disp] HAL display state ON\n" : "[EKERN][disp] HAL display state OFF\n");
			return DoHalFunction(aPtr, aFunction, a1, a2);
		// ONE display mode, index 0, as on a real phone (2026-10-15, DSA under NGA): Syborg lists 3 identical 32 bpp
		// modes with 2 current, and the two halves of a DSA then name the screen surface differently -- scdv (WSERV's
		// DSA device) takes the FIRST 32 bpp mode (0), CDirectScreenBitmap the CURRENT one (2), so the app's updates
		// hit an unregistered surface id (KErrSurfaceNotRegistered, boot 493). The driver starts in mode 0 (below).
		case EDisplayHalModeCount:
			v = 1;
			kumemput32(a1, &v, sizeof(v));
			return KErrNone;
		case EDisplayHalSetMode:
			return (TInt)a1 == 0 ? KErrNone : KErrArgument;
		default:
			return DoHalFunction(aPtr, aFunction, a1, a2);
		}
	}
#define AddHalEntry(aGroup, aFunc, aPtr) AddHalEntry(aGroup, A64FbHal, aPtr)

#define ReadReg A64FbReadReg
#define WriteReg A64FbWriteReg
#ifdef __SMP__
// The Syborg driver uses __SPIN_LOCK_IRQSAVE(callbackLock) without ever declaring it (the UP macros drop the
// argument, so it never mattered); under __SMP__ it is a real TSpinLock.
static TSpinLock callbackLock(TSpinLock::EOrderGenericIrqLow3);
#endif
static const TInt A64FbInitialMode = 0;		// the driver's initial mode (svpframebuffer.h says 2; see EDisplayHalModeCount)
#define KConfigLcdDisplayMode A64FbInitialMode
#include "svpframebuffer.cpp"
#undef KConfigLcdDisplayMode

// THE MINIMAL SAFE DISPLAY DRIVER (2026-10-09, user decision): this file alone = the golden image's display -- a
// legacy framebuffer (the window server draws into video RAM, the DE scans it) with no vblank, no flips, no
// composition. Keep it working: it is the fallback when the NGA layer (a64dispchan.cpp, RDisplayChannel + vblank
// flips) misbehaves, and the pattern to copy for a first picture on a new board (a register model for Syborg's real
// driver + a scan-out of its video RAM). link_ekern.sh A64_DISPLAY=legacy builds only this.
extern "C" TUint32 A64FbLegacyBase() { return A64FbBase; }

