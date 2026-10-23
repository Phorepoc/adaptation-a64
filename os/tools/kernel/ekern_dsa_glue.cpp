// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// ekern_dsa_glue.cpp -- OURS: the memory behind direct screen access under the
// NGA window server. The 2010 stack (openwfsupport surfacestream.cpp GetBufferPtrL, scdv scnew.cpp / Cdsb.cpp
// DoCreate) asks the display HAL for EDisplayHalGetDisplayMemoryHandle = an RChunk handle over the pixels the app
// draws and the compositor (libWFC, CPU) reads. Syborg's svpframebuffer has no such case and its video RAM is a
// DPlatChunkHw (no DChunk to hand out). So: a KERNEL-OWNED shared chunk (zeroed by the page allocator, ordinary
// cached RAM, no alias of the scanned video RAM -- a second mapping of that RAM with other attributes hung the
// phone 3 times: a64_display.cpp, phonelab.md). Producer and consumer are both CPUs, so cached is coherent.
// Called from the a64 / virt HAL wrappers (a64fb.cpp, virtfb.cpp); one chunk for the life of the boot.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);

// OURS (2026-10-22): when nonzero, the DSA chunk is a window over this physical RAM (the legacy video RAM) instead of its own pages,
// so the screen memory an app writes directly (UserSvr::ScreenInfo's iScreenAddress) IS the surface the compositor reads
extern "C" TPhysAddr ekseam_dsa_backing_phys;
TPhysAddr ekseam_dsa_backing_phys;
static DChunk* TheDsaChunk;
static TUint TheDsaSize;
static volatile TInt TheDsaState;	// 0 = none, 1 = being created, 2 = ready, 3 = failed

// aBytes = width * height * 4 of the screen (flipped modes reuse the same bytes). Returns a handle (>0) in the
// calling process's handle space written to the user pointer a1 (KErrNone), or an error.
extern "C" TInt ekseam_display_memory_handle(TInt aScreen, TAny* a1, TUint aBytes)
	{
	if (aScreen != 0 || !aBytes)
		return KErrNotSupported;
	NKern::ThreadEnterCS();
	TInt r = KErrNone;
	TUint32 expect = 0;
	if (__e32_atomic_cas_acq32(&TheDsaState, &expect, 1))
		{
		TUint size = Kern::RoundToPageSize(aBytes);
		TChunkCreateInfo info;
		info.iType = TChunkCreateInfo::ESharedKernelMultiple;
		info.iMaxSize = size;
#ifdef A64_FB_CACHED
		info.iMapAttr = (TUint)EMapAttrCachedMax;	// the video RAM is mapped cached everywhere (svpfb_patch.py PH_FB_CACHED): same attributes
#else
		info.iMapAttr = ekseam_dsa_backing_phys ? (TUint)EMapAttrBufferedC : (TUint)EMapAttrCachedMax;
#endif
		info.iOwnsMemory = ekseam_dsa_backing_phys ? EFalse : ETrue;
		info.iDestroyedDfc = 0;
		DChunk* chunk = 0;
		TLinAddr lin = 0;
		TUint32 attr = 0;
		r = Kern::ChunkCreate(info, chunk, lin, attr);
		if (r == KErrNone)
			{
			r = ekseam_dsa_backing_phys ? Kern::ChunkCommitPhysical(chunk, 0, size, ekseam_dsa_backing_phys) : Kern::ChunkCommit(chunk, 0, size);
			if (r != KErrNone)
				Kern::ChunkClose(chunk);
			}
		if (r == KErrNone)
			{
			TheDsaChunk = chunk;
			TheDsaSize = size;
			ekseam_puts("[EKERN][dsa] chunk lin="); ekseam_puthex(lin);
			ekseam_puts("[EKERN][dsa] chunk size="); ekseam_puthex(size);
			__e32_atomic_store_rel32(&TheDsaState, 2);
			}
		else
			{
			ekseam_puts("[EKERN][dsa] chunk create FAILED r="); ekseam_puthex((unsigned)r);
			__e32_atomic_store_rel32(&TheDsaState, 3);
			}
		}
	else
		{
		while (TheDsaState == 1)
			NKern::Sleep(1);
		}
	if (r == KErrNone)
		{
		if (TheDsaState != 2)
			r = KErrNoMemory;
		else
			{
			TInt h = Kern::MakeHandleAndOpen(NULL, TheDsaChunk);
			if (h < 0)
				r = h;
			else
				{
				kumemput32(a1, &h, sizeof(h));
				r = KErrNone;
				}
			}
		}
	NKern::ThreadLeaveCS();
	return r;
	}
