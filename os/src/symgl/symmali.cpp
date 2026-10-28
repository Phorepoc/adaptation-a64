// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// symmali.cpp -- OURS (GPU rung G4): the Symbian side of Mesa's lima driver -- RMaliChannel (our Mali-400 kernel
// driver) behind the C seam of core's src/symgl/symgpu.h. The DRM shim (drm_symbian.c) does the lima
// uAPI emulation (BO handles, syncobjs, sync files); this file only talks to the kernel.
#include <e32std.h>
#include <e32debug.h>
#include "rmalichannel.h"
#include "symgpu.h"

struct symgpu
	{
	RMaliChannel iChannel;
	RChunk iArena;
	TMaliInfo iInfo;
	TRequestStatus iStatus[2];
	TBool iInFlight[2];
	};

extern "C" symgpu_t* symgpu_open(void)
	{
	symgpu* m = new symgpu;
	if (!m)
		return NULL;
	m->iInFlight[0] = m->iInFlight[1] = EFalse;
	TInt r = m->iChannel.Open();
	if (r == KErrNone)
		r = m->iChannel.GetInfo(m->iInfo);
	if (r == KErrNone)
		r = m->iChannel.OpenArena(m->iArena);
	if (r != KErrNone)
		{
		RDebug::Printf("[symmali] no Mali channel (r=%d)", r);
		m->iArena.Close();
		m->iChannel.Close();
		delete m;
		return NULL;
		}
	return m;
	}

extern "C" void symgpu_close(symgpu_t* m)
	{
	if (!m)
		return;
	for (TInt p = 0; p < 2; ++p)
		if (m->iInFlight[p])
			User::WaitForRequest(m->iStatus[p]);
	m->iArena.Close();
	m->iChannel.Close();
	delete m;
	}

extern "C" void symgpu_info(symgpu_t* m, symgpu_info_t* out)
	{
	out->gp_version = m->iInfo.iGpVersion;
	out->pp_version = m->iInfo.iPpVersion;
	out->num_pp = m->iInfo.iNumPp;
	out->arena_va = m->iInfo.iArenaVa;
	out->arena_max = m->iInfo.iArenaMaxSize;
	out->arena_base = m->iArena.Base();
	}

extern "C" int symgpu_commit(symgpu_t* m, uint32_t offset, uint32_t size)
	{
	return m->iChannel.Commit(offset, size);
	}

extern "C" int symgpu_import(symgpu_t* m, int chunk_handle, uint32_t offset, uint32_t size, uint32_t* va)
	{
	RChunk c;
	c.SetHandle(chunk_handle);
	TUint32 gpuVa = 0;
	const TInt r = m->iChannel.Import(c, offset, size, gpuVa);
	*va = gpuVa;
	return r;
	}

extern "C" int symgpu_import_addr(symgpu_t* m, const void* addr, uint32_t size, uint32_t* va)
	{
	TUint32 gpuVa = 0;
	const TInt r = m->iChannel.ImportAddress(addr, size, gpuVa);
	*va = gpuVa;
	return r;
	}

extern "C" int symgpu_unimport(symgpu_t* m, uint32_t va)
	{
	return m->iChannel.Unimport(va);
	}

extern "C" int symgpu_cache_sync(symgpu_t* m, uint32_t va, uint32_t size, int op)
	{
	return m->iChannel.CacheSync(va, size, op);
	}

extern "C" int symgpu_submit(symgpu_t* m, int pipe, const uint32_t* frame, unsigned words)
	{
	if (pipe < 0 || pipe > 1 || m->iInFlight[pipe])
		return KErrInUse;
	if (pipe == SYMGPU_PIPE_GP)
		{
		if (words != sizeof(TMaliGpFrame) / 4)
			return KErrArgument;
		m->iChannel.SubmitGp(*reinterpret_cast<const TMaliGpFrame*>(frame), m->iStatus[0]);
		}
	else
		{
		if (words != sizeof(TMaliPpFrame) / 4)
			return KErrArgument;
		m->iChannel.SubmitPp(*reinterpret_cast<const TMaliPpFrame*>(frame), m->iStatus[1]);
		}
	m->iInFlight[pipe] = ETrue;
	return KErrNone;
	}

extern "C" int symgpu_wait(symgpu_t* m, int pipe, int poll)
	{
	if (pipe < 0 || pipe > 1)
		return KErrArgument;
	if (!m->iInFlight[pipe])
		return KErrNone;
	if (poll && m->iStatus[pipe].Int() == KRequestPending)
		return 1;
	User::WaitForRequest(m->iStatus[pipe]);
	m->iInFlight[pipe] = EFalse;
	return m->iStatus[pipe].Int();
	}

extern "C" unsigned symgpu_fast_counter()
	{
	return User::FastCounter();
	}

extern "C" void symgpu_log(const char* msg)
	{
	RDebug::Printf("%s", msg);
	}
