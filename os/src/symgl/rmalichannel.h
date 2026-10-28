/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// rmalichannel.h -- OURS (GPU rung G3): the user-side API of our Mali
// Utgard kernel driver (mali.ldd, built into ekern). The Symbian counterpart of Linux's lima DRM uAPI:
//   GET_PARAM      -> GetInfo
//   GEM_CREATE/VA  -> one per-channel ARENA: a shared chunk the GPU sees at a fixed VA (TMaliInfo::iArenaVa);
//                     the user commits pages (Commit) and sub-allocates buffers itself (GPU VA = iArenaVa + offset)
//   PRIME import   -> Import: a shared chunk of another driver (a SurfaceManager surface) mapped for the GPU at a VA
//                     of the import region (G5: GL renders straight into the window's surface buffers)
//   GEM_SUBMIT     -> SubmitGp / SubmitPp (one job per pipe in flight per driver; completes when the GPU finishes)
// CPU mappings of the arena are uncached (write-combining); the driver flushes the Mali L2 before every job.
// Imported chunks keep their own CPU cache attribute: CacheSync does the CPU cache maintenance around GPU access to
// a CACHED import (the window surfaces are cached, the compositor reads them with the CPU).
// Open never needs User::LoadLogicalDevice (the LDD is part of the kernel).
#ifndef RMALICHANNEL_H
#define RMALICHANNEL_H

#include <e32cmn.h>
#ifndef __KERNEL_MODE__
#include <e32std.h>
#endif

_LIT(KMaliLddName, "Mali");

struct TMaliInfo
	{
	TUint32 iGpVersion;		// GP VERSION register (Mali-400: 0x0B07xxxx)
	TUint32 iPpVersion;		// PP0 VERSION register (Mali-400: 0xCD07xxxx)
	TInt iNumPp;			// pixel processors (A64: 2)
	TUint32 iArenaVa;		// GPU virtual address of arena offset 0
	TInt iArenaMaxSize;		// bytes
	};

// lima's drm_lima_gp_frame: VSCL start/end, PLBU command list start/end, PLBU heap start/end (GPU VAs)
struct TMaliGpFrame
	{
	TUint32 iFrame[6];
	};

// lima's drm_lima_m400_pp_frame
struct TMaliPpFrame
	{
	TUint32 iFrame[23];
	TUint32 iNumPp;
	TUint32 iWb[3 * 12];
	TUint32 iPlbuArrayAddress[4];
	TUint32 iFragmentStackAddress[4];
	};

// Import: a chunk handle of the calling process + a page-aligned range of it; out: the GPU VA
struct TMaliImport
	{
	TInt iChunkHandle;
	TInt iOffset;
	TInt iSize;
	TUint32 iVa;
	};

// CacheSync ops on an imported range (no-ops for an uncached import)
enum TMaliCacheOp
	{
	EMaliCacheBeforeGpuRead,	// CPU wrote it, the GPU will read it: clean
	EMaliCacheBeforeGpuWrite,	// the GPU will write it: clean + invalidate
	EMaliCacheAfterGpuWrite		// the GPU wrote it, the CPU will read it: invalidate
	};
struct TMaliCacheSync
	{
	TUint32 iVa;
	TInt iSize;
	TInt iOp;
	};

// request numbers (shared with the kernel side)
enum TMaliControl { EMaliCtrlGetInfo, EMaliCtrlOpenArena, EMaliCtrlCommit, EMaliCtrlImport, EMaliCtrlUnimport,
	EMaliCtrlCacheSync,
	EMaliCtrlImportAddress,			// (G6b, appended: an old LDD answers KErrNotSupported)
	EMaliCtrlSubmit,				// start a job, returns its job number (any thread; wait with EMaliReqWait)
	EMaliCtrlCompleted };			// the last finished job number of a pipe
enum TMaliRequest { EMaliReqSubmitGp, EMaliReqSubmitPp, EMaliReqWait, EMaliReqCount };
const TInt KMaliMajorVersion = 1;

#ifndef __KERNEL_MODE__
class RMaliChannel : public RBusLogicalChannel
	{
public:
	inline static TVersion VersionRequired() { return TVersion(KMaliMajorVersion, 0, 0); }
	// process-owned: every thread of the process drives the GPU through it (Alfred GPU mode has a second GL thread; a
	// thread-owned handle was KERN-EXEC 0 in drmIoctl there)
	inline TInt Open() { return DoCreate(KMaliLddName, VersionRequired(), KNullUnit, NULL, NULL, EOwnerProcess); }
	inline TInt GetInfo(TMaliInfo& aInfo)
		{ TPckg<TMaliInfo> p(aInfo); return DoControl(EMaliCtrlGetInfo, &p); }
	// a handle to the arena chunk (Base() = offset 0)
	inline TInt OpenArena(RChunk& aChunk)
		{ TInt h = DoControl(EMaliCtrlOpenArena); return h < 0 ? h : aChunk.SetReturnedHandle(h); }
	// commit [aOffset, aOffset + aSize) of the arena (page aligned) and map it for the GPU
	inline TInt Commit(TInt aOffset, TInt aSize) { return DoControl(EMaliCtrlCommit, (TAny*)aOffset, (TAny*)aSize); }
	// map [aOffset, aOffset + aSize) of a shared chunk of this process for the GPU; aVa = where the GPU sees it
	inline TInt Import(const RChunk& aChunk, TInt aOffset, TInt aSize, TUint32& aVa)
		{
		TMaliImport imp;
		imp.iChunkHandle = aChunk.Handle();
		imp.iOffset = aOffset;
		imp.iSize = aSize;
		imp.iVa = 0;
		TPckg<TMaliImport> p(imp);
		TInt r = DoControl(EMaliCtrlImport, &p);
		aVa = imp.iVa;
		return r;
		}
	// G6b: the same for a shared chunk found by an ADDRESS in this process (an RSgImage's pixel data: the sgresource
	// kernel extension keeps the chunk handle to itself, the client only sees DataAddress()). aAddress is page aligned,
	// aSize a page multiple within the chunk; the chunk's offset is whatever the address is in it.
	inline TInt ImportAddress(const TAny* aAddress, TInt aSize, TUint32& aVa)
		{
		TMaliImport imp;
		imp.iChunkHandle = 0;
		imp.iOffset = (TInt)aAddress;
		imp.iSize = aSize;
		imp.iVa = 0;
		TPckg<TMaliImport> p(imp);
		TInt r = DoControl(EMaliCtrlImportAddress, &p);
		aVa = imp.iVa;
		return r;
		}
	inline TInt Unimport(TUint32 aVa) { return DoControl(EMaliCtrlUnimport, (TAny*)aVa); }
	// the job timeline, usable from ANY thread of the process (SubmitGp/SubmitPp complete to the submitting thread
	// only): Submit = the job number (> 0) or an error; Wait completes aStatus -- the caller's -- with that job's result
	inline TInt Submit(TInt aPipe, const TAny* aFrame) { return DoControl(EMaliCtrlSubmit, (TAny*)aPipe, (TAny*)aFrame); }
	inline TInt Completed(TInt aPipe) { return DoControl(EMaliCtrlCompleted, (TAny*)aPipe); }
	inline void Wait(TInt aPipe, TInt aJob, TRequestStatus& aStatus) { DoRequest(EMaliReqWait, aStatus, (TAny*)aPipe, (TAny*)aJob); }
	inline TInt CacheSync(TUint32 aVa, TInt aSize, TInt aOp)
		{
		TMaliCacheSync s;
		s.iVa = aVa;
		s.iSize = aSize;
		s.iOp = aOp;
		TPckgC<TMaliCacheSync> p(s);
		return DoControl(EMaliCtrlCacheSync, &p);
		}
	inline void SubmitGp(const TMaliGpFrame& aFrame, TRequestStatus& aStatus)
		{ DoRequest(EMaliReqSubmitGp, aStatus, (TAny*)&aFrame); }
	inline void SubmitPp(const TMaliPpFrame& aFrame, TRequestStatus& aStatus)
		{ DoRequest(EMaliReqSubmitPp, aStatus, (TAny*)&aFrame); }
	inline void CancelAll() { DoCancel(0xFFFFFFFF); }
	};
#endif

#endif
