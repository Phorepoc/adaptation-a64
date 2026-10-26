// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// OURS (PinePhone track): the seam's kext loaders + boot-stage signal for
// BOARD=a64 (tools/kernel/ekern_seam.cpp; virt's are src/phorepoc-arm/virt/specific/virt_glue_null.cpp).
// No A64 display / input / storage drivers yet (P4), so:
//   display: none;  input: none;  media: the REAL ELOCD + the internal RAM drive as C: (a64/estart.txt).
// ON HARDWARE (2026-10-05): the boot CPU reaches stage 4 (white) on the phone. Without a serial cable the PinePhone's RGB LED and vibrator are the console: the
// bootstrap shows red (stage 1) and yellow (stage 2) (a64/bootstrap/a64.s STAGE_LED); this file shows
//   3 = green: the kernel is up (kernel server starting)
//       If the generic timer does not count here, red/green alternate forever (every NanoWait would hang).
//   5 = cyan (green + blue): media drivers loaded (C: = the RAM drive)
//   4 = white: the first user-mode thread is entered
//   (blue = boot.cmd's, just before it jumps to us: a steady blue = our code never took over the LED)
//   9 = magenta (red + blue): a kernel fault
// No buzz: the vibrator is powered from an AXP803 regulator we do not switch on yet (Tow-Boot does), and the buzz's
// 300 ms NanoWait was the one thing between green and the next stage.
// The log: raw SD sectors 4096.. written by the kernel itself (A64SdFlush below).
// LED / vibrator pins from the Linux DTS (sun50i-a64-pinephone.dtsi): PD18 green, PD19 red, PD20 blue, PD2 vibrator.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include "syborg_priv.h"
#include <kernel/kpower.h>
#include <e32keys.h>
extern "C" void A64CpuInputBoost();	// power/a64_cpufreq.cpp: input -> the top allowed CPU clock for 1 s

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);
TInt InitExtension_locmedia();
DLogicalDevice* CreateLogicalDevice_locmedia();
TInt InitExtension_medint();
DPhysicalDevice* CreatePhysicalDevice_medint();
TInt InitExtension_mednvme();
DPhysicalDevice* CreatePhysicalDevice_mednvme();
TInt InitExtension_mednvmeE();
DPhysicalDevice* CreatePhysicalDevice_mednvmeE();
TInt InitExtension_usbcc();
extern "C" TInt PhLinkStart();
extern "C" void A64DisplayStart();
extern "C" void A64DisplayFlipStats();		// a64_display.cpp: the NGA flip counters for the [mon] line
extern "C" void A64MaliInstall();
extern "C" void A64VdeInstall();
extern "C" void A64DeOvInstall();		// display/a64deov.cpp: the DE2 video overlay plane (RPheDeOv)
extern "C" TInt A64ThsInit();				// power/a64_ths.cpp
extern "C" TBool A64ThsTick();				// ... every 5 s: logs "[EKERN][power] temp", ETrue = critical (>= 110 C twice)
extern "C" TInt A64CpuFreqInit();
extern "C" void A64CpuOppSelfTest();
extern "C" TInt A64CpuGovernorStart();			// power/a64_cpufreq.cpp (maps the CCU; no frequency change)
#ifdef __SMP__
extern "C" TInt A64IdleStart();				// power/a64_idle.cpp (power plan P5: idle tick suppression + core retirement)
#endif
extern "C" void A64SoundInstall();			// sound/a64sound.cpp: the codec PDD under the real sound LDD
extern "C" void A64KeysInstall();			// input/a64keys.cpp: the volume buttons (LRADC)
extern "C" void A64UartInstall();			// bt/a64uart.cpp: the real serial LDD + the UART1 PDD (Bluetooth)
extern "C" void A64SdioInstall();			// wlan/a64sdio.cpp: the SMHC1 SDIO host (WiFi); A64_SDIO_TEST = the probe
extern "C" void A64WlanInstall();			// wlan/a64wlan.cpp: the RTL8723CS WiFi chip core (rtw88); A64_WLAN_TEST = the beacon demo
extern "C" void A64SensorsInstall();		// sensors/a64twi.cpp: the TWI1 sensor bus; A64_SENSOR_TEST = the identity probe
extern "C" void A64CameraInstall();		// camera/a64camera.cpp: the 2010 camera LDD + template PDD (C1); the C2 sensor chip-ID probe
extern "C" void ekseam_load_nga_drivers() __attribute__((weak));

// PIO port D (A64 manual: port n at PIO + n*0x24; CFG0..3 = 4 bits per pin, 1 = output; DAT at +0x10)
const TUint KPioPortD = KHwBaseCcuPio + 0x800 + 3 * 0x24;
const TUint KPioCfg2 = 0x08, KPioDat = 0x10;
const TUint32 KLedGreen = 1u << 18, KLedRed = 1u << 19, KLedBlue = 1u << 20;

static inline volatile TUint32& PioD(TUint aOffset) { return *(volatile TUint32*)(KPioPortD + aOffset); }

static void A64Leds(TUint32 aOn)
	{
	PioD(KPioCfg2) = (PioD(KPioCfg2) & ~0x000FFF00u) | 0x00011100u;		// PD18-20 = output
	PioD(KPioDat) = (PioD(KPioDat) & ~(KLedGreen | KLedRed | KLedBlue)) | aOn;
	}

// The generic timer must count for every NanoWait / tick: spin a while; a frozen CNTPCT -> red/green blink forever.
static void A64CheckTimer()
	{
	TUint32 lo, hi;
	asm volatile("isb; mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
	const TUint64 start = ((TUint64)hi << 32) | lo;
	for (volatile TUint i = 0; i < 20000000u; ++i) {}
	asm volatile("isb; mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
	if ((((TUint64)hi << 32) | lo) != start)
		return;
	for (;;)
		{
		A64Leds(KLedRed);
		for (volatile TUint i = 0; i < 20000000u; ++i) {}
		A64Leds(KLedGreen);
		for (volatile TUint i = 0; i < 20000000u; ++i) {}
		}
	}

// THE SD LOG (no serial cable; user idea 2026-10-05): every seam trace byte goes to a static buffer, and the kernel
// writes that buffer to RAW sectors of the SD card -- straight through the A64's SD controller (SMHC0, 0x01C0F000)
// as U-Boot left it: controller clocked and set up, card initialised and selected (transfer state), 4-bit bus. No
// driver: one CMD24 (WRITE_BLOCK) per sector, PIO through the FIFO (register map: Linux sunxi-mmc.c, U-Boot
// sunxi_mmc.c). Sectors 4096.. (2 MiB: free space between U-Boot and partition 1 of our image; 4096 is a BLOCK
// number = an SDHC/SDXC card -- an old byte-addressed SDSC card would put it inside U-Boot). Sector 4096 = a text
// header line, 4097.. = the log. Read it from Mobian:
//   sudo dd if=/dev/mmcblk0 bs=512 skip=4096 count=130 2>/dev/null | tr -d '\000'
// Written at each boot stage, on a kernel fault, and every 5 s after the first user thread (for the whole run). Any SD error
// stops all further writes (SdLogDead).
// 256 KB RING (2026-10-06: a fixed buffer filled up during the S60 boot and dropped everything after): LogLen counts
// every byte ever logged, byte i lives at LogBuf[i % size]. The USB link (src/phorepoc-arm/a64/usbcc/phlink.cpp)
// streams it and skips ahead if it falls a whole ring behind; the SD log takes the LAST 128 KB.
static char LogBuf[0x40000];
const TUint KLogMask = sizeof(LogBuf) - 1;
static volatile TUint LogLen;
const TUint KSdLogMax = 0x20000;

extern "C" const char* ekseam_board_log_buf(unsigned* aLen)
	{
	*aLen = LogLen;
	return LogBuf;
	}

// User-side RDebug / Kern::Printf through Syborg::DebugOutput into the board log: diagnosis only (SSM traces);
// build with A64DEFS=-DA64_RDEBUG_LOG. Off, a chatty component cannot throttle the UI through the USB log.
#ifdef A64_RDEBUG_LOG
extern "C" TBool A64UserDebugLog = ETrue;
#else
extern "C" TBool A64UserDebugLog = EFalse;
#endif

#ifdef A64_RTC_TAIL
// A64DEFS=-DA64_RTC_TAIL (2026-10-09, the NGA boot power cut: DRAM is lost, the SD log ends up to 250 ms early): the
// last 28 log characters also go to the RTC's general-purpose registers (0x01F00100.., battery-backed, survive the
// power cut): words 0-6 = a 28-byte ring, word 7 = LogLen. Read them with PhoneLab peek after the next slot A boot.
extern TUint32 ph_a64_r;
static TUint32 RtcTail[7];
static volatile TBool RtcTailFrozen;	// stage 9 (a fault): keep the fault's own last line
#endif
extern "C" void ekseam_board_log(unsigned c)
	{
	const TUint n = LogLen;
	LogBuf[n & KLogMask] = (char)c;
	LogLen = n + 1;
#ifdef A64_RTC_TAIL
	if (ph_a64_r && !RtcTailFrozen)
		{
		static TBool printed;
		if (!printed)
			{
			// the PREVIOUS boot's tail, before this boot's log overwrites it (the golden: a slot B fault's last words)
			printed = ETrue;
			volatile TUint32* old = (volatile TUint32*)(ph_a64_r + 0x100);
			TUint32 words[8];
			for (TInt i = 0; i < 8; ++i)
				words[i] = old[i];
			const TUint start = words[7] % 28;
			char line[30];
			for (TUint i = 0; i < 28; ++i)
				{
				const TUint p = (start + i) % 28;
				const char ch = (char)(words[p / 4] >> ((p % 4) * 8));
				line[i] = ch >= 32 && ch < 127 ? ch : (ch == '\n' ? '|' : '.');
				}
			line[28] = 0;
			ekseam_puts("[EKERN][rtc] previous boot's last log chars: ");
			ekseam_puts(line);
			ekseam_puts("\n");
			}
		const TUint p = n % 28, w = p / 4, sh = (p % 4) * 8;
		RtcTail[w] = (RtcTail[w] & ~(0xFFu << sh)) | ((c & 0xFFu) << sh);
		volatile TUint32* gp = (volatile TUint32*)(ph_a64_r + 0x100);
		gp[w] = RtcTail[w];
		gp[7] = n + 1;
		}
#endif
	}

const TPhysAddr KSmhc0Phys = 0x01C0F000u;
const TUint KSdLogSector = 4096;		// slot A's log; slot B's = KSdLogSectorB (so a fallback boot of A never overwrites
const TUint KSdLogSectorB = 4608;		// the log of the B trial that just died; both < the control sector 6144)
static TUint SdLogBase = KSdLogSector;
enum { KSdGctl = 0x00, KSdBlkSz = 0x10, KSdBytCnt = 0x14, KSdCmd = 0x18, KSdArg = 0x1C, KSdRint = 0x38,
	   KSdStatus = 0x3C, KSdFifo = 0x200 };
// KSdRintErrors = the sunxi-mmc error mask (resp error/CRC/timeout, data CRC/timeout, FIFO run, HW locked, start/end bit) + bit 10, the
// host data-starvation timeout: a stalled transfer raises one of these once the TMOUT below has run out
const TUint32 KSdRintCmdDone = 1u << 2, KSdRintDataOver = 1u << 3, KSdRintErrors = 0xBBC2u | (1u << 10);
const TUint32 KSdStatusFifoFull = 1u << 3, KSdStatusCardBusy = 1u << 9;
static volatile TUint32* Smhc;
static DPlatChunkHw* SmhcChunk;
static TBool SdLogDead;
static volatile TBool SdLogBusy;
static TUint SdFlushes;

static inline volatile TUint32& Sd(TUint aOffset) { return Smhc[aOffset >> 2]; }

// SMHC TMOUT (+0x08): data timeout in bits 31:8 (card clocks), response timeout in bits 7:0. Left at U-Boot's maximum a card that
// stalls mid-sector never raised DRTO (boot 1371: rint=0x14 and only our software timeout ended it); a finite value makes the
// controller flag the stall (sunxi-mmc programs the register at host init and after every reset). 0x800000 clocks = ~168 ms at 50 MHz,
// well above a card's read latency (SD spec: 100 ms), so only a real stall trips it.
const TUint KSdTmout = 0x08;
const TUint32 KSdTmoutValue = (0x800000u << 8) | 0x40u;
static inline void SdSetTimeouts() { Sd(KSdTmout) = KSdTmoutValue; }
const TUint KSdWaitMs = 2000;			// the longest a status wait runs (a card's write busy is <= 500 ms); the iteration cap is for stopped ticks

static TBool SdWaitClear(TUint aOffset, TUint32 aMask)
	{
	const TUint32 t0 = NKern::TickCount();
	for (TUint i = 0; i < 50000000u; ++i)
		{
		if (!(Sd(aOffset) & aMask))
			return ETrue;
		if (!(i & 0xFFFF) && NKern::TickCount() - t0 > KSdWaitMs)
			break;
		}
	return EFalse;
	}

// A FIFO-status wait that also ends as soon as the controller flags an error/timeout in RINT (a stalled transfer), or after
// KSdWaitMs: used by the PIO loops, which used to spin their full iteration count on a card that had stopped answering.
static TBool SdWaitFifo(TUint32 aMask)
	{
	const TUint32 t0 = NKern::TickCount();
	for (TUint i = 0; i < 50000000u; ++i)
		{
		if (!(Sd(KSdStatus) & aMask))
			return ETrue;
		if (!(i & 15) && (Sd(KSdRint) & KSdRintErrors))
			break;
		if (!(i & 0xFFFF) && NKern::TickCount() - t0 > KSdWaitMs)
			break;
		}
	return EFalse;
	}

// Wait for the end of a transfer in RINT: an error/timeout bit, or the wanted bits (all of them, or any). Returns the last RINT.
static TUint32 SdWaitRint(TUint32 aWant, TBool aAll)
	{
	const TUint32 t0 = NKern::TickCount();
	TUint32 r = 0;
	for (TUint i = 0; i < 50000000u; ++i)
		{
		r = Sd(KSdRint);
		if ((r & KSdRintErrors) || (aAll ? (r & aWant) == aWant : (r & aWant) != 0))
			break;
		if (!(i & 0xFFFF) && NKern::TickCount() - t0 > KSdWaitMs)
			break;
		}
	return r;
	}

static TBool SdWriteSector(TUint aSector, const TUint32* aData)
	{
	if (!SdWaitClear(KSdStatus, KSdStatusCardBusy))
		return EFalse;
	Sd(KSdRint) = 0xFFFFFFFFu;
	Sd(KSdGctl) |= 1u << 31;													// FIFO access by the CPU (AHB), as U-Boot's PIO
	Sd(KSdBlkSz) = 512;
	Sd(KSdBytCnt) = 512;
	Sd(KSdArg) = aSector;
	// START | WAIT_PRE_OVER | WRITE | DATA_EXPIRE | CHECK_RESPONSE_CRC | RESP_EXPIRE | CMD24
	Sd(KSdCmd) = (1u << 31) | (1u << 13) | (1u << 10) | (1u << 9) | (1u << 8) | (1u << 6) | 24;
	for (TUint w = 0; w < 128; ++w)
		{
		if (!SdWaitFifo(KSdStatusFifoFull))
			return EFalse;
		Sd(KSdFifo) = aData[w];
		}
	const TUint32 r = SdWaitRint(KSdRintCmdDone | KSdRintDataOver, ETrue);
	Sd(KSdRint) = 0xFFFFFFFFu;
	if ((r & KSdRintErrors) || !(r & KSdRintDataOver))
		return EFalse;
	return SdWaitClear(KSdStatus, KSdStatusCardBusy);
	}

// CMD17 (READ_SINGLE_BLOCK), PIO from the FIFO (STATUS bit 2 = FIFO empty)
const TUint32 KSdStatusFifoEmpty = 1u << 2;
static TBool SdReadSector(TUint aSector, TUint32* aData)
	{
	if (!SdWaitClear(KSdStatus, KSdStatusCardBusy))
		return EFalse;
	Sd(KSdRint) = 0xFFFFFFFFu;
	Sd(KSdGctl) |= 1u << 31;
	Sd(KSdBlkSz) = 512;
	Sd(KSdBytCnt) = 512;
	Sd(KSdArg) = aSector;
	// START | WAIT_PRE_OVER | DATA_EXPIRE | CHECK_RESPONSE_CRC | RESP_EXPIRE | CMD17
	Sd(KSdCmd) = (1u << 31) | (1u << 13) | (1u << 9) | (1u << 8) | (1u << 6) | 17;
	for (TUint w = 0; w < 128; ++w)
		{
		if (!SdWaitFifo(KSdStatusFifoEmpty))
			return EFalse;
		aData[w] = Sd(KSdFifo);
		}
	const TUint32 r = SdWaitRint(KSdRintDataOver, EFalse);
	Sd(KSdRint) = 0xFFFFFFFFu;
	return !(r & KSdRintErrors) && (r & KSdRintDataOver);
	}

// THE CARD'S SIZE (2026-10-08, for E: = P4 grown to the end of the card): we run on U-Boot's card set-up and never read
// the CSD. (A first try probed with CMD17 past the end: the card did not fail fast and the E: thread spun for minutes.)
// So, as an SD host does at enumeration, under the SD lock: CMD7(0) = deselect (-> stand-by), CMD3 = a new RCA (R6,
// valid in stand-by), CMD9(RCA) = the CSD (R2, 136 bits), CMD7(RCA) = select again (R1b). Bus width + clock stay as
// U-Boot set them. CSD v2 (SDHC/SDXC): C_SIZE = bits 69:48, capacity = (C_SIZE + 1) * 1024 sectors. The SMHC's long
// response: RESP0 = bits 31:0 .. RESP3 = bits 127:96 (Linux sunxi-mmc reverses them into the core's resp[0..3]).
const TUint KSdResp0 = 0x20;
static const char KNl[] = { 10, 0 };
const TUint32 KSdCmdStart = 1u << 31, KSdCmdWaitPre = 1u << 13, KSdCmdCrc = 1u << 8, KSdCmdLong = 1u << 7,
	KSdCmdResp = 1u << 6;
static TBool SdCommand(TUint32 aCmd, TUint32 aArg)
	{
	Sd(KSdRint) = 0xFFFFFFFFu;
	Sd(KSdArg) = aArg;
	Sd(KSdCmd) = KSdCmdStart | KSdCmdWaitPre | aCmd;
	TUint32 r = 0;
	for (TUint i = 0; i < 5000000u; ++i)
		{
		r = Sd(KSdRint);
		if (r & (KSdRintCmdDone | KSdRintErrors))
			break;
		}
	Sd(KSdRint) = 0xFFFFFFFFu;
	return (r & KSdRintCmdDone) && !(r & KSdRintErrors);
	}

static NFastMutex SdMutex;
static inline TBool SdLock();
static inline void SdUnlock(TBool aLocked);
// the number of sectors on the card (0 = could not tell)
extern "C" TUint A64SdCardSectors(TUint /*aFrom*/, TUint32* /*aBuf*/)
	{
	if (!Smhc)
		return 0;
	const TBool locked = SdLock();
	TUint sectors = 0;
	TUint32 csd[4] = { 0, 0, 0, 0 }, rca = 0;
	TBool ok = SdWaitClear(KSdStatus, KSdStatusCardBusy) && SdCommand(7, 0);		// deselect: no response
	if (ok)
		ok = SdCommand(KSdCmdResp | KSdCmdCrc | 3, 0);
	if (ok)
		{
		rca = Sd(KSdResp0) & 0xFFFF0000u;
		ok = rca != 0 && SdCommand(KSdCmdResp | KSdCmdLong | KSdCmdCrc | 9, rca);
		if (ok)
			for (TInt i = 0; i < 4; ++i)
				csd[i] = Sd(KSdResp0 + 4 * i);
		}
	// select again whatever happened above (rca 0 = U-Boot's card stays deselected: report that, nothing else to do)
	const TBool sel = rca && SdCommand(KSdCmdResp | KSdCmdCrc | 7, rca) && SdWaitClear(KSdStatus, KSdStatusCardBusy);
	SdUnlock(locked);
	ekseam_puts("[EKERN][sd] csd rca="); ekseam_puthex(rca); ekseam_puts(" selected="); ekseam_puthex(sel);
	ekseam_puts(" csd3..0="); ekseam_puthex(csd[3]); ekseam_puthex(csd[2]); ekseam_puthex(csd[1]); ekseam_puthex(csd[0]); ekseam_puts(KNl);
	if (ok && sel && (csd[3] >> 30) == 1)
		{
		const TUint csize = ((csd[2] & 0x3F) << 16) | (csd[1] >> 16);
		sectors = (csize + 1) * 1024;
		}
	return sectors;
	}

// one SD user at a time: the log flusher (DFC queue 0), the slot uploader (the USB DFC thread)
static inline TBool SdLock()
	{
	if (NKern::CurrentContext() != NKern::EThread)
		return EFalse;						// a fault (stage 9): just go
	NKern::FMWait(&SdMutex);
	return ETrue;
	}
static inline void SdUnlock(TBool aLocked) { if (aLocked) NKern::FMSignal(&SdMutex); }
// the SD lock for the SDIO probe's self-check on SMHC0 (wlan/a64sdio.cpp, A64_SDIO_TEST)
extern "C" TBool A64SdProbeLock() { return SdLock(); }
extern "C" void A64SdProbeUnlock(TBool aLocked) { SdUnlock(aLocked); }

// MULTI-BLOCK (2026-10-08, "the home screen freezes while the C: driver reads"): one CMD18 (READ_MULTIPLE_BLOCK) /
// CMD25 (WRITE_MULTIPLE_BLOCK) per run of sectors, the SMHC sending CMD12 itself at the end (SEND_AUTO_STOP, CMD bit
// 12; done = RINT bit 14, as Linux sunxi-mmc), instead of one command per 512-byte sector. Still PIO through the FIFO.
// On an error: FIFO reset (GCTL bit 1) and the caller falls back to single sectors.
const TUint KSdMultiMax = 256;							// 128 KB per command
const TUint32 KSdRintAutoCmdDone = 1u << 14;
static TUint SdMultiFails;
static volatile TUint SdStatCalls, SdStatSectors, SdStatMs;	// media-driver I/O since the last monitor line
static TBool SdMulti(TUint aSector, TUint aCount, TUint32* aBuf, TBool aWrite)
	{
	if (!SdWaitClear(KSdStatus, KSdStatusCardBusy))
		return EFalse;
	Sd(KSdRint) = 0xFFFFFFFFu;
	Sd(KSdGctl) |= 1u << 31;
	Sd(KSdBlkSz) = 512;
	Sd(KSdBytCnt) = aCount * 512;
	Sd(KSdArg) = aSector;
	// START | WAIT_PRE_OVER | SEND_AUTO_STOP | (WRITE) | DATA_EXPIRE | CHECK_RESPONSE_CRC | RESP_EXPIRE | CMD18/25
	Sd(KSdCmd) = (1u << 31) | (1u << 13) | (1u << 12) | (aWrite ? 1u << 10 : 0) | (1u << 9) | (1u << 8) | (1u << 6)
		| (aWrite ? 25 : 18);
	TBool ok = ETrue;
	const TUint words = aCount * 128;
	for (TUint w = 0; w < words && ok; ++w)
		{
		if (aWrite)
			{
			ok = SdWaitFifo(KSdStatusFifoFull);
			if (ok) Sd(KSdFifo) = aBuf[w];
			}
		else
			{
			ok = SdWaitFifo(KSdStatusFifoEmpty);
			if (ok) aBuf[w] = Sd(KSdFifo);
			}
		if (!(w & 127) && (Sd(KSdRint) & KSdRintErrors))
			ok = EFalse;
		}
	const TUint32 want = KSdRintDataOver | KSdRintAutoCmdDone;
	const TUint32 r = ok ? SdWaitRint(want, ETrue) : Sd(KSdRint);
	ok = ok && !(r & KSdRintErrors) && (r & want) == want;
	Sd(KSdRint) = 0xFFFFFFFFu;
	if (!ok)
		{
		Sd(KSdGctl) |= 1u << 1;							// FIFO reset
		SdWaitClear(KSdGctl, 1u << 1);
		Sd(KSdRint) = 0xFFFFFFFFu;
		return EFalse;
		}
	return SdWaitClear(KSdStatus, KSdStatusCardBusy);
	}

// SD DMA (2026-10-15): the SMHC's internal DMA (IDMAC), registers + descriptor format as Linux sunxi-mmc.c (read as a
// datasheet). The bus already runs at 50 MHz, 4 bits (CCU SDMMC0 0x8100000b, new timing mode, CLKDIV 0; boot 487) =
// 25 MB/s raw, but the PIO loop above polls the FIFO status per 32-bit word: ~6 MB/s. Here the controller moves the
// data itself. All transfers go through ONE physically contiguous, uncached bounce area (128 KB data + the descriptor
// chain behind it): no cache maintenance, any caller buffer works; the copy runs at memory speed. Any DMA error = the
// caller's PIO path. Reads first; writes switch on with KSdDmaWrites once the reads have proven out.
const TBool KSdDmaWrites = ETrue;
const TUint KSdDmaBytes = KSdMultiMax * 512;			// 128 KB = one CMD18/CMD25 run
const TUint KSdDmaDesBytes = 0x8000;					// 32 KB per descriptor (the size field holds 16 bits)
enum { KSdFtrgl = 0x40, KSdDmac = 0x80, KSdDlba = 0x84, KSdIdst = 0x88, KSdIdie = 0x8C };
const TUint32 KIdmaOwn = 1u << 31, KIdmaEr = 1u << 5, KIdmaCh = 1u << 4, KIdmaFd = 1u << 3, KIdmaLd = 1u << 2,
	KIdmaDic = 1u << 1;
struct SSdIdmaDes { TUint32 iConfig, iSize, iBuf, iNext; };
static DPlatChunkHw* SdDmaChunk;
static TUint8* SdDmaBuf;
static TPhysAddr SdDmaPhys;
static TUint SdDmaFails;

static void SdDmaInit()
	{
	const TInt size = (TInt)(KSdDmaBytes + 0x1000);
	TPhysAddr phys;
	NKern::ThreadEnterCS();
	TInt r = Epoc::AllocPhysicalRam(size, phys, 12);
	if (r == KErrNone)
		{
		r = DPlatChunkHw::New(SdDmaChunk, phys, size, EMapAttrSupRw | EMapAttrFullyBlocking);
		if (r != KErrNone)
			Epoc::FreePhysicalRam(phys, size);
		}
	NKern::ThreadLeaveCS();
	if (r == KErrNone)
		{
		SdDmaBuf = (TUint8*)SdDmaChunk->LinearAddress();
		SdDmaPhys = phys;
		}
	ekseam_puts("[sdblk] DMA bounce area r="); ekseam_puthex((unsigned)r); ekseam_puts(" phys="); ekseam_puthex(phys);
	ekseam_puts(KSdDmaWrites ? " (reads + writes)\n" : " (reads)\n");
	}

// Error recovery after a failed or stalled transfer, as sunxi-mmc's error path does it: abort the card's open data transfer (CMD12
// STOP_TRANSMISSION with the abort bit, R1b; no wait for the previous data: there is none to wait for), reset the FIFO and the DMA
// engine, wait for the card to leave busy, and program the timeouts again. Harmless on an idle card (it answers ILLEGAL_COMMAND).
// Caller holds the SD lock.
static void SdRecover()
	{
	Sd(KSdRint) = 0xFFFFFFFFu;
	Sd(KSdArg) = 0;
	Sd(KSdCmd) = KSdCmdStart | KSdCmdResp | KSdCmdCrc | (1u << 14) | 12;		// STOP_ABORT_CMD
	const TUint32 t0 = NKern::TickCount();
	for (TUint i = 0; i < 5000000u; ++i)
		{
		if (Sd(KSdRint) & (KSdRintCmdDone | KSdRintErrors))
			break;
		if (!(i & 0xFFF) && NKern::TickCount() - t0 > 200)
			break;
		}
	Sd(KSdDmac) = 1u;															// IDMAC soft reset, off
	Sd(KSdIdst) = 0xFFFFFFFFu;
	Sd(KSdGctl) = (Sd(KSdGctl) & ~(1u << 5)) | (1u << 1) | (1u << 2);		// DMA off; FIFO + DMA reset
	SdWaitClear(KSdGctl, (1u << 1) | (1u << 2));
	Sd(KSdRint) = 0xFFFFFFFFu;
	SdWaitClear(KSdStatus, KSdStatusCardBusy);
	SdSetTimeouts();
	}

static TBool SdMultiDma(TUint aSector, TUint aCount, TUint32* aBuf, TBool aWrite)
	{
	const TUint bytes = aCount * 512;
	if (aWrite)
		memcpy(SdDmaBuf, aBuf, bytes);
	SSdIdmaDes* des = (SSdIdmaDes*)(SdDmaBuf + KSdDmaBytes);
	const TPhysAddr desPhys = SdDmaPhys + KSdDmaBytes;
	const TUint n = (bytes + KSdDmaDesBytes - 1) / KSdDmaDesBytes;
	for (TUint i = 0; i < n; ++i)
		{
		const TUint left = bytes - i * KSdDmaDesBytes;
		des[i].iConfig = KIdmaOwn | KIdmaCh | KIdmaDic | (i == 0 ? KIdmaFd : 0);
		des[i].iSize = left < KSdDmaDesBytes ? left : KSdDmaDesBytes;
		des[i].iBuf = SdDmaPhys + i * KSdDmaDesBytes;
		des[i].iNext = desPhys + (i + 1) * sizeof(SSdIdmaDes);
		}
	des[n - 1].iConfig = (des[n - 1].iConfig | KIdmaLd | KIdmaEr) & ~KIdmaDic;
	des[n - 1].iNext = 0;
	asm volatile("dsb" : : : "memory");
	if (!SdWaitClear(KSdStatus, KSdStatusCardBusy))
		return EFalse;
	Sd(KSdRint) = 0xFFFFFFFFu;
	// the FIFO to the DMA (GCTL bit 31 = 0), DMA on (bit 5) + DMA reset (bit 2); the IDMAC: soft reset, then on with
	// fixed bursts, the chain base, no interrupts (polled as the PIO path); FIFO thresholds as Linux (burst 8)
	Sd(KSdGctl) = (Sd(KSdGctl) & ~(1u << 31)) | (1u << 5);
	Sd(KSdGctl) |= 1u << 2;
	Sd(KSdDmac) = 1u;
	Sd(KSdIdst) = 0xFFFFFFFFu;
	Sd(KSdIdie) = 0;
	Sd(KSdFtrgl) = 0x20070008u;
	Sd(KSdDmac) = (1u << 1) | (1u << 7);
	Sd(KSdDlba) = desPhys;
	Sd(KSdBlkSz) = 512;
	Sd(KSdBytCnt) = bytes;
	Sd(KSdArg) = aSector;
	Sd(KSdCmd) = (1u << 31) | (1u << 13) | (1u << 12) | (aWrite ? 1u << 10 : 0) | (1u << 9) | (1u << 8) | (1u << 6)
		| (aWrite ? 25 : 18);
	const TUint32 want = KSdRintDataOver | KSdRintAutoCmdDone;
	const TUint32 r = SdWaitRint(want, ETrue);
	const TBool ok = !(r & KSdRintErrors) && (r & want) == want;
	// back to the PIO configuration the other paths expect: IDMAC reset + off, DMA off, its state cleared
	Sd(KSdDmac) = 1u;
	Sd(KSdGctl) = (Sd(KSdGctl) & ~(1u << 5)) | (1u << 2);
	Sd(KSdIdst) = 0xFFFFFFFFu;
	Sd(KSdRint) = 0xFFFFFFFFu;
	if (!ok)
		{
		if (++SdDmaFails <= 4)
			{ ekseam_puts("[sdblk] DMA FAILED rint="); ekseam_puthex(r); ekseam_puts(" sector="); ekseam_puthex(aSector); ekseam_puts("\n"); }
		Sd(KSdGctl) |= 1u << 1;							// FIFO reset
		SdWaitClear(KSdGctl, 1u << 1);
		return EFalse;
		}
	if (!aWrite)
		memcpy(aBuf, SdDmaBuf, bytes);
	return SdWaitClear(KSdStatus, KSdStatusCardBusy);
	}

// for the SD media driver (blk/a64sdblk.cpp, C: on partition 3): aCount sectors from aSector, PIO, under the SD lock
static void A64MapSd();
extern "C" TBool A64SdTransfer(TUint aSector, TUint aCount, TUint32* aBuf, TBool aWrite)
	{
	if (!Smhc)
		A64MapSd();		// the media driver can start before boot stage 3 maps it
	if (!Smhc)
		return EFalse;
	const TUint32 t0 = NKern::TickCount();
	TBool ok = ETrue;
	for (TUint done = 0; done < aCount && ok; )
		{	// up to KSdMultiMax sectors per command (CMD18/CMD25); a failed multi-block run is retried sector by sector
		const TUint n = aCount - done < KSdMultiMax ? aCount - done : KSdMultiMax;
		TBool locked = SdLock();
		ok = EFalse;
		if (SdDmaBuf && (!aWrite || KSdDmaWrites))
			ok = SdMultiDma(aSector + done, n, aBuf + done * 128, aWrite);		// (a failure: PIO below)
		if (!ok)
			ok = n > 1 && SdMulti(aSector + done, n, aBuf + done * 128, aWrite);
		if (!ok)
			SdRecover();								// a stalled run leaves the card's transfer open
		SdUnlock(locked);
		if (ok)
			{
			done += n;
			continue;
			}
		if (n > 1 && ++SdMultiFails <= 4)
			{
			ekseam_puts("[sdblk] multi-block FAILED (sector by sector instead) sector="); ekseam_puthex(aSector + done);
			ekseam_puts(" count="); ekseam_puthex(n); ekseam_puts(" write="); ekseam_puthex(aWrite); ekseam_puts("\n");
			}
		ok = ETrue;
		for (TUint i = 0; i < n && ok; ++i)
			{
			locked = SdLock();
			const TUint s = aSector + done + i;
			ok = aWrite ? SdWriteSector(s, aBuf + (done + i) * 128) : SdReadSector(s, aBuf + (done + i) * 128);
			SdUnlock(locked);
			if (!ok)
				{
			// 2026-10-23: a marginal flash block (E: FAT sector 0x424ac4) stalls the card mid-sector with no error bit. With the finite
			// TMOUT the controller now flags it; abort the open transfer (CMD12), reset FIFO/DMA, retry the sector (the 2nd try
			// succeeded in boot 1371). Reads and writes, under the SD lock.
			for (TUint tries = 0; !ok && tries < 3; ++tries)
				{
				locked = SdLock();
				SdRecover();
				ok = aWrite ? SdWriteSector(s, aBuf + (done + i) * 128) : SdReadSector(s, aBuf + (done + i) * 128);
				SdUnlock(locked);
				ekseam_puts("[sdblk] retry sector="); ekseam_puthex(s); ekseam_puts(aWrite ? " write" : " read");
				ekseam_puts(ok ? " ok" : " failed"); ekseam_puts(KNl);
				}
				}
			if (!ok)
				{
				ekseam_puts("[sdblk] PIO FAILED sector="); ekseam_puthex(s);
				ekseam_puts(" rint="); ekseam_puthex(Sd(KSdRint)); ekseam_puts(" status="); ekseam_puthex(Sd(KSdStatus));
				ekseam_puts(" locked="); ekseam_puthex(locked); ekseam_puts("\n");
				}
			}
		done += n;
		}
	SdStatCalls++; SdStatSectors += aCount; SdStatMs += NKern::TickCount() - t0;
	return ok;
	}

static void HexTo(char* aDst, TUint32 aValue)
	{
	for (TInt i = 7; i >= 0; --i, aValue >>= 4)
		aDst[i] = "0123456789abcdef"[aValue & 0xF];
	}

// The SD log is a RING (2026-10-08): log byte p (counting from boot) lives in sector SdLogBase + 1 + (p / 512) % 256,
// at offset p % 512; the header's tot = bytes logged so far, so bytes [max(0, tot - 128 KB), tot) are on the card.
// A flush writes only the sectors touched since the previous one: it used to rewrite the whole last 128 KB (257
// polled sector writes) every 5 s -- PhoneLab hotspots put DfcThread0 at 29% of the S60 menu's first draw.
static TUint SdLogDone;		// bytes on the card already (whole sectors below it are final)
static void A64SdFlush(TInt aStage)
	{
	if (!Smhc || SdLogDead)
		return;
	// (2026-10-09) a flush already running used to mean "skip": a panic's / a fault's flush (the last words before a
	// restart) was dropped whenever the 250 ms flusher on another core was mid-write. Thread context: the mutex waits
	// for it (SdLogBusy under the mutex = this thread re-entered: skip). Fault context: spin up to ~0.5 s for it.
	const TBool locked = SdLock();
	if (SdLogBusy && !locked)
		for (TInt i = 0; i < 3000 && SdLogBusy; ++i)	// (0.5 s lost a WSERV panic's last lines, 2026-10-09)
			Kern::NanoWait(1000000);
	if (SdLogBusy)
		{
		SdUnlock(locked);
		return;
		}
	SdLogBusy = ETrue;
	TUint32 sector[128];
	char* h = (char*)sector;
	memclr(sector, sizeof(sector));
	memcpy(h, "PHORELOG tot=xxxxxxxx stage=xxxxxxxx flush=xxxxxxxx ring\n", 57);
	const TUint total = LogLen;
	HexTo(h + 13, total);
	HexTo(h + 28, (TUint32)aStage);
	HexTo(h + 43, ++SdFlushes);
	TBool ok = SdWriteSector(SdLogBase, sector);
	TUint from = SdLogDone & ~511u;
	if (total - from > KSdLogMax)
		from = (total - KSdLogMax + 511) & ~511u;	// lapped: the oldest sectors hold newer data now
	for (TUint p = from; ok && p < total; p += 512)
		{
		memclr(sector, sizeof(sector));
		char* d = (char*)sector;
		for (TUint i = p; i < total && i < p + 512; ++i)
			*d++ = LogBuf[i & KLogMask];
		ok = SdWriteSector(SdLogBase + 1 + (p / 512) % (KSdLogMax / 512), sector);
		}
	if (ok)
		SdLogDone = total;
	if (!ok)
		SdLogDead = ETrue;
	SdLogBusy = EFalse;
	SdUnlock(locked);
	}

// THREAD CONTEXT ONLY (takes the SD mutex): power/a64_idle.cpp's core-retirement test flushes its prints with this
// right after printing them, so they survive a hang (the 5 s timed flush loses the last lines).
extern "C" void A64SdFlushNow(TInt aStage) { A64SdFlush(aStage); }
// ... only if something was logged since the last flush (the core-retirement test calls this every 2 ms)
extern "C" void A64SdFlushIfDirty(TInt aStage) { if (LogLen != SdLogDone) A64SdFlush(aStage); }

// ---- A/B SLOTS (2026-10-05, user idea) ----
// Slot A = symbian.img on the FAT partition (the golden image: written from the PC / Etcher only).
// Slot B = partition 2, raw sectors from 1088 MiB (KSlotBSector; the 1 GiB partition, the IMAGE up to 112 MiB), written over USB
// (phlink 'U'). The CONTROL sector (KCtlSector, in the gap before partition 1) = {magic 'PHSL', state, len, sum}.
// boot.cmd (ONE-SHOT, no health timing): PENDING -> mark TRYING, boot B once; TRYING -> mark DONE (KStBad), boot A.
// phlink 'B' re-arms (PENDING) + reboots. The kernel feeds the watchdog every 2 s from stage 4, so a hang = reset =
// back to A, no hands needed. (Until 2026-10-06 a B that lived 15 s was marked GOOD and booted again: S60 trials that
// died later looped forever.)
const TUint KCtlSector = 6144;
const TUint KSlotBSector = 0x220000;	// P2 (tools/phorepoc-arm/sd_layout.py, boot.cmd) -- was 131072 before the 4-partition card
// the whole slot B partition (1 GiB, sd_layout.py): boot.cmd loads high (0x80000000) and copies down, no other bound (2026-10-15; was 112 MiB)
const TUint KSlotBMax = 1024u << 20;
enum { KCtlMagic = 0x4C534850, KStPending = 1, KStTrying = 2, KStGood = 3, KStBad = 4 };
static TUint32 BootCtlState;			// the control state this kernel booted with (0 = none)

static TBool CtlRead(TUint32* aCtl)
	{
	TUint32 s[128];
	const TBool locked = SdLock();
	const TBool ok = SdReadSector(KCtlSector, s);
	SdUnlock(locked);
	if (ok)
		memcpy(aCtl, s, 16);
	return ok;
	}

static TBool CtlWrite(TUint32 aState, TUint32 aLen, TUint32 aSum)
	{
	TUint32 s[128];
	memclr(s, sizeof(s));
	s[0] = KCtlMagic; s[1] = aState; s[2] = aLen; s[3] = aSum;
	const TBool locked = SdLock();
	const TBool ok = SdWriteSector(KCtlSector, s);
	SdUnlock(locked);
	return ok;
	}

// The SD log of the PREVIOUS slot B boot (sectors KSdLogSectorB..), replayed into this boot's log before we overwrite it:
// a slot B trial that faults before its USB link exists (seen 2026-10-06: the S60 ROM, magenta then watchdog reset)
// would otherwise leave no trace at all. Bounded: the last 48 KB of it.
static void A64ReplayPreviousLog(TBool aForce)
	{
	TUint32 s[128];
	TBool locked = SdLock();
	TBool ok = SdReadSector(KSdLogSectorB, s);
	SdUnlock(locked);
	const char* h = (const char*)s;
	if (!ok || memcompare((const TUint8*)h, 8, (const TUint8*)"PHORELOG", 8) != 0)
		{
		ekseam_puts("[EKERN][slot] no previous SD log to replay\n");
		return;
		}
	TUint len = 0, stage = 0;
	for (TInt i = 0; i < 8; ++i)
		{
		const char a = h[13 + i], b = h[28 + i];
		len = (len << 4) | (TUint)(a >= 'a' ? a - 'a' + 10 : a - '0');
		stage = (stage << 4) | (TUint)(b >= 'a' ? b - 'a' + 10 : b - '0');
		}
	if (len > KSdLogMax)
		len = KSdLogMax;
	if (!aForce && stage != 9)
		return;							// a slot B boot that lived: nothing to replay (a fault = stage 9)
	// the whole ring (2026-10-09: 48 KB ended in the loader's [cs] tracing, the fault's own lines were beyond it)
	const TUint KReplayMax = KSdLogMax;
	const TUint from = len > KReplayMax ? (len - KReplayMax) & ~511u : 0;
	ekseam_puts("[EKERN][slot] ===== previous boot's SD log (stage "); ekseam_puthex(stage);
	ekseam_puts("[EKERN][slot]   len "); ekseam_puthex(len);
	ekseam_puts("[EKERN][slot] ===== replay begins\n");
	for (TUint off = from; ok && off < len; off += 512)
		{
		locked = SdLock();
		ok = SdReadSector(KSdLogSectorB + 1 + off / 512, s);
		SdUnlock(locked);
		if (!ok)
			break;
		char line[513];
		const TUint n = len - off < 512 ? len - off : 512;
		memcpy(line, s, n);
		line[n] = 0;
		ekseam_puts(line);
		}
	ekseam_puts("\n[EKERN][slot] ===== replay ends\n");
	}

static void A64SlotBootInfo()
	{
	TUint32 c[4];
	if (!Smhc || !CtlRead(c) || c[0] != KCtlMagic)
		{
		ekseam_puts("[EKERN][slot] running slot A (no slot B)\n");
		return;
		}
	BootCtlState = c[1];
	const char* what = (c[1] == KStTrying) ? "running slot B (one-shot boot)" :
					   (c[1] == KStGood) ? "running slot B (good)" :
					   (c[1] == KStBad) ? "running slot A (back from slot B's one-shot boot)" : "running slot A";
	ekseam_puts("[EKERN][slot] "); ekseam_puts(what); ekseam_puts(", slot B len="); ekseam_puthex(c[2]);
	if (c[1] == KStTrying || c[1] == KStGood)
		SdLogBase = KSdLogSectorB;
	A64ReplayPreviousLog(c[1] == KStBad);	// slot B died before its USB link came up: its SD log is all there is
	}

// phlink 'B': arm slot B again (PENDING) for one more one-shot boot (boot.cmd); the caller reboots
extern "C" TInt A64SlotRearm()
	{
	TUint32 c[4];
	if (!Smhc || !CtlRead(c) || c[0] != KCtlMagic || c[2] == 0)
		return KErrNotFound;
	return CtlWrite(KStPending, c[2], c[3]) ? KErrNone : KErrGeneral;
	}

// the upload (phlink): stream into slot B in 64 KB runs (multi-block CMD25 through A64SdTransfer -- one sector at a time
// waited for the card after every write), then read it all back and check the word sum. NOTE the word sum of a whole
// ROM is the SAME for every image (rombuild adjusts the ROM header checksum to that end): it catches corruption, it does
// not identify the image -- the delta upload below uses a real per-block hash.
const TUint KUpRun = 128;					// sectors per SD write (64 KB) = the delta block size
const TUint KUpBytes = KUpRun * 512;
// TWO buffers (2026-10-15, push speed): a full one goes to the writer thread while USB fills the other -- the SD write
// (10.5 s of a 19.8 s stream, boot 504) no longer stops the reception. UpBuf = the one being filled.
static TUint32 UpBufs[2][KUpRun * 128];
static TUint32* UpBuf = UpBufs[0];
static TUint UpFill, UpLen, UpDone, UpSum, UpNext;
static volatile TBool UpOk;
static volatile TUint32 UpActiveSince, UpLastProgress;	// NKern::TickCount(); 0 = no upload (WdogTimerFn's stall guard)
// the CPU governor holds the clock still while a USB upload streams (a64_cpufreq.cpp: every switch parks the CPU on
// OSC24M for up to a millisecond; pushes died mid-stream, boots 545-549)
extern "C" TBool A64UploadActive() { return UpActiveSince != 0; }
static TUint32 UpWriteTicks, UpWaitTicks, UpStartTick;	// upload profile: SD write time, receive-side waits, the whole stream

// the writer: its own DFC queue (thread); UpFree counts the free buffers besides the one being filled (0 or 1)
static TDfcQue* UpQ;
static DSemaphore* UpFree;
static TDfc* UpDfc;
static TUint32* UpWrBuf;
static TUint UpWrSector, UpWrCount;

static void UpWriteDfcFn(TAny*)
	{
	const TUint32 t0 = NKern::TickCount();
	if (!A64SdTransfer(UpWrSector, UpWrCount, UpWrBuf, ETrue))
		UpOk = EFalse;
	UpWriteTicks += NKern::TickCount() - t0;
	memclr(UpWrBuf, KUpBytes);
	NKern::ThreadEnterCS();
	Kern::SemaphoreSignal(*UpFree);
	NKern::ThreadLeaveCS();
	}

static TBool UpAsyncReady()
	{
	if (UpDfc)
		return ETrue;
	_LIT(KUpQName, "SdUpWriter");
	_LIT(KUpSemName, "SdUpFree");
	NKern::ThreadEnterCS();
	TInt r = Kern::DfcQCreate(UpQ, 26, &KUpQName);
	if (r == KErrNone)
		r = Kern::SemaphoreCreate(UpFree, KUpSemName, 1);
	if (r == KErrNone)
		UpDfc = new TDfc(UpWriteDfcFn, NULL, UpQ, 1);
	NKern::ThreadLeaveCS();
	ekseam_puts("[sdblk] upload writer thread r="); ekseam_puthex((TUint)r);
	return UpDfc != NULL;
	}

// wait until no write is in flight (both buffers ours again)
static void UpDrain()
	{
	if (!UpFree)
		return;
	const TUint32 t0 = NKern::TickCount();
	NKern::ThreadEnterCS();
	Kern::SemaphoreWait(*UpFree);
	Kern::SemaphoreSignal(*UpFree);
	NKern::ThreadLeaveCS();
	UpWaitTicks += NKern::TickCount() - t0;
	}

static void UpReset(TUint aFirstSector, TUint aLen)
	{
	UpDrain();
	UpLen = aLen; UpDone = 0; UpFill = 0; UpNext = aFirstSector; UpOk = ETrue;
	UpActiveSince = UpLastProgress = NKern::TickCount() | 1;
	UpBuf = UpBufs[0];
	memclr(UpBufs, sizeof(UpBufs));
	}

extern "C" TInt A64SlotUploadBegin(TUint aLen, TUint aSum)
	{
	if (!Smhc || aLen == 0 || aLen > KSlotBMax)
		return KErrArgument;
	UpReset(KSlotBSector, aLen);
	UpSum = aSum;
	UpWriteTicks = UpWaitTicks = 0;
	UpStartTick = NKern::TickCount();
	CtlWrite(KStBad, 0, 0);					// slot B is not bootable while it is half-written
	return KErrNone;
	}

// write the buffered bytes (whole sectors; a partial last sector is zero-padded) and start a new run -- synchronous,
// after any write in flight
static void UpFlush()
	{
	UpDrain();
	const TUint sectors = (UpFill + 511) / 512;
	if (sectors)
		{
		const TUint32 t0 = NKern::TickCount();
		UpOk = UpOk && A64SdTransfer(UpNext, sectors, UpBuf, ETrue);
		UpWriteTicks += NKern::TickCount() - t0;
		}
	UpNext += sectors;
	memclr(UpBuf, KUpBytes);
	UpFill = 0;
	}

// a FULL buffer: hand it to the writer (waiting while the other one is still being written) and fill the other
static void UpFlushFull()
	{
	if (!UpAsyncReady())
		{
		UpFlush();
		return;
		}
	const TUint32 t0 = NKern::TickCount();
	NKern::ThreadEnterCS();
	Kern::SemaphoreWait(*UpFree);
	NKern::ThreadLeaveCS();
	UpWaitTicks += NKern::TickCount() - t0;
	UpWrBuf = UpBuf;
	UpWrSector = UpNext;
	UpWrCount = KUpRun;
	UpNext += KUpRun;
	UpBuf = UpBuf == UpBufs[0] ? UpBufs[1] : UpBufs[0];
	UpFill = 0;
	UpDfc->Enque();
	}

extern "C" TInt A64SlotUploadData(const TUint8* aData, TInt aLen)
	{
	TInt i = 0;
	while (i < aLen && UpDone < UpLen)
		{
		TUint n = (TUint)(aLen - i);
		if (n > UpLen - UpDone) n = UpLen - UpDone;
		if (n > KUpBytes - UpFill) n = KUpBytes - UpFill;
		memcpy((TUint8*)UpBuf + UpFill, aData + i, n);
		UpFill += n; UpDone += n; i += (TInt)n;
		UpLastProgress = NKern::TickCount() | 1;
		// the writer thread (UpFlushFull) is OFF: boot 506 hung the link at 36.7 MB with USB_DFC spinning on CPU 0 and no SD
		// I/O (bug-backlog.md); A64DEFS=-DA64_UP_ASYNC re-enables it for that investigation
		if (UpFill == KUpBytes)
#ifdef A64_UP_ASYNC
			UpFlushFull();
#else
			UpFlush();
#endif
		}
	return UpOk ? KErrNone : KErrCorrupt;
	}

extern "C" TInt A64SlotUploadEnd(TUint* aSum)
	{
	UpFlush();
	UpActiveSince = 0;
	const TUint32 tStream = NKern::TickCount();
	if (!UpOk)
		return KErrCorrupt;
	TUint32 sum = 0;
	for (TUint sec = KSlotBSector; sec < UpNext; sec += KUpRun)
		{
		const TUint n = UpNext - sec < KUpRun ? UpNext - sec : KUpRun;
		if (!A64SdTransfer(sec, n, UpBuf, EFalse))
			return KErrCorrupt;
		for (TUint w = 0; w < n * 128; ++w)
			sum += UpBuf[w];
		}
	*aSum = sum;
	// the profile, in ms (1 tick = 1 ms on this board): the whole stream, the SD writes inside it, the read-back check
	ekseam_puts("[sdblk] upload ms: stream="); ekseam_puthex(tStream - UpStartTick);
	ekseam_puts("[sdblk]   sd writes="); ekseam_puthex(UpWriteTicks);
	ekseam_puts("[sdblk]   receive waits="); ekseam_puthex(UpWaitTicks);
	ekseam_puts("[sdblk]   verify="); ekseam_puthex(NKern::TickCount() - tStream);
	if (sum != UpSum)
		return KErrCorrupt;
	return CtlWrite(KStPending, UpLen, UpSum) ? KErrNone : KErrCorrupt;
	}

// ---- DELTA UPLOAD (2026-10-09, user idea: re-send only what changed; PhoneLab push mode=delta) ----
// 'Y' len: the hash of every 64 KB block of slot B over [0, len) -> PhoneLab compares with the new image's blocks.
// 'X' off len + len bytes: write them into slot B at byte offset off (a multiple of 512) -- only the changed runs.
// 'Z' len hash: re-hash slot B, compare the hash of the block hashes with what PhoneLab expects, then PENDING.
// Block hash = FNV-1a over the block's little-endian 32-bit words (whole sectors, zero-padded): cheap, and unlike the
// word sum it sees swapped or misplaced blocks. The same function is in PhoneLab (pinephone.go BlockHashes).
static TUint32 BlockHash(const TUint32* aW, TUint aWords)
	{
	TUint32 h = 2166136261u;
	for (TUint i = 0; i < aWords; ++i)
		h = (h ^ aW[i]) * 16777619u;
	return h;
	}

extern "C" TInt A64SlotHashBlocks(TUint aLen, TUint32* aOut, TInt aMax)
	{
	if (!Smhc || aLen == 0 || aLen > KSlotBMax)
		return KErrArgument;
	const TUint sectors = (aLen + 511) / 512;
	TInt n = 0;
	for (TUint s = 0; s < sectors && n < aMax; s += KUpRun)
		{
		const TUint c = sectors - s < KUpRun ? sectors - s : KUpRun;
		if (!A64SdTransfer(KSlotBSector + s, c, UpBuf, EFalse))
			return KErrCorrupt;
		aOut[n++] = BlockHash(UpBuf, c * 128);
		}
	return n;
	}

extern "C" TInt A64SlotPatchBegin(TUint aOff, TUint aLen)
	{
	if (!Smhc || (aOff & 511) || aLen == 0 || aOff + aLen > KSlotBMax)
		return KErrArgument;
	CtlWrite(KStBad, 0, 0);					// not bootable until 'Z' has checked the whole image
	UpReset(KSlotBSector + aOff / 512, aLen);
	return KErrNone;
	}

extern "C" TInt A64SlotPatchEnd()
	{
	UpFlush();
	UpActiveSince = 0;
	return UpOk ? KErrNone : KErrCorrupt;
	}

extern "C" TInt A64SlotCommit(TUint aLen, TUint32 aHash, TUint32* aGot)
	{
	static TUint32 hashes[(KSlotBMax / 512 + KUpRun - 1) / KUpRun];
	const TInt n = A64SlotHashBlocks(aLen, hashes, sizeof(hashes) / sizeof(hashes[0]));
	if (n <= 0)
		return n < 0 ? n : KErrCorrupt;
	*aGot = BlockHash(hashes, (TUint)n);
	if (*aGot != aHash)
		return KErrCorrupt;
	return CtlWrite(KStPending, aLen, aHash) ? KErrNone : KErrCorrupt;
	}

// the watchdog (WDOG0, 0x01C20CA0: CTRL +0x10 = 0xA57 << 1 | 1 restarts the count): fed every 2 s from an NTimer.
// boot.cmd arms it (16 s) for slot B; slot A boots with it off, where the feeding is harmless.
static volatile TBool WdogRebooting;
static void WdogKick() { if (!WdogRebooting) *(volatile TUint32*)(KHwBaseCcuPio + 0xCB0) = (0xA57u << 1) | 1u; }
// CRUMBS (2026-10-09, the NGA boot freeze): a hard freeze stops the USB + SD logs before their last seconds, then the
// watchdog resets. Every feed (2 s, ISR context) snapshots what each core runs into a DRAM page Symbian never uses
// (just past Symbian's RAM -- see CrumbMap) and a warm reset keeps: after the fallback to slot A,
// PhoneLab `dump <crumbs address from the boot log>:...` reads the last snapshot before the freeze. Per core (16 words from word 4 + 16 * c):
// NThread*, the user pc / lr in its exception frame (valid when it entered the kernel from user mode), then
// "<process>::<thread>" (first 52 chars). Word 0 = 'CRMB', word 1 = the snapshot count, word 2 = ms since boot.
static volatile TUint32* Crumbs;
static DPlatChunkHw* CrumbChunk;
static volatile TUint32* CrumbUsb;		// the MUSB block (0x01C19000), read into the crumbs during uploads
static DPlatChunkHw* CrumbUsbChunk;
static void CrumbMap()
	{
	if (Crumbs)
		{
		Crumbs[3] = Crumbs[3] + 1;		// stage 4 calls (= user threads started), written from thread context
		return;
		}
	NKern::ThreadEnterCS();
	// a page past Symbian's RAM, whose size is the user's boot config since 2026-10-12: up to 1 GB -> 0x90000000 (as
	// before), more -> 0xF0000000 = the reserved top 256 MB of the 3 GB phone (Symbian's maximum ends there; U-Boot's
	// loads stay below 0x50000000, its relocated self sits in the last few MB). NOT base + iTotalRamSize: that excludes
	// the ROM, which runs from RAM (boot 379 put the page inside Symbian's RAM). Logged for PhoneLab's dump.
	const TUint32 crumbPhys = (TUint32)Kern::SuperPage().iTotalRamSize > 0x40000000u ? 0xF0000000u : 0x90000000u;
	if (DPlatChunkHw::New(CrumbChunk, crumbPhys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking) == KErrNone)
		Crumbs = (volatile TUint32*)CrumbChunk->LinearAddress();
	if (DPlatChunkHw::New(CrumbUsbChunk, 0x01C19000, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking) == KErrNone)
		CrumbUsb = (volatile TUint32*)CrumbUsbChunk->LinearAddress();
	NKern::ThreadLeaveCS();
	ekseam_puts("[EKERN] RAM "); ekseam_puthex((TUint32)Kern::SuperPage().iTotalRamSize >> 20);
	ekseam_puts(" MB; crumbs at "); ekseam_puthex(crumbPhys); ekseam_puts("\n");
	if (Crumbs && Crumbs[0] == 0x424D5243)	// the previous boot's last snapshot survived the reset: print it first
		{
		ekseam_puts("[EKERN][crumbs] previous boot: snapshots="); ekseam_puthex(Crumbs[1]);
		ekseam_puts(" ms="); ekseam_puthex(Crumbs[2]); ekseam_puts("\n");
		for (TInt cpu = 0; cpu < 4; ++cpu)
			{
			volatile TUint32* s = Crumbs + 4 + 16 * cpu;
			char name[53];
			for (TInt i = 0; i < 52; ++i)
				{
				const char ch = (char)((volatile TUint8*)(s + 3))[i];
				name[i] = ch >= 32 && ch < 127 ? ch : (ch ? '?' : 0);
				}
			name[52] = 0;
			ekseam_puts("[EKERN][crumbs]   cpu"); ekseam_puthex(cpu); ekseam_puts(" pc="); ekseam_puthex(s[1]);
			ekseam_puts(" lr="); ekseam_puthex(s[2]); ekseam_puts(" "); ekseam_puts(name); ekseam_puts("\n");
			}
		if (Crumbs[80] == 0x444C5055)		// 'UPLD': an upload was streaming at the last snapshot
			{
			ekseam_puts("[EKERN][crumbs] upload: done="); ekseam_puthex(Crumbs[81]); ekseam_puts(" len="); ekseam_puthex(Crumbs[82]);
			ekseam_puts(" ms since progress="); ekseam_puthex(Crumbs[83]); ekseam_puts(" ms active="); ekseam_puthex(Crumbs[84]);
			ekseam_puts("\n[EKERN][crumbs] usb regs 0x40..0xbc:");
			for (TInt i = 0; i < 32; ++i)
				{
				ekseam_puts(" "); ekseam_puthex(Crumbs[96 + i]);
				}
			ekseam_puts("\n");
			}
		}
	if (Crumbs)
		{
		Crumbs[80] = 0;
		Crumbs[0] = 0x31424D43;		// 'CMB1': mapped, no snapshot yet (CrumbSnapshot writes 'CRMB')
		Crumbs[1] = 0;
		Crumbs[3] = 1;
		}
	}
static void CrumbName(volatile TUint8* aDst, TInt aMax, const DObject* aObj, TInt& aPos)
	{
	if (!aObj || !aObj->iName)
		return;
	const TDesC& n = *(const TDesC*)aObj->iName;
	for (TInt i = 0; i < n.Length() && aPos < aMax; ++i)
		aDst[aPos++] = (TUint8)n[i];
	}
static void CrumbSnapshot()
	{
	volatile TUint32* c = Crumbs;
	if (!c)
		return;
	c[0] = 0x424D5243;		// 'CRMB'
	c[1] = c[1] + 1;
	c[2] = NKern::TickCount();
	for (TInt cpu = 0; cpu < NKern::NumberOfCpus() && cpu < 4; ++cpu)
		{
		volatile TUint32* s = c + 4 + 16 * cpu;
		NThread* nt = TheSubSchedulers[cpu].iCurrentThread;
		s[0] = (TUint32)nt;
		s[1] = s[2] = 0;
		volatile TUint8* name = (volatile TUint8*)(s + 3);
		for (TInt i = 0; i < 52; ++i)
			name[i] = 0;
		if (!nt)
			continue;
		if (nt->iStackBase)
			{
			const SThreadExcStack* f = (const SThreadExcStack*)((TUint8*)nt->iStackBase + nt->iStackSize) - 1;
			s[1] = f->iR15;
			s[2] = f->iR14usr;
			}
		const DThread* t = _LOFF(nt, DThread, iNThread);
		TInt pos = 0;
		CrumbName(name, 50, t->iOwningProcess, pos);
		if (pos < 49) { name[pos++] = ':'; name[pos++] = ':'; }
		CrumbName(name, 52, t, pos);
		}
	// the push-failure record (2026-10-16): upload progress + the MUSB registers while an upload streams
	if (UpActiveSince)
		{
		const TUint32 now = NKern::TickCount();
		c[80] = 0x444C5055;		// 'UPLD'
		c[81] = UpDone;
		c[82] = UpLen;
		c[83] = (now - UpLastProgress) * NKern::TickPeriod() / 1000;
		c[84] = (now - UpActiveSince) * NKern::TickPeriod() / 1000;
		if (CrumbUsb)
			for (TInt i = 0; i < 32; ++i)
				c[96 + i] = CrumbUsb[(0x40 >> 2) + i];
		}
	else
		c[80] = 0;
	}

static void WdogTimerFn(TAny*);
static NTimer WdogTimer(WdogTimerFn, 0);
// UPLOAD STALL GUARD (2026-10-12, the first high-speed push froze the phone at 31 MB, the USB silent, cores at 100%):
// an upload that makes no progress for 15 s stops the feeding -> the watchdog's warm reset -> slot A, the crumbs kept.
// USB LINK GUARD (A64DEFS=-DA64_USB_GUARD, 2026-10-15, for builds that change the USB PSL / phlink): slot B with no
// host configuration within 120 s of the first feeding = the link is broken -> stop feeding -> warm reset -> boot.cmd
// sends the one-shot slot B back to golden A. No hands needed when an experiment cuts the only link to the phone.
static volatile TBool UsbLinkSeen;
extern "C" void A64UsbLinkUp() { UsbLinkSeen = ETrue; }
static TBool UsbGuardTripped()
	{
#ifdef A64_USB_GUARD
	static TUint32 start;
	static TBool said;
	if (!start)
		start = NKern::TickCount() | 1;
	if (UsbLinkSeen || !(BootCtlState == KStTrying || BootCtlState == KStGood)
		|| NKern::TickCount() - start < (TUint32)NKern::TimerTicks(120000))
		return EFalse;
	if (!said)
		{
		said = ETrue;
		ekseam_puts("[EKERN][usb] no USB host in 120 s on slot B: watchdog reset -> golden A\n");
		A64SdFlush(0x0ff);
		}
	return ETrue;
#else
	return EFalse;
#endif
	}

// BOOT-COMPLETION + LIVENESS GUARD (2026-10-16; A64DEFS=-DA64_NO_HANG_GUARD = off; slot B only, golden A never trips):
// a slot-B image whose user side hangs (WSERV stuck in boot, SSM waiting forever) kept the feeding timer alive, so
// the one-shot slot B never fell back to golden A by itself. Signal = "UI up": the HWRM power-state plug-in
// (src/phorepoc-arm/a64/hwrm/PowerStatePlugin.cpp, in !HWRMServer, a 10 s CPeriodic) calls the power HAL function 0x5047
// whenever WSERV and the homescreen both exist. First ping = UI up. Then
//  (1) BOOT: no ping within A64_BOOT_GUARD_S (180) s of the first feeding -> stop feeding -> reset -> golden A.
//  (2) LIVENESS: after the first ping, no ping for A64_LIVE_GUARD_S (60) s -> same (A64_NO_LIVE_GUARD = only (1)).
// Idle / screen off do not matter (the plug-in's timer runs regardless); uploads keep their own guard (15 s stall).
// A build without the HWRM plug-in (SSM_TEST ROMs without the UI) must pass -DA64_NO_HANG_GUARD.
#ifndef A64_BOOT_GUARD_S
#define A64_BOOT_GUARD_S 180
#endif
#ifndef A64_LIVE_GUARD_S
#define A64_LIVE_GUARD_S 60
#endif
// the BOOT timeout is runtime data: the developer menu's "Boot guard timeout" (CenRep e7015a01 key 3) reaches it through the power HAL
// function 0x5049, sent by the HWRM power-state plug-in as soon as it starts (before that, and without the key, A64_BOOT_GUARD_S)
static volatile TUint32 UiBootGuardS = A64_BOOT_GUARD_S;
static volatile TUint32 UiLastPing;		// NKern::TickCount() | 1 of the newest user-side ping; 0 = UI not up yet
static void UiGuardPing() { UiLastPing = NKern::TickCount() | 1; }
static TBool UiGuardTripped()
	{
#ifdef A64_NO_HANG_GUARD
	return EFalse;
#else
	static TUint32 start;
	static TBool said;
	if (!(BootCtlState == KStTrying || BootCtlState == KStGood))
		return EFalse;
	const TUint32 now = NKern::TickCount();
	if (!start)
		start = now | 1;
	const TUint32 last = UiLastPing;
	const char* why = 0;
	if (!last)
		{
		if (now - start >= (TUint32)NKern::TimerTicks(UiBootGuardS * 1000))
			why = "[EKERN][guard] boot incomplete: UI not up in time on slot B -> watchdog reset -> golden A\n";
		}
#ifndef A64_NO_LIVE_GUARD
	else if (now - last >= (TUint32)NKern::TimerTicks(A64_LIVE_GUARD_S * 1000))
		why = "[EKERN][guard] UI heartbeat lost on slot B -> watchdog reset -> golden A\n";
#endif
	if (!why)
		return EFalse;
	if (!said)
		{
		said = ETrue;
		ekseam_puts(why);
		A64SdFlush(0x0ff);
		}
	return ETrue;
#endif
	}

static void WdogTimerFn(TAny*)
	{
	const TUint32 last = UpLastProgress;
	if (!(UpActiveSince && last && NKern::TickCount() - last > (TUint32)NKern::TimerTicks(15000)) && !UsbGuardTripped()
		&& !UiGuardTripped())
		WdogKick();
	CrumbSnapshot();
	if (!WdogRebooting)
		WdogTimer.OneShot(NKern::TimerTicks(2000));
	}

// reboot now: stop feeding, then whole-system reset after the shortest interval (0.5 s). Feeding on would keep
// restarting the count (seen on the phone 2026-10-05: 'R' did nothing once the feeding timer ran).
extern "C" void A64WatchdogReboot()
	{
	WdogRebooting = ETrue;
	volatile TUint32* wdog = (volatile TUint32*)(KHwBaseCcuPio + 0xCA0);
	wdog[0x14 / 4] = 1;						// CFG: whole system
	wdog[0x18 / 4] = 1;						// MODE: enable, interval 0 = 0.5 s
	wdog[0x10 / 4] = (0xA57u << 1) | 1u;	// CTRL: restart the count -- a new interval only applies from a restart
	}										// (2026-10-06: without it a running 16 s count never expired; Linux
											// sunxi_wdt_restart likewise re-writes until the reset)

static void A64MapSd()
	{
	if (Smhc)
		return;							// already mapped (the SD media driver maps it on demand)
	NKern::ThreadEnterCS();
	TInt r = DPlatChunkHw::New(SmhcChunk, KSmhc0Phys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	NKern::ThreadLeaveCS();
	if (r == KErrNone)
		{
		Smhc = (volatile TUint32*)SmhcChunk->LinearAddress();
		// the SD clock as U-Boot left it (2026-10-15, before SD DMA work: is the bus at 25 or 50 MHz?): CCU
		// SDMMC0_CLK_REG (+0x088: src bits 25:24, N bits 17:16, M bits 3:0), SMHC CLKDIV (+0x04), CTYPE (+0x0C: bus
		// width), NTSR (+0x5C: new timing mode bit 31)
		ekseam_puts("[sdblk] clock: CCU SDMMC0=");  ekseam_puthex(*(volatile TUint32*)(KHwBaseCcuPio + 0x088));
		ekseam_puts("[sdblk]   CLKDIV=");           ekseam_puthex(Smhc[0x04 / 4]);
		ekseam_puts("[sdblk]   CTYPE=");            ekseam_puthex(Smhc[0x0C / 4]);
		ekseam_puts("[sdblk]   NTSR=");             ekseam_puthex(Smhc[0x5C / 4]);
		ekseam_puts("[sdblk]   TMOUT=");            ekseam_puthex(Smhc[0x08 / 4]); ekseam_puts(" -> "); ekseam_puthex(KSdTmoutValue); ekseam_puts(KNl);
		SdSetTimeouts();
		SdDmaInit();
		}
	}

// every 5 s after the first user thread: NTimer (ISR context) -> a DFC on DFC queue 0 does the write
#ifdef __SMP__
// the monitor's thread view (2026-10-06, "what spins on cores 2+3"): what each core runs right now (a spinner never
// switches out, so its iTotalCpuTime never updates -- it only shows here), then the top 3 threads by CPU time used
// since the last report.
static void MonPutName(DThread* aT)
	{
	TBuf8<64> n;
	aT->TraceAppendFullName(n, EFalse);
	char b[65];
	TInt i = 0;
	for (; i < n.Length() && i < 64; ++i) b[i] = (char)n[i];
	b[i] = 0;
	ekseam_puts(b);
	}

// CPU time used so far including the current stretch: iTotalCpuTime is only banked when a thread switches out, so a
// spinner (or a core idle the whole period) otherwise reads 0 new time (the busy%=100-on-idle-cores artifact)
static TUint64 MonCpuTime(NThread* aT, TUint64 aNow)
	{
	TUint64 t = aT->iTotalCpuTime.i64;
	for (TInt c = 0; c < NKern::NumberOfCpus() && c < KMaxCpus; ++c)
		if (TheSubSchedulers[c].iCurrentThread == aT && aNow > aT->iLastStartTime.i64)
			t += aNow - aT->iLastStartTime.i64;
	return t;
	}

// Per-core busy % since the caller's previous call (the caller keeps the state): a64_bars.cpp's 1 s sampler, the
// same sum as the 5 s monitor below. Returns the number of cores.
// PROFILE SNAPSHOT (PhoneLab 'Q', 2026-10-08): every thread's CPU time so far, every process, every chunk's size,
// as text lines (the host diffs two snapshots into CPU % and charts RAM per process):
//   N <timestamp hex> <timestamp Hz hex> <cores>          header
//   I <core> <idle-thread CPU time hex>                    per core
//   P <process id hex> <name>                               every process
//   T <thread id hex> <process id hex> <CPU time hex> <name>
//   C <owner process id hex, 0 = global> <committed bytes hex> <name>
//   M <free RAM KB hex> <kernel heap KB hex>
// Returns the length written. Runs on phlink's DFC thread; the container mutexes need a critical section.
static TInt ProfPut(TUint8* aBuf, TInt aMax, TInt aN, const char* aS)
	{
	while (*aS && aN < aMax) aBuf[aN++] = (TUint8)*aS++;
	return aN;
	}
static TInt ProfHex(TUint8* aBuf, TInt aMax, TInt aN, TUint64 aV)
	{
	char t[17]; TInt k = 16;
	t[16] = 0;
	do { t[--k] = "0123456789abcdef"[aV & 15]; aV >>= 4; } while (aV && k);
	aN = ProfPut(aBuf, aMax, aN, t + k);
	return ProfPut(aBuf, aMax, aN, " ");
	}
static TInt ProfName(TUint8* aBuf, TInt aMax, TInt aN, DObject* aObj)
	{
	TBuf8<KMaxKernelName> nm;
	aObj->Name(nm);
	for (TInt i = 0; i < nm.Length() && aN < aMax - 1; ++i)
		aBuf[aN++] = nm[i] == '\n' ? ' ' : nm[i];
	return ProfPut(aBuf, aMax, aN, "\n");
	}
static TUint64 MonCpuTime(NThread* aT, TUint64 aNow);
extern "C" TInt A64ProfileSnapshot(TUint8* aBuf, TInt aMax)
	{
	TInt n = 0;
	NKern::ThreadEnterCS();
	const TUint64 now = NKern::Timestamp();
	n = ProfPut(aBuf, aMax, n, "N ");
	n = ProfHex(aBuf, aMax, n, now);
	n = ProfHex(aBuf, aMax, n, NKern::TimestampFrequency());
	n = ProfHex(aBuf, aMax, n, (TUint)NKern::NumberOfCpus());
	n = ProfPut(aBuf, aMax, n, "\n");
	for (TInt c = 0; c < NKern::NumberOfCpus() && c < KMaxCpus; ++c)
		{
		NThread* idle = TheSubSchedulers[c].iInitialThread;
		n = ProfPut(aBuf, aMax, n, "I ");
		n = ProfHex(aBuf, aMax, n, (TUint)c);
		n = ProfHex(aBuf, aMax, n, idle ? MonCpuTime(idle, now) : 0);
		n = ProfPut(aBuf, aMax, n, "\n");
		}
	DObjectCon* const* cons = Kern::Containers();
	DObjectCon& procs = *cons[EProcess];
	procs.Wait();
	for (TInt i = 0; i < procs.Count(); ++i)
		{
		DProcess* p = (DProcess*)procs[i];
		n = ProfPut(aBuf, aMax, n, "P ");
		n = ProfHex(aBuf, aMax, n, p->iId);
		n = ProfName(aBuf, aMax, n, p);
		}
	procs.Signal();
	DObjectCon& thrs = *cons[EThread];
	thrs.Wait();
	for (TInt i = 0; i < thrs.Count(); ++i)
		{
		DThread* t = (DThread*)thrs[i];
		n = ProfPut(aBuf, aMax, n, "T ");
		n = ProfHex(aBuf, aMax, n, t->iId);
		n = ProfHex(aBuf, aMax, n, t->iOwningProcess ? t->iOwningProcess->iId : 0);
		// the thread taking this snapshot (phlink's USB DFC) only counts its banked time: its "current stretch" is the
		// snapshot itself and read as 100% of a core on the phone (2026-10-08)
		n = ProfHex(aBuf, aMax, n, &t->iNThread == NKern::CurrentThread() ? (TUint64)t->iNThread.iTotalCpuTime.i64
			: MonCpuTime(&t->iNThread, now));
		n = ProfName(aBuf, aMax, n, t);
		}
	thrs.Signal();
	DObjectCon& chks = *cons[EChunk];
	chks.Wait();
	for (TInt i = 0; i < chks.Count(); ++i)
		{
		DChunk* c = (DChunk*)chks[i];
		n = ProfPut(aBuf, aMax, n, "C ");
		n = ProfHex(aBuf, aMax, n, c->iOwningProcess ? c->iOwningProcess->iId : 0);
		n = ProfHex(aBuf, aMax, n, (TUint)c->Size());
		n = ProfName(aBuf, aMax, n, c);
		}
	chks.Signal();
	n = ProfPut(aBuf, aMax, n, "M ");
	n = ProfHex(aBuf, aMax, n, (TUint)(Kern::FreeRamInBytes() >> 10));
	n = ProfHex(aBuf, aMax, n, (TUint)(K::Allocator->Size() >> 10));
	n = ProfPut(aBuf, aMax, n, "\n");
	NKern::ThreadLeaveCS();
	return n;
	}

extern "C" TInt A64CpuBusy(TUint64* aLastNow, TUint64* aLastIdle, TUint* aBusy, TInt aMax)
	{
	const TUint64 now = NKern::Timestamp();
	const TInt n = NKern::NumberOfCpus() < aMax ? NKern::NumberOfCpus() : aMax;
	for (TInt c = 0; c < n && c < KMaxCpus; ++c)
		{
		NThread* idle = TheSubSchedulers[c].iInitialThread;
		const TUint64 t = idle ? MonCpuTime(idle, now) : 0;
		aBusy[c] = 0;
		if (*aLastNow && now > *aLastNow)
			{
			const TUint64 di = t - aLastIdle[c], dn = now - *aLastNow;
			aBusy[c] = di >= dn ? 0 : (TUint)(100 - (di * 100) / dn);
			}
		aLastIdle[c] = t;
		}
	*aLastNow = now;
	return n;
	}

// the PC sampler (2026-10-06, "why does Menu::Menu spin 40 s"): every 2 ms (NTimer, ISR context on the tick core)
// read the user-entry exception frame at the top of each other core's current thread's supervisor stack -- the
// user PC at its latest kernel entry -- then send that core a reschedule IPI so the next sample is a fresh IRQ
// frame. The monitor prints the busiest thread's PCs (map them with the ROM map: tools/romsym.py).
extern "C" void send_resched_ipi(TInt aCpu);
const TInt KPcSlots = 32;
static DThread* PcThread;					// the thread being sampled (the top CPU user at the last report)
static TUint32 PcVal[KPcSlots], PcHits[KPcSlots], PcOther;
static void PcSampleFn(TAny*);
static NTimer PcTimer(PcSampleFn, 0);
static void PcSampleFn(TAny*)
	{
	const TInt me = NKern::CurrentCpu();
	for (TInt c = 0; c < NKern::NumberOfCpus() && c < KMaxCpus; ++c)
		{
		NThread* nt = TheSubSchedulers[c].iCurrentThread;
		if (c == me || !nt || nt == TheSubSchedulers[c].iInitialThread)
			continue;
		if (PcThread && nt == &PcThread->iNThread && nt->iStackBase)
			{
			const SThreadExcStack* f = (const SThreadExcStack*)((TUint8*)nt->iStackBase + nt->iStackSize) - 1;
			const TUint32 pc = f->iR15 & ~1u, lr = f->iR14usr & ~1u;
			TInt s = 0;
			for (; s < KPcSlots && PcHits[s] && PcVal[s] != pc; ++s) {}
			if (s < KPcSlots) { PcVal[s] = pc; ++PcHits[s]; } else ++PcOther;
			}
		send_resched_ipi(c);
		}
	PcTimer.Again(NKern::TimerTicks(2));
	}

static void MonPcReport(DThread* aTop)
	{
	if (PcThread)
		{
		ekseam_puts("[EKERN][mon] pcs of the last top thread (pc*hits):");	// no name: it may have died since
		for (TInt s = 0; s < KPcSlots && PcHits[s]; ++s)
			{
			ekseam_puts(" "); ekseam_puthex(PcVal[s]); ekseam_puts("*"); ekseam_puthex(PcHits[s]);
			}
		ekseam_puts(" other="); ekseam_puthex(PcOther); ekseam_puts("\n");
		}
	for (TInt s = 0; s < KPcSlots; ++s) PcVal[s] = PcHits[s] = 0;
	PcOther = 0;
	PcThread = aTop;
	static TBool started;
	if (!started) { started = ETrue; PcTimer.OneShot(NKern::TimerTicks(2)); }
	}

// HOTSPOTS (PhoneLab 'H', 2026-10-08, "Calendar's first launch = 9 s of Wserv"): the PC sampler above for EVERY busy
// core and thread, into a (thread, pc) histogram. A64HotStart clears + starts it; A64HotStop stops it and writes:
//   S <samples> <idle samples> <lost (table full)> <period ms>
//   W <NThread address hex> <process name> / <thread name>        every live thread (to name the X lines)
//   X <NThread address hex> <user pc hex> <hits hex> <user lr hex>   (the lr names the caller of a leaf, e.g. an Exec stub)
// The pc is the thread's user pc at its latest kernel entry (the reschedule IPI makes that the sampled moment); a
// kernel-only thread has no user frame, its pc is noise. The tick core's own thread is sampled without an IPI.
const TInt KHotSlots = 8192;
static NThread* HotThr[KHotSlots];
static TUint32 HotPc[KHotSlots], HotLr[KHotSlots], HotHits[KHotSlots];
static TUint32 HotSamples, HotIdle, HotLost;
static volatile TBool HotOn;
static void HotFn(TAny*);
static NTimer HotTimer(HotFn, 0);
static void HotFn(TAny*)
	{
	if (!HotOn)
		return;
	const TInt me = NKern::CurrentCpu();
	for (TInt c = 0; c < NKern::NumberOfCpus() && c < KMaxCpus; ++c)
		{
		NThread* nt = TheSubSchedulers[c].iCurrentThread;
		if (!nt || nt == TheSubSchedulers[c].iInitialThread)
			{ ++HotIdle; continue; }
		++HotSamples;
		if (nt->iStackBase)
			{
			const SThreadExcStack* f = (const SThreadExcStack*)((TUint8*)nt->iStackBase + nt->iStackSize) - 1;
			const TUint32 pc = f->iR15 & ~1u, lr = f->iR14usr & ~1u;
			TUint h = ((pc >> 1) ^ (lr * 40503u) ^ ((TUint32)nt >> 4) * 2654435761u) & (KHotSlots - 1);
			TInt k = 0;
			for (; k < 16; ++k, h = (h + 1) & (KHotSlots - 1))
				{
				if (!HotHits[h]) { HotThr[h] = nt; HotPc[h] = pc; HotLr[h] = lr; }
				if (HotThr[h] == nt && HotPc[h] == pc && HotLr[h] == lr) { ++HotHits[h]; break; }
				}
			if (k == 16) ++HotLost;
			}
		if (c != me)
			send_resched_ipi(c);
		}
	HotTimer.Again(NKern::TimerTicks(2));
	}

extern "C" void A64HotStart()
	{
	HotOn = EFalse;
	HotTimer.Cancel();
	for (TInt s = 0; s < KHotSlots; ++s) { HotThr[s] = 0; HotPc[s] = HotLr[s] = HotHits[s] = 0; }
	HotSamples = HotIdle = HotLost = 0;
	HotOn = ETrue;
	HotTimer.OneShot(NKern::TimerTicks(2));
	}

extern "C" TInt A64HotStop(TUint8* aBuf, TInt aMax)
	{
	HotOn = EFalse;
	HotTimer.Cancel();
	TInt n = ProfPut(aBuf, aMax, 0, "S ");
	n = ProfHex(aBuf, aMax, n, HotSamples);
	n = ProfHex(aBuf, aMax, n, HotIdle);
	n = ProfHex(aBuf, aMax, n, HotLost);
	n = ProfHex(aBuf, aMax, n, 2);
	n = ProfPut(aBuf, aMax, n, "\n");
	for (TInt s = 0; s < KHotSlots; ++s)
		if (HotHits[s])
			{
			n = ProfPut(aBuf, aMax, n, "X ");
			n = ProfHex(aBuf, aMax, n, (TUint32)HotThr[s]);
			n = ProfHex(aBuf, aMax, n, HotPc[s]);
			n = ProfHex(aBuf, aMax, n, HotHits[s]);
			n = ProfHex(aBuf, aMax, n, HotLr[s]);
			n = ProfPut(aBuf, aMax, n, "\n");
			}
	NKern::ThreadEnterCS();
	DObjectCon& thrs = *Kern::Containers()[EThread];
	thrs.Wait();
	for (TInt i = 0; i < thrs.Count(); ++i)
		{
		DThread* t = (DThread*)thrs[i];
		n = ProfPut(aBuf, aMax, n, "W ");
		n = ProfHex(aBuf, aMax, n, (TUint32)&t->iNThread);
		if (t->iOwningProcess)
			{
			TBuf8<KMaxKernelName> pn;
			t->iOwningProcess->Name(pn);
			for (TInt j = 0; j < pn.Length() && n < aMax - 1; ++j) aBuf[n++] = pn[j];
			}
		n = ProfPut(aBuf, aMax, n, " / ");
		n = ProfName(aBuf, aMax, n, t);
		}
	thrs.Signal();
	NKern::ThreadLeaveCS();
	return n;
	}

static void A64MonThreads()
	{
	ekseam_puts("[EKERN][mon] running now:");
	for (TInt c = 0; c < NKern::NumberOfCpus() && c < KMaxCpus; ++c)
		{
		NThread* nt = TheSubSchedulers[c].iCurrentThread;
		ekseam_puts(c ? " | " : " ");
		if (nt && nt != TheSubSchedulers[c].iInitialThread)
			MonPutName(_LOFF(nt, DThread, iNThread));
		else
			ekseam_puts("(idle)");
		}
	ekseam_puts("\n");
	const TInt KSlots = 192;
	static DThread* who[KSlots];
	static TUint64 was[KSlots];
	DThread* top[3] = { 0, 0, 0 };
	TUint64 topd[3] = { 0, 0, 0 };
	DObjectCon& threads = *Kern::Containers()[EThread];
	NKern::ThreadEnterCS();
	threads.Wait();
	for (TInt i = 0; i < threads.Count(); ++i)
		{
		DThread* t = (DThread*)threads[i];
		const TUint64 cur = MonCpuTime(&t->iNThread, NKern::Timestamp());
		TInt s = 0, freeSlot = -1;
		for (; s < KSlots && who[s] != t; ++s)
			if (!who[s] && freeSlot < 0) freeSlot = s;
		TUint64 d = 0;
		if (s < KSlots) { d = cur - was[s]; was[s] = cur; }
		else if (freeSlot >= 0) { who[freeSlot] = t; was[freeSlot] = cur; }
		for (TInt k = 0; k < 3; ++k)
			if (d > topd[k])
				{
				for (TInt m = 2; m > k; --m) { topd[m] = topd[m - 1]; top[m] = top[m - 1]; }
				topd[k] = d; top[k] = t;
				break;
				}
		}
	ekseam_puts("[EKERN][mon] top CPU (ms in 5 s):");
	for (TInt k = 0; k < 3 && top[k]; ++k)
		{
		ekseam_puts(" "); MonPutName(top[k]); ekseam_puts("=");
		const TUint ms = (TUint)(topd[k] * 1000 / NKern::TimestampFrequency());	// (not 24 MHz: read 2x)
		char b[8]; TInt n = 0; TUint v = ms; char r[8]; do { r[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 7);
		for (TInt j = 0; j < n; ++j) b[j] = r[n - 1 - j];
		b[n] = 0; ekseam_puts(b);
		}
	ekseam_puts("\n");
	MonPcReport(top[0]);			// under the container lock: top[0] stays valid while it is recorded
	threads.Signal();
	NKern::ThreadLeaveCS();
	}
#endif

#ifdef __SMP__
// THREAD DUMP (2026-10-07; Path C's xk_threaddump, X4.27, for ARM): "everything waits and nothing says why" --
// the S60 boot stopped after baksrvs with all cores idle. Per thread: name, DThread::iMState, the object it waits
// on (iWaitObj, by name), and for a user thread blocked in an exec call its user pc/lr from the exception frame
// at the top of its supervisor stack (map them with the ROM listing: tools/rom.py <rom> ls).
extern "C" void A64ThreadDump()
	{
	ekseam_puts("[dump] ===== thread dump =====\n");
	DObjectCon& threads = *Kern::Containers()[EThread];
	NKern::ThreadEnterCS();
	threads.Wait();
	for (TInt i = 0; i < threads.Count(); ++i)
		{
		DThread* t = (DThread*)threads[i];
		ekseam_puts("[dump] "); MonPutName(t);
		ekseam_puts(" ms="); ekseam_puthex((unsigned)t->iWaitLink.iSpare1);	// iMState (kern_priv.h)
		if (t->iWaitObj)
			{
			TBuf8<64> n;
			((DObject*)t->iWaitObj)->Name(n);
			n.Append('\0');
			ekseam_puts(" waits on "); ekseam_puts((const char*)n.Ptr());
			}
		NThread& nt = t->iNThread;
		if (t->iThreadType == EThreadUser && nt.iStackBase && t->iWaitLink.iSpare1 != DThread::EDead)
			{
			const SThreadExcStack* f = (const SThreadExcStack*)((TUint8*)nt.iStackBase + nt.iStackSize) - 1;
			if ((f->iCPSR & 0x1f) == 0x10)				// a user-mode frame: blocked in an exec call
				{
				ekseam_puts(" pc="); ekseam_puthex(f->iR15);
				ekseam_puts(" lr="); ekseam_puthex(f->iR14usr);
				}
			}
		ekseam_puts("\n");
		}
	threads.Signal();
	NKern::ThreadLeaveCS();
	ekseam_puts("[dump] ===== end =====\n");
	}
#endif

// the seam calls this after printing a thread PANIC (ekern_seam.cpp DThread::Die): flush the SD log at once -- a
// system-critical panic reboots the phone before the next timed flush (the task switcher crash, 2026-10-07)
static void A64SdFlush(TInt aStage);
extern "C" void ekseam_board_panic() { A64SdFlush(10); }

extern "C" TUint32 TheTickReload;		// interrupts.cpp: CNTFRQ/1000, the 1 ms tick's timer reload
static void SdFlushDfcFn(TAny*);
static TDfc* SdFlushDfc;
static void SdFlushTimerFn(TAny*) { if (SdFlushDfc) SdFlushDfc->Add(); }
static NTimer SdFlushTimer(SdFlushTimerFn, 0);
static TInt SdTimedFlushes;
// the battery-backed RTC (2026-10-06, "the clock starts a couple of minutes behind": kernel time started at the
// ROM's build time). sun6i RTC at 0x01F00000 (Linux rtc-sun6i): +0x10 = day[4:0] month[11:8] year-2000[21:16],
// +0x14 = sec[5:0] min[13:8] hour[20:16]; Linux/Mobian keep it in UTC, as Symbian's system time is.
static void A64RtcToKernelTime()
	{
	DPlatChunkHw* ch = 0;
	NKern::ThreadEnterCS();
	TInt r = DPlatChunkHw::New(ch, 0x01F00000, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		return;
	volatile TUint32* rtc = (volatile TUint32*)ch->LinearAddress();
	TUint32 date, tod;
	do { date = rtc[0x10 / 4]; tod = rtc[0x14 / 4]; } while (date != rtc[0x10 / 4]);	// no midnight tear
	// the year register counts from 1970 (Linux sun6i-rtc, which last set it: 0x38 = 56 = 2026; boot 304 read it as 2056)
	const TInt y = 1970 + ((date >> 16) & 0x3F), mo = (date >> 8) & 0xF, d = date & 0x1F;
	const TInt hh = (tod >> 16) & 0x1F, mm = (tod >> 8) & 0x3F, ss = tod & 0x3F;
	ekseam_puts("[EKERN][rtc] date="); ekseam_puthex(date); ekseam_puts("[EKERN][rtc] time="); ekseam_puthex(tod);
	if (mo < 1 || mo > 12 || d < 1 || y < 2020)
		return;								// never set (Linux not booted since the battery went flat)
	static const TInt KCum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	TInt days = 0;
	for (TInt yy = 1970; yy < y; ++yy) days += (yy % 4 == 0 && (yy % 100 || yy % 400 == 0)) ? 366 : 365;
	days += KCum[mo - 1] + d - 1;
	if (mo > 2 && (y % 4 == 0 && (y % 100 || y % 400 == 0))) ++days;
	const TInt64 secs = 62168256000LL + (TInt64)days * 86400 + hh * 3600 + mm * 60 + ss;	// 1970-01-01 in Symbian's TTime (s)
	r = Kern::SetSystemTime(secs * 1000000, ETimeSetTime | ETimeSetAllowTimeReversal);
	ekseam_puts("[EKERN][rtc] kernel time set from the RTC, r="); ekseam_puthex((unsigned)r);
	}

static void A64BatteryTick();
#ifdef A64_SDLOG_FAST
// A64DEFS=-DA64_SDLOG_FAST (2026-10-09, the NGA boot power cut): the SD log every 1 s with the battery reading, so a
// sudden power loss (no fault, no PowerDown, DRAM lost) still leaves its last second for the slot A replay
static void SdFastDfcFn(TAny*);
static TDfc* SdFastDfc;
static void SdFastTimerFn(TAny*) { if (SdFastDfc) SdFastDfc->Add(); }
static NTimer SdFastTimer(SdFastTimerFn, 0);
extern "C" TInt A64PmicReadBatt(TInt* aOut);
static void BattPrint(const char* aWhy, const TInt* b);
static void SdFastDfcFn(TAny*)
	{	// every 250 ms from the first user thread (the NGA death comes ~4 s in, before the first 5 s flush)
	static TUint n;
	ekseam_puts("[EKERN][fast] tick="); ekseam_puthex(n++);
	TInt b[6];
	if (A64PmicReadBatt(b) == KErrNone)
		BattPrint("fast", b);
	A64SdFlush(0x200);
	SdFastTimer.OneShot(NKern::TimerTicks(250));
	}
#endif
#ifdef __SMP__
// the monitor (2026-10-06; every second since 2026-10-19, PhoneLab's device card): per-core busy % over the last
// second = 100 - the idle thread's share of the elapsed timestamp time (its iTotalCpuTime is brought up to date at
// each switch, so a core idle right now lags by its current idle stretch), free RAM and the kernel heap
static TDfc* MonDfc;
static void MonTimerFn(TAny*) { if (MonDfc) MonDfc->Add(); }
static NTimer MonTimer(MonTimerFn, 0);
static void MonDfcFn(TAny*)
	{
	static TUint64 lastNow, lastIdle[KMaxCpus];
	const TUint64 now = NKern::Timestamp();
	ekseam_puts("\n[EKERN][mon] busy% cpu0..3 =");
	for (TInt c = 0; c < NKern::NumberOfCpus() && c < KMaxCpus; ++c)
		{
		NThread* idle = TheSubSchedulers[c].iInitialThread;
		const TUint64 t = idle ? MonCpuTime(idle, now) : 0;
		TUint busy = 0;
		if (lastNow && now > lastNow)
			{
			const TUint64 di = t - lastIdle[c], dn = now - lastNow;
			busy = di >= dn ? 0 : (TUint)(100 - (di * 100) / dn);
			}
		lastIdle[c] = t;
		ekseam_puts(" "); ekseam_puts(busy >= 100 ? "100" : "");
		if (busy < 100) { char b[3] = { (char)('0' + busy / 10), (char)('0' + busy % 10), 0 }; ekseam_puts(b); }
		}
	lastNow = now;
	ekseam_puts("\n[EKERN][mon] free RAM KB="); ekseam_puthex((unsigned)(Kern::FreeRamInBytes() >> 10));
	{	// the kernel heap (2026-10-07: a 1 MB max ran dry under S60): committed now + the peak
	static TInt kheapPeak = 0;
	const TInt kh = K::Allocator->Size();
	if (kh > kheapPeak) kheapPeak = kh;
	ekseam_puts(" kheap KB="); ekseam_puthex((unsigned)(kh >> 10));
	ekseam_puts(" peak KB="); ekseam_puthex((unsigned)(kheapPeak >> 10));
	}
	ekseam_puts("\n");
	MonTimer.OneShot(NKern::TimerTicks(1000));
	}
#endif

static void SdFlushDfcFn(TAny*)
	{
	if (SdTimedFlushes == 0)
		A64RtcToKernelTime();
	A64BatteryTick();
	A64SdFlush(0x100 + SdTimedFlushes);
	ekseam_puts("[EKERN][tick] seconds since the first user thread="); ekseam_puthex((unsigned)(5 * (SdTimedFlushes + 1)));
#ifdef __SMP__
	{	// the slow part of the monitor (every 5 s): what the cores run, SD I/O, the display stats; cpu % and RAM are
		// in MonDfcFn (every second)
	A64MonThreads();
	ekseam_puts("[EKERN][mon] sd io: calls="); ekseam_puthex(SdStatCalls); ekseam_puts(" sectors="); ekseam_puthex(SdStatSectors);
	ekseam_puts(" ms="); ekseam_puthex(SdStatMs);
	SdStatCalls = SdStatSectors = SdStatMs = 0;
	ekseam_puts("\n");
	A64DisplayFlipStats();					// the NGA posts + the vblank each completed at (a64_display.cpp)
	if (SdTimedFlushes == 12 || SdTimedFlushes == 24)	// 60 s and 120 s after the first user thread
		A64ThreadDump();
	}
#endif
	{	// the clock question (2026-10-06: kernel time ran 6.58x fast): the raw counter + the frequency the tick uses
	TUint32 lo, hi, frq;
	asm volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));	// CNTPCT
	asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(frq));			// CNTFRQ
	ekseam_puts("[EKERN][tick]   CNTPCT lo="); ekseam_puthex(lo);
	ekseam_puts("[EKERN][tick]   CNTFRQ="); ekseam_puthex(frq);
	ekseam_puts("[EKERN][tick]   TheTickReload="); ekseam_puthex(TheTickReload);
	}
	++SdTimedFlushes;						// for the whole run (2026-10-06): with the ring, the SD log = the last ~5 s
	SdFlushTimer.OneShot(NKern::TimerTicks(5000));	// before a death, which the next slot A boot replays
	}

// THE POWER KEY (2026-10-08): every 50 ms a DFC asks the AXP803 for latched PEK edges (a64_display.cpp
// A64PmicKeyPoll) and posts them as EStdKeyDevice2 -- the power key, as the x86 board's F9 and virtinput -- for
// SysAp: a short press = the power menu (lock, profiles), a long press = off.
extern "C" TInt A64PmicKeyPoll();
static void PowerKeyDfcFn(TAny*);
static TDfc* PowerKeyDfc;
static void PowerKeyTimerFn(TAny*) { if (PowerKeyDfc) PowerKeyDfc->Add(); }
static NTimer PowerKeyTimer(PowerKeyTimerFn, 0);
static void PowerKeyEvent(TBool aDown)
	{
	static TBool down;
	if (aDown == down)
		return;
	down = aDown;
	TRawEvent ev;
	ev.Set(aDown ? TRawEvent::EKeyDown : TRawEvent::EKeyUp, EStdKeyDevice2, 0);
	A64CpuInputBoost();
	Kern::AddEvent(ev);
	ekseam_puts(aDown ? "[EKERN][key] power down\n" : "[EKERN][key] power up\n");
	}
// The press edge (0x20) = down, the release edge (0x40) = up -- the key is down exactly as long as it is held, so the
// window server's key repeat lets SysAp time a long press; a key never stays down past 12 s on its own.
static TInt PowerKeyHeld;		// polls since the press; < 0 = a long press (polls left)
static TInt PowerKeyUpIn;		// polls until the pending release is posted (0 = none)
static void PowerKeyDfcFn(TAny*)
	{
	const TInt s = A64PmicKeyPoll();
	if (s > 0)
		{
		ekseam_puts("[EKERN][key] pek bits="); ekseam_puthex((unsigned)s); ekseam_puts("\n");
		// The edges, measured on the phone (boot 468, a held key): bit 5 (0x20) arrives AT THE PRESS, bit 3 (0x08, the
		// PMIC's long-press flag) ~1.5 s later, bit 6 (0x40) AT THE RELEASE -- the PEK pin is active low (the press is its
		// falling edge). Until 2026-10-15 they were read the other way round: a held key went "down" only at the long
		// flag and the menu came at the release. Now: 0x20 = down, 0x40 = up (a tap seen in one poll is held 3 polls =
		// 150 ms: a zero-length key reached no handler); SysAp times the hold itself (2 s menu, 5 s off -- its delta).
		if (s & 0x20)
			{
			PowerKeyEvent(ETrue);
			PowerKeyHeld = 0;
			PowerKeyUpIn = 0;
			}
		if (s & 0x40)
			PowerKeyUpIn = (s & 0x20) ? 3 : 1;
		}
	if (PowerKeyUpIn > 0 && --PowerKeyUpIn == 0)
		PowerKeyEvent(EFalse);
	else if (PowerKeyUpIn == 0 && ++PowerKeyHeld == 240)
		PowerKeyEvent(EFalse);				// safety: no release in 12 s (the PMIC's own 10 s power-off stays untouched)
	PowerKeyTimer.OneShot(NKern::TimerTicks(50));
	}

static void A64StartSdTimer()
	{
	if (!PowerKeyDfc)
		{
		NKern::ThreadEnterCS();
		PowerKeyDfc = new TDfc(PowerKeyDfcFn, 0, Kern::DfcQue0(), 4);
		NKern::ThreadLeaveCS();
		if (PowerKeyDfc)
			PowerKeyTimer.OneShot(NKern::TimerTicks(50));
		}
#ifdef __SMP__
	if (!MonDfc)
		{
		NKern::ThreadEnterCS();
		MonDfc = new TDfc(MonDfcFn, 0, Kern::DfcQue0(), 3);
		NKern::ThreadLeaveCS();
		if (MonDfc)
			MonTimer.OneShot(NKern::TimerTicks(1000));
		}
#endif
	if (SdFlushDfc || !Smhc)
		return;
	NKern::ThreadEnterCS();
	SdFlushDfc = new TDfc(SdFlushDfcFn, 0, Kern::DfcQue0(), 3);
	NKern::ThreadLeaveCS();
	if (SdFlushDfc)
		SdFlushTimer.OneShot(NKern::TimerTicks(5000));
#ifdef A64_SDLOG_FAST
	NKern::ThreadEnterCS();
	SdFastDfc = new TDfc(SdFastDfcFn, 0, Kern::DfcQue0(), 1);
	NKern::ThreadLeaveCS();
	if (SdFastDfc)
		SdFastTimer.OneShot(NKern::TimerTicks(250));
#endif
	}

// POWER CONTROLLER (2026-10-06): SSM's shutdown (and the power key, later) ends in Power::PowerDown; without a
// registered controller the power manager's path faulted the kernel (TDfc::Enque, a null DFC). EPwOff = the AXP803
// cuts power (a64_display.cpp A64PmicPowerOff); standby is not implemented yet -- PowerDown returns = stays up.
// IDLE (2026-10-07): NKIdle (kernel/arm/cinit.cpp) calls the power controller's CpuIdle -- the kernel's default
// controller has an empty one too, so until now every idle core SPUN at full power instead of sleeping: a core
// with nothing to run drained the battery as if busy. WFI with interrupts masked still wakes on any pending
// interrupt (the tick, a reschedule IPI), which is what the SMP idle path expects.
extern "C" TInt A64PmicMap();
extern "C" TInt A64PmicPowerOff();
extern "C" TInt A64PmicChargeSetup();
extern "C" TInt A64PmicReadBatt(TInt* aOut);
extern "C" TInt A64PmicChargeAssert();
extern "C" void A64StatusBattery(TInt aPct, TBool aCharging, TInt aMv);
// SWITCH-OFF DIAGNOSIS (2026-10-08, "Switch off = white screen, never powers off"): the power manager asks every
// DPowerHandler (driver) to power down and waits for each PowerDownDone(); SysAp's switch off sat in Exec::PowerDown
// forever (PhoneLab threads: !SusAdaptionSrv in Exec::PowerDown) -- one driver never answered, and with Register()'s
// timeout of 0 the kernel waits for ever. Now: Register(KA64ShutdownTimeoutMs), and every PowerDownDone is logged
// (link_ekern.sh --wrap); at PowerDown the controller lists every handler, so the missing "done" names the culprit.
// DPowerHandler's private layout AS COMPILED (objdump of ekern.elf: the ctor stores the TDesC8& at +8, Add() links
// +12/+16, Done() swaps +20) -- NOT kpower.h's order: vtable, ?, const TDesC8& iName (+8), iNext (+12), iPrev (+16),
// iSem (+20). (A first try read the name at +4 and faulted the kernel on "SYBO" text, 2026-10-08.)
const TUint KA64ShutdownTimeoutMs = 5000;
static DPowerHandler* PwSeen;
static void PwPutName(DPowerHandler* aH)
	{
	ekseam_puts("vt="); ekseam_puthex(*(TUint32*)aH);		// symbolise: the driver's class
	const TDesC8* n = *(const TDesC8**)((TUint8*)aH + 8);
	char b[48];
	TInt i = 0;
	for (; n && i < n->Length() && i < 47; ++i) b[i] = (char)(*n)[i];
	b[i] = 0;
	ekseam_puts(" name="); ekseam_puts(b);
	}
extern "C" void __real__ZN13DPowerHandler13PowerDownDoneEv(DPowerHandler*);
extern "C" void __wrap__ZN13DPowerHandler13PowerDownDoneEv(DPowerHandler* aH)
	{
	PwSeen = aH;
	ekseam_puts("[EKERN][power] PowerDownDone: "); PwPutName(aH); ekseam_puts("\n");
	__real__ZN13DPowerHandler13PowerDownDoneEv(aH);
	}
static void PwListHandlers()
	{
	if (!PwSeen)
		{ ekseam_puts("[EKERN][power] no handler answered PowerDown\n"); return; }
	ekseam_puts("[EKERN][power] handlers:");
	DPowerHandler* h = PwSeen;
	TInt guard = 0;
	do
		{
		ekseam_puts(" "); PwPutName(h);
		h = *(DPowerHandler**)((TUint8*)h + 12);	// iNext
		} while (h && h != PwSeen && ++guard < 64);
	ekseam_puts("\n");
	}

// RESTART (2026-10-15, the power menu's Restart): every restart ends in Kern::Restart -> the ROM header's restart
// vector (a64.s RestartEntry: PSCI SYSTEM_RESET) -- the power controller never sees it (boot 473: the restart came back
// in golden A). Wrapped (link_ekern.sh BOARD_WRAPS): a restart from slot B re-arms B (one-shot) so the same image
// comes back; from golden A it is A again. The SD log gets its last lines first.
// ONLY a deliberate restart (the power controller's EPwRestart sets RestartDeliberate) re-arms B: a kernel fault
// (K::Fault -> Kern::Restart, e.g. a system-critical WSERV panic) must fall back to golden A, else a crashing slot-B
// image loops forever (2026-10-16, boots 687-723: the user had to catch a 1 s window to force golden).
static volatile TBool RestartDeliberate;
extern "C" void __real__ZN4Kern7RestartEi(TInt aMode);
extern "C" void __wrap__ZN4Kern7RestartEi(TInt aMode)
	{
	const TBool slotB = BootCtlState == KStTrying || BootCtlState == KStGood;
	const TBool again = slotB && RestartDeliberate;
	ekseam_puts(again ? "[EKERN][power] restart -> slot B again\n"
				: slotB ? "[EKERN][power] unplanned restart (fault) on slot B -> golden A\n" : "[EKERN][power] restart\n");
	if (again)
		A64SlotRearm();
	A64SdFlush(0x0ff);
	__real__ZN4Kern7RestartEi(aMode);
	}

class DA64PowerController : public DPowerController
	{
public:
	void CpuIdle()
		{
#ifndef A64_NO_WFI		// A64DEFS=-DA64_NO_WFI: the spinning idle of before (an experiment switch)
		asm volatile("dsb\n\twfi" : : : "memory");
#endif
		}
	void EnableWakeupEvents() {}
	void DisableWakeupEvents() {}
	void AbsoluteTimerExpired() {}
	void PowerDown(TTimeK /*aWakeupTime*/)
		{
		ekseam_puts("[EKERN][power] PowerDown, target state="); ekseam_puthex((unsigned)iTargetState);
		PwListHandlers();
		if (iTargetState == EPwOff)
			{
			A64SdFlush(0x0ff);							// the last words reach the SD log
			const TInt r = A64PmicPowerOff();
			ekseam_puts("[EKERN][power] AXP803 power-off FAILED r="); ekseam_puthex((unsigned)r);	// (only if still here)
			}
		else if (iTargetState == EPwRestart)
			{
			RestartDeliberate = ETrue;
			Kern::Restart(0);
			}		// SSM's restart ends HERE (boot 484: target 3 returned = a half shut-down phone);
									// Kern::Restart is wrapped: slot B re-armed, the SD log flushed, PSCI reset
		}
	};
// POWER HAL (2026-10-07): once a controller registers, the kernel's power HAL is the power manager's, which hands
// every call to a registered DPowerHal -- with none it answered KErrNotSupported (-5), and UserSvr::
// TestBootSequence() took that as TRUE: EWSRV stopped starting ESHELL (blue screen, the golden with power
// management). This answers as the kernel's default HAL did (sinit.cpp PowerHal), with the real battery.
class DA64PowerHal : public DPowerHal
	{
public:
	TInt PowerHalFunction(TInt aFunction, TAny* a1, TAny* /*a2*/);
	};
static void A64PowerInit()
	{
#ifdef A64_NO_POWERCTL
	return;		// A64DEFS=-DA64_NO_POWERCTL: no controller/HAL (an experiment switch)
#endif
	TInt r = A64PmicMap();
	NKern::ThreadEnterCS();
	DA64PowerController* pc = r == KErrNone ? new DA64PowerController : 0;
	if (pc)
		{
		pc->Register(KA64ShutdownTimeoutMs);
		DA64PowerHal* hal = new DA64PowerHal;
		if (hal)
			hal->Register();
		}
	NKern::ThreadLeaveCS();
	ekseam_puts("[EKERN][power] controller registered r="); ekseam_puthex(pc ? 0 : (unsigned)KErrNoMemory);
	}

// BATTERY (2026-10-07). Thresholds are loaded-cell voltages for a 1-cell Li-ion: below KBattBootMv the full boot
// (4 cores + the panel) can brown the phone out -> reset -> boot again, the loop that flattened it. KBattOffMv =
// the floor: below it, with no charge going in, the AXP803 cuts power before the cell is damaged.
const TInt KBattBootMv = 3450, KBattResumeMv = 3600, KBattOffMv = 3300;
static TInt BattLast[6];

static void BattPrint(const char* aWhy, const TInt* b)
	{
	ekseam_puts("[EKERN][batt] "); ekseam_puts(aWhy);
	ekseam_puts(" mV="); ekseam_puthex((unsigned)b[2]);
	ekseam_puts("[EKERN][batt]   fuel%="); ekseam_puthex((unsigned)b[3]);
	ekseam_puts("[EKERN][batt]   chg mA="); ekseam_puthex((unsigned)b[4]);
	ekseam_puts("[EKERN][batt]   dis mA="); ekseam_puthex((unsigned)b[5]);
	ekseam_puts("[EKERN][batt]   REG00/01="); ekseam_puthex((unsigned)((b[0] << 8) | (b[1] & 0xFF)));
	}

static TBool BattCharging(const TInt* b) { return (b[1] & 0x40) || b[4] > 0; }

static void A64PowerOffNow(const char* aWhy)
	{
	ekseam_puts("[EKERN][batt] POWER OFF: "); ekseam_puts(aWhy); ekseam_puts("\n");
	A64SdFlush(0x0fe);
	const TInt r = A64PmicPowerOff();
	ekseam_puts("[EKERN][batt] power-off FAILED r="); ekseam_puthex((unsigned)r);
	}

// Stage 3 (supervisor thread, before the panel and the user side start): charging on, then -- if the cell is too
// low to boot -- wait here, display off, idle cores in WFI, until it has charged. LED: a short red blink every 5 s
// = charging-wait. No charge going in and below the floor = power off (else it would drain to damage).
static void A64BatteryGuard()
	{
#ifdef A64_NO_PMIC
	return;		// A64DEFS=-DA64_NO_PMIC: QEMU's H3 (run_h3.sh) has no R_PRCM/RSB block -- the access aborts
#endif
	if (A64PmicMap() != KErrNone)
		return;
	TInt r = A64PmicChargeSetup();
	ekseam_puts("[EKERN][batt] charger setup r="); ekseam_puthex((unsigned)r);
	r = A64PmicChargeAssert();						// + the TS pin (temperature gate off) and the input limit
	ekseam_puts("[EKERN][batt] input REG84/2F/35/30 at boot="); ekseam_puthex((unsigned)r);
	TInt b[6];
	if (A64PmicReadBatt(b) != KErrNone)
		return;
	BattPrint("at boot", b);
	memcpy(BattLast, b, sizeof(b));
	A64StatusBattery(b[3], BattCharging(b), b[2]);
	if (b[2] >= KBattBootMv)
		return;
	TInt noCharge = 0;
	for (TInt n = 0; ; ++n)
		{
		if (BattCharging(b))
			noCharge = 0;
		else if (++noCharge >= 12 || (noCharge >= 6 && b[2] < KBattOffMv))	// 30-60 s with nothing going in
			A64PowerOffNow("battery too low to boot, not charging");	// (off, the AXP803 still charges)
		if (b[2] >= KBattResumeMv)
			break;
		if ((n % 12) == 0)
			{
			BattPrint("charge-wait", b);
			A64SdFlush(0x0c0);
			}
		A64Leds(KLedRed);
		NKern::Sleep(NKern::TimerTicks(100));
		A64Leds(0);
		WdogKick();
		NKern::Sleep(NKern::TimerTicks(4900));
		WdogKick();
		if (A64PmicReadBatt(b) != KErrNone)
			break;
		}
	BattPrint("charged enough -- booting", b);
	memcpy(BattLast, b, sizeof(b));
	A64StatusBattery(b[3], BattCharging(b), b[2]);
	}

// Every 30 s from the monitor DFC: log, and power off before the cell goes below the floor with no charger.
static void A64BatteryTick()
	{
#ifdef A64_NO_PMIC
	return;
#endif
	static TInt n, low, lastInput = -1;
	if (A64ThsTick())								// thermal: 110 C twice in a row = the existing switch-off path
		A64PowerOffNow("thermal critical, >= 110 C");
	const TInt in = A64PmicChargeAssert();			// every 5 s: BC detection at plug-in resets the input limit
	const TBool changed = in >= 0 && lastInput >= 0 && (in & 3) != (lastInput & 3);
	if (in >= 0) lastInput = in;
	if ((++n % 6) != 0 && !changed)
		return;
	ekseam_puts("[EKERN][batt] input REG84/2F/35/30 (before re-assert)="); ekseam_puthex((unsigned)in);
	TInt b[6];
	if (A64PmicReadBatt(b) != KErrNone)
		return;
	memcpy(BattLast, b, sizeof(b));
	BattPrint("now", b);
	A64StatusBattery(b[3], BattCharging(b), b[2]);
	low = (b[2] < KBattOffMv && !BattCharging(b)) ? low + 1 : 0;
	if (low >= 2)
		A64PowerOffNow("battery critical, not charging");
	}

extern "C" int a64wlan_is_probed(void);				// a64wlan_glue.c (the rtw88 probe at boot)
extern "C" void a64wlan_mac_addr(unsigned char out[6]);

TInt DA64PowerHal::PowerHalFunction(TInt aFunction, TAny* a1, TAny*)
	{
	if (aFunction == EPowerHalTestBootSequence)
		return 0;
	if (aFunction == EPowerHalSupplyInfo)
		{
		const TInt mv = BattLast[2];
		TSupplyInfoV1 info;
		memclr(&info, sizeof(info));
		info.iMainBatteryStatus = mv <= 0 ? EGood : mv < KBattOffMv ? EZero : mv < KBattBootMv ? EVeryLow
			: mv < 3650 ? ELow : EGood;
		info.iMainBatteryMilliVolts = mv > 0 ? mv : 3700;
		info.iMainBatteryMaxMilliVolts = 4200;
		info.iBackupBatteryStatus = EGood;
		info.iBackupBatteryMilliVolts = 3000;
		info.iBackupBatteryMaxMilliVolts = 3000;
		info.iExternalPowerPresent = BattCharging(BattLast);
		Kern::InfoCopy(*(TDes8*)a1, TPtrC8((const TUint8*)&info, sizeof(info)));
		return KErrNone;
		}
	if (aFunction == 0x574C)
		{	// OURS (2026-10-10, WiFi W3): the WLAN MAC address (the chip's efuse, read by the rtw88 probe at boot) for
			// our wlanhwinit.dll (src/wlanhwinit): the engine refuses to start on the stub's 00:E0:DE:AD:BE:EF
		if (!a64wlan_is_probed())
			return KErrNotReady;
		TUint8 mac[6];
		a64wlan_mac_addr(mac);
		Kern::InfoCopy(*(TDes8*)a1, TPtrC8(mac, 6));
		return KErrNone;
		}
	if (aFunction == 0x5047)
		{	// OURS (2026-10-16): the UI-up / liveness ping of the hang guard (UiGuardTripped; sent by the HWRM plug-in)
		UiGuardPing();
		return KErrNone;
		}
	if (aFunction == 0x5049)
		{	// OURS (2026-10-17): the boot-guard timeout in seconds (a1), from the developer menu (UiGuardTripped); clamped to 30..3600
		const TInt secs = (TInt)(TLinAddr)a1;
		if (secs < 30 || secs > 3600)
			return KErrArgument;
		UiBootGuardS = (TUint32)secs;
		return KErrNone;
		}
	if (aFunction == 0x5048)
		{	// OURS (2026-10-08): the battery for HWRM's power-state plug-in (src/phorepoc/hwrm/PowerStatePlugin.cpp),
			// which turns it into the KPSUidHWRMPowerState keys SysAp shows in Avkon's battery pane:
			// [0] fuel gauge % (-1 = not valid), [1] charging, [2] battery mV, [3] VBUS/ACIN present
		TInt out[4] = { BattLast[3], BattCharging(BattLast), BattLast[2], (BattLast[0] & 0xA0) != 0 };
		if (BattLast[2] <= 0)
			return KErrNotReady;			// no reading yet
		Kern::InfoCopy(*(TDes8*)a1, TPtrC8((const TUint8*)out, sizeof(out)));
		return KErrNone;
		}
	return KErrNotSupported;
	}

// The CPU-speed probe (2026-10-17, alf-plan 5ar: a user ALU loop ran ~65 ns/iteration, 20x too slow). Bounded and
// one-shot: the real core clock = PMU cycle counter against CNTPCT (24 MHz) over 10 ms, the A53 control registers,
// a kernel-side ALU loop (same shape as phgl's user loop) and the memory attributes the MMU gives this code and this
// stack (PAR after ATS1CPR: [0] fault, [9] NOS, [7] SH, [6:4] inner, [3:2] outer -- 5/7 = WB, 0 = NC/device).
// PMU access from EL1 needs MDCR_EL2 = no traps (a53shim/shim.S sets it).
extern "C" void A64CpuFreqReport();		// power/a64_cpufreq.cpp
static void A64CpuProbe(const char* aWhen)
	{
	TUint32 pmcr, lo, hi, ectlrLo, ectlrHi, l2ctlr, actlr, sctlr, c0, c1;
	asm volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));
	asm volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"((pmcr & ~8u) | 5u));	// E, reset PMCCNTR, D = 0 (every cycle)
	asm volatile("mcr p15, 0, %0, c14, c15, 7" :: "r"(0u));					// PMCCFILTR: count EL1 + EL0
	asm volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(0x80000000u));			// PMCNTENSET: the cycle counter
	asm volatile("isb");
	TUint32 t0;
	asm volatile("isb; mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
	t0 = lo;
	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c0));
	do
		asm volatile("isb; mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
	while (lo - t0 < 240000u);													// 10 ms of 24 MHz
	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c1));
	const TUint32 cyc = c1 - c0, ticks = lo - t0;
	// kernel ALU loop: 1,048,576 dependent add/shift/xor, timed by CNTPCT
	TUint32 x = 1, n = 1u << 20;
	asm volatile("isb; mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
	t0 = lo;
	asm volatile("1: add %0, %0, %0, lsl #3\n\teor %0, %0, %0, lsr #7\n\tsubs %1, %1, #1\n\tbne 1b"
				 : "+r"(x), "+r"(n) :: "cc");
	asm volatile("isb; mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
	const TUint32 aluUs = (lo - t0) / 24;
	asm volatile("mrrc p15, 1, %0, %1, c15" : "=r"(ectlrLo), "=r"(ectlrHi));	// CPUECTLR (bit 6 = SMPEN)
	asm volatile("mrc p15, 1, %0, c9, c0, 2" : "=r"(l2ctlr));
	asm volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr));
	asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
	TUint32 parCode, parStack, mpidr;
	asm volatile("mcr p15, 0, %1, c7, c8, 0; isb; mrc p15, 0, %0, c7, c4, 0" : "=r"(parCode) : "r"((TUint32)&A64CpuProbe));
	asm volatile("mcr p15, 0, %1, c7, c8, 0; isb; mrc p15, 0, %0, c7, c4, 0" : "=r"(parStack) : "r"((TUint32)&mpidr));
	asm volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
	ekseam_puts("[EKERN][cpuprobe] "); ekseam_puts(aWhen);
	ekseam_puts(" cpu="); ekseam_puthex(mpidr & 0xff);
	ekseam_puts(" MHz(PMCCNTR/10ms)="); ekseam_puthex(ticks ? (TUint32)((TUint64)cyc * 24 / ticks) : 0);
	ekseam_puts(" cyc="); ekseam_puthex(cyc);
	ekseam_puts(" aluLoop1M_us="); ekseam_puthex(aluUs); ekseam_puts(" x="); ekseam_puthex(x);
	ekseam_puts(" CPUECTLR="); ekseam_puthex(ectlrHi); ekseam_puthex(ectlrLo);
	ekseam_puts(" L2CTLR="); ekseam_puthex(l2ctlr);
	ekseam_puts(" ACTLR="); ekseam_puthex(actlr);
	ekseam_puts(" SCTLR="); ekseam_puthex(sctlr);
	ekseam_puts(" PARcode="); ekseam_puthex(parCode);
	ekseam_puts(" PARstack="); ekseam_puthex(parStack);
	ekseam_puts("\n");
	A64CpuFreqReport();
	}

// ~60 s after stage 4 (the governor has had time to step down): the probe again, on EVERY core (a probe thread that
// moves itself across the cores with an affinity change; a retired core simply never answers -- bounded, one-shot).
static TDfcQue* ProbeQ;
static void ProbeLater(TAny*)
	{
	NKern::Sleep(NKern::TimerTicks(60000));
	for (TInt c = 0; c < NKern::NumberOfCpus(); ++c)
		{
		NKern::ThreadSetCpuAffinity(NKern::CurrentThread(), c);
		NKern::Sleep(1);								// the migration happens at the next reschedule
		A64CpuProbe("+60s");
		}
	}
static TDfc* ProbeDfc;

extern "C" void ekseam_board_stage(int aStage)
	{
	switch (aStage)
		{
	case 3: A64Leds(KLedGreen); WdogKick();
#ifdef A64_NO_WDOG
			// A64DEFS=-DA64_NO_WDOG: an experiment build that switches off the watchdog boot.cmd armed (WDOG0 MODE = 0)
			*(volatile TUint32*)(KHwBaseCcuPio + 0xCB8) = 0;
			ekseam_puts("[EKERN][slot] NO_WDOG build: watchdog switched off\n");
#endif
			A64CheckTimer(); A64MapSd(); A64SlotBootInfo(); A64SdFlush(3);
			A64BatteryGuard(); A64Leds(KLedGreen);
			A64PowerInit();		// here, not at stage 4: registering it inside the first user thread's entry wrapper
			break;				// made user programs jump to 0 (2026-10-07; reproduced on QEMU H3)
	case 4: A64Leds(KLedRed | KLedGreen | KLedBlue); WdogKick();
			{	// what the CPU really runs with (2026-10-06, the "slow UI" question): SCTLR (C = bit 2), ACTLR (SMP = bit 6)
			TUint32 sctlr, actlr, cpacr;
			asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
			asm volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr));
			asm volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(cpacr));
			ekseam_puts("[EKERN][cpu] SCTLR="); ekseam_puthex(sctlr);
			ekseam_puts("[EKERN][cpu] ACTLR="); ekseam_puthex(actlr);
			ekseam_puts("[EKERN][cpu] CPACR="); ekseam_puthex(cpacr);
			}
			{	// the real clock + A53 state, ONCE: stage 4 is signalled at EVERY user thread start (ekern_seam.cpp)
			static TInt probed = 0;
			if (__e32_atomic_swp_ord32(&probed, 1) == 0)
				A64CpuProbe("stage4");
			}
#ifdef A64_HANG_TEST
			// A64DEFS=-DA64_HANG_TEST: the A/B fallback test -- a "broken" build that never feeds the watchdog again
			ekseam_puts("[EKERN][slot] HANG TEST build: not feeding the watchdog -- expect a reset in 16 s\n");
#else
			CrumbMap();
			WdogTimer.OneShot(NKern::TimerTicks(2000));
#endif

			A64SdFlush(4); A64StartSdTimer(); break;
	case 5: A64Leds(KLedGreen | KLedBlue); A64SdFlush(5); break;
	case 9:
#ifdef A64_RTC_TAIL
		RtcTailFrozen = ETrue;
#endif
		A64Leds(KLedRed | KLedBlue); A64SdFlush(9); break;
	default: break;
		}
	}

// The --wrap pass-throughs link_ekern.sh applies to every board live in tools/kernel/ekern_fb_glue.cpp (linked
// for a64 since the display, 2026-10-06).

// Kern::ThreadDesRead for a KERNEL thread reading its own (kernel) memory: the real one copies with user-privilege
// loads, which fault on kernel addresses. phlink (a kernel-side USB client) hands the USB PIL kernel descriptors
// where an LDD would hand user ones (SetCSInterfaceDescriptorBlock -> r=-38 on the phone, 2026-10-05). User
// threads -- every other caller -- go to the real function unchanged.
extern "C" TInt __real__ZN4Kern13ThreadDesReadEP7DThreadPKvR5TDes8ii(DThread*, const TAny*, TDes8&, TInt, TInt);
extern "C" TInt __wrap__ZN4Kern13ThreadDesReadEP7DThreadPKvR5TDes8ii(DThread* aThread, const TAny* aSrc, TDes8& aDest,
																	TInt aOffset, TInt aMode)
	{
	if (aThread && aThread->iThreadType != EThreadUser)
		{
		const TDesC8& src = *(const TDesC8*)aSrc;
		if (aOffset < 0 || aOffset > src.Length())
			return KErrArgument;
		const TPtrC8 part = src.Mid(aOffset);
		if (part.Length() > aDest.MaxLength())
			return KErrOverflow;
		aDest.Copy(part);
		return KErrNone;
		}
	return __real__ZN4Kern13ThreadDesReadEP7DThreadPKvR5TDes8ii(aThread, aSrc, aDest, aOffset, aMode);
	}

// ekseam_load_fb_driver: tools/kernel/ekern_fb_glue.cpp (Syborg's framebuffer driver on a64fb.cpp); the panel
// bring-up (display/a64_display.cpp) starts from the driver's FB_ENABLED write.

// ekseam_load_input_drivers: input/a64touch.cpp (the GT917S touch panel)

extern "C" void ekseam_load_media_drivers()
	{
	TInt r = InitExtension_locmedia();
	ekseam_puts("[EKERN][med] locmedia (ELOCD) InitExtension r="); ekseam_puthex((unsigned)r);
	DLogicalDevice* ld = CreateLogicalDevice_locmedia();
	r = ld ? Kern::InstallLogicalDevice(ld) : KErrNoMemory;
	ekseam_puts("[EKERN][med] InstallLogicalDevice(LocDrv) r="); ekseam_puthex((unsigned)r);
	DPhysicalDevice* pd = CreatePhysicalDevice_medint();
	r = pd ? Kern::InstallPhysicalDevice(pd) : KErrNoMemory;
	ekseam_puts("[EKERN][med] InstallPhysicalDevice(Media.IRam) r="); ekseam_puthex((unsigned)r);
	r = InitExtension_medint();
	ekseam_puts("[EKERN][med] medint InitExtension (IRAM = local drive 0 = D:) r="); ekseam_puthex((unsigned)r);
	// C: = SD partition 3 (blk/a64sdblk.cpp: Syborg's NVMemory driver on a model over the SD PIO) = local drive 1
	pd = CreatePhysicalDevice_mednvme();
	r = pd ? Kern::InstallPhysicalDevice(pd) : KErrNoMemory;
	ekseam_puts("[EKERN][med] InstallPhysicalDevice(Media.MEDNVMEMORY on SD P3) r="); ekseam_puthex((unsigned)r);
	r = InitExtension_mednvme();
	ekseam_puts("[EKERN][med] mednvmemory InitExtension (SD P3 = local drive 1 = C:) r="); ekseam_puthex((unsigned)r);
	// E: = SD partition 4 (the same driver, a second copy: blk/a64sdblk.cpp -DA64SDBLK_E) = local drive 2
	pd = CreatePhysicalDevice_mednvmeE();
	r = pd ? Kern::InstallPhysicalDevice(pd) : KErrNoMemory;
	ekseam_puts("[EKERN][med] InstallPhysicalDevice(Media.MEDNVMEMORY.E on SD P4) r="); ekseam_puthex((unsigned)r);
	r = InitExtension_mednvmeE();
	ekseam_puts("[EKERN][med] mednvmemory InitExtension (SD P4 = local drive 2 = E:) r="); ekseam_puthex((unsigned)r);
	// USB (step 2): the A64 device controller under the real USB client PIL, then the phlink CDC ACM function
	r = InitExtension_usbcc();
	ekseam_puts("[EKERN][usb] usbcc (A64 MUSB PSL + PIL) InitExtension r="); ekseam_puthex((unsigned)r);
	if (r == KErrNone)
		{
		r = PhLinkStart();
		ekseam_puts("[EKERN][usb] phlink (CDC ACM debug link) r="); ekseam_puthex((unsigned)r);
		}
		// the GPU: Mali-400 MP2 driver, RMaliChannel (gpu/mali_a64.cpp + mali/mali_ldd.cpp; NGA plan rung G3)
	A64MaliInstall();
	// the Video Engine: phevde, RPheVde (vde/phevde_a64.cpp + ../vde/phevde_ldd.cpp; VDU plan rung V0)
	A64VdeInstall();
	// the DE2 video overlay plane: PheDeOv, RPheDeOv (display/a64deov.cpp + a64_display.cpp; de2-overlay-plan.md)
	A64DeOvInstall();
	// power plan P5: the SMP idle handler (tick suppression; idle cores can be retired by the governor's core control)
#ifdef __SMP__
	A64IdleStart();
#endif
	// power plan P1/P2 (power/a64_ths.cpp, a64_cpufreq.cpp): the thermal sensor; the CPU OPP driver is only mapped here
	// (the CPU frequency is NOT changed at boot -- U-Boot's setting stays). TEST HOOK (left to the owner): a power
	// HAL function calling A64CpuSetOpp(aIndex) / A64CpuCurrentMHz() from PowerHalFunction (thread context, OK).
#ifndef A64_NO_PMIC
	A64ThsInit();
	A64CpuFreqInit();
#ifdef A64_OPP_TEST
	A64CpuOppSelfTest();					// power plan P2 (A64DEFS=-DA64_OPP_TEST)
#else
	A64CpuGovernorStart();					// power plan P3: the load-monitor governor (A64_NO_GOVERNOR = off)
#endif
#endif
	// the CPU-speed probe ~60 s on, on every core (A64CpuProbe above; created here, in a plain kernel context)
	{
	_LIT8(KProbeQName, "CpuProbe");
	if (Kern::DfcQCreate(ProbeQ, 12, &KProbeQName) == KErrNone && (ProbeDfc = new TDfc(ProbeLater, NULL, ProbeQ, 1)) != NULL)
		ProbeDfc->Enque();
	}
	// sound: the A64 codec + DMA PDD "SoundSc.A64" under the real ESOUNDSC LDD (sound/a64sound.cpp)
	A64SoundInstall();
	A64KeysInstall();
	A64UartInstall();
	A64SdioInstall();
	A64WlanInstall();
	A64SensorsInstall();
	A64CameraInstall();
	if (ekseam_load_nga_drivers) ekseam_load_nga_drivers();	// the NGA kernel side (surface manager, sg device): tools/kernel/ekern_nga_glue.cpp
	}
