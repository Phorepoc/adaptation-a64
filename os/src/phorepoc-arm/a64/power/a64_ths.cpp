// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// a64_ths.cpp -- OURS (power plan P1, docs/phorepoc/power-plan.md): the A64 thermal sensor (THS) driver.
// Facts: docs/phorepoc/research/power-linux-modern.md section 4 (registers/values/formula written up from mainline
// sun8i_thermal.c as facts only). [V] = read in that research, [U] = unverified: check on the phone (PhoneLab).
//
// Sensors: 0 = CPU, 1 and 2 = GPU. T (milli-degC) = 260890 - 117 * raw [V]. raw 0 = no sample yet.
// Polled (no IRQ): A64ThsTick() is called from the 5 s monitor DFC (a64_glue.cpp A64BatteryTick), which prints
// "[EKERN][power] temp cpu=..C gpu=..C" every 30 s (every 5 s from 75 C) and returns ETrue after two consecutive
// readings at or above 110 C (the research's critical trip), upon which the glue runs the existing switch-off path
// (A64PowerOffNow). All register accesses are plain 32-bit MMIO; only A64ThsInit (boot, supervisor thread) maps.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);
extern "C" TInt A64ThsReadMilliC(TInt aSensor, TInt& aMilliC);

const TPhysAddr KCcuPhys = 0x01C20000, KThsPhys = 0x01C25000, KSidPhys = 0x01C14000;
enum
	{
	KCcuBusGate2	= 0x068,	// BUS_CLK_GATING_REG2: [8] THS [V per research]
	KCcuThsClk		= 0x074,	// THS_CLK: [31] enable, [25:24] source (0 = OSC24M), [1:0] divider (0:/1 1:/2 2:/4 3:/6) [V]
	KCcuBusRst3		= 0x2D0,	// BUS_SOFT_RST_REG3: [8] THS, 1 = released [V]
	// THS registers [V]
	KThsCtrl0		= 0x00,
	KThsCtrl2		= 0x40,
	KThsIntCtrl		= 0x44,
	KThsIntSta		= 0x48,		// write 1 to clear
	KThsMfc			= 0x70,		// filter control
	KThsCalib01		= 0x74,		// [11:0] sensor 0, [27:16] sensor 1
	KThsCalib2		= 0x78,		// [11:0] sensor 2
	KThsData0		= 0x80,		// sensor 0 (CPU); 0x84, 0x88 = sensors 1, 2 (GPU)
	// SID: the efuse words sit at SID+0x200 (sunxi-sid value_offset for the A64) [U]; the thermal calibration cell is
	// at cell offset 0x34 -> 0x234, 8 bytes = three 12-bit words (one per sensor) in the low bits of u16 words [U]
	KSidThsCalib	= 0x234
	};

const TInt KShutdownMilliC = 110000;		// critical trip, all zones [V]
const TInt KThrottleMilliC = 75000;			// CPU passive trip [V]; no throttling here yet (P3), only a faster print

static volatile TUint32* TheThs;
static TBool ThsReady;

static TInt MapHw(volatile TUint32*& aOut, TPhysAddr aPhys, TInt aSize)
	{
	DPlatChunkHw* c;
	NKern::ThreadEnterCS();
	TInt r = DPlatChunkHw::New(c, aPhys, aSize, EMapAttrSupRw | EMapAttrFullyBlocking);	// kept for good
	NKern::ThreadLeaveCS();
	if (r == KErrNone)
		aOut = (volatile TUint32*)c->LinearAddress();
	return r;
	}

// The init sequence: clock + reset in the CCU, calibration from the SID, then the sensor/filter/interrupt registers
// with the values from the research (we poll the data registers; INT_CTRL's data-valid enables are what makes the
// hardware keep sampling, no GIC line is hooked).
extern "C" TInt A64ThsInit()
	{
	volatile TUint32 *ccu = 0, *sid = 0;
	TInt r = MapHw(ccu, KCcuPhys, 0x1000);
	if (r == KErrNone) r = MapHw(sid, KSidPhys, 0x1000);
	if (r == KErrNone) r = MapHw(TheThs, KThsPhys, 0x1000);
	if (r != KErrNone)
		{
		ekseam_puts("[EKERN][power] THS map failed r="); ekseam_puthex((unsigned)r);
		return r;
		}
	ccu[KCcuBusRst3 / 4] &= ~(1u << 8);						// reset asserted while the clock is set up
	ccu[KCcuBusGate2 / 4] |= 1u << 8;						// bus clock gate: THS
	ccu[KCcuThsClk / 4] = (1u << 31) | 3u;					// enable, source OSC24M, /6 = 4 MHz [the /6 is U: the research gives the field, not the rate]
	ccu[KCcuBusRst3 / 4] |= 1u << 8;						// release the reset
	Kern::NanoWait(100000);
	// calibration: three 12-bit words; absent (0 or all-ones) = leave the 0x800 reset value [U on the cell layout]
	const volatile TUint32* cal = sid + KSidThsCalib / 4;
	const TUint32 c0 = cal[0], c1 = cal[1];
	const TUint32 w[3] = { c0 & 0xFFF, (c0 >> 16) & 0xFFF, c1 & 0xFFF };
	ekseam_puts("[EKERN][power] THS calibration words="); ekseam_puthex((w[0] << 24) ^ (w[1] << 12) ^ w[2]);
	if (w[0] && w[0] != 0xFFF) TheThs[KThsCalib01 / 4] = (TheThs[KThsCalib01 / 4] & ~0xFFFu) | w[0];
	if (w[1] && w[1] != 0xFFF) TheThs[KThsCalib01 / 4] = (TheThs[KThsCalib01 / 4] & ~(0xFFFu << 16)) | (w[1] << 16);
	if (w[2] && w[2] != 0xFFF) TheThs[KThsCalib2 / 4] = (TheThs[KThsCalib2 / 4] & ~0xFFFu) | w[2];
	TheThs[KThsMfc / 4] = 0x5;
	TheThs[KThsCtrl0 / 4] = 0x1DF;
	TheThs[KThsCtrl2 / 4] = 0x01DF0007;
	TheThs[KThsIntCtrl / 4] = 0x0016D700;
	TheThs[KThsIntSta / 4] = 0xFFFFFFFF;
	ThsReady = ETrue;
	Kern::NanoWait(20000000);								// ~20 ms: a first sample (a longer wait would delay boot)
	TInt t0 = 0;
	const TInt rr = A64ThsReadMilliC(0, t0);
	ekseam_puts("[EKERN][power] THS up, cpu read r="); ekseam_puthex((unsigned)rr);
	ekseam_puts("[EKERN][power] THS cpu milliC="); ekseam_puthex((unsigned)t0);
	return KErrNone;
	}

// aSensor 0 = CPU, 1 and 2 = GPU. KErrNotReady until the first sample of that sensor.
extern "C" TInt A64ThsReadMilliC(TInt aSensor, TInt& aMilliC)
	{
	if (!ThsReady)
		return KErrNotReady;
	if (aSensor < 0 || aSensor > 2)
		return KErrArgument;
	const TUint32 raw = TheThs[KThsData0 / 4 + aSensor] & 0xFFF;
	if (!raw)
		return KErrNotReady;
	aMilliC = 260890 - 117 * (TInt)raw;
	return KErrNone;
	}

static void PutDec(TInt aV)		// small signed decimal (the kernel's ekseam has only hex)
	{
	char b[14];
	TInt n = 0;
	TUint u = aV < 0 ? (TUint)-aV : (TUint)aV;
	if (aV < 0)
		b[n++] = '-';
	char d[10];
	TInt k = 0;
	do { d[k++] = (char)('0' + u % 10); u /= 10; } while (u);
	while (k)
		b[n++] = d[--k];
	b[n] = 0;
	ekseam_puts(b);
	}

// Every 5 s from the monitor DFC. Returns ETrue = critical (two consecutive readings >= 110 C): the caller powers off.
extern "C" TBool A64ThsTick()
	{
	static TInt n, hot;
	if (!ThsReady)
		return EFalse;
	TInt t[3] = { 0, 0, 0 };
	TBool ok[3];
	TInt maxT = -1000000;
	for (TInt i = 0; i < 3; ++i)
		{
		ok[i] = A64ThsReadMilliC(i, t[i]) == KErrNone;
		if (ok[i] && t[i] > maxT)
			maxT = t[i];
		}
	if ((++n % 6) == 1 || maxT >= KThrottleMilliC)
		{
		ekseam_puts("[EKERN][power] temp cpu=");
		if (ok[0]) { PutDec(t[0] / 1000); ekseam_puts("C"); } else ekseam_puts("?");
		ekseam_puts(" gpu=");
		if (ok[1]) { PutDec(t[1] / 1000); ekseam_puts("C"); } else ekseam_puts("?");
		if (ok[2]) { ekseam_puts("/"); PutDec(t[2] / 1000); ekseam_puts("C"); }
		ekseam_puts("\n");
		}
	hot = maxT >= KShutdownMilliC ? hot + 1 : 0;
	return hot >= 2;
	}
