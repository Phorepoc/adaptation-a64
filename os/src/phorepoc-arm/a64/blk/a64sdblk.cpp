// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// OURS (PinePhone track): a persistent C: on the PinePhone's SD card = Syborg's REAL
// NVMemory media driver (oss.FCL.sf.adapt.Qemu/baseport/syborg/mednvmemory/syborg_mednvmemory.cpp, compiled unmodified
// below) on partition 3 of the card (tools/phorepoc-arm/sd_layout.py). The same shape as virt/blk/virtblk.cpp: the
// driver only knows the Syborg NVMemory registers (a shared transfer buffer + offset/size/direction/execute, an
// interrupt, STATUS = the sectors moved), so its ReadReg/WriteReg go to a small model of that device:
//   - the device = a window onto P3 (start + size from the card's MBR, read once);
//   - EXECUTE moves the sectors synchronously with the kernel's SD PIO (a64_glue.cpp A64SdTransfer, SMHC0 polled --
//     the same code that writes the SD log and slot B), then a one-tick NTimer stands in for the completion
//     interrupt and calls the driver's own Isr, whose STATUS read retires the request;
//   - `Interrupt::` in the driver = SdNvInterrupt below (it records the Isr; no GIC line is involved).
// Messages go through ekseam_puts: Kern::Printf only reaches the board log in A64_RDEBUG_LOG builds.
// TEMPORARY in shape: a real SD/MMC stack (DMA, the A64 SMHC interrupt, eMMC = SMHC2) comes later.
// E: (2026-10-08, Nokia's mass memory letter): the same file compiled a second time with -DA64SDBLK_E (link_ekern.sh)
// = the driver on P4, under renamed classes / PDD name / media slot (EFixedMedia2) / local drive 2. P4's size in the
// card image is 0 ("to the end of the card"): the first boot measures the card (a64_glue.cpp A64SdCardSectors) and
// writes the real size into the MBR; ESTART then formats the empty E: (FS_FORMAT_CORRUPT).
#include "locmedia.h"
#include "platform.h"
#include <kernel/kern_priv.h>
#include <assp.h>
#include <syborg_priv.h>
#include "syborg_mednvmemory.h"

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);
extern "C" TBool A64SdTransfer(TUint aSector, TUint aCount, TUint32* aBuf, TBool aWrite);
extern "C" TUint A64SdCardSectors(TUint aFrom, TUint32* aBuf);

#ifdef A64SDBLK_E
#define SDBLK_NAME "[sdblk E:] "
#define SdNvInterrupt SdNvInterruptE
const TInt KMbrPart = 3;				// MBR entry index of P4 (E:)
#else
#define SDBLK_NAME "[sdblk] "
const TInt KMbrPart = 2;				// MBR entry index of P3 (C:) -- sd_layout.py
#endif

const TUint KHwNVMemoryDevice = 0;		// the driver's base argument: unused by the model
static inline TBool IsFatType(TUint8 t) { return t == 0x06 || t == 0x0E || t == 0x0B || t == 0x0C; }	// FAT16 (CHS/LBA), FAT32 (CHS/LBA)

// READ CACHE (2026-10-16, the boot / Applications-grid freezes): the file server's FAT walks, directory scans and file
// reads arrive as thousands of small sequential requests (boot: ~2200 per 5 s, ~7 sectors each; the SD itself was busy
// only ~10% of the time -- the rest is per-request latency). Reads are served from direct-mapped lines of
// KCacheLineSectors, a miss reads the whole line in ONE DMA transfer (read-ahead for the next sectors of the FAT / the
// directory / the file). Writes stay write-THROUGH (nothing is ever only in the cache: a power cut loses nothing the
// card would have kept) and update any resident line. Big reads (>= 2 lines) bypass it. Used from the driver's DFC thread only.
const TUint KCacheLineSectors = 16;						// 8 KB
#ifdef A64SDBLK_E
const TUint KCacheLines = 32;							// E: (mass memory): 256 KB
#else
const TUint KCacheLines = 256;							// C: 2 MB
#endif
const TUint KCacheLineBytes = KCacheLineSectors * 512;
const TUint32 KNoLine = 0xFFFFFFFFu;
// WRITE/READ HISTOGRAM (2026-10-16): who hits the card? 64 KB buckets over the first 64 MB of the partition (C:'s FATs,
// root, early directories); the first KWriteLog writes are also printed one by one (sector + count) for the offline
// mapper tools/phorepoc-arm/sd_sector_map.py
const TUint KHistShift = 7, KHistBuckets = 1024, KWriteLog = 200;

static struct
	{
	TBool iInit;
	TUint iStart, iSectors;			// the partition on the card (P3 = C:, P4 = E:)
	TUint32 iShared, iSharedSize, iOffset, iSize, iDirection, iMoved, iFails, iDone, iFmtSkipped;
	TBool iFormatting;				// a FORMAT request's zero-fill is in the buffer (see the hooks at the bottom)
	DPlatChunkHw* iBufChunk;
	TUint32* iBuf;					// the driver's transfer buffer, mapped for the kernel
	TIsr iIsr;
	TAny* iIsrPtr;
	// the read cache (below)
	DPlatChunkHw* iCacheChunk;
	TUint8* iCache;					// KCacheLines * KCacheLineBytes, or 0 (not allocated / failed: no cache)
	TBool iCacheTried;
	TUint32 iTag[KCacheLines];		// partition line number held by each slot (KNoLine = empty)
	TUint16 iHW[KHistBuckets], iHR[KHistBuckets];	// writes / read requests per KHistShift bucket of partition sectors
	TUint32 iHWBeyond, iHRBeyond;
	TUint32 iCReads, iCReadSectors, iCHits, iCMisses, iCWrites, iCWriteSectors, iCBypass;
	} S;

// the completion "interrupt". It used to be an NTimer one-shot (a 1-tick wait, then the Isr in ISR context); every
// request paid that tick on top of the SD time -- thousands of small sequential requests at boot (the home-screen
// freeze, 2026-10-16). The Isr only reads STATUS and Add()s the driver's session-end DFC, both legal from the thread
// context this runs in (the driver's own DFC thread), so it is called directly: the DFC runs as soon as the launch
// DFC returns.
static void SdDone(TAny*)
	{
	if (S.iIsr)
		S.iIsr(S.iIsrPtr);
	}

class SdNvInterrupt
	{
public:
	static TInt Bind(TInt, TIsr aIsr, TAny* aPtr) { S.iIsr = aIsr; S.iIsrPtr = aPtr; return KErrNone; }
	static TInt Unbind(TInt) { S.iIsr = 0; return KErrNone; }
	static TInt Enable(TInt) { return KErrNone; }
	static TInt Disable(TInt) { return KErrNone; }
	static TInt Clear(TInt) { return KErrNone; }
	};

static void SdBlkInit()
	{
	if (S.iInit)
		return;
	S.iInit = ETrue;
	TUint32 mbr[128];
	ekseam_puts(SDBLK_NAME "init: reading the MBR, context="); ekseam_puthex(NKern::CurrentContext()); ekseam_puts("\n");
	if (!A64SdTransfer(0, 1, mbr, EFalse))
		{
		ekseam_puts(SDBLK_NAME "MBR read FAILED -- C: has no medium\n");
		return;
		}
	const TUint8* m = (const TUint8*)mbr;
	const TUint8* e = m + 446 + 16 * KMbrPart;
	if (m[510] != 0x55 || m[511] != 0xAA || !IsFatType(e[4]))
		{
		ekseam_puts(SDBLK_NAME "no C: partition (P3) on the card -- reflash the card image; type=");
		ekseam_puthex(e[4]); ekseam_puts(" sig="); ekseam_puthex(m[510] | (m[511] << 8)); ekseam_puts("\n");
		return;
		}
	S.iStart = e[8] | (e[9] << 8) | (e[10] << 16) | (e[11] << 24);
	S.iSectors = e[12] | (e[13] << 8) | (e[14] << 16) | (e[15] << 24);
#ifdef A64SDBLK_E
	{	// every boot: P4 must end at the end of the card (the card's CSD). Size 0 = this card's first boot; a wrong
		// size = an earlier bad measurement (a CMD17 probe once wrote ~130 GiB) -- then E:'s boot sector is wiped
		// too, so ESTART formats it afresh at the right size
	TUint32 probe[128];
	const TUint card = A64SdCardSectors(S.iStart, probe);
	ekseam_puts(SDBLK_NAME "card sectors (CSD)="); ekseam_puthex(card); ekseam_puts("\n");
	if (card <= S.iStart + 2048)
		{ S.iSectors = 0; return; }			// (no room, or the CSD read failed: no E:)
	if (S.iSectors != card - S.iStart)
		{
		const TBool wasSet = S.iSectors != 0;
		S.iSectors = card - S.iStart;
		TUint8* w = (TUint8*)mbr + 446 + 16 * KMbrPart;
		for (TInt i = 0; i < 4; ++i) w[12 + i] = (TUint8)(S.iSectors >> (8 * i));
		TBool ok = A64SdTransfer(0, 1, mbr, ETrue);
		if (ok && wasSet)
			{
			memclr(probe, sizeof(probe));
			ok = A64SdTransfer(S.iStart, 1, probe, ETrue);
			}
		ekseam_puts(SDBLK_NAME "P4 set to the end of the card (boot sector wiped="); ekseam_puthex(wasSet);
		ekseam_puts(") ok="); ekseam_puthex(ok); ekseam_puts("\n");
		if (!ok)
			{ S.iSectors = 0; return; }
		}
	}
#else
#ifdef A64_FACTORY_RESET
	{	// FACTORY RESET (2026-10-08, a test build: A64DEFS=-DA64_FACTORY_RESET): C:'s boot sector wiped -> ESTART
		// finds C: corrupt and formats it (FS_FORMAT_CORRUPT; quick now, see the format hooks). EVERY boot of such a
		// build resets C: -- the user-facing mode (picked when switching to B) replaces this
	TUint32 z[128];
	memclr(z, sizeof(z));
	const TBool ok = A64SdTransfer(S.iStart, 1, z, ETrue);
	ekseam_puts(SDBLK_NAME "FACTORY RESET: C: boot sector wiped ok="); ekseam_puthex(ok); ekseam_puts("\n");
	}
#endif
#endif
	ekseam_puts(SDBLK_NAME "partition: start="); ekseam_puthex(S.iStart);
	ekseam_puts(" sectors="); ekseam_puthex(S.iSectors); ekseam_puts("\n");
	}

static inline TUint LMax(TUint a, TUint b) { return a > b ? a : b; }
static inline TUint LMin(TUint a, TUint b) { return a < b ? a : b; }

static void SdCacheInit()
	{
	S.iCacheTried = ETrue;
	const TInt size = (TInt)(KCacheLines * KCacheLineBytes);
	TPhysAddr phys;
	NKern::ThreadEnterCS();
	TInt r = Epoc::AllocPhysicalRam(size, phys, 12);
	if (r == KErrNone)
		{
		r = DPlatChunkHw::New(S.iCacheChunk, phys, size, EMapAttrSupRw | EMapAttrCachedMax);
		if (r != KErrNone)
			Epoc::FreePhysicalRam(phys, size);
		}
	NKern::ThreadLeaveCS();
	if (r == KErrNone)
		{
		S.iCache = (TUint8*)S.iCacheChunk->LinearAddress();
		for (TUint i = 0; i < KCacheLines; ++i)
			S.iTag[i] = KNoLine;
		}
	ekseam_puts(SDBLK_NAME "read cache r="); ekseam_puthex((unsigned)r); ekseam_puts(" bytes="); ekseam_puthex(size); ekseam_puts("\n");
	}

// aSectors (< 2 lines) from the partition-relative aOffset into the driver's buffer, through the cache; EFalse = a transfer failed
static TBool SdCachedRead(TUint aOffset, TUint aCount, TUint8* aDst)
	{
	const TUint last = (aOffset + aCount - 1) / KCacheLineSectors;
	for (TUint line = aOffset / KCacheLineSectors; line <= last; ++line)
		{
		const TUint slot = line % KCacheLines;
		TUint8* base = S.iCache + slot * KCacheLineBytes;
		if (S.iTag[slot] != line)
			{
			S.iTag[slot] = KNoLine;
			const TUint first = line * KCacheLineSectors;
			TUint n = S.iSectors - first;					// the last line of the partition may be short
			if (n > KCacheLineSectors)
				n = KCacheLineSectors;
			if (!A64SdTransfer(S.iStart + first, n, (TUint32*)base, EFalse))
				return EFalse;
			S.iTag[slot] = n == KCacheLineSectors ? line : KNoLine;	// (a short tail line is not kept: it is read whole again)
			++S.iCMisses;
			}
		else
			++S.iCHits;
		// the part of [aOffset, aOffset + aCount) inside this line
		const TUint lo = LMax(aOffset, line * KCacheLineSectors), hi = LMin(aOffset + aCount, (line + 1) * KCacheLineSectors);
		memcpy(aDst + (lo - aOffset) * 512, base + (lo - line * KCacheLineSectors) * 512, (hi - lo) * 512);
		}
	return ETrue;
	}

// a write went through to the card: refresh the resident lines it touched
static void SdCacheWrote(TUint aOffset, TUint aCount, const TUint8* aSrc)
	{
	const TUint last = (aOffset + aCount - 1) / KCacheLineSectors;
	for (TUint line = aOffset / KCacheLineSectors; line <= last; ++line)
		{
		const TUint slot = line % KCacheLines;
		if (S.iTag[slot] != line)
			continue;
		const TUint lo = LMax(aOffset, line * KCacheLineSectors), hi = LMin(aOffset + aCount, (line + 1) * KCacheLineSectors);
		memcpy(S.iCache + slot * KCacheLineBytes + (lo - line * KCacheLineSectors) * 512, aSrc + (lo - aOffset) * 512, (hi - lo) * 512);
		}
	}

// the hottest buckets (by writes, then by reads): bucket n = partition sectors [n << KHistShift, (n + 1) << KHistShift)
static void SdHistTop(const TUint16* aH, const char* aTag)
	{
	TUint16 tmp[KHistBuckets];
	memcpy(tmp, aH, sizeof(tmp));
	for (TInt k = 0; k < 10; ++k)
		{
		TUint best = 0, bi = 0;
		for (TUint i = 0; i < KHistBuckets; ++i)
			if (tmp[i] > best) { best = tmp[i]; bi = i; }
		if (!best)
			break;
		ekseam_puts(SDBLK_NAME); ekseam_puts(aTag); ekseam_puts(" bucket "); ekseam_puthex(bi << KHistShift); ekseam_puts(" n="); ekseam_puthex(best); ekseam_puts("\n");
		tmp[bi] = 0;
		}
	}

static void SdCacheStats()
	{
	ekseam_puts(SDBLK_NAME "stats reads="); ekseam_puthex(S.iCReads); ekseam_puts(" sectors="); ekseam_puthex(S.iCReadSectors);
	ekseam_puts(" lineHits="); ekseam_puthex(S.iCHits); ekseam_puts(" lineMisses="); ekseam_puthex(S.iCMisses);
	ekseam_puts(" bypass="); ekseam_puthex(S.iCBypass); ekseam_puts(" writes="); ekseam_puthex(S.iCWrites);
	ekseam_puts(" wsectors="); ekseam_puthex(S.iCWriteSectors); ekseam_puts(" writesBeyond64M="); ekseam_puthex(S.iHWBeyond);
	ekseam_puts(" readsBeyond64M="); ekseam_puthex(S.iHRBeyond); ekseam_puts("\n");
	SdHistTop(S.iHW, "hotW"); SdHistTop(S.iHR, "hotR");
	}

static void SdBlkExecuteInner()
	{
	S.iMoved = 0;
	if (!S.iBuf && S.iShared)
		{
		NKern::ThreadEnterCS();
		TInt r = DPlatChunkHw::New(S.iBufChunk, S.iShared, S.iSharedSize, EMapAttrSupRw | EMapAttrFullyBlocking);
		NKern::ThreadLeaveCS();
		ekseam_puts(SDBLK_NAME "transfer buffer mapped r="); ekseam_puthex(r); ekseam_puts(" phys="); ekseam_puthex(S.iShared);
		ekseam_puts(" size="); ekseam_puthex(S.iSharedSize); ekseam_puts("\n");
		if (r == KErrNone)
			S.iBuf = (TUint32*)S.iBufChunk->LinearAddress();
		}
	if (S.iFormatting && S.iDirection == NVMEM_TRANSACTION_WRITE && S.iSize && S.iOffset + S.iSize <= S.iSectors)
		{	// a FAT full format's blanking: acknowledged, not written (an SD card needs no zero-fill; the format's own
			// boot sector / FATs / root directory come through ordinary writes after it)
		S.iMoved = S.iSize;
		if (S.iFmtSkipped++ == 0)
			{ ekseam_puts(SDBLK_NAME "format: zero-fill skipped from sector "); ekseam_puthex(S.iOffset); }
		SdDone(0);
		return;
		}
	if (S.iBuf && S.iSize && S.iOffset + S.iSize <= S.iSectors
		&& S.iSize * KDiskSectorSize <= S.iSharedSize)
		{
		const TBool write = S.iDirection == NVMEM_TRANSACTION_WRITE;
		TBool ok;
		if (write)
			{
			ok = A64SdTransfer(S.iStart + S.iOffset, S.iSize, S.iBuf, ETrue);
			if (S.iCache)
				{
				if (ok)
					SdCacheWrote(S.iOffset, S.iSize, (const TUint8*)S.iBuf);
				else
					{	// a failed (possibly half-done) write: forget the lines it touched, the next read goes to the card
					for (TUint l = S.iOffset / KCacheLineSectors; l <= (S.iOffset + S.iSize - 1) / KCacheLineSectors; ++l)
						if (S.iTag[l % KCacheLines] == l)
							S.iTag[l % KCacheLines] = KNoLine;
					}
				}
			++S.iCWrites; S.iCWriteSectors += S.iSize;
			if (S.iCWrites <= KWriteLog)
				{
				ekseam_puts(SDBLK_NAME "W "); ekseam_puthex(S.iOffset); ekseam_puts(" "); ekseam_puthex(S.iSize); ekseam_puts("\n");
				}
			if ((S.iOffset >> KHistShift) < KHistBuckets) { if (S.iHW[S.iOffset >> KHistShift] != 0xFFFF) ++S.iHW[S.iOffset >> KHistShift]; } else ++S.iHWBeyond;
			}
		else
			{
			if (!S.iCacheTried)
				SdCacheInit();
			++S.iCReads; S.iCReadSectors += S.iSize;
			if ((S.iOffset >> KHistShift) < KHistBuckets) { if (S.iHR[S.iOffset >> KHistShift] != 0xFFFF) ++S.iHR[S.iOffset >> KHistShift]; } else ++S.iHRBeyond;
			if (S.iCache && S.iSize < 2 * KCacheLineSectors)
				ok = SdCachedRead(S.iOffset, S.iSize, (TUint8*)S.iBuf);
			else
				{
				++S.iCBypass;
				ok = A64SdTransfer(S.iStart + S.iOffset, S.iSize, S.iBuf, EFalse);
				}
			}
		if (ok)
			{
			S.iMoved = S.iSize;
			if (++S.iDone <= 4)
				{
				ekseam_puts(SDBLK_NAME "ok dir="); ekseam_puthex(S.iDirection); ekseam_puts(" sector="); ekseam_puthex(S.iOffset);
				ekseam_puts(" count="); ekseam_puthex(S.iSize); ekseam_puts("\n");
				}
			if ((S.iCReads + S.iCWrites) % 4096 == 0)
				SdCacheStats();
			SdDone(0);
			return;
			}
		}
	if (++S.iFails <= 8)
		{
		ekseam_puts(SDBLK_NAME "transfer FAILED dir="); ekseam_puthex(S.iDirection); ekseam_puts(" sector="); ekseam_puthex(S.iOffset);
		ekseam_puts(" count="); ekseam_puthex(S.iSize); ekseam_puts(" buf="); ekseam_puthex((TUint)S.iBuf);
		ekseam_puts(" max="); ekseam_puthex(S.iSectors); ekseam_puts("\n");
		}
	SdDone(0);
	}

// C: HAS PRIORITY OVER E: (2026-10-16): the two drives share the card (one SD mutex) but not their DFC threads. E: is the
// 236 GB mass memory: its FAT32 mount scans the whole FAT (~31 MB) in a background thread at every boot with an
// unclean volume, in 128 KB bursts of ~6 ms each, which queued the home screen's C: reads behind them. E: now waits
// (1 ms steps, at most 25 ms per request) while a C: request is being served; C: counts itself in A64SdCBusy.
#ifdef A64SDBLK_E
extern "C" volatile TInt A64SdCBusy;
#else
extern "C" { volatile TInt A64SdCBusy; }
#endif
static void SdBlkExecute()
	{
#ifdef A64SDBLK_E
	for (TInt i = 0; i < 25 && A64SdCBusy; ++i)
		NKern::Sleep(NKern::TimerTicks(1));
	SdBlkExecuteInner();
#else
	++A64SdCBusy;
	SdBlkExecuteInner();
	--A64SdCBusy;
#endif
	}

static TUint32 SdBlkReadReg(TUint32 /*aBase*/, TUint32 aReg)
	{
	SdBlkInit();
	switch (aReg)
		{
	case R_NVMEM_NV_MEMORY_SIZE:
		return S.iSectors;
	case R_NVMEM_STATUS:				// the Isr's read: what the last EXECUTE moved
		return S.iMoved;
	default:
		return 0;
		}
	}

static void SdBlkWriteReg(TUint32 /*aBase*/, TUint32 aReg, TUint32 aVal)
	{
	SdBlkInit();
	switch (aReg)
		{
	case R_NVMEM_SHARED_MEMORY_BASE: S.iShared = aVal; break;
	case R_NVMEM_SHARED_MEMORY_SIZE: S.iSharedSize = aVal; break;
	case R_NVMEM_TRANSACTION_OFFSET: S.iOffset = aVal; break;
	case R_NVMEM_TRANSACTION_SIZE: S.iSize = aVal; break;
	case R_NVMEM_TRANSACTION_DIRECTION: S.iDirection = aVal; break;
	case R_NVMEM_TRANSACTION_EXECUTE: SdBlkExecute(); break;
	default: break;
		}
	}

#ifdef A64SDBLK_E
// the second copy of the driver: its own names, its own media slot + local drive
#include "variantmediadef.h"
#define DPhysicalDeviceMediaNVMemory DPhysicalDeviceMediaNVMemoryE
#define DMediaDriverNVMemory DMediaDriverNVMemoryE
#define EFixedMedia1 EFixedMedia2
_LIT(KNVMemPddNameE, "Media.MEDNVMEMORY.E");
#define SetName(aName) SetName(&KNVMemPddNameE)
#undef NVMEM1_DRIVELIST
#define NVMEM1_DRIVELIST 2				// E: = local drive 2 (estart.txt)
#undef NVMEM1_DRIVENAME
#define NVMEM1_DRIVENAME "NVMemE"
#endif
// FORMAT = no zero-fill (2026-10-08: ESTART's boot-time format is a FULL one -- it zeroed the 254 GB E: over PIO for
// minutes before we stopped it, and made the 1 GB C:'s first format look like a hang). In this driver memset is
// only in Format() (the zero buffer), and every real write first copies the client's data with ReadRemote: so a
// memset marks "formatting" and the next ReadRemote unmarks it; writes while marked are acknowledged unwritten.
#define memset(aPtr, aVal, aLen) (S.iFormatting = ETrue, memclr((aPtr), (aLen)))
#define ReadRemote(aDes, aOffset) ReadRemote((S.iFormatting = EFalse, (aDes)), (aOffset))
// SIZE (2026-10-15, the E: drive): DoCreate computes `Int64 size = GetNVMemSize() << KDiskSectorShift` with a TUint32
// sector count -- the shift happens in 32 bits, so a partition of 4 GiB or more (E: = the rest of a 256 GB card) got
// its size modulo 4 GiB. Recompute it in 64 bits where it is handed to the media layer.
#define SetTotalSizeInBytes(aSize) SetTotalSizeInBytes((Int64)GetNVMemSize() << KDiskSectorShift)
#define ReadReg SdBlkReadReg
#define WriteReg SdBlkWriteReg
#define Interrupt SdNvInterrupt
#define EIntNVMemoryDevice 0			// no such line on the A64: SdNvInterrupt ignores it
#include "syborg_mednvmemory.cpp"
