// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// ekern_fb_glue.cpp -- Track B M5.5 glue.
//
// Loads the REAL in-source Syborg framebuffer driver (baseport/syborg/svpframebuffer/
// svpframebuffer.cpp, a DECLARE_STANDARD_EXTENSION kernel extension) by calling its
// InitExtension() entry directly from the supervisor thread -- the exact context the
// ROM extension-start DFC would run it in -- then paints a proof pattern into the
// driver's OWN allocated video RAM. This REPLACES the seam's ekseam_fb_test() raw
// CLCD register poke (M5). The driver does the legitimate work: Epoc::AllocPhysicalRam
// + DPlatChunkHw::New for the framebuffer, Kern::AddHalEntry, Kern::DfcQCreate, and
// DisplayOn()->PowerUpLcd() programming the CLCD. We only fill the buffer to prove it
// is the live, scanned surface. This is bring-up glue, not Symbian logic; it lives in
// its own TU so the driver's display.h/videodriver.h include set stays out of the main
// seam.
#include <e32def.h>
#include <kernel/kernel.h>
#include "svpframebuffer.h"

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);

// DECLARE_STANDARD_EXTENSION() in svpframebuffer.cpp expands to `TInt InitExtension()`.
int InitExtension();

// --- bring-up bisection: bracket the driver's Create()/AllocateFrameBuffer() path with raw-UART
// markers (ekseam_puts, the known-good channel) so we can see exactly where init hangs. Wired via
// `ld --wrap` in link_ekern.sh. Remove once the driver inits cleanly. -------------------------
extern "C" int  __real__ZN16DLcdPowerHandler6CreateEv(void*);
extern "C" int  __wrap__ZN16DLcdPowerHandler6CreateEv(void* t)
	{ ekseam_puts("[fb]  DLcdPowerHandler::Create...\n");
	  int r = __real__ZN16DLcdPowerHandler6CreateEv(t);
	  ekseam_puts("[fb]  DLcdPowerHandler::Create done r="); ekseam_puthex((unsigned)r); return r; }
extern "C" int  __real__ZN16DLcdPowerHandler19AllocateFrameBufferEv(void*);
extern "C" int  __wrap__ZN16DLcdPowerHandler19AllocateFrameBufferEv(void* t)
	{ ekseam_puts("[fb]   AllocateFrameBuffer...\n");
	  int r = __real__ZN16DLcdPowerHandler19AllocateFrameBufferEv(t);
	  ekseam_puts("[fb]   AllocateFrameBuffer done r="); ekseam_puthex((unsigned)r); return r; }
extern "C" int  __real__ZN4Epoc16AllocPhysicalRamEiRmi(int, unsigned long*, int);
extern "C" int  __wrap__ZN4Epoc16AllocPhysicalRamEiRmi(int sz, unsigned long* pa, int a)
	{ ekseam_puts("[fb]    Epoc::AllocPhysicalRam size="); ekseam_puthex((unsigned)sz);
	  int r = __real__ZN4Epoc16AllocPhysicalRamEiRmi(sz, pa, a);
	  ekseam_puts("[fb]    Epoc::AllocPhysicalRam done r="); ekseam_puthex((unsigned)r);
	  ekseam_puts("[fb]    phys="); ekseam_puthex((unsigned)(pa ? *pa : 0)); return r; }
extern "C" int  __real__ZN12DPlatChunkHw3NewERPS_mij(void**, unsigned long, int, unsigned);
extern "C" int  __wrap__ZN12DPlatChunkHw3NewERPS_mij(void** c, unsigned long pa, int sz, unsigned attr)
	{ ekseam_puts("[fb]    DPlatChunkHw::New phys="); ekseam_puthex((unsigned)pa);
	  int r = __real__ZN12DPlatChunkHw3NewERPS_mij(c, pa, sz, attr);
	  ekseam_puts("[fb]    DPlatChunkHw::New done r="); ekseam_puthex((unsigned)r); return r; }
extern "C" void __real__ZN16DLcdPowerHandler9DisplayOnEv(void*);
extern "C" void __wrap__ZN16DLcdPowerHandler9DisplayOnEv(void* t)
	{ ekseam_puts("[fb]  DLcdPowerHandler::DisplayOn...\n");
	  __real__ZN16DLcdPowerHandler9DisplayOnEv(t);
	  ekseam_puts("[fb]  DLcdPowerHandler::DisplayOn done\n"); }
extern "C" void __real__ZN13DPowerHandlerC2ERK6TDesC8(void*, const void*);
extern "C" void __wrap__ZN13DPowerHandlerC2ERK6TDesC8(void* t, const void* n)
	{ ekseam_puts("[fb]   DPowerHandler::DPowerHandler(base ctor)...\n");
	  __real__ZN13DPowerHandlerC2ERK6TDesC8(t, n);
	  ekseam_puts("[fb]   DPowerHandler::DPowerHandler done\n"); }

// --- M6 tail bring-up: the DECLARE_STANDARD_EXTENSION body continues past Create() with
// Kern::DfcQCreate(iDfcQ,29,&KLitLcd) (spawns a priority-29 supervisor DFC thread) then
// Kern::InstallPhysicalDevice(new DDisplayPddFactory) (registers the RDisplayChannel PDD).
// ThreadCreate/ThreadResume are called for EVERY thread during boot, so gate their markers on
// g_fb_tail (set only while we run the tail) to avoid flooding the log. --wrap wired in link_ekern.sh.
extern "C" volatile int g_fb_tail = 0;
extern "C" int  __real__ZN4Kern12ThreadCreateER17SThreadCreateInfo(void*);
extern "C" int  __wrap__ZN4Kern12ThreadCreateER17SThreadCreateInfo(void* info)
	{ if (g_fb_tail) ekseam_puts("[fb]    Kern::ThreadCreate...\n");
	  int r = __real__ZN4Kern12ThreadCreateER17SThreadCreateInfo(info);
	  if (g_fb_tail) { ekseam_puts("[fb]    Kern::ThreadCreate done r="); ekseam_puthex((unsigned)r); }
	  return r; }
extern "C" void __real__ZN4Kern12ThreadResumeER7DThread(void*);
extern "C" void __wrap__ZN4Kern12ThreadResumeER7DThread(void* t)
	{ if (g_fb_tail) ekseam_puts("[fb]    Kern::ThreadResume (schedules the new DFC thread)...\n");
	  __real__ZN4Kern12ThreadResumeER7DThread(t);
	  if (g_fb_tail) ekseam_puts("[fb]    Kern::ThreadResume returned (control came back to us)\n"); }
extern "C" int  __real__ZN4Kern10DfcQCreateERP7TDfcQueiPK6TDesC8(void*, int, const void*);
extern "C" int  __wrap__ZN4Kern10DfcQCreateERP7TDfcQueiPK6TDesC8(void* q, int pri, const void* nm)
	{ if (g_fb_tail) { ekseam_puts("[fb]   Kern::DfcQCreate pri="); ekseam_puthex((unsigned)pri); }
	  int r = __real__ZN4Kern10DfcQCreateERP7TDfcQueiPK6TDesC8(q, pri, nm);
	  if (g_fb_tail) { ekseam_puts("[fb]   Kern::DfcQCreate done r="); ekseam_puthex((unsigned)r); }
	  return r; }
extern "C" int  __real__ZN4Kern21InstallPhysicalDeviceEP15DPhysicalDevice(void*);
extern "C" int  __wrap__ZN4Kern21InstallPhysicalDeviceEP15DPhysicalDevice(void* d)
	{ ekseam_puts("[fb]   Kern::InstallPhysicalDevice...\n");
	  int r = __real__ZN4Kern21InstallPhysicalDeviceEP15DPhysicalDevice(d);
	  ekseam_puts("[fb]   Kern::InstallPhysicalDevice done r="); ekseam_puthex((unsigned)r);
	  return r; }
extern "C" int  __real__ZN4Kern20InstallLogicalDeviceEP14DLogicalDevice(void*);
extern "C" int  __wrap__ZN4Kern20InstallLogicalDeviceEP14DLogicalDevice(void* d)
	{ ekseam_puts("[fb]   Kern::InstallLogicalDevice...\n");
	  int r = __real__ZN4Kern20InstallLogicalDeviceEP14DLogicalDevice(d);
	  ekseam_puts("[fb]   Kern::InstallLogicalDevice done r="); ekseam_puthex((unsigned)r);
	  return r; }

// The generic display LDD's DECLARE_STANDARD_LDD entry (display.cpp) -> DLogicalDevice factory.
class DLogicalDevice;
DLogicalDevice* CreateLogicalDevice();
extern "C" DPhysicalDevice* ekseam_display_pdd_factory() __attribute__((weak));

extern "C" void ekseam_load_fb_driver()
	{
	ekseam_puts("\n[EKERN][fb] === loading REAL svpframebuffer driver ===\n");
	// Inlined equivalent of the essential DECLARE_STANDARD_EXTENSION body (new + Create), with
	// raw-UART markers between steps so we can see where init progresses. Create() does the real
	// work: AllocateFrameBuffer (AllocPhysicalRam + DPlatChunkHw::New), AddHalEntry, DisplayOn ->
	// PowerUpLcd (CLCD programming). The DfcQCreate/InstallPhysicalDevice tail of InitExtension is
	// only needed for the RDisplayChannel LDD (WSERV path, M6), not to light the panel.
	ekseam_puts("[fb] operator new DLcdPowerHandler...\n");
	DLcdPowerHandler* h = new DLcdPowerHandler;
	ekseam_puts("[fb] constructed h="); ekseam_puthex((unsigned)(unsigned long)h);
	if (!h)
		{ ekseam_puts("[EKERN][fb] alloc failed -- out of memory\n"); return; }
	ekseam_puts("[fb] calling Create()...\n");
	TInt r = h->Create();
	ekseam_puts("[EKERN][fb] Create r="); ekseam_puthex((unsigned)r);
	if (r != KErrNone)
		{ ekseam_puts("[EKERN][fb] Create failed -- panel not up\n"); return; }

	TUint32* fb = (TUint32*)h->iVideoInfo.iVideoAddress;   // kernel linear addr of video RAM
	TInt w  = h->iVideoInfo.iSizeInPixels.iWidth;
	TInt ht = h->iVideoInfo.iSizeInPixels.iHeight;
	ekseam_puts("[EKERN][fb] driver video VA="); ekseam_puthex((unsigned)(unsigned long)fb);
	ekseam_puts("[EKERN][fb]        WIDTH=");     ekseam_puthex((unsigned)w);
	ekseam_puts("[EKERN][fb]        HEIGHT=");    ekseam_puthex((unsigned)ht);
	if (!fb || w <= 0 || ht <= 0)
		{ ekseam_puts("[EKERN][fb] bad video geometry -- skipping paint\n"); return; }

	// R / G / B vertical thirds + a white border: identical proof pattern to the M5 poke,
	// but now drawn into the driver-owned, driver-mapped, driver-scanned framebuffer.
	for (TInt y = 0; y < ht; y++)
		for (TInt x = 0; x < w; x++)
			{
			TUint32 c = (x < w/3) ? 0x00FF0000u : (x < 2*w/3) ? 0x0000FF00u : 0x000000FFu;
			if (x < 2 || y < 2 || x >= w-2 || y >= ht-2) c = 0x00FFFFFFu;
			fb[y*w + x] = c;
			}
	ekseam_puts("[EKERN][fb] pattern painted into driver video RAM -- display now driven by svpframebuffer.cpp\n");

	// --- M6 tail: register the RDisplayChannel PDD so WSERV's screen driver can Open() it. This is
	// the rest of DECLARE_STANDARD_EXTENSION, run AFTER the paint so a hang here never costs us the
	// known-good M5.5 display. Step 1: create the priority-29 DFC thread the LDD/PDD run their DFCs
	// on. Step 2: register the PDD factory. Traced via the gated --wrap markers above.
	ekseam_puts("\n[EKERN][fb] === M6 tail: DfcQCreate + InstallPhysicalDevice ===\n");
	g_fb_tail = 1;
	ekseam_puts("[fb] Kern::DfcQCreate(iDfcQ, 29, KLitLcd)...\n");
	TInt dr = Kern::DfcQCreate(h->iDfcQ, 29, &KLitLcd);
	ekseam_puts("[EKERN][fb] DfcQCreate r="); ekseam_puthex((unsigned)dr);
	ekseam_puts("[EKERN][fb] iDfcQ="); ekseam_puthex((unsigned)(unsigned long)h->iDfcQ);
	if (dr != KErrNone)
		{ ekseam_puts("[EKERN][fb] DfcQCreate failed -- no PDD\n"); g_fb_tail = 0; return; }
	// Create() above bound its power DFCs + message queue to iDfcQ while it was still NULL (in the real
	// InitExtension the queue exists first): the first power transition -- SSM's shutdown -> DPowerManager::
	// PowerDown -> DLcdPowerHandler::PowerDown -> iPowerDownDfc.Enque() -- faulted the kernel in TDfc::DoEnque
	// (PinePhone, 2026-10-07). Rebind them to the queue now that it exists.
	h->iPowerUpDfc.SetDfcQ(h->iDfcQ);
	h->iPowerDownDfc.SetDfcQ(h->iDfcQ);
	h->iMsgQ.SetDfcQ(h->iDfcQ);

	// a board with its own RDisplayChannel PDD (a64dispchan.cpp: vblank-paced posts) supplies the factory
	ekseam_puts("[fb] new DDisplayPddFactory...\n");
	DPhysicalDevice* dev = ekseam_display_pdd_factory ? ekseam_display_pdd_factory() : new DDisplayPddFactory;
	ekseam_puts("[EKERN][fb] factory="); ekseam_puthex((unsigned)(unsigned long)dev);
	if (!dev)
		{ ekseam_puts("[EKERN][fb] factory alloc failed\n"); g_fb_tail = 0; return; }

	ekseam_puts("[fb] Kern::InstallPhysicalDevice(factory)...\n");
	TInt ir = Kern::InstallPhysicalDevice(dev);
	ekseam_puts("[EKERN][fb] InstallPhysicalDevice r="); ekseam_puthex((unsigned)ir);
	if (ir != KErrNone)
		{ ekseam_puts("[EKERN][fb] InstallPhysicalDevice failed\n"); g_fb_tail = 0; return; }

	// Step 3: install the generic RDisplayChannel LOGICAL device (display.cpp DECLARE_STANDARD_LDD).
	// With both LDD and PDD registered in the kernel object index, RDisplayChannel::Open() can bind
	// a channel -- the kernel-side prerequisite for WSERV's screen driver.
	ekseam_puts("[fb] CreateLogicalDevice() (display LDD factory)...\n");
	DLogicalDevice* ldd = CreateLogicalDevice();
	ekseam_puts("[EKERN][fb] ldd factory="); ekseam_puthex((unsigned)(unsigned long)ldd);
	if (!ldd)
		{ ekseam_puts("[EKERN][fb] LDD factory alloc failed\n"); g_fb_tail = 0; return; }
	ekseam_puts("[fb] Kern::InstallLogicalDevice(ldd)...\n");
	TInt lr = Kern::InstallLogicalDevice(ldd);
	ekseam_puts("[EKERN][fb] InstallLogicalDevice r="); ekseam_puthex((unsigned)lr);

	g_fb_tail = 0;
	ekseam_puts("[EKERN][fb] === M6 tail complete -- RDisplayChannel LDD+PDD registered ===\n");
	}
