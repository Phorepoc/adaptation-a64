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

static void ServiceDfcFn(TAny*)
	{
	if (!gStarted)
		{
		gStarted = ETrue;
		ekseam_puts("[usbh] usb_init (FreeBSD USB, kshim) in the UsbHost DFC thread\n");
		usb_init();
		ekseam_puts("[usbh] usb_init done\n");
		const TInt tick0 = NKern::TickCount();
		Kern::NanoWait(20000000);
		if (NKern::TickCount() == tick0)
			{
			// no kernel tick yet (the QEMU H3 test ROM: the nanokernel tick stands still while the extensions start,
			// so neither NTimer nor NKern::Sleep comes back): poll from this thread on the generic timer instead
			ekseam_puts("[usbh] no kernel tick: polling the stack from the UsbHost thread\n");
			const unsigned int t0 = phe_usb_time_us();
			TBool dumped = EFalse;
			for (;;)
				{
				phe_usb_service();
				for (TInt i = 0; i < gIrqCount; i++)
					Interrupt::Enable(gIrq[i]);
				if (!dumped && (unsigned int)(phe_usb_time_us() - t0) > 8000000u)
					{
					dumped = ETrue;
					phe_usb_dump_devices();
					ekseam_puts("[usbh] dump done\n");
					}
				Kern::NanoWait(1000000);
				}
			}
		gTick->OneShot(1);
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
	if (gServiceDfc)
		gServiceDfc->Enque();
	}

// the dump after the first enumeration window, as the bench does
static TDfc* gDumpDfc;
static NTimer* gDumpTimer;
static void DumpTimerFn(TAny*) { gDumpDfc->Add(); }
static void DumpDfcFn(TAny*)
	{
	phe_usb_dump_devices();
	ekseam_puts("[usbh] dump done\n");
	}

extern "C" void phe_usb_irq_attach(int aIrq)
	{
	if (aIrq < 0 || gIrqCount >= 4)
		return;
	if (Interrupt::Bind(aIrq, Isr, (TAny*)aIrq) == KErrNone)
		{
		gIrq[gIrqCount++] = aIrq;
		Interrupt::Enable(aIrq);
		}
	}

TInt InitExtension_usbhost()
	{
	TInt r = Epoc::AllocPhysicalRam(KPoolSize, gPoolPhys, 12);
	if (r != KErrNone)
		return r;
	DPlatChunkHw* ch = NULL;
	r = DPlatChunkHw::New(ch, gPoolPhys, KPoolSize, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return r;
	gPoolLin = ch->LinearAddress();
	r = phe_usb_board_init();				// the SoC: register mappings, clocks, resets, PHY
	if (r != 0)
		return r;
	_LIT(KQName, "UsbHost");
	r = Kern::DynamicDfcQCreate(gQ, 24, KQName);	// below the kernel DFC threads (27)
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
	gServiceDfc->Enque();					// the first run calls usb_init()
	gDumpTimer->OneShot(NKern::TimerTicks(8000));
	ekseam_puts("[usbh] host started: DFC thread UsbHost, 2 MiB uncached pool\n");
	return KErrNone;
	}
