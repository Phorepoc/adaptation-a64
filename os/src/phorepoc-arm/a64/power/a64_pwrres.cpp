// a64_pwrres.cpp -- OURS (power plan P4, docs/phorepoc/power-plan.md): the A64 power RESOURCE MANAGER, the small
// kernel-side model of the 2010 drivers/resourceman idea (named, reference-counted resources; the hardware switches
// only on the 0 <-> 1 transitions) over the AXP803 rails and the A64 module clocks that can be shut while the screen is
// off. Every consumer takes a reference (A64PwrResGet) before it touches the hardware behind a resource and drops it
// (A64PwrResPut) when it is done; the screen driver (display/a64_display.cpp) drops its own on screen-off and takes
// it back on screen-on, the sensor driver (sensors/a64twi.cpp) takes DLDO1 for good.
//
// Rails (AXP803, Linux axp20x/regulator tables, research/power-linux-modern.md 1.3): output enables in PWR_OUT_CTRL2
// (reg 0x12): b3 DLDO1 (3.3 V: the DSI link supply, and the IMU + magnetometer), b4 DLDO2 (1.8 V: MIPI IO). LDO_IO0 /
// GPIO0-LDO (3.3 V: the touch controller, the STK3311, the panel VCC) is listed so the status line shows who holds it,
// but the screen never releases it -- the touch panel is the wake-up source (a tap wakes the phone) and the proximity
// sensor needs it during a call.
//
// Clocks (A64 user manual CCU, mainline clk-sun50i-a64 / sun8i-gpu): PLL_GPU (0x038) + GPU_CLK (0x1A0) and PLL_VE
// (0x018) + VE_CLK (0x13C). Only the MODULE clocks go; the bus gates and resets stay released so the registers stay
// readable. They are only used when built with -DA64_SCREENOFF_GATE_ACCEL (A64DEFS): a gated Mali core stalls a job
// that is in flight, so this is the part to enable second, after the display half has passed on the phone.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include "a64_pwrres.h"

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);
extern "C" TInt A64PmicUpdateReg(TUint8 aReg, TUint8 aClr, TUint8 aSet);	// display/a64_display.cpp (RSB, locked)

namespace
{
const TPhysAddr KCcuPhys = 0x01C20000;
enum
	{
	KCcuPllVe	= 0x018,	// [31] enable, [28] lock, [24] integer mode, [14:8] N-1, [3:0] M-1
	KCcuPllGpu	= 0x038,
	KCcuVeClk	= 0x13C,	// [31] gating
	KCcuGpuClk	= 0x1A0		// [31] gating
	};
// the factors the drivers' own power-on code uses (gpu/mali_a64.cpp: 24 MHz * 18 = 432 MHz; vde/phevde_a64.cpp: 297 MHz)
const TUint32 KPllGpu432 = (17u << 8) | 0u;
const TUint32 KPllVe297 = 0x03006207;
const TUint32 KPllEnable = 1u << 31, KPllLock = 1u << 28;

enum TKind { ERail, ERailHeld, EClkGpu, EClkVe };
struct TRes
	{
	const char* iName;
	TKind iKind;
	TUint8 iReg, iBit;		// ERail: the PMIC output-enable register + bit
	TInt iRefs;
	};
// everything is ON at boot with its owners already counted: the screen driver holds the rails and the accelerator
// clocks (the compositor needs both while the screen scans), LDO_IO0 also has the touch driver
TRes Res[KA64ResCount] =
	{
	{ "dldo1",  ERail,     0x12, 3, 1 },
	{ "dldo2",  ERail,     0x12, 4, 1 },
	{ "ldo_io0", ERailHeld, 0,   0, 2 },
	{ "gpu_clk", EClkGpu,  0,    0, 1 },
	{ "ve_clk",  EClkVe,   0,    0, 1 }
	};

NFastMutex Lock;
volatile TUint32* Ccu;		// a private mapping, made on first use of a clock resource (thread context)

TBool MapCcu()
	{
	if (Ccu)
		return ETrue;
	DPlatChunkHw* c;
	NKern::ThreadEnterCS();
	TInt r = DPlatChunkHw::New(c, KCcuPhys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);	// kept for good
	NKern::ThreadLeaveCS();
	if (r == KErrNone)
		Ccu = (volatile TUint32*)c->LinearAddress();
	return Ccu != 0;
	}

TInt PllUp(TUint aReg, TUint32 aFactors)
	{
	if (Ccu[aReg / 4] & KPllEnable)
		return KErrNone;
	Ccu[aReg / 4] = aFactors;				// (off while the factors change)
	Kern::NanoWait(10000);
	Ccu[aReg / 4] = aFactors | KPllEnable;
	TInt i;
	for (i = 0; i < 1000 && !(Ccu[aReg / 4] & KPllLock); ++i)
		Kern::NanoWait(10000);
	return i < 1000 ? KErrNone : KErrTimedOut;
	}

void Switch(TInt aId, TBool aOn)
	{
	const TRes& r = Res[aId];
	TInt rc = KErrNone;
	switch (r.iKind)
		{
	case ERail:
		rc = A64PmicUpdateReg(r.iReg, aOn ? 0 : (TUint8)(1u << r.iBit), aOn ? (TUint8)(1u << r.iBit) : 0);
		if (aOn)
			Kern::NanoWait(2000000);		// the LDO's ramp (the panel / IMU need their supply stable before the first access)
		break;
	case ERailHeld:
		break;								// never switched here
	case EClkGpu:
	case EClkVe:
		{
		if (!MapCcu())
			{ rc = KErrNoMemory; break; }
		const TUint clk = r.iKind == EClkGpu ? KCcuGpuClk : KCcuVeClk;
		const TUint pll = r.iKind == EClkGpu ? KCcuPllGpu : KCcuPllVe;
		const TUint32 fac = r.iKind == EClkGpu ? KPllGpu432 : KPllVe297;
		if (aOn)
			{
			rc = PllUp(pll, fac);
			Ccu[clk / 4] = 1u << 31;
			}
		else
			{
			Ccu[clk / 4] = 0;				// the module clock gate first, then the PLL behind it
			Ccu[pll / 4] &= ~KPllEnable;
			}
		}
		break;
		}
	ekseam_puts("[EKERN][power] res "); ekseam_puts(r.iName); ekseam_puts(aOn ? " ON r=" : " OFF r="); ekseam_puthex((unsigned)rc);
	}
}

// Thread context only (the PMIC access waits on the RSB bus, the PLL waits for its lock).
extern "C" TInt A64PwrResGet(TInt aId)
	{
	if (aId < 0 || aId >= KA64ResCount)
		return KErrArgument;
	NKern::FMWait(&Lock);
	const TBool up = Res[aId].iRefs++ == 0;
	if (up)
		Switch(aId, ETrue);
	NKern::FMSignal(&Lock);
	return KErrNone;
	}

extern "C" void A64PwrResPut(TInt aId)
	{
	if (aId < 0 || aId >= KA64ResCount)
		return;
	NKern::FMWait(&Lock);
	if (Res[aId].iRefs > 0 && --Res[aId].iRefs == 0)
		Switch(aId, EFalse);
	NKern::FMSignal(&Lock);
	}

extern "C" TInt A64PwrResRefs(TInt aId)
	{
	return aId < 0 || aId >= KA64ResCount ? -1 : Res[aId].iRefs;
	}

// "[EKERN][power] res refs dldo1=1 dldo2=1 ..." for the log
extern "C" void A64PwrResDump()
	{
	ekseam_puts("[EKERN][power] res refs");
	for (TInt i = 0; i < KA64ResCount; ++i)
		{
		ekseam_puts(" "); ekseam_puts(Res[i].iName); ekseam_puts("="); ekseam_puthex((unsigned)Res[i].iRefs);
		}
	ekseam_puts("\n");
	}
