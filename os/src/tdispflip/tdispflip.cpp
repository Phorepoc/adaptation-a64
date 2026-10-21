// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// tdispflip.cpp -- OURS (NGA rung G1a): drives RDisplayChannel the way an NGA
// composer does -- GetCompositionBuffer, draw, PostCompositionBuffer, WaitForPost -- alternating red and blue full
// frames, and logs the post rate. With vblank-paced posts (DDisplayPddA64) the rate is the panel's (~60/s, or an
// integer fraction of it if drawing takes longer than a frame) and the picture has no tear line. Then it hands the
// screen back to the legacy buffer (the window server's) and exits.
//   "[tdispflip] <n> posts in <ms> ms" -- the result line (RDebug: A64DEFS=-DA64_RDEBUG_LOG on the phone)
#include <e32base.h>
#include <e32debug.h>
#include <dispchannel.h>

static const TInt KFrames = 240;

static TInt Run()
	{
	RDebug::Printf("[tdispflip] start");
	// (no User::LoadLogicalDevice: the display LDD is part of the kernel here, installed by the board glue)
	RDisplayChannel ch;
	TInt r = ch.Open(0);
	RDebug::Printf("[tdispflip] open: %d", r);
	if (r != KErrNone)
		{
		RDebug::Printf("[tdispflip] FAIL open: %d", r);
		return r;
		}
	// a TPckgBuf, not a TPckg: the LDD writes a whole TPckgBuf<TDisplayInfo> over the descriptor (display.cpp)
	TPckgBuf<RDisplayChannel::TDisplayInfo> pkg;
	r = ch.GetDisplayInfo(pkg);
	const RDisplayChannel::TDisplayInfo& info = pkg();
	RDebug::Printf("[tdispflip] info r=%d %dx%d stride=%d bpp=%d cbs=%d %dHz fmt=%08x", r, info.iNormal.iWidth,
		info.iNormal.iHeight, info.iNormal.iOffsetBetweenLines, info.iBitsPerPixel, info.iNumCompositionBuffers,
		info.iRefreshRateHz, info.iPixelFormat);
	// (no RDebug in normal phone builds: the exit reason, in the kernel log, says which check failed)
	if (r != KErrNone || info.iBitsPerPixel != 32)
		{
		ch.Close();
		return r ? -1000 + r : -2000 - (TInt)info.iBitsPerPixel;
		}
	RChunk chunk[2];
	TUint32* buf[2];
	for (TInt i = 0; i < 2; ++i)
		{
		TInt off = 0;
		r = ch.GetCompositionBufferInfo(i, chunk[i], off);
		if (r != KErrNone)
			{
			RDebug::Printf("[tdispflip] FAIL buffer info %d: %d", i, r);
			ch.Close();
			return r;
			}
		buf[i] = (TUint32*)(chunk[i].Base() + off);
		}
	const TInt w = info.iNormal.iWidth, h = info.iNormal.iHeight, stride = info.iNormal.iOffsetBetweenLines / 4;
	const TUint32 t0 = User::NTickCount();
	TInt posted = 0, maxIdx = 0;
	for (TInt f = 0; f < KFrames; ++f)
		{
		TUint idx = 0;
		TRequestStatus st;
		ch.GetCompositionBuffer(idx, st);
		User::WaitForRequest(st);
		if (st.Int() != KErrNone || idx > 1)
			{
			RDebug::Printf("[tdispflip] FAIL get buffer: %d idx=%d", st.Int(), idx);
			break;
			}
		maxIdx = idx > (TUint)maxIdx ? idx : maxIdx;
		const TUint32 c = (f & 1) ? 0xFF0000FF : 0xFFFF0000;	// blue / red
		for (TInt y = 0; y < h; ++y)
			{
			TUint32* p = buf[idx] + y * stride;
			for (TInt x = 0; x < w; ++x)
				p[x] = c;
			}
		RDisplayChannel::TPostCount count;
		r = ch.PostCompositionBuffer(NULL, count);
		if (r != KErrNone)
			{
			RDebug::Printf("[tdispflip] FAIL post: %d", r);
			break;
			}
		ch.WaitForPost(count, st);
		User::WaitForRequest(st);
		++posted;
		}
	const TUint32 ms = User::NTickCount() - t0;
	RDebug::Printf("[tdispflip] %d posts in %d ms (both buffers used: %d)", posted, ms, maxIdx == 1);
	RDisplayChannel::TPostCount count;
	r = ch.PostLegacyBuffer(NULL, count);
	TRequestStatus st;
	ch.WaitForPost(count, st);
	User::WaitForRequest(st);
	RDebug::Printf("[tdispflip] legacy buffer back: %d; done", r);
	chunk[0].Close();
	chunk[1].Close();
	ch.Close();
	return posted == KFrames ? KErrNone : KErrGeneral;
	}

TInt E32Main()
	{
	RDebug::Printf("[tdispflip] E32Main");
	User::After(20000000);		// let the UI settle (started by the noncritical SSM list)
	RDebug::Printf("[tdispflip] running");
	TInt r = Run();
	RDebug::Printf("[tdispflip] %s (%d)", r == KErrNone ? "PASS" : "FAIL", r);
	return r;
	}
