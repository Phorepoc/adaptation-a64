// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// a64wlan.cpp -- OURS (2026-10-10, PinePhone WiFi: the chip core; "WiFi: the chip
// core"): the RTL8723CS's WiFi half brought up by our fresh RTL8703B driver (wlan/rtl8703b,
// ) over our SMHC1 SDIO host (a64sdio.cpp). This file is the Symbian side:
//   - the WLAN DFC queue, the one thread the whole driver runs on (shim_rt.cpp);
//   - the firmware: rtw8703b_fw.bin read straight out of the XIP ROM (Z:\resource\wlan\, staged by tools/zdata_gen.py
//     from src/phorepoc-arm/board/wlan/fw/) and handed to the driver (a64wlan_set_firmware);
//   - the probe (power-on check, the efuse);
//   - A64_WLAN_TEST (A64DEFS): the W2 demo -- power on (MAC, 8051 firmware download, MAC/PHY init), the MAC address
//     from the efuse, channel 1 .. 13 in turn with the receive filter opened to every beacon, the SDIO interrupt polled
//     every 10 ms, the driver's counters logged.
// Without A64_WLAN_TEST nothing runs yet: the WHA front (W3) will own the start.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <nkern.h>
#include <e32rom.h>
extern "C" {
#include "shim/shim_rt.h"
}

#include "a64wlan_api.h"					// rtl8703b/r8703b_api.c: the probe, core up, channel, the polled interrupt

extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(TUint32);
extern "C" TBool A64SdioHostInit();		// a64sdio.cpp: SMHC1 up, the card identified, function 1 ready, 4-bit 50 MHz HS
extern "C" void A64WlanPddInstall();		// a64wlanpdd.cpp: "wlan.phys", the WHA PDD under the real wlanldd
DLogicalDevice* CreateLogicalDevice_wlanldd();	// the REAL wlanldd's DECLARE_STANDARD_LDD (WLanLogicalDevice.cpp, renamed by -D)

static TDfcQue* WlanDfcQ;
static const TUint8 KRomAttDir = 0x10;		// TRomEntry::iAtt: KEntryAttDir (e32const.h)
extern "C" TDfcQue* A64WlanDfcQ() { return WlanDfcQ; }

// ---- the firmware out of the ROM: walk the root directory for resource\wlan\<name> (TRomDir / TRomEntry, UTF-16 names)
static TBool NameIs(const TRomEntry* aEntry, const char* aAscii)
	{
	TInt n = 0; while (aAscii[n]) ++n;
	if (aEntry->iNameLength != n) return EFalse;
	const TUint16* u = (const TUint16*)aEntry->iName;
	for (TInt i = 0; i < n; ++i)
		{
		TUint16 c = u[i]; char a = aAscii[i];
		if (c >= 'A' && c <= 'Z') c = (TUint16)(c + 32);
		if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
		if (c != (TUint16)(TUint8)a) return EFalse;
		}
	return ETrue;
	}
static const TRomEntry* FindIn(const TRomDir* aDir, const char* aName)
	{
	const TUint8* p = (const TUint8*)&aDir->iEntry;
	const TUint8* end = p + aDir->iSize;
	while (p < end)
		{
		const TRomEntry* e = (const TRomEntry*)p;
		if (NameIs(e, aName)) return e;
		p += (sizeof(TRomEntry) - 2 + e->iNameLength * 2 + 3) & ~3u;
		}
	return 0;
	}
static const TUint8* RomFile(const char* const* aPath, TInt aDepth, TInt& aSize)
	{
	const TRomHeader& rh = Epoc::RomHeader();
	const TRomRootDirectoryList* roots = (const TRomRootDirectoryList*)rh.iRomRootDirectoryList;
	if (!roots || roots->iNumRootDirs < 1) return 0;
	const TRomDir* dir = (const TRomDir*)roots->iRootDir[0].iAddressLin;
	for (TInt i = 0; i < aDepth; ++i)
		{
		const TRomEntry* e = FindIn(dir, aPath[i]);
		if (!e) { ekseam_puts("[wlan] ROM: no "); ekseam_puts(aPath[i]); ekseam_puts("\n"); return 0; }
		if (i == aDepth - 1) { aSize = e->iSize; return (const TUint8*)e->iAddressLin; }
		if (!(e->iAtt & KRomAttDir)) return 0;
		dir = (const TRomDir*)e->iAddressLin;
		}
	return 0;
	}

// ---- the demo, as DFC steps on the WLAN thread
static TInt DemoChannel = 1;
static NTimer* PollTimer;
static NTimer* HopTimer;
static TInt PollCount;
static void PollDfc(TAny*)
	{
	a64wlan_irq_poll();
#ifdef A64_WLAN_TEST
	if (++PollCount % 500 == 0) a64wlan_demo_report();			// every 5 s
#endif
	PollTimer->OneShot(NKern::TimerTicks(10));
	}
static void HopDfc(TAny*)
	{
	DemoChannel = DemoChannel >= 13 ? 1 : DemoChannel + 1;
	a64wlan_set_channel(DemoChannel);
	ekseam_puts("[wlan] channel "); ekseam_puthex((TUint32)DemoChannel);
	ekseam_puts(" -- rx frames so far "); ekseam_puthex(a64wlan_rx_frames());
	HopTimer->OneShot(NKern::TimerTicks(3000));
	}
static void StartDfc(TAny*)
	{
	ekseam_puts("[wlan] === RTL8723CS WiFi chip core (rtl8703b over SMHC1) ===\n");
		{
		const char* const path[] = { "resource", "wlan", "rtw8703b_fw.bin" };
		TInt size = 0;
		const TUint8* fw = RomFile(path, 3, size);
		ekseam_puts("[wlan] firmware Z:\\resource\\wlan\\rtw8703b_fw.bin size="); ekseam_puthex((TUint32)size);
		if (fw && size > 0)
			a64wlan_set_firmware(fw, (unsigned)size);
		}
#ifdef A64_WLAN_RTW88
	// WIFI_DRV=rtw88: rtw88 waits for the WoWLAN image too before it powers on (rtw_wait_firmware_completion)
		{
		const char* const path[] = { "resource", "wlan", "rtw8703b_wow_fw.bin" };
		TInt size = 0;
		const TUint8* fw = RomFile(path, 3, size);
		ekseam_puts("[wlan] firmware Z:\\resource\\wlan\\rtw8703b_wow_fw.bin size="); ekseam_puthex((TUint32)size);
		if (fw && size > 0)
			a64wlan_set_wow_firmware(fw, (unsigned)size);
		}
#endif
	if (!A64SdioHostInit())
		{
		ekseam_puts("[wlan] SDIO host init FAILED -- stop\n");
		return;
		}
	int r = a64wlan_probe();
	ekseam_puts("[wlan] probe r="); ekseam_puthex((TUint32)r);
	if (r)
		return;
	// the polled SDIO interrupt runs from here on (a no-op until the core is up: the PDD's Initialize, or the demo)
	PollTimer = new NTimer(PollDfc, 0, WlanDfcQ, 3);
	if (PollTimer) PollTimer->OneShot(NKern::TimerTicks(10));
#ifdef A64_WLAN_TEST
	r = a64wlan_core_up();
	ekseam_puts("[wlan] demo start (power on, firmware, mac/phy init) r="); ekseam_puthex((TUint32)r);
	if (r)
		return;
	a64wlan_set_channel(DemoChannel);
	HopTimer = new NTimer(HopDfc, 0, WlanDfcQ, 3);
	if (HopTimer) HopTimer->OneShot(NKern::TimerTicks(3000));
#endif
	}

// a64wlan_tx (any thread) -> the queued frames go into the chip on the WLAN thread
static TDfc* TxDfcObj;
static void TxDfc(TAny*) { a64wlan_tx_flush(); }
extern "C" void a64wlan_kick_tx() { if (TxDfcObj) TxDfcObj->Enque(); }

_LIT(KWlanDfc, "A64WlanDfc");
static TDfc* StartDfcObj;
static NTimer* KickTimer;
extern "C" void A64WlanInstall()
	{
#ifdef A64_SDIO_TEST
	return;		// the rung-0 probe owns the host in that build
#endif
	TDynamicDfcQue* q = 0;
	if (Kern::DynamicDfcQCreate(q, 27, KWlanDfc) != KErrNone)
		{
		ekseam_puts("[wlan] no DFC queue\n");
		return;
		}
	WlanDfcQ = q;
	// the REAL wlanldd ("wlan", the UMAC) + our WHA PDD ("wlan.phys"), as a phone ROM's wlanldd.ldd + wlanpdd.pdd:
	// wlmserver loads them by file name (the loader delta answers KErrAlreadyExists: toolchain/efile_exe/
	// ldr_builtin_devices.cpp) and opens RWlanLogicalChannel on the pair
	NKern::ThreadEnterCS();
	DLogicalDevice* ld = CreateLogicalDevice_wlanldd();
	TInt r = ld ? Kern::InstallLogicalDevice(ld) : KErrNoMemory;
	ekseam_puts("[wlan] InstallLogicalDevice(wlan) r="); ekseam_puthex((TUint32)r);
	// the engine frees both devices after every idle period (CWlmDriverIf::UnloadDrivers) and loads them again by file
	// name on the next scan: a built-in device has no file, so it must survive the free -- one extra reference makes
	// ExecHandler::DeviceFree close the loader's "extra reference" without deleting the factory (boot 228: PDD -1)
	if (r == KErrNone) ld->Open();
	A64WlanPddInstall();
	NKern::ThreadLeaveCS();
	StartDfcObj = new TDfc(StartDfc, 0, WlanDfcQ, 3);
	TxDfcObj = new TDfc(TxDfc, 0, WlanDfcQ, 3);
	// after the boot noise: 8 s in (the Bluetooth PDD and the display are up by then; the chip is shared with BT)
	KickTimer = new NTimer(StartDfc, 0, WlanDfcQ, 3);
	if (KickTimer) KickTimer->OneShot(NKern::TimerTicks(8000));
	}
