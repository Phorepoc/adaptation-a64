// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// phe_usb_ekern.cpp -- the Phorepoc kernel as the host of the FreeBSD USB stack (phe_usb_port.h; the stack itself
// is FreeBSD's, unmodified, on its stand/kshim). The kshim is single-threaded, so everything USB runs in ONE DFC
// queue thread ("UsbHost"):
//   - an NTimer fires every tick and queues the service DFC (the kshim's 1 ms callout tick);
//   - each controller's GIC interrupt is bound; its ISR masks the line and queues the same DFC, which runs the
//     stack (device_run_interrupts -> ehci_interrupt / ohci_interrupt) and unmasks it again;
//   - the stack's memory (softcs, descriptors, transfer buffers) is one physically contiguous, uncached pool
//     (Epoc::AllocPhysicalRam + DPlatChunkHw, as the sound driver's DMA ring): the kshim's malloc (HAVE_MALLOC)
//     takes from it, and phe_usb_v2p() is the constant virtual -> physical offset of that pool (the kshim busdma
//     assumes physical = virtual; phe_usb_hc.c wraps usbd_get_page to translate).
// Started by InitExtension_usbhost() (the board's driver start list calls it).
#include <kernel/kern_priv.h>
#include <kernel/kernel.h>
#include <platform.h>
#include "syborg_priv.h"	// Interrupt:: (the shared virt/A64 variant)

extern "C" {
#include "../phe_usb_port.h"
void usb_init(void);
void ekseam_puts(const char*);
int phe_usb_printf(const char*, ...);
}

static const TInt KPoolSize = 0x200000;		// 2 MiB, as the bench's USB_POOL_SIZE
static TPhysAddr gPoolPhys;
static TLinAddr gPoolLin;
static TUint gPoolUsed;

struct SFreeBlock { SFreeBlock* iNext; TUint iSize; };
static SFreeBlock* gFree;

static TDynamicDfcQue* gQ;
static TDfc* gServiceDfc;
static NTimer* gTick;
static TBool gStarted;
static volatile TBool gExplore;		// phe_usb_hotplug(): explore every root hub on the next service run
extern "C" void usb_needs_explore_all(void);
static TInt gIrq[4];
static TInt gIrqCount;

extern "C" unsigned int phe_usb_v2p(unsigned long va)
	{
	if (va >= gPoolLin && va < gPoolLin + KPoolSize)
		return (unsigned int)(va - gPoolLin + gPoolPhys);
	return (unsigned int)va;		// not from the pool: nothing the controller may see
	}

// the kshim's malloc (HAVE_MALLOC): 8-byte header, exact-size free list (the kshim's own pool policy), bump otherwise
extern "C" void* usb_malloc(unsigned long aSize)
	{
	TUint size = (TUint)((aSize + 7) & ~7ul) + 8;
	for (SFreeBlock** p = &gFree; *p; p = &(*p)->iNext)
		if ((*p)->iSize == size)
			{
			SFreeBlock* b = *p; *p = b->iNext;
			memclr((TUint8*)b + 8, size - 8);
			return (TUint8*)b + 8;
			}
	if (gPoolUsed + size > (TUint)KPoolSize)
		{
		ekseam_puts("[usbh] pool exhausted\n");
		return NULL;
		}
	SFreeBlock* b = (SFreeBlock*)(gPoolLin + gPoolUsed);
	gPoolUsed += size;
	b->iSize = size;
	memclr((TUint8*)b + 8, size - 8);
	return (TUint8*)b + 8;
	}

extern "C" void usb_free(void* aPtr)
	{
	if (!aPtr)
		return;
	SFreeBlock* b = (SFreeBlock*)((TUint8*)aPtr - 8);
	b->iNext = gFree;
	gFree = b;
	}

extern "C" unsigned int phe_usb_time_us(void)
	{
	// the generic timer (NKern::Timestamp), not the tick count: the stack busy-waits in its own loops
	static TUint32 div;
	if (!div)
		div = NKern::TimestampFrequency() / 1000000u;
	return (unsigned int)(NKern::Timestamp() / (div ? div : 1));
	}

extern "C" void DELAY(unsigned int aUs)
	{
	Kern::NanoWait(aUs * 1000u);
	}

extern "C" void delay(unsigned int aUs)
	{
	Kern::NanoWait(aUs * 1000u);
	}

// kshim pause(): sleep aTicks (hz = 1000) on the USB thread; the callouts and interrupts that came due run after
extern "C" int pause(const char*, int aTicks)
	{
	if (aTicks <= 0)
		aTicks = 1;
	// a busy wait on the generic timer: NKern::Sleep never returned here during boot (the tick count stood still on
	// the H3 test ROM while the extensions start), so neither the kernel tick nor NTimer is used for it
	unsigned int t0 = phe_usb_time_us();
	while ((unsigned int)(phe_usb_time_us() - t0) < (unsigned int)aTicks * 1000u)
		Kern::NanoWait(50000);
	return 0;
	}

static void TickFn(TAny*)
	{
	gServiceDfc->Add();
	gTick->Again(1);
	}

static void Isr(TAny* aIrq)
	{
	Interrupt::Disable((TInt)aIrq);
	gServiceDfc->Add();
	}

// phe_usb_tty.c asks for a service run (data to send, a port to open)
extern "C" void phe_usb_kick(void)
	{
	if (gServiceDfc)
		gServiceDfc->Enque();
	}

#ifdef A64_H3TEST
// the QEMU test: AT out through the first USB serial port (QEMU's FTDI usb-serial, its chardev an echo server), and
// what comes back. Runs on the USB thread, from the service loop.
static void AtSelfTest()
	{
	static int state;
	static unsigned int t0;
	static char rx[64];
	static int got;
	const unsigned int now = phe_usb_time_us();
	switch (state)
		{
	case 0:
		if (phe_usb_tty_name(0))
			{
			phe_usb_tty_open(0);
			t0 = now;
			state = 1;
			}
		break;
	case 1:
		if (now - t0 > 500000u)
			{
			phe_usb_tty_write(0, "AT\r", 3);
			ekseam_puts("[usbh] tty0 tx: AT\n");
			t0 = now;
			state = 2;
			}
		break;
	case 2:
		{
		const int n = phe_usb_tty_read(0, rx + got, (int)sizeof(rx) - 1 - got);
		if (n > 0)
			got += n;
		if (got >= 3 || now - t0 > 3000000u)
			{
			rx[got] = 0;
			for (int i = 0; i < got; i++)
				if (rx[i] == '\r' || rx[i] == '\n')
					rx[i] = '.';
			ekseam_puts(got >= 3 && rx[0] == 'A' && rx[1] == 'T' ? "[usbh] tty0 AT echo PASS rx: "
																   : "[usbh] tty0 AT echo FAIL rx: ");
			ekseam_puts(rx);
			ekseam_puts("\n");
			state = 3;
			}
		break;
		}
	default:
		break;
		}
	}
#endif

static NTimer* gDumpTimer;		// (the dump after the first enumeration window, below)
#if defined(PHE_USBHOST_TRACE) && !defined(A64_H3TEST)
extern "C" void A64SdFlushNow(TInt aStage);		// a64_glue.cpp: the SD log, thread context only
#define USBH_FLUSH(n) A64SdFlushNow(0x500 + (n))
#else
#define USBH_FLUSH(n) do { } while (0)
#endif

static void ServiceDfcFn(TAny*)
	{
	if (!gStarted)
		{
		gStarted = ETrue;
		ekseam_puts("[usbh] usb_init (FreeBSD USB, kshim) in the UsbHost DFC thread\n");
		USBH_FLUSH(1);
		usb_init();
		ekseam_puts("[usbh] usb_init done\n");
		USBH_FLUSH(2);
		const TInt tick0 = NKern::TickCount();
		Kern::NanoWait(20000000);
#ifdef A64_H3TEST
		// (the bench only: on the phone this loop never ends and, polling at priority 24 on CPU 0 with no kernel tick,
		// it starves the Supervisor thread that is still starting the extensions -- boot stops before stage 4)
		if (NKern::TickCount() == tick0)
			{
			// no kernel tick yet (the QEMU H3 test ROM: the nanokernel tick stands still while the extensions start,
			// so neither NTimer nor NKern::Sleep comes back): poll from this thread on the generic timer instead
			ekseam_puts("[usbh] no kernel tick: polling the stack from the UsbHost thread\n");
			const unsigned int t0 = phe_usb_time_us();
			TBool dumped = EFalse;
			for (;;)
				{
				if (gExplore)
					{
					gExplore = EFalse;
					usb_needs_explore_all();
					}
				phe_usb_service();
				for (TInt i = 0; i < gIrqCount; i++)
					Interrupt::Enable(gIrq[i]);
#ifdef A64_H3TEST
				if (dumped)
					AtSelfTest();
#endif
				if (!dumped && (unsigned int)(phe_usb_time_us() - t0) > 8000000u)
					{
					dumped = ETrue;
					phe_usb_dump_devices();
					ekseam_puts("[usbh] dump done\n");
					}
				Kern::NanoWait(1000000);
				}
			}
#endif
		gTick->OneShot(1);
#ifndef A64_H3TEST
		gDumpTimer->OneShot(NKern::TimerTicks(25000));	// the modem enumerates 15-20 s after PWRKEY: dump what attached
#endif
		}
	if (gExplore)
		{
		gExplore = EFalse;
		usb_needs_explore_all();
		}
	phe_usb_service();
	for (TInt i = 0; i < gIrqCount; i++)
		Interrupt::Enable(gIrq[i]);
	}

// hot-plug: a device that appears after boot (the modem, once phe_modem_power_on has run) -- the port-change interrupt
// normally finds it; this makes the hubs look again in case the PHY missed the connect while the port had no power
extern "C" void phe_usb_hotplug(void)
	{
	gExplore = ETrue;
	ekseam_puts("[usbh] hotplug: the modem is up\n");
	USBH_FLUSH(0);
	if (gServiceDfc)
		gServiceDfc->Enque();
	}

// the dump after the first enumeration window, as the bench does
static TDfc* gDumpDfc;
static void DumpTimerFn(TAny*) { gDumpDfc->Add(); }
static void DumpDfcFn(TAny*)
	{
	phe_usb_dump_devices();
	ekseam_puts("[usbh] dump done\n");
	}

extern "C" void phe_usb_irq_attach(int aIrq)
	{
#ifndef A64_H3TEST
	// the phone polls the controllers (the service DFC runs every tick, the kshim callout every tick): the GIC lines
	// stay unbound. Boot 2728: the hang came right after the EHCI1 line was enabled (ISR -> mask -> DFC -> unmask
	// re-fires without end, the storm of boot 2682, and CPU 0 -- the debug link, the watchdog feed -- stops).
	ekseam_puts("[usbh] irq line left unbound: polled\n");
	(void)aIrq;
	return;
#endif
	if (aIrq < 0 || gIrqCount >= 4)
		return;
	if (Interrupt::Bind(aIrq, Isr, (TAny*)aIrq) == KErrNone)
		{
		gIrq[gIrqCount++] = aIrq;
		Interrupt::Enable(aIrq);
		}
	}

TInt UsbCommInstall();						// phe_usb_comm.cpp
#ifdef A64_USBHOST_NET
TInt UsbEtherInstall();						// phe_usb_ether.cpp
#endif

#if defined(PHE_USBHOST_TRACE) && !defined(A64_H3TEST)
// Boot-trial breadcrumbs: this runs in the Supervisor thread between SD-log stages 3 and 4. A hang here is a watchdog
// reset back to slot A with nothing from slot B on the debug link, so each step is flushed to the SD log (the
// replay of the died B trial in slot A's log); a64_glue.cpp's A64SdFlushNow is thread-context only.
extern "C" void A64SdFlushNow(TInt aStage);
#define USBH_STEP(n, s) do { ekseam_puts("[usbh] step " s "\n"); A64SdFlushNow(0x400 + (n)); } while (0)
#else
#define USBH_STEP(n, s) do { } while (0)
#endif

TInt InitExtension_usbhost()
	{
	USBH_STEP(1, "1: AllocPhysicalRam 2 MiB");
	TInt r = Epoc::AllocPhysicalRam(KPoolSize, gPoolPhys, 12);
	if (r != KErrNone)
		return r;
	USBH_STEP(2, "2: pool chunk");
	DPlatChunkHw* ch = NULL;
	r = DPlatChunkHw::New(ch, gPoolPhys, KPoolSize, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return r;
	gPoolLin = ch->LinearAddress();
	USBH_STEP(3, "3: board init (CCU gates, resets)");
	r = phe_usb_board_init();				// the SoC: register mappings, clocks, resets, PHY
	if (r != 0)
		return r;
	USBH_STEP(4, "4: DFC queue");
	_LIT(KQName, "UsbHost");
	// priority 16: the stack busy-waits (pause/DELAY on the generic timer) for whole milliseconds while it enumerates;
	// at 24 that outranked the Supervisor thread and the system servers on CPU 0
	r = Kern::DynamicDfcQCreate(gQ, 16, KQName);
	if (r != KErrNone)
		return r;
#ifdef __SMP__
	NKern::ThreadSetCpuAffinity((NThread*)gQ->iThread, 0);	// the GIC routes SPIs to CPU 0 (a64_usbcc.cpp)
#endif
	gServiceDfc = new TDfc(ServiceDfcFn, NULL, gQ, 1);
	gTick = new NTimer(TickFn, NULL);
	gDumpDfc = new TDfc(DumpDfcFn, NULL, gQ, 2);
	gDumpTimer = new NTimer(DumpTimerFn, NULL);
	if (!gServiceDfc || !gTick || !gDumpDfc || !gDumpTimer)
		return KErrNoMemory;
	USBH_STEP(5, "5: Comm.USB PDD");
	UsbCommInstall();						// phe_usb_comm.cpp: the "Comm.USB" PDD for the serial ports
#ifdef A64_USBHOST_NET
	UsbEtherInstall();						// phe_usb_ether.cpp: the "Ethernet.USB" PDD (the modem in ECM/NCM mode)
#endif
#ifdef A64_H3TEST
	gServiceDfc->Enque();					// the first run calls usb_init()
	gDumpTimer->OneShot(NKern::TimerTicks(8000));
#else
	// the phone: usb_init() runs when something asks for the host (phe_usb_hotplug(), called when the modem is powered
	// up), not while the Supervisor thread is still starting the extensions with no kernel tick
	ekseam_puts("[usbh] usb_init deferred until the first phe_usb_hotplug()\n");
#endif
	ekseam_puts("[usbh] host started: DFC thread UsbHost, 2 MiB uncached pool\n");
	USBH_STEP(6, "6: done");
	return KErrNone;
	}
