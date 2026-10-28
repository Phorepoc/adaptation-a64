// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// OURS (track): the touch panel (a Goodix GT9xx controller) ->
// Symbian pointer events. Reference: the NuttX (Apache-2.0) gt9xx.c driver and Linux i2c-mv64xxx.c (the Allwinner TWI
// is a Marvell mv64xxx core).
//   - bus: the TWI, its pins and the I2C address are the board's (a64_board_config.h A64_TOUCH_*); power = the
//     supply the panel bring-up already enables (a64_display.cpp).
//   - protocol: 16-bit big-endian register addresses; 0x8140 = product ID ("917S"), 0x814E = status (bit 7 = new
//     data, bits 3:0 = touch points), 0x8150 = point 1 (x lo/hi, y lo/hi, in panel pixels 720x1440); writing 0 to
//     0x814E hands the buffer back to the controller.
//   - first rung: POLLED (an NTimer every 15 ms -> a DFC on our own queue does the I2C), no interrupt line yet;
//     the TWI is driven polled too (INT_FLAG busy-waits, ~1 ms per poll at 100 kHz).
// The UI (360x640) is scaled x2 and centred on the panel (a64_display.cpp ProgramSource): screen = (x/2, (y-80)/2).
#include <kernel/kern_priv.h>
#include <e32keys.h>
#include <assp.h>
#include <syborg_priv.h>
#include "a64_board_config.h"	// the board's panel size and touch wiring
#ifndef A64_TOUCH_I2C_ADDR
#error "the board must define the touch wiring in a64_board_config.h (A64_TOUCH_*)"
#endif
extern "C" void A64CpuInputBoost();	// power/a64_cpufreq.cpp: input -> the top allowed CPU clock for 1 s

extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(unsigned);

#ifndef VIRT_FB_WIDTH
#define VIRT_FB_WIDTH 360
#endif
#ifndef VIRT_FB_HEIGHT
#define VIRT_FB_HEIGHT 640
#endif
const TInt KPanelW = A64_PANEL_W, KPanelH = A64_PANEL_H, KScale = 2;
const TInt KOffY = (KPanelH - VIRT_FB_HEIGHT * KScale) / 2, KOffX = (KPanelW - VIRT_FB_WIDTH * KScale) / 2;

const TUint32 KCcuPioPhys = 0x01C20000, KTwi0Phys = A64_TOUCH_TWI_PHYS;
const TUint KBusGate3 = 0x6C, KBusRst4 = 0x2D8;	// CCU offsets
// TWI registers + CNTR bits (Allwinner: writing 1 to INT_FLAG clears it)
const TUint KTwiData = 0x08, KTwiCntr = 0x0C, KTwiStat = 0x10, KTwiCcr = 0x14, KTwiSrst = 0x18, KTwiEfr = 0x1C;
const TUint32 KBusEn = 0x40, KMSta = 0x20, KMStp = 0x10, KIntFlag = 0x08, KAAck = 0x04;
const TUint8 KAddr = A64_TOUCH_I2C_ADDR;
// PIO port n at 0x800 + n*0x24 in the CCU/PIO page; 4 bits per pin, CFG0..3 at +0, +4, +8, +0xC
static void SetPinFunc(volatile TUint32* aCcuPio, TUint aPort, TUint aPin)
	{
	volatile TUint32& cfg = aCcuPio[(0x800 + aPort * 0x24 + (aPin / 8) * 4) / 4];
	const TUint sh = (aPin % 8) * 4;
	cfg = (cfg & ~(0xFu << sh)) | ((TUint32)A64_TOUCH_PIN_FUNC << sh);
	}
extern "C" volatile TInt A64TouchSupplyState;		// a64_display.cpp: 1 = our supply is off, 2 = back on

class DA64Touch
	{
public:
	DA64Touch();
	TInt Init();
private:
	static void TimerFn(TAny*);
	static void PollDfc(TAny*);
	void Poll();
	TBool Wait();
	TBool Start(TUint32 aExpect1, TUint32 aExpect2);
	TBool Send(TUint8 aByte, TUint32 aExpect);
	void Stop();
	TInt Read(TUint16 aReg, TUint8* aBuf, TInt aLen);
	TInt Write(TUint16 aReg, TUint8 aVal);
	void Event(TRawEvent::TType aType);
private:
	volatile TUint32* iTwi;
	DPlatChunkHw* iTwiChunk;
	DPlatChunkHw* iCcuChunk;
	NTimer iTimer;
	TDfc iDfc;
	TDfcQue* iQue;
	TBool iDown;
	TInt iX, iY;
	TInt iKey;			// the soft key (scan code) held by a touch in the bottom bar, 0 = none
	TInt iErrors;
	TBool iRecoveredLogged;
	TBool iOvl;			// the current touch belongs to the kernel overlay (a64_bars.cpp, golden image)
	TInt iOx, iOy;		// its last panel position
	};

#ifdef A64_GOLDEN
// a64_bars.cpp: the golden menu / keyboard. The Menu soft key opens the menu instead of being EStdKeyApplication0.
extern "C" void A64BarsMenuKey();
extern "C" TBool A64BarsTouch(TInt aX, TInt aY, TInt aAction);
const TInt KMenuSoftKey = -1;	// iKey while the Menu soft key is held (no key event was sent)
#endif

static DA64Touch* TheTouch;
// the poll period (ms): 15 while the screen is on, slower while it is off (display/a64_display.cpp, power rung P4) --
// the touch only has to wake the phone then
static volatile TInt PollMs = 15;
extern "C" void A64TouchSetPollMs(TInt aMs) { PollMs = aMs; }

DA64Touch::DA64Touch()
	: iTwi(0), iTwiChunk(0), iCcuChunk(0), iTimer(TimerFn, this), iDfc(PollDfc, this, 1), iQue(0), iDown(EFalse),
	  iX(0), iY(0), iKey(0), iErrors(0), iRecoveredLogged(EFalse), iOvl(EFalse), iOx(0), iOy(0)
	{}

TBool DA64Touch::Wait()
	{
	for (TInt i = 0; i < 200000; ++i)
		if (iTwi[KTwiCntr / 4] & KIntFlag)
			return ETrue;
	return EFalse;
	}

TBool DA64Touch::Start(TUint32 aExpect1, TUint32 aExpect2)
	{
	iTwi[KTwiCntr / 4] = KBusEn | KMSta | (iTwi[KTwiCntr / 4] & KIntFlag);	// (repeated) start; clears a pending flag
	if (!Wait())
		return EFalse;
	const TUint32 s = iTwi[KTwiStat / 4];
	return s == aExpect1 || s == aExpect2;
	}

TBool DA64Touch::Send(TUint8 aByte, TUint32 aExpect)
	{
	iTwi[KTwiData / 4] = aByte;
	iTwi[KTwiCntr / 4] = KBusEn | KIntFlag;
	return Wait() && iTwi[KTwiStat / 4] == aExpect;
	}

void DA64Touch::Stop()
	{
	iTwi[KTwiCntr / 4] = KBusEn | KMStp | KIntFlag;
	for (TInt i = 0; i < 200000 && (iTwi[KTwiCntr / 4] & KMStp); ++i) {}
	}

// START, addr+W, reg hi, reg lo, RESTART, addr+R, aLen bytes (ACK all but the last), STOP
TInt DA64Touch::Read(TUint16 aReg, TUint8* aBuf, TInt aLen)
	{
	TInt r = KErrGeneral;
	if (Start(0x08, 0x08) && Send(KAddr << 1, 0x18) && Send(aReg >> 8, 0x28) && Send(aReg & 0xFF, 0x28)
		&& Start(0x10, 0x08) && Send((KAddr << 1) | 1, 0x40))
		{
		r = KErrNone;
		for (TInt i = 0; i < aLen; ++i)
			{
			const TBool last = i == aLen - 1;
			iTwi[KTwiCntr / 4] = KBusEn | KIntFlag | (last ? 0 : KAAck);
			if (!Wait() || iTwi[KTwiStat / 4] != (last ? 0x58u : 0x50u))
				{ r = KErrGeneral; break; }
			aBuf[i] = (TUint8)iTwi[KTwiData / 4];
			}
		}
	Stop();
	return r;
	}

TInt DA64Touch::Write(TUint16 aReg, TUint8 aVal)
	{
	TInt r = KErrGeneral;
	if (Start(0x08, 0x08) && Send(KAddr << 1, 0x18) && Send(aReg >> 8, 0x28) && Send(aReg & 0xFF, 0x28)
		&& Send(aVal, 0x28))
		r = KErrNone;
	Stop();
	return r;
	}

TInt DA64Touch::Init()
	{
	NKern::ThreadEnterCS();
	TInt r = DPlatChunkHw::New(iCcuChunk, KCcuPioPhys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r == KErrNone)
		r = DPlatChunkHw::New(iTwiChunk, KTwi0Phys & ~0xFFFu, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r == KErrNone)
		r = Kern::DfcQCreate(iQue, 27, &KNullDesC);
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		return r;
	volatile TUint32* ccu = (volatile TUint32*)iCcuChunk->LinearAddress();
	iTwi = (volatile TUint32*)(iTwiChunk->LinearAddress() + (KTwi0Phys & 0xFFF));
	SetPinFunc(ccu, A64_TOUCH_SCK_PORT, A64_TOUCH_SCK_PIN);	// the board's TWI clock + data pins
	SetPinFunc(ccu, A64_TOUCH_SDA_PORT, A64_TOUCH_SDA_PIN);
	ccu[KBusRst4 / 4] |= 1u << A64_TOUCH_TWI_BUS_GATE_BIT;	// de-assert the TWI reset
	ccu[KBusGate3 / 4] |= 1u << A64_TOUCH_TWI_BUS_GATE_BIT;	// the TWI bus clock on
	iTwi[KTwiSrst / 4] = 1;									// soft reset the controller
	for (TInt i = 0; i < 100000 && (iTwi[KTwiSrst / 4] & 1); ++i) {}
	iTwi[KTwiCcr / 4] = (5 << 3) | 1;						// 24 MHz / (10 * 6 * 2^2) = 100 kHz
	iTwi[KTwiEfr / 4] = 0;
	iTwi[KTwiCntr / 4] = KBusEn;
	TUint8 id[4] = { 0, 0, 0, 0 };
	r = Read(0x8140, id, 4);
	ekseam_puts("[EKERN][touch] GT9xx product id read r="); ekseam_puthex((unsigned)r);
	ekseam_puts("[EKERN][touch]   id="); ekseam_puthex(id[0] | id[1] << 8 | id[2] << 16 | (TUint)id[3] << 24);
	iDfc.SetDfcQ(iQue);
	iTimer.OneShot(NKern::TimerTicks(15));
	return r;
	}

void DA64Touch::TimerFn(TAny* aPtr)
	{
	((DA64Touch*)aPtr)->iDfc.Add();
	}

void DA64Touch::PollDfc(TAny* aPtr)
	{
	DA64Touch& d = *(DA64Touch*)aPtr;
	d.Poll();
	d.iTimer.OneShot(NKern::TimerTicks(PollMs));
	}

void DA64Touch::Poll()
	{
	if (A64TouchSupplyState == 1)
		return;										// the display init has our supply off: stay off the bus
	if (A64TouchSupplyState == 2)
		A64TouchSupplyState = 0;					// back on (the chip restarts by itself, seen 2026-10-27)
	TUint8 s = 0;
	if (Read(0x814E, &s, 1) != KErrNone)
		{
		if (++iErrors <= 4 || (iErrors & 0x3FF) == 0)
			{ ekseam_puts("[EKERN][touch] status read failed, errors="); ekseam_puthex((unsigned)iErrors); }
		iTwi[KTwiSrst / 4] = 1;							// recover a stuck controller: the soft reset clears the
		for (TInt i = 0; i < 100000 && (iTwi[KTwiSrst / 4] & 1); ++i) {}	// clock setup too, so redo Init's
		iTwi[KTwiCcr / 4] = (5 << 3) | 1;
		iTwi[KTwiEfr / 4] = 0;
		iTwi[KTwiCntr / 4] = KBusEn;
		return;
		}
	if (iErrors && !iRecoveredLogged)
		{ iRecoveredLogged = ETrue; ekseam_puts("[EKERN][touch] reads recovered after errors="); ekseam_puthex((unsigned)iErrors); }
	if (!(s & 0x80))
		return;										// no new data
	if (s & 0x0F)
		{
		TUint8 p[4];
		if (Read(0x8150, p, 4) == KErrNone)
			{
			const TInt px = p[0] | p[1] << 8, py = p[2] | p[3] << 8;
			// SOFT KEYS (2026-10-07): a touch that STARTS in the panel's bottom bar (below the OS picture, where
			// a64_display.cpp draws green | Menu | red) is a real key press, held until release -- WSERV's own
			// repeat gives the long presses (Menu held = the task switcher, as on S^3 phones)
#ifdef A64_GOLDEN
			// the kernel overlay (menu / confirm / keyboard) takes a touch that starts on it, and the rest of it
			if (iOvl || (!iDown && !iKey && py < KOffY + VIRT_FB_HEIGHT * KScale && A64BarsTouch(px, py, 0)))
				{
				if (iOvl)
					A64BarsTouch(px, py, 1);
				iOvl = ETrue; iOx = px; iOy = py;
				Write(0x814E, 0);
				return;
				}
			if (!iDown && !iKey && py >= KOffY + VIRT_FB_HEIGHT * KScale && px >= KPanelW / 3 && px < 2 * KPanelW / 3)
				{
				iKey = KMenuSoftKey;
				A64BarsMenuKey();
				ekseam_puts("[EKERN][touch] golden menu key\n");
				}
#endif
			if (!iDown && (iKey || py >= KOffY + VIRT_FB_HEIGHT * KScale))
				{
				if (!iKey)
					{
					iKey = px < KPanelW / 3 ? EStdKeyYes : (px < 2 * KPanelW / 3 ? EStdKeyApplication0 : EStdKeyNo);
					TRawEvent ev;
					ev.Set(TRawEvent::EKeyDown, iKey);
					A64CpuInputBoost();
					Kern::AddEvent(ev);
					ekseam_puts("[EKERN][touch] soft key down "); ekseam_puthex((unsigned)iKey);
					}
				Write(0x814E, 0);
				return;
				}
			TInt x = (px - KOffX) / KScale, y = (py - KOffY) / KScale;
			x = x < 0 ? 0 : (x >= VIRT_FB_WIDTH ? VIRT_FB_WIDTH - 1 : x);
			y = y < 0 ? 0 : (y >= VIRT_FB_HEIGHT ? VIRT_FB_HEIGHT - 1 : y);
			const TBool moved = x != iX || y != iY;
			iX = x; iY = y;
			if (!iDown)
				{ iDown = ETrue; Event(TRawEvent::EButton1Down); }
			else if (moved)
				Event(TRawEvent::EPointerMove);
			}
		}
#ifdef A64_GOLDEN
	else if (iOvl)
		{
		A64BarsTouch(iOx, iOy, 2);
		iOvl = EFalse;
		}
	else if (iKey == KMenuSoftKey)
		iKey = 0;
#endif
	else if (iKey)
		{
		TRawEvent ev;
		ev.Set(TRawEvent::EKeyUp, iKey);
		A64CpuInputBoost();
		Kern::AddEvent(ev);
		ekseam_puts("[EKERN][touch] soft key up "); ekseam_puthex((unsigned)iKey);
		iKey = 0;
		}
	else if (iDown)
		{
		iDown = EFalse;
		Event(TRawEvent::EButton1Up);
		}
	Write(0x814E, 0);
	}

void DA64Touch::Event(TRawEvent::TType aType)
	{
	TRawEvent ev;
	// Z = 0 explicitly (2026-10-08): TRawEvent() zeroes only the type word and Set(type, x, y) fills x and y, so
	// iU.pos3D.iZ was stack garbage -- and on S^3 WSERV reads Z (positive = proximity / hovering, negative =
	// pressure, but only with HAL 3D-pointer thresholds, which this board does not set). NOT the cause of "lists ignore
	// PhoneLab taps" (that was S60 highlight mode: after a key press the first tap only leaves it) -- kept: deterministic.
	ev.Set(aType, iX, iY, 0);
	A64CpuInputBoost();
	Kern::AddEvent(ev);
	if (aType != TRawEvent::EPointerMove)
		{
		ekseam_puts(aType == TRawEvent::EButton1Down ? "[EKERN][touch] down x=" : "[EKERN][touch] up x=");
		ekseam_puthex((unsigned)iX); ekseam_puts("[EKERN][touch]   y="); ekseam_puthex((unsigned)iY);
		}
	}

// the mouse HAL group, as virtinput.cpp / the x86 board: answering EMouseHalMouseInfo makes UserHal::MachineInfo
// report a pointer device, without which WSERV and Avkon ignore pointer events
static TInt MouseHal(TAny*, TInt aFunction, TAny* a1, TAny*)
	{
	switch (aFunction)
		{
	case EMouseHalMouseInfo:
		{
		TPckgBuf<TMouseInfoV01> v;
		v().iMouseButtons = 1;
		v().iOffsetToDisplay.iX = 0;
		v().iOffsetToDisplay.iY = 0;
		v().iMouseAreaSize.iWidth = VIRT_FB_WIDTH;
		v().iMouseAreaSize.iHeight = VIRT_FB_HEIGHT;
		Kern::InfoCopy(*(TDes8*)a1, v);
		return KErrNone;
		}
	case EMouseHalMouseState:
		{
		TBool on = ETrue;
		kumemput32(a1, &on, sizeof(TBool));
		return KErrNone;
		}
	case EMouseHalSetMouseState:
		return KErrNone;
	default:
		return KErrNotSupported;
		}
	}

// S60's hardware-switch HAL group (EHalGroupPlatformSpecific1 = 29; SysApAppUi.cpp KHWSwitchGrip = 2): SysAp reads the
// keyboard slide at startup and publishes KHWRMGripStatus, which turns the FEP's hardware-QWERTY mode on or off. The
// phone is touch-only, a slide that is always CLOSED (1): an editor tap opens the virtual keyboard. Without this
// group SysAp publishes nothing and editors get neither a cursor nor a keyboard (boot 307). x86: X64SwitchHal, SLIDE=1.
static const TInt KA64HalSwitchGrip = 2;
static TInt A64SwitchHal(TAny* /*aPtr*/, TInt aFunction, TAny* a1, TAny* /*a2*/)
	{
	if (aFunction != KA64HalSwitchGrip)
		return KErrNotSupported;
	const TInt closed = 1;
	kumemput32(a1, &closed, sizeof(TInt));
	return KErrNone;
	}

extern "C" void ekseam_load_input_drivers()
	{
	(void)Kern::AddHalEntry(EHalGroupPlatformSpecific1, A64SwitchHal, NULL);
	TInt r = Kern::AddHalEntry(EHalGroupMouse, MouseHal, NULL);
	ekseam_puts("[EKERN][touch] mouse HAL r="); ekseam_puthex((unsigned)r);
	TheTouch = new DA64Touch;
	r = TheTouch ? TheTouch->Init() : KErrNoMemory;
	ekseam_puts("[EKERN][touch] init r="); ekseam_puthex((unsigned)r);
	}
