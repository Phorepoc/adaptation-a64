// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// a64_cpufreq.cpp -- OURS (power plan P2, docs/phorepoc/power-plan.md): the A64 CPU OPP driver (PLL_CPUX + DCDC2).
// Facts: docs/phorepoc/research/power-linux-modern.md sections 1.3 and 2 ([V] read in mainline, [U] unverified).
//
// All four cores share one PLL and one rail, so there is one cluster-wide OPP. NOTHING here runs at boot: U-Boot's
// setting stays until someone calls A64CpuSetOpp. Only A64CpuFreqInit (a64_glue.cpp, boot) maps the CCU and creates
// the mutex.
//
// Locking: A64CpuSetOpp takes a DMutex (Kern::MutexWait), so it must be called from a kernel thread (a DFC thread or
// a supervisor thread; NOT from an ISR, an IDFC, or with a fast mutex / the kernel lock held). It blocks on the
// mutex, uses busy NanoWait delays inside the PLL window, and takes the RSB lock inside the PMIC helpers. SMP: the
// switch affects every core (all run on OSC24M for the ~0.1-1 ms of the relock); that is intended.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);
extern "C" TInt A64PmicUpdateReg(TUint8 aReg, TUint8 aClr, TUint8 aSet);	// a64_display.cpp (RSB, locked)
extern "C" TInt A64PmicReadReg(TUint8 aReg);								// a64_display.cpp (added with this driver)

const TPhysAddr KCcuPhys = 0x01C20000;
enum
	{
	KCcuPllCpux		= 0x000,	// [31] enable, [28] lock, [17:16] P, [12:8] N-1, [5:4] K-1, [1:0] M-1 [V]
	KCcuCpuxAxi		= 0x050,	// [17:16] CPUX source (1 = OSC24M, 2/3 = PLL_CPUX), [1:0] AXI divider [V; AXI field U]
	KPmicDcdc2		= 0x21		// [6:0] selector [V]
	};

struct TOpp { TUint16 iMHz; TUint16 iMilliV; };
static const TOpp KOpp[] =		// sun50i-a64-cpu-opp.dtsi [V]
	{ {648, 1040}, {816, 1100}, {912, 1120}, {960, 1160}, {1008, 1200}, {1056, 1240}, {1104, 1260}, {1152, 1300} };
const TInt KOppCount = sizeof(KOpp) / sizeof(KOpp[0]);

static volatile TUint32* TheCcu;
static DMutex* TheLock;
static TBool TheQuiet;				// the governor switches often: it logs a summary instead (A64CpuGovernorTick)
static TInt TheOpp = -1;			// last index we set, -1 = never (U-Boot's frequency)

// DCDC2 selector: 0x00-0x46 = 500-1200 mV in 10 mV steps, 0x47-0x4B = 1220-1300 mV in 20 mV steps [V]
static TUint8 SelFor(TInt aMilliV)
	{
	return aMilliV <= 1200 ? (TUint8)((aMilliV - 500) / 10) : (TUint8)(0x47 + (aMilliV - 1220) / 20);
	}
static TInt MilliVFor(TUint aSel)
	{
	return aSel <= 0x46 ? 500 + 10 * (TInt)aSel : 1220 + 20 * ((TInt)aSel - 0x47);
	}

extern "C" TInt A64CpuFreqInit()
	{
	DPlatChunkHw* c;
	NKern::ThreadEnterCS();
	TInt r = DPlatChunkHw::New(c, KCcuPhys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r == KErrNone)
		{
		TheCcu = (volatile TUint32*)c->LinearAddress();
		_LIT(KName, "A64CpuFreqLock");
		r = Kern::MutexCreate(TheLock, KName, KMutexOrdGeneral0);
		}
	NKern::ThreadLeaveCS();
	return r;
	}

extern "C" TInt A64CpuOppCount() { return KOppCount; }

// Current core clock in MHz from the live registers (24 when running from OSC24M), 0 before A64CpuFreqInit.
extern "C" TInt A64CpuCurrentMHz()
	{
	if (!TheCcu)
		return 0;
	const TUint32 src = (TheCcu[KCcuCpuxAxi / 4] >> 16) & 3;
	if (src < 2)
		return src == 1 ? 24 : 0;							// 0 = OSC32K (not a state we expect)
	const TUint32 p = TheCcu[KCcuPllCpux / 4];
	return (TInt)(24 * (((p >> 8) & 0x1F) + 1) * (((p >> 4) & 3) + 1) / (((p & 3) + 1) << ((p >> 16) & 3)));
	}

static TInt SetVoltage(TInt aMilliV)
	{
	const TUint8 sel = SelFor(aMilliV);
	TInt r = A64PmicUpdateReg(KPmicDcdc2, 0x7F, sel);
	if (r != KErrNone)
		return r;
	const TInt rb = A64PmicReadReg(KPmicDcdc2);
	return (rb < 0 || (rb & 0x7F) != sel) ? KErrGeneral : KErrNone;
	}

// Switch to OPP aIndex (0 = 648 MHz ... 7 = 1152 MHz). Up: DCDC2 first; down: DCDC2 after the clock. Logs the change.
extern "C" TInt A64CpuSetOpp(TInt aIndex)
	{
	if (!TheCcu || !TheLock)
		return KErrNotReady;
	if (aIndex < 0 || aIndex >= KOppCount)
		return KErrArgument;
	const TOpp& o = KOpp[aIndex];
	NKern::ThreadEnterCS();
	Kern::MutexWait(*TheLock);
	TInt r = KErrNone;
	const TInt curMHz = A64CpuCurrentMHz();
	const TInt oldIdx = TheOpp;
	const TBool up = curMHz < o.iMHz;
	const TInt rb0 = A64PmicReadReg(KPmicDcdc2);
	const TInt curMv = rb0 < 0 ? 0 : MilliVFor(rb0 & 0x7F);
	if (up || (curMv && curMv < o.iMilliV))					// raise the rail first when going up (or below need)
		{
		r = SetVoltage(o.iMilliV);
		if (r == KErrNone)
			{
			const TInt dv = curMv && o.iMilliV > curMv ? o.iMilliV - curMv : 0;
			Kern::NanoWait((TUint32)(dv * 400 + 20000));	// ~2.5 mV/us ramp [U]: 400 ns per mV + 20 us margin
			}
		}
	if (r == KErrNone && curMHz != o.iMHz)
		{
		// factors with M = 0, P = 0: f = 24 * (N+1) * (K+1) [model V]; K+1 = 2 for even multipliers, else 1
		const TUint mult = o.iMHz / 24;						// 27, 34, 38, 40, 42, 44, 46, 48: all exact
		const TUint k = (mult & 1) ? 0 : 1;
		const TUint n = mult / (k + 1) - 1;
		const TUint32 oldPll = TheCcu[KCcuPllCpux / 4];
		const TUint32 axi = TheCcu[KCcuCpuxAxi / 4];
		TUint32 newAxi = axi;
		if (o.iMHz >= 816 && (axi & 3) < 1)
			newAxi = (axi & ~3u) | 1u;						// keep AXI <= CPUX/2 above ~800 MHz [U]
		// 1. CPUX to OSC24M (and the AXI divider while off the PLL)
		TheCcu[KCcuCpuxAxi / 4] = (newAxi & ~(3u << 16)) | (1u << 16);
		Kern::NanoWait(2000);
		// 2. gate the PLL, write N/K (M = P = 0), ungate
		TheCcu[KCcuPllCpux / 4] = oldPll & ~(1u << 31);
		TheCcu[KCcuPllCpux / 4] = (n << 8) | (k << 4);
		TheCcu[KCcuPllCpux / 4] = (n << 8) | (k << 4) | (1u << 31);
		// 3. wait for the lock (<= 10 ms)
		TInt i = 0;
		for (; i < 1000 && !(TheCcu[KCcuPllCpux / 4] & (1u << 28)); ++i)
			Kern::NanoWait(10000);
		if (i == 1000)
			{
			// failed: put the old factors back, wait again, and go back to the PLL
			r = KErrTimedOut;
			TheCcu[KCcuPllCpux / 4] = oldPll & ~(1u << 31);
			TheCcu[KCcuPllCpux / 4] = oldPll | (1u << 31);
			for (i = 0; i < 1000 && !(TheCcu[KCcuPllCpux / 4] & (1u << 28)); ++i)
				Kern::NanoWait(10000);
			}
		Kern::NanoWait(20000);
		// 4. CPUX back to the PLL (source 2)
		TheCcu[KCcuCpuxAxi / 4] = (newAxi & ~(3u << 16)) | (2u << 16);
		Kern::NanoWait(2000);
		}
	if (r == KErrNone && !up && curMv != o.iMilliV)			// going down: the rail after the clock
		r = SetVoltage(o.iMilliV);
	if (r == KErrNone)
		TheOpp = aIndex;
	const TInt mhz = A64CpuCurrentMHz();
	if (!TheQuiet || r != KErrNone)
		{
		const TInt rb = A64PmicReadReg(KPmicDcdc2);
		ekseam_puts("[EKERN][power] cpu opp "); ekseam_puthex((unsigned)oldIdx); ekseam_puts(" -> "); ekseam_puthex((unsigned)aIndex);
		ekseam_puts(" MHz(read back)="); ekseam_puthex((unsigned)mhz);
		ekseam_puts(" mV(read back)="); ekseam_puthex((unsigned)(rb < 0 ? 0 : MilliVFor(rb & 0x7F)));
		ekseam_puts(" r="); ekseam_puthex((unsigned)r);
		}
	Kern::MutexSignal(*TheLock);
	NKern::ThreadLeaveCS();
	return r;
	}

#ifdef A64_OPP_TEST
// Power plan P2 test (A64DEFS=-DA64_OPP_TEST): every OPP, a fixed busy loop timed by the 24 MHz generic counter ->
// MHz as the loop sees it, then 100 random switches; back to the boot frequency (U-Boot's) at the end.
static TUint32 Cntpct()
	{
	TUint32 lo, hi;
	asm volatile("isb; mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
	return lo;
	}
static TUint32 LoopTicks()
	{
	const TUint32 t0 = Cntpct();
	for (volatile TInt i = 0; i < 2000000; ++i)
		{
		}
	return Cntpct() - t0;
	}
extern "C" void A64CpuOppSelfTest()
	{
	const TInt bootMHz = A64CpuCurrentMHz();
	ekseam_puts("[EKERN][power] opp test: boot MHz="); ekseam_puthex(bootMHz);
	const TUint32 ref = LoopTicks();
	ekseam_puts("[EKERN][power] opp test: boot loop ticks="); ekseam_puthex(ref);
	for (TInt i = 0; i < KOppCount; ++i)
		{
		const TInt r = A64CpuSetOpp(i);
		const TUint32 t = LoopTicks();
		// apparent MHz = bootMHz * ref / t
		ekseam_puts("[EKERN][power] opp test: opp="); ekseam_puthex(i);
		ekseam_puts(" r="); ekseam_puthex(r);
		ekseam_puts(" MHz(reg)="); ekseam_puthex(A64CpuCurrentMHz());
		ekseam_puts(" MHz(loop)="); ekseam_puthex(t ? (TUint32)((TUint64)bootMHz * ref / t) : 0);
		ekseam_puts("\n");
		}
	TUint32 seed = 12345;
	TInt fails = 0;
	for (TInt n = 0; n < 100; ++n)
		{
		seed = seed * 1103515245 + 12345;
		if (A64CpuSetOpp((seed >> 16) % KOppCount) != KErrNone)
			++fails;
		}
	ekseam_puts("[EKERN][power] opp test: 100 switches, fails="); ekseam_puthex(fails);
	// back to the boot frequency: the nearest OPP at or below it
	TInt back = 0;
	for (TInt i = 0; i < KOppCount; ++i)
		if (KOpp[i].iMHz <= bootMHz)
			back = i;
	A64CpuSetOpp(back);
	ekseam_puts("[EKERN][power] opp test: done, MHz="); ekseam_puthex(A64CpuCurrentMHz());
	}
#endif

// ---------------------------------------------------------------------------------------------------------------
// Power plan P3: the governor -- a kernel load monitor (the 701's dvfs.dll shape), ondemand-like:
//   every 100 ms the busiest core's load; >= 70 % -> straight to the highest allowed OPP; < 25 % for 500 ms -> one
//   step down. Thermal cap from the THS every second: >= 85 C -> 960 MHz, >= 75 C -> 1056 MHz, < 70 C -> no cap.
// A summary every 30 s (time per OPP, switches, temperature) instead of a line per switch.
// A64DEFS=-DA64_NO_GOVERNOR keeps U-Boot's 816 MHz.
extern "C" TInt A64CpuBusy(TUint64* aLastNow, TUint64* aLastIdle, TUint* aBusy, TInt aMax);	// a64_glue.cpp
extern "C" TInt A64ThsReadMilliC(TInt aSensor, TInt& aMilliC);								// a64_ths.cpp
extern "C" TBool A64UploadActive();														// a64_glue.cpp

const TInt KGovPeriodMs = 100;
static TDfcQue* GovQ;
static TDfc* GovDfc;
static NTimer* GovTimer;
static TUint64 GovLastNow, GovLastIdle[4];
static TInt GovLowTicks, GovCap = KOppCount - 1, GovTicks, GovSwitches;
static TUint32 GovTimeIn[KOppCount];	// ticks per OPP since the last summary

static void GovTimerFn(TAny*) { GovDfc->Add(); }

// Power plan P5: core control. Screen off (the lights-off state of the screensaver / SysAp) and a low load for 2 s ->
// ONE active core (NKern::SetNumberOfActiveCpus(1): the kernel migrates the threads to CPU 0 and retires the others
// through the idle handler, power/a64_idle.cpp -> PSCI CPU_OFF). Screen on, an upload, or CPU 0 >= 60 % busy -> all
// cores back (PSCI CPU_ON). If the kernel keeps trying to retire CPU 0 (refused) the policy gives up for the session.
#ifdef __SMP__
extern "C" TBool A64DisplayBacklightIsOff();		// display/a64_display.cpp
extern "C" TBool A64CoreRetireReady();				// power/a64_idle.cpp
extern "C" TInt A64CoreRetireRefusals();
extern "C" void A64IdleReport();
static TInt CcLevel, CcLowTicks;
static TBool CcGaveUp;

static void CoreControl(TUint aMaxBusy)
	{
#ifdef A64_CC_TEST
	return;						// the unattended core-retirement test (a64_idle.cpp) drives the cores instead
#endif
	const TInt ncpu = NKern::NumberOfCpus();
	if (ncpu < 2 || CcGaveUp || !A64CoreRetireReady())
		return;
	if (!CcLevel)
		CcLevel = ncpu;
	const TBool off = A64DisplayBacklightIsOff() && !A64UploadActive();
	if (A64CoreRetireRefusals() > 3)
		{
		CcGaveUp = ETrue;
		NKern::SetNumberOfActiveCpus(ncpu);
		CcLevel = ncpu;
		ekseam_puts("[EKERN][power] core control: kernel keeps retiring CPU 0 -- giving up, all cores on\n");
		return;
		}
	if (CcLevel > 1)
		{
		if (off && aMaxBusy < 40)
			{
			if (++CcLowTicks >= 20)
				{
				CcLowTicks = 0;
				CcLevel = 1;
				ekseam_puts("[EKERN][power] core control: screen off, idle -> 1 active core\n");
				NKern::SetNumberOfActiveCpus(1);
				}
			}
		else
			CcLowTicks = 0;
		}
	else if (!off || aMaxBusy >= 60)
		{
		CcLevel = ncpu;
		CcLowTicks = 0;
		ekseam_puts("[EKERN][power] core control: -> all cores\n");
		NKern::SetNumberOfActiveCpus(ncpu);
		}
	}
#endif

static void GovRun(TAny*)
	{
	TUint busy[4] = { 0, 0, 0, 0 };
	const TInt n = A64CpuBusy(&GovLastNow, GovLastIdle, busy, 4);
	TUint maxB = 0;
	for (TInt c = 0; c < n && c < 4; ++c)
		maxB = busy[c] > maxB ? busy[c] : maxB;
	if (++GovTicks % 10 == 0)
		{
		TInt mC = 0;
		if (A64ThsReadMilliC(0, mC) == KErrNone)
			GovCap = mC >= 85000 ? 3 : mC >= 75000 ? 5 : mC < 70000 ? KOppCount - 1 : GovCap;
		}
#ifdef __SMP__
	CoreControl(maxB);
#endif
	const TInt cur = TheOpp < 0 ? 1 : TheOpp;
	if (A64UploadActive())				// a USB upload (PIO): full speed once, then no more switches until it ends
		{
		if (cur != GovCap)
			A64CpuSetOpp(GovCap);
		++GovTimeIn[TheOpp < 0 ? 1 : TheOpp];
		GovTimer->OneShot(NKern::TimerTicks(KGovPeriodMs));
		return;
		}
	TInt want = cur;
	if (maxB >= 70)
		want = GovCap, GovLowTicks = 0;
	else if (maxB < 25)
		{
		if (++GovLowTicks >= 5 && cur > 0)
			want = cur - 1, GovLowTicks = 0;
		}
	else
		GovLowTicks = 0;
	if (want > GovCap)
		want = GovCap;
	if (want != cur || TheOpp < 0)
		{
		if (A64CpuSetOpp(want) == KErrNone)
			++GovSwitches;
		}
	++GovTimeIn[TheOpp < 0 ? 1 : TheOpp];
	if (GovTicks % (30000 / KGovPeriodMs) == 0)
		{
		TInt mC = 0;
		A64ThsReadMilliC(0, mC);
		ekseam_puts("[EKERN][power] gov 30 s: switches="); ekseam_puthex(GovSwitches);
		ekseam_puts(" ticks per opp 648..1152:");
		for (TInt i = 0; i < KOppCount; ++i)
			{
			ekseam_puts(" "); ekseam_puthex(GovTimeIn[i]);
			GovTimeIn[i] = 0;
			}
		ekseam_puts(" cap="); ekseam_puthex(GovCap); ekseam_puts(" cpu mC="); ekseam_puthex((unsigned)mC);
		ekseam_puts("\n");
#ifdef __SMP__
		A64IdleReport();
#endif
		GovSwitches = 0;
		}
	GovTimer->OneShot(NKern::TimerTicks(KGovPeriodMs));
	}

extern "C" TInt A64CpuGovernorStart()
	{
#ifdef A64_NO_GOVERNOR
	return KErrNone;
#else
	if (!TheCcu || !TheLock)
		return KErrNotReady;
	_LIT8(KQName, "CpuGov");
	TInt r = Kern::DfcQCreate(GovQ, 26, &KQName);
	if (r != KErrNone)
		return r;
	GovDfc = new TDfc(GovRun, NULL, GovQ, 1);
	GovTimer = new NTimer(GovTimerFn, NULL);
	if (!GovDfc || !GovTimer)
		return KErrNoMemory;
	TheQuiet = ETrue;
	TUint b[4];
	A64CpuBusy(&GovLastNow, GovLastIdle, b, 4);	// prime the window
	GovTimer->OneShot(NKern::TimerTicks(1000));
	ekseam_puts("[EKERN][power] governor started (100 ms, 648..1152 MHz, thermal cap)\n");
	return KErrNone;
#endif
	}

// The CPU-speed probe (a64_glue.cpp A64CpuProbe, the "phone is 20x too slow" question, 2026-10-17): the raw clock
// registers, the MHz they decode to and the governor's OPP index (-1 = never set = U-Boot's frequency).
extern "C" void A64CpuFreqReport()
	{
	if (!TheCcu)
		{
		ekseam_puts("[EKERN][cpuprobe] CCU not mapped yet\n");
		return;
		}
	ekseam_puts("[EKERN][cpuprobe] PLL_CPUX="); ekseam_puthex(TheCcu[KCcuPllCpux / 4]);
	ekseam_puts(" CPUX_AXI_CFG="); ekseam_puthex(TheCcu[KCcuCpuxAxi / 4]);
	ekseam_puts(" MHz(regs)="); ekseam_puthex((unsigned)A64CpuCurrentMHz());
	ekseam_puts(" opp="); ekseam_puthex((unsigned)TheOpp);
	ekseam_puts(" cap="); ekseam_puthex((unsigned)GovCap);
	ekseam_puts("\n");
	}
