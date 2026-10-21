// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// a64keys.cpp -- OURS (2026-10-09): the PinePhone's volume buttons. Both sit on the A64's LRADC (a 6-bit "low-rate
// ADC" key scanner at 0x01C21800, GIC SPI 30; Linux drivers/input/keyboard/sun4i-lradc-keys.c, the PinePhone DTS
// `lradc` node: Vol+ = 200 mV, Vol- = 400 mV, Vref = 3.0 V (ALDO3) * 3/4 = 2.25 V, 63 = full scale; the research in
// 3.4). Key down/up interrupts; the key is read once per press from DATA0 and
// posted as EStdKeyIncVolume / EStdKeyDecVolume raw events (as the x86 board does for its volume keys).
#include <kernel/kern_priv.h>
#include <assp.h>
#include <e32keys.h>

extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(unsigned);

static const TPhysAddr KPhysLradcPage = 0x01C21000;
static const TUint KLradc = 0x800;
static const TUint KCtrl = 0x00, KIntc = 0x04, KInts = 0x08, KData0 = 0x0C;
static const TInt KIntLradc = 32 + 30;
static const TUint32 KIntKeyDown = 1u << 1, KIntKeyUp = 1u << 4;
static const TInt KVolPlusMaxCode = 8;						// 200 mV = 5.6, 400 mV = 11.2 (63 = 2.25 V)

class DA64Keys
	{
public:
	TInt Init();
	static void Isr(TAny* aPtr);
	static void DfcFn(TAny* aPtr);
	inline volatile TUint32& R(TUint aOff) { return *(volatile TUint32*)(iBase + KLradc + aOff); }
public:
	TLinAddr iBase;
	TUint32 iPending;											// INTS bits for the DFC
	TUint32 iData;
	TInt iHeld;													// the key down, or 0
	TDfc iDfc;
	DA64Keys() : iDfc(DfcFn, this, Kern::DfcQue0(), 1) {}
	};

static DA64Keys* TheKeys;

TInt DA64Keys::Init()
	{
	DPlatChunkHw* ch = NULL;
	TInt r = DPlatChunkHw::New(ch, KPhysLradcPage, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return r;
	iBase = ch->LinearAddress();
	// FIRST_CONVERT_DLY 2 << 24 | LEVELA_B_CNT 1 << 8 | HOLD_EN (bit 6) | SAMPLE_RATE 250 Hz (0) | ENABLE
	R(KCtrl) = 0x02000141u;
	R(KInts) = 0xFFFFFFFFu;
	R(KIntc) = KIntKeyDown | KIntKeyUp;
	r = Interrupt::Bind(KIntLradc, Isr, this);
	if (r >= 0)
		r = Interrupt::Enable(KIntLradc);
	return r < 0 ? r : KErrNone;
	}

void DA64Keys::Isr(TAny* aPtr)
	{
	DA64Keys& k = *(DA64Keys*)aPtr;
	const TUint32 ints = k.R(KInts);
	if (ints & KIntKeyDown)
		k.iData = k.R(KData0) & 0x3F;
	k.R(KInts) = ints;											// write-1-to-clear
	k.iPending |= ints;
	if (ints & (KIntKeyDown | KIntKeyUp))
		k.iDfc.Add();
	}

void DA64Keys::DfcFn(TAny* aPtr)
	{
	DA64Keys& k = *(DA64Keys*)aPtr;
	const TUint32 ints = (TUint32)__e32_atomic_swp_ord32(&k.iPending, 0);
	TRawEvent ev;
	if ((ints & KIntKeyDown) && !k.iHeld)
		{
		k.iHeld = (TInt)k.iData <= KVolPlusMaxCode ? EStdKeyIncVolume : EStdKeyDecVolume;
		ev.Set(TRawEvent::EKeyDown, k.iHeld);
		Kern::AddEvent(ev);
		ekseam_puts("[EKERN][keys] volume key down, adc="); ekseam_puthex(k.iData);
		}
	if ((ints & KIntKeyUp) && k.iHeld)
		{
		ev.Set(TRawEvent::EKeyUp, k.iHeld);
		Kern::AddEvent(ev);
		k.iHeld = 0;
		}
	}

extern "C" void A64KeysInstall()
	{
	NKern::ThreadEnterCS();
	TheKeys = new DA64Keys;
	const TInt r = TheKeys ? TheKeys->Init() : KErrNoMemory;
	NKern::ThreadLeaveCS();
	ekseam_puts("[EKERN][keys] LRADC volume keys r="); ekseam_puthex((unsigned)r);
	}
