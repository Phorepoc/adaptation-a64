// OURS (PinePhone track, docs/phorepoc/pinephone-port.md): the QEMU `virt` variant, derived from
// oss.FCL.sf.adapt.Qemu/baseport/syborg/specific/assp.cpp. Class names stay Syborg/TSyborg for now (the ROM
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
* NTT Docomo, Inc : BUG 1296
* NTT DOCOMO, INC - Fix for bug 1292 "E32test t_mstim.exe failed to load test LDD"
*
* Description: implementation of class Syborg
*
*/

#include <syborg_priv.h>
#include <hal_data.h>

#ifdef __EMI_SUPPORT__
#include <emi.h>
#endif

//#define ASSP_DEBUG

#ifdef ASSP_DEBUG
#define __DEBUG_PRINT(format...)    Kern::Printf(format)
#else
#define __DEBUG_PRINT(format...)    __KTRACE_OPT(KBOOT,Kern::Printf(format))
#endif

Syborg* Syborg::Variant=NULL;
// !@!
#if 0
TPhysAddr Syborg::VideoRamPhys;
TPhysAddr Syborg::VideoRamPhysSecure;		// Secure display memory
#endif

GLDEF_D Syborg TheVariant;

// Wait for interrupt idle routine
extern TInt SyborgWFIIdle();

DECLARE_STANDARD_ASSP()

#ifdef __SMP__
// VIRT SMP: the nanokernel interface block (NaviEngine ne1_tb/specific/variant.cpp InitVIB is the template).
// The kernel fills iUncached[] from the AP boot page (A::CreateVariant) and NKern::Init0 copies the rest into
// TheScheduler. There is no SCU and no A9 private/global timer: the generic-timer stand-in reaches the timer
// through cp15 (src/phorepoc-arm/nkernsmp-gt/arm_tmr.h), so iLocalTimerAddr only has to be a valid pointer.
extern "C" {
SVariantInterfaceBlock VIB;
}

// The kernel reads the CPU count from the A9 SCU's configuration register (A::InitAPs: SCU.iConfig & 3, +1) --
// Cortex-A7/A53 have no memory-mapped SCU. A RAM stand-in shaped like ArmScu (arm_scu.h; every other SCU read is
// a debug trace) answers it. Its iConfig CPU count comes from the GIC (GICD_TYPER CPUNumber, bits 7:5 = number of
// CPU interfaces - 1 -- QEMU sets it from -smp), the same source the bootstrap's FinalInitialise uses to hand the
// APs their boot page; KMaxVirtCpus caps it (the SCU field is 2 bits: up to 4 CPUs, as the A7/A53 clusters).
const TUint32 KMaxVirtCpus = 4;
static TUint32 FakeScu[64];		// iCtrl, iConfig, ... all 0 until InitVIB
#ifdef __A64__
extern "C" TCpuPowerUpFn A64CpuPowerUpFn();		// power/a64_idle.cpp (0 unless A64_RETIRE or A64_CC_TEST)
#endif

static SVariantInterfaceBlock* InitVIB()
{
	SVariantInterfaceBlock* v = &VIB;
	v->iVer = 0;
	v->iSize = sizeof(VIB);
	v->iMaxCpuClock = UI64LIT(1000000000);	// nominal 1 GHz (QEMU) -- only scales the AP boot wait loop
	TUint32 cntfrq;
	asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(cntfrq));
	v->iMaxTimerClock = cntfrq;			// CNTFRQ (firmware-set: QEMU virt 62.5 MHz, A64 24 MHz; = KTimestampFrequency)
	TUint32 ncpus = ((*(volatile TUint32*)(KHwBaseGicDist + 4) >> 5) & 7) + 1;		// GICD_TYPER
	if (ncpus > KMaxVirtCpus)
		ncpus = KMaxVirtCpus;
	FakeScu[1] = ncpus - 1;
	v->iScuAddr = (TLinAddr)FakeScu;		// none on A7/A53 (coherency is architectural): the RAM stand-in above
	v->iGicDistAddr = KHwBaseGicDist;
	v->iGicCpuIfcAddr = KHwBaseGicCpu;
	v->iLocalTimerAddr = KHwBaseGicCpu;	// never dereferenced (cp15 proxies); must not be 0
	v->iGlobalTimerAddr = 0;
#ifdef __A64__
	// power plan P5: the A64 retires idle cores through PSCI CPU_OFF (power/a64_idle.cpp); the power-up is PSCI CPU_ON
	// (iCpuPowerUpFn != 0 = the kernel's core control is supported). Cores power themselves off: no power-down function.
	v->iCpuPowerUpFn = A64CpuPowerUpFn();
#else
	v->iCpuPowerUpFn = 0;					// no CPU retirement yet
#endif
	v->iCpuPowerDownFn = 0;
	return v;
}

EXPORT_C TAny* VariantInitialise(TInt a)
{
	switch (a)
	{
	case 0:	return &TheVariant;
	case 1: return InitVIB();				// asked for because the constructor sets the VIB flag
	default: return 0;
	}
}
#else
EXPORT_C Asic* VariantInitialise()
{
	return &TheVariant;
}
#endif

// #define TICK_PROBE	// diagnosis build (2026-10-07; CPU 0 timer + a CPU 3 tick watcher): see Syborg::Idle
extern "C" { extern TUint32 TheTickReload; void ekseam_puts(const char*); void ekseam_puthex(unsigned); }

void Syborg::Idle()
{
	// Use the basic Wait For Interrupt call to idle
	TInt irq = NKern::DisableAllInterrupts();
#ifdef __EMI_SUPPORT__
	EMI::EnterIdle();
#endif
#if defined(__SMP__) && defined(TICK_PROBE)
	// TICK PROBE (2026-10-07, diagnosis): the S60 boot on virt stalls with every core in WFI and the tick dead.
	// CPU 0 owns the tick (CNTP, PPI 30): before it sleeps, check the timer would wake it.
	if (NKern::CurrentCpu() == 0)
		{
		TUint32 ctl, cvl, cvh, cnl, cnh;
		asm volatile("mrc p15, 0, %0, c14, c2, 1" : "=r"(ctl));
		asm volatile("mrrc p15, 2, %0, %1, c14" : "=r"(cvl), "=r"(cvh));
		asm volatile("mrrc p15, 0, %0, %1, c14" : "=r"(cnl), "=r"(cnh));
		const TUint64 cval = ((TUint64)cvh << 32) | cvl, cnt = ((TUint64)cnh << 32) | cnl;
		static TInt reported;
		static TInt overdue;
		if (overdue < 2 && TheTickReload && cnt > cval && cnt - cval > 100ull * TheTickReload)
			{
			++overdue;
			volatile TUint32* gicc = (volatile TUint32*)KHwBaseGicCpu;
			volatile TUint32* gicd = (volatile TUint32*)KHwBaseGicDist;
			ekseam_puts("[tickprobe] CPU0 tick OVERDUE (ms)="); ekseam_puthex((TUint32)((cnt - cval) / TheTickReload));
			ekseam_puts("[tickprobe]   CNTP_CTL="); ekseam_puthex(ctl);
			ekseam_puts("[tickprobe]   GICC_PMR="); ekseam_puthex(gicc[0x04 / 4]);
			ekseam_puts("[tickprobe]   GICC_RPR="); ekseam_puthex(gicc[0x14 / 4]);
			ekseam_puts("[tickprobe]   GICC_HPPIR="); ekseam_puthex(gicc[0x18 / 4]);
			for (TInt i = 0; i < 4; ++i)
				{
				ekseam_puts("[tickprobe]   GICD_ISENABLER/ISPENDR/ISACTIVER "); ekseam_puthex((TUint32)i);
				ekseam_puthex(gicd[(0x100 + 4 * i) / 4]); ekseam_puthex(gicd[(0x200 + 4 * i) / 4]); ekseam_puthex(gicd[(0x300 + 4 * i) / 4]);
				}
			}
		if (reported < 3 && ((ctl & 3) != 1 || (cval > cnt && cval - cnt > 100ull * TheTickReload)))
			{
			++reported;
			ekseam_puts("[tickprobe] CPU0 tick timer wrong before WFI: CNTP_CTL="); ekseam_puthex(ctl);
			ekseam_puts("[tickprobe]   CVAL-CNT (hi,lo)="); ekseam_puthex((TUint32)((cval - cnt) >> 32));
			ekseam_puthex((TUint32)(cval - cnt)); ekseam_puts("[tickprobe]   reload="); ekseam_puthex(TheTickReload);
			}
		}
#endif
#if defined(__SMP__) && defined(TICK_PROBE)
	// CPU 3 never sleeps (diagnosis): it watches the kernel tick from outside CPU 0. Stale for 200 ms of counter
	// time -> print the GIC state once: a stuck-active SPI holding CPU 0's running priority would starve the tick.
	if (NKern::CurrentCpu() == 3)
		{
		static TUint32 lastTick, lastLo; static TUint64 lastCnt; static TInt told;
		TUint32 lo, hi; asm volatile("mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));		// CNTVCT
		const TUint64 c = ((TUint64)hi << 32) | lo;
		const TUint32 t = NKern::TickCount();
		if (t != lastTick || !lastCnt) { lastTick = t; lastCnt = c; }
		else if (told < 2 && t > 5000 && c - lastCnt > 200ull * TheTickReload)
			{
			++told; lastCnt = c;
			volatile TUint32* gicd = (volatile TUint32*)KHwBaseGicDist;
			volatile TUint32* gicc = (volatile TUint32*)KHwBaseGicCpu;
			ekseam_puts("[tickwatch] TICK STALE at ms="); ekseam_puthex(t);
			ekseam_puts("[tickwatch]   cpu3 GICC_RPR/HPPIR="); ekseam_puthex(gicc[0x14 / 4]); ekseam_puthex(gicc[0x18 / 4]);
			for (TInt i = 0; i < 4; ++i)
				{
				ekseam_puts("[tickwatch]   word/EN/PEND/ACT "); ekseam_puthex((TUint32)i);
				ekseam_puthex(gicd[(0x100 + 4 * i) / 4]); ekseam_puthex(gicd[(0x200 + 4 * i) / 4]); ekseam_puthex(gicd[(0x300 + 4 * i) / 4]);
				}
			ekseam_puts("[tickwatch]   GICD_ITARGETSR 32..47 ");
			for (TInt i = 0; i < 4; ++i) ekseam_puthex(gicd[(0x800 + 32 + 4 * i) / 4]);
			}
		NKern::RestoreInterrupts(irq);
		return;
		}
#endif
	SyborgWFIIdle();
#ifdef __EMI_SUPPORT__
	EMI::LeaveIdle();
#endif
	NKern::RestoreInterrupts(irq);
}

TUint32 Syborg::NanoWaitCalibration()
{
	return 17;	// 2 cycles approx 17ns at 125MHz
}

TInt Syborg::VariantHal(TInt aFunction, TAny* a1, TAny* a2)
{
	__KTRACE_OPT(KHARDWARE,Kern::Printf("Syborg::VariantHal(%d, %0x, %0x)",aFunction,a1,a2));

	switch(aFunction)
    {
		case EVariantHalVariantInfo:
		{
			TVariantInfoV01Buf infoBuf;
			TVariantInfoV01& info = infoBuf();
			TUint clock=0;
			info.iRomVersion = Epoc::RomHeader().iVersion;
// NTT Docomo - Defect 1292 fix - E32test t_mstim.exe failed to load test LDD - start
			const TInt KMachineUid_Syborg = 0x10005152;
    		info.iMachineUniqueId = (TInt64(KMachineUid_Syborg)<<32);
// NTT Docomo - Defect 1292 fix - E32test t_mstim.exe failed to load test LDD - end
   			info.iLedCapabilities = (8<<16) + KLedMaskGreen1;
			info.iProcessorClockInKHz = clock;
			info.iSpeedFactor = clock/25;
			Kern::InfoCopy(*(TDes8*)a1,infoBuf);
			break;
	    }
		case EVariantHalDebugPortSet:
	    {
			TUint32 thePort = (TUint32)a1;
			switch(thePort) // Accept UART(0-3)
		    {
				case 0:
				case 1:
			    case 2:
                case 3:
                {
                    TSyborg::MarkDebugPortOff();           // mark port is not initialised
					Kern::SuperPage().iDebugPort = thePort; // update the super page
                    Syborg::DebugInit();                   // init debug port        
                    break;
                }
				case (TUint32)KNullDebugPort:               // debug output supressed
                {
                    TSyborg::MarkDebugPortOff();           // mark port is not initialised
					Kern::SuperPage().iDebugPort = thePort; // update the super page
					break;
                }
				default:
					return KErrNotSupported;
		    }
			break;
	    }
		case EVariantHalDebugPortGet:
	    {
			TUint32 thePort = Kern::SuperPage().iDebugPort;
			kumemput32(a1, &thePort, sizeof(TUint32));
			break;
	    }
		case EVariantHalSwitches:
	    {
		  TUint32 x = 0; //Register32(KHwBaseSystemReg+KHoRoSystemSw);
			kumemput32(a1, &x, sizeof(x));
			break;
	    }
		case EVariantHalLedMaskSet:
	    {
		  //SetRegister32(KHwBaseSystemReg+KHoRwSystemLed, (TUint32)a1 & 0xFF);
			break;
	    }
		case EVariantHalLedMaskGet:
	    {
		  TUint32 x = 0; //Register32(KHwBaseSystemReg+KHoRwSystemLed);
			kumemput32(a1, &x, sizeof(x));
			break;
	    }
		case EVariantHalCustomRestartReason:
	    {
			// Restart reason is stored in super page
		  TInt x = (Kern::SuperPage().iHwStartupReason); // & KmRestartCustomReasons) >> KsRestartCustomReasons;
			kumemput32(a1, &x, sizeof(TInt));
			break;
	    }
		case EVariantHalCustomRestart:
	    {
            __KERNEL_CAPABILITY_CHECK(
                if(!Kern::CurrentThreadHasCapability(ECapabilityPowerMgmt,
			       __PLATSEC_DIAGNOSTIC_STRING("Checked by Hal function EVariantHalCustomRestart")))
				    return KErrPermissionDenied;
            )
			  Kern::Restart(0);
			break;
	    }
		default:
    	{
			return KErrNotSupported;
	    }
    }
	return KErrNone;
}

TPtr8 Syborg::MachineConfiguration()
{
	return TPtr8((TUint8*)&Kern::MachineConfig(),40,40);
}

EXPORT_C Syborg::Syborg()
{
#ifdef __SMP__
	__VARIANT_SUPPORTS_NANOKERNEL_INTERFACE_BLOCK__();	// -> the kernel calls VariantInitialise(1) for the VIB
#endif
	iDebugPortBase = TSyborg::DebugPortAddr(); // initialised in bootstrap
}

EXPORT_C TMachineStartupType Syborg::StartupReason()
{
	TUint s = Kern::SuperPage().iHwStartupReason;
	__DEBUG_PRINT("Syborg::StartupReason CPU page value 0x%08X", s);
	return EStartupColdReset;
}

EXPORT_C void Syborg::Init1()
{
	__DEBUG_PRINT("Syborg::Init1()");

	// Enable the CLCD in the System registers
	SyborgInterrupt::Init1();
}

EXPORT_C void Syborg::Init3()
{
	NTimerQ& m = *(NTimerQ*)NTimerQ::TimerAddress();
	m.iRounding = -5;
		
	TInt r = Interrupt::Bind(EIntTickTimer,SyborgInterrupt::MsTimerTick,&m);	// VIRT
	if (r != KErrNone)
	{
		Kern::Fault("BindMsTick",r);
	}

	TSyborg::Init3();

	r = Interrupt::Enable(EIntTickTimer);	// VIRT
	if (r != KErrNone)
	{
		Kern::Fault("EnbMsTick",r);
	}

	SyborgInterrupt::StartTick();	// VIRT: CNTV_TVAL = CNTFRQ/1000, CNTV_CTL = enable
	
	SyborgInterrupt::Init3();

	// !@!
#if 0
	// Allocate physical RAM for video
	TInt vSize = TSyborg::VideoRamSize();

	r = Epoc::AllocPhysicalRam(vSize,Syborg::VideoRamPhys);
	if (r != KErrNone)
	{
		Kern::Fault("AllocVideoRam",r);
	}

	// Allocate physical RAM for secure display
	r = Epoc::AllocPhysicalRam(vSize,Syborg::VideoRamPhysSecure);
	if (r != KErrNone)
	{
		Kern::Fault("AllocVideoRam 2",r);
	}
#endif
	__DEBUG_PRINT("Finished Syborg::Init3()");
}

EXPORT_C void Syborg::DebugInit()
{
	iDebugPortBase = TSyborg::DebugPortAddr();

    if(iDebugPortBase != (TUint32)KNullDebugPort)   // supress debug output
    {
	  TUint8 res = ReadReg(iDebugPortBase, 0);
    }
}

//
// Output a character to the debug port
//
#ifdef __A64__
extern "C" void ekseam_board_log(unsigned c);
extern "C" TBool A64UserDebugLog;
#endif
EXPORT_C void Syborg::DebugOutput(TUint aLetter)
{
	if(!iDebugPortBase)
    {
		DebugInit();
	}

    if(iDebugPortBase != (TUint32)KNullDebugPort)   // supress debug output
    {
#ifdef __A64__
	  // A64: Kern::Printf / RDebug::Print also go to the board log (USB + SD; the phone's real channel -- before
	  // 2026-10-07 user RDebug output never left the 16550), and never spin on THR (as ekseam_uart_put: drop)
	  // ... only with A64DEFS=-DA64_RDEBUG_LOG (a64_glue.cpp): always-on, the S60 fader's per-rect prints (~10k a
	  // minute in a faded state) throttled WSERV until the UI looked frozen (2026-10-07)
	  if (A64UserDebugLog)
		  ekseam_board_log(aLetter);
	  if (!(*(volatile TUint32*)(iDebugPortBase + KUartLsr) & KUartLsrThre))
		  return;
#endif
	  WriteReg(iDebugPortBase,0,aLetter);	// VIRT: PL011 UARTDR is at +0 (A64: the 16550 THR too)
    }
}

EXPORT_C TInt Syborg::MsTickPeriod()
{
	return KMsTickPeriod;
}

EXPORT_C TInt Syborg::SystemTimeInSecondsFrom2000(TInt& aTime)
{
	__KTRACE_OPT(KHARDWARE,Kern::Printf("RTC READ: %d",aTime));
	return KErrNone;
}

EXPORT_C TInt Syborg::SetSystemTimeInSecondsFrom2000(TInt aTime)
{
	__KTRACE_OPT(KHARDWARE,Kern::Printf("Set RTC: %d",aTime));					// do this here to allow the value to be loaded...
	return KErrNone;
}

#ifdef __SMP__
void Syborg::Init2AP()
{
	// VIRT SMP: nothing per CPU -- the nanokernel brings up each CPU's GIC interface and timeslice timer
}
#endif
