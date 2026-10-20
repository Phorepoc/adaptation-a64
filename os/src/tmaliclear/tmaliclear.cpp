// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// tmaliclear.cpp -- OURS (GPU rung G3-K2, docs/phorepoc/gpu-lima-research.md): the first frame the Mali-400 renders
// under Symbian. It builds by hand exactly the jobs Mesa's lima driver issues for a clear-only frame (lima_job.c
// lima_do_job / lima_pack_head_plbu_cmd / lima_generate_pp_stream / lima_pack_pp_frame_reg, lima_screen.c's pp_buffer:
// Mesa is MIT) -- a GP job whose PLBU writes the tile lists, then a PP job on both pixel processors that clears every
// 16x16 tile to a colour and writes the 360x640 frame back (B8G8R8A8 linear = Symbian's XRGB8888) -- checks every
// pixel, then shows the frame through the display channel for 5 s.
// The result is the exit reason (logged by the kernel): 0 = PASS; > 0 = that many wrong pixels; < 0 = a failed step
// (-100 - step, or the job's error: GP -0x1xxxx / PP -0x2xxxx carry the interrupt status, -33 = timeout);
// 0xC?xxxxxx = every pixel wrong: xxxxxx = the first pixel's low 24 bits, 0x01000000 set = the middle one is the same.
#include <e32base.h>
#include <dispchannel.h>
#include "../phorepoc-arm/mali/rmalichannel.h"

static const TInt KW = 360, KH = 640;
static const TUint32 KColour = 0xFF20A040;			// green-ish, so it is unmistakable on the screen

// arena layout (offsets)
enum
	{
	KOffPpBuf	= 0x00000,		// render state word block (RSW) at +0, the clear fragment program at +0x40
	KOffGpStream = 0x01000,		// the PLB block pointer array the PLBU fills
	KOffPlbuCmd	= 0x02000,
	KOffPpStream = 0x03000,		// two PP tile streams
	KOffPlb		= 0x10000,		// 512 blocks x 512 bytes
	KOffHeap	= 0x50000,		// the PLBU tile heap, 1 MB
	KHeapSize	= 0x100000,
	KOffRt		= 0x150000,		// the render target, KW * KH * 4
	KArenaUse	= 0x240000
	};
static const TInt KPlbMaxBlk = 512, KPlbBlkSize = 512;

static void HilbertRotate(TInt n, TInt* x, TInt* y, TInt rx, TInt ry)
	{
	if (ry == 0)
		{
		if (rx == 1)
			{
			*x = n - 1 - *x;
			*y = n - 1 - *y;
			}
		TInt t = *x; *x = *y; *y = t;
		}
	}
static void HilbertCoords(TInt n, TInt d, TInt* x, TInt* y)
	{
	TInt rx, ry, t = d;
	*x = *y = 0;
	for (TInt i = 0; (1 << i) < n; ++i)
		{
		rx = 1 & (t / 2);
		ry = 1 & (t ^ rx);
		HilbertRotate(1 << i, x, y, rx, ry);
		*x += rx << i;
		*y += ry << i;
		t /= 4;
		}
	}

static TInt Run()
	{
	RMaliChannel mali;
	TInt r = mali.Open();
	if (r != KErrNone)
		return -100 - 1;
	TMaliInfo info;
	if (mali.GetInfo(info) != KErrNone)
		return -100 - 2;
	RChunk arena;
	if (mali.OpenArena(arena) != KErrNone)
		return -100 - 3;
	if (mali.Commit(0, KArenaUse) != KErrNone)
		return -100 - 4;
	TUint8* base = arena.Base();
	const TUint32 va = info.iArenaVa;

	// the framebuffer in 16x16 tiles, grouped into at most 512 PLB blocks (lima_get_fb_info)
	TInt tiledW = (KW + 15) >> 4, tiledH = (KH + 15) >> 4;		// 23 x 40
	TInt w = tiledW, h = tiledH, shiftW = 0, shiftH = 0;
	while (w * h > KPlbMaxBlk || w > 0xff || h > 0xff)
		{
		if (w >= h || w > 0xff) { w = (w + 1) >> 1; ++shiftW; }
		else { h = (h + 1) >> 1; ++shiftH; }
		}
	const TInt blockW = w, blockH = h;
	const TInt shiftMin = Min(Min(shiftW, shiftH), 2);

	// pp_buffer: the frame's render state + the clear program (lima_screen.c)
	TUint32* rsw = (TUint32*)(base + KOffPpBuf);
	rsw[8] = 0x0000f008;
	rsw[9] = va + KOffPpBuf + 0x40;
	rsw[13] = 0x00000100;
	static const TUint32 KClearProgram[] = { 0x00020425, 0x0000000c, 0x01e007cf, 0xb0000000,
		0x000005f5, 0x00000000, 0x00000000, 0x00000000 };
	Mem::Copy(base + KOffPpBuf + 0x40, KClearProgram, sizeof(KClearProgram));

	// the PLB block pointers (static for any framebuffer)
	TUint32* gpStream = (TUint32*)(base + KOffGpStream);
	for (TInt j = 0; j < KPlbMaxBlk; ++j)
		gpStream[j] = va + KOffPlb + KPlbBlkSize * j;

	// the PLBU command list: the head only (a clear-only frame has no draws), then END
	TUint32* c = (TUint32*)(base + KOffPlbuCmd);
	TInt n = 0;
	c[n++] = 0x00000200; c[n++] = 0x1000010B;									// UNKNOWN2
	c[n++] = (shiftMin << 28) | (shiftH << 16) | shiftW; c[n++] = 0x1000010C;	// BLOCK_STEP
	c[n++] = ((tiledW - 1) << 24) | ((tiledH - 1) << 8); c[n++] = 0x10000109;	// TILED_DIMENSIONS
	c[n++] = blockW & 0xff; c[n++] = 0x30000000;								// BLOCK_STRIDE
	c[n++] = va + KOffGpStream; c[n++] = 0x28000000 | ((blockW * blockH) - 1) | 1;	// ARRAY_ADDRESS
	c[n++] = 0x00000000; c[n++] = 0x50000000;									// END

	TMaliGpFrame gp;
	gp.iFrame[0] = gp.iFrame[1] = 0;						// no vertex shader
	gp.iFrame[2] = va + KOffPlbuCmd;
	gp.iFrame[3] = va + KOffPlbuCmd + n * 4;
	gp.iFrame[4] = va + KOffHeap;
	gp.iFrame[5] = va + KOffHeap + KHeapSize;
	TRequestStatus st;
	mali.SubmitGp(gp, st);
	User::WaitForRequest(st);
	if (st.Int() != KErrNone)
		return st.Int();

	// the PP tile streams: every tile, Hilbert order, interleaved over the PPs (lima_generate_pp_stream)
	const TInt numPp = info.iNumPp;
	TUint32 off[4];
	{
	TInt delta = tiledW * tiledH / numPp * 16 + 16, remain = tiledW * tiledH % numPp, o = 0;
	for (TInt i = 0; i < numPp; ++i)
		{
		off[i] = o;
		o += delta;
		if (remain) { o += 16; --remain; }
		o = (o + 0x1f) & ~0x1f;
		}
	}
	TUint32* stream[4];
	TInt si[4] = { 0, 0, 0, 0 };
	for (TInt i = 0; i < numPp; ++i)
		stream[i] = (TUint32*)(base + KOffPpStream + off[i]);
	const TInt maxDim = Max(tiledW, tiledH);
	TInt dim = 0;
	while ((1 << dim) < maxDim)
		++dim;
	const TInt count = 1 << (dim + dim);
	TInt index = 0;
	for (TInt i = 0; i < count; ++i)
		{
		TInt x, y;
		HilbertCoords(maxDim, i, &x, &y);
		if (x < tiledW && y < tiledH)
			{
			const TInt pp = index % numPp;
			const TUint32 plbVa = va + KOffPlb + ((y >> shiftH) * blockW + (x >> shiftW)) * KPlbBlkSize;
			stream[pp][si[pp]++] = 0;
			stream[pp][si[pp]++] = 0xB8000000 | x | (y << 8);
			stream[pp][si[pp]++] = 0xE0000002 | ((plbVa >> 3) & ~0xE0000003);
			stream[pp][si[pp]++] = 0xB0000000;
			++index;
			}
		}
	for (TInt i = 0; i < numPp; ++i)
		{
		stream[i][si[i]++] = 0;
		stream[i][si[i]++] = 0xBC000000;
		stream[i][si[i]++] = 0;
		stream[i][si[i]++] = 0;
		}

	// the PP frame (lima_pack_pp_frame_reg + lima_pack_wb_cbuf_reg)
	TMaliPpFrame pp;
	Mem::FillZ(&pp, sizeof(pp));
	TUint32* f = pp.iFrame;
	f[1] = va + KOffPpBuf;				// render_address (the RSW)
	f[3] = 0x02;						// flags
	f[4] = 0x00ffffff;					// clear depth
	// the clear colour register holds A8B8G8R8 (Mesa packs RGBA bytes from the low byte up); the B8G8R8A8 write-back
	// then stores Symbian's 0xAARRGGBB (the first run, R/B unswapped, came back 0x40A020 for 0x20A040)
	f[6] = f[7] = f[8] = f[9] = (KColour & 0xFF00FF00) | ((KColour >> 16) & 0xFF) | ((KColour & 0xFF) << 16);
	f[10] = KW - 1;
	f[11] = KH - 1;
	f[16] = 1;
	f[17] = KH * 2 - 1;					// supersampled height
	f[18] = 0x77;
	f[19] = 1;							// onscreen
	f[20] = (shiftMin << 28) | (shiftH << 16) | shiftW;
	f[21] = 0xE0C;						// scale
	f[22] = 0x8888;						// channel layout
	TUint32* wb = pp.iWb;
	wb[0] = 0x02;						// colour buffer
	wb[1] = va + KOffRt;
	wb[2] = 0x03;						// B8G8R8A8
	wb[4] = 0;							// linear
	wb[5] = KW * 4 / 8;					// pitch in 8-byte units
	pp.iNumPp = numPp;
	for (TInt i = 0; i < numPp; ++i)
		pp.iPlbuArrayAddress[i] = va + KOffPpStream + off[i];
	mali.SubmitPp(pp, st);
	User::WaitForRequest(st);
	if (st.Int() != KErrNone)
		return st.Int();

	// check every pixel
	const TUint32* rt = (const TUint32*)(base + KOffRt);
	TInt bad = 0;
	for (TInt i = 0; i < KW * KH; ++i)
		if (rt[i] != KColour)
			++bad;

	// show it: the GPU's frame through the display channel, 5 s, then the window server's screen again
	RDisplayChannel ch;
	if (ch.Open(0) == KErrNone)
		{
		TUint idx;
		ch.GetCompositionBuffer(idx, st);
		User::WaitForRequest(st);
		RChunk cb;
		TInt cbOff = 0;
		if (st.Int() == KErrNone && ch.GetCompositionBufferInfo(idx, cb, cbOff) == KErrNone)
			{
			Mem::Copy(cb.Base() + cbOff, rt, KW * KH * 4);
			RDisplayChannel::TPostCount pc;
			ch.PostCompositionBuffer(NULL, pc);
			ch.WaitForPost(pc, st);
			User::WaitForRequest(st);
			User::After(5000000);
			ch.PostLegacyBuffer(NULL, pc);
			ch.WaitForPost(pc, st);
			User::WaitForRequest(st);
			cb.Close();
			}
		ch.Close();
		}
	const TUint32 first = rt[0], mid = rt[KW * (KH / 2) + KW / 2];
	arena.Close();
	mali.Close();
	// all wrong: report what IS there (the low 24 bits of the first pixel, and whether the middle one matches it)
	if (bad == KW * KH)
		return (TInt)(0xC0000000 | (first == mid ? 0x01000000 : 0) | (first & 0x00FFFFFF));
	return bad;
	}

TInt E32Main()
	{
	User::After(20000000);		// let the UI settle (started by the noncritical SSM list)
	return Run();
	}
