// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// phedetest.cpp -- OURS (DE2 overlay plan): tests PheDeOv (RPheDeOv), the A64 DE2 video
// plane driver. Picture memory = the VE arena (RPheVde: one physically contiguous uncached chunk). Stages:
//   1 open + info                               6 coarse scaling: a 1920x1080 picture -> 720x405 (VDS programmed)
//   2 NV12 test frames (1280x720, moving box)   7 argument checks (rotation, bad pointer, odd size, off-panel)
//   3 Set -> scaled into the panel centre       8 Disable: plane + blender pipe off
//   4 register readback vs values derived here  ("PH: de2ov chk <name> got <x> want <y> OK|BAD")
//   5 240 vblank-paced Flips: rate, completion, the frame counters
// Ends "PH: de2ov PASS" (every check OK) or "PH: de2ov FAIL n". The picture is for the human eye too: a vertical colour-bar
// ramp with a white box moving left to right, 720x405 in the middle of the portrait panel, the Symbian UI around it.
#include <e32base.h>
#include <e32debug.h>
#include "rphedeov.h"
#include "rphevde.h"

static TInt Fails;

static void Chk(const char* aName, TUint32 aGot, TUint32 aWant)
	{
	const TBool ok = aGot == aWant;
	if (!ok)
		++Fails;
	RDebug::Printf("PH: de2ov chk %s got %08x want %08x %s", aName, aGot, aWant, ok ? "OK" : "BAD");
	}

static void ChkTrue(const char* aName, TBool aOk, TInt aA = 0, TInt aB = 0)
	{
	if (!aOk)
		++Fails;
	RDebug::Printf("PH: de2ov chk %s (%d %d) %s", aName, aA, aB, aOk ? "OK" : "BAD");
	}

static void ChkErr(const char* aName, TInt aGot, TInt aWant)
	{
	const TBool ok = aGot == aWant;
	if (!ok)
		++Fails;
	RDebug::Printf("PH: de2ov chk %s r=%d want %d %s", aName, aGot, aWant, ok ? "OK" : "BAD");
	}

// ---- the picture: NV12, w x h, pitch = w. Bars of 8 colours (limited-range YUV), a white 64x64 box at x = aBoxX.
static const TUint8 KBarY[8] = { 180, 162, 131, 112, 84, 65, 35, 16 };		// white yellow cyan green magenta red blue black (BT.601 limited)
static const TUint8 KBarU[8] = { 128, 44, 156, 72, 184, 100, 212, 128 };
static const TUint8 KBarV[8] = { 128, 142, 44, 58, 198, 212, 114, 128 };

static void Paint(TUint8* aY, TUint8* aUV, TInt aW, TInt aH, TInt aBoxX)
	{
	for (TInt y = 0; y < aH; ++y)
		{
		TUint8* row = aY + y * aW;
		for (TInt x = 0; x < aW; ++x)
			row[x] = KBarY[x * 8 / aW];
		}
	for (TInt y = 0; y < aH / 2; ++y)
		{
		TUint8* row = aUV + y * aW;
		for (TInt x = 0; x < aW / 2; ++x)
			{
			row[2 * x] = KBarU[x * 16 / aW];
			row[2 * x + 1] = KBarV[x * 16 / aW];
			}
		}
	const TInt bs = aH / 8;
	const TInt by = aH / 2 - bs / 2;
	for (TInt y = by; y < by + bs; ++y)
		Mem::Fill(aY + y * aW + aBoxX, bs, 235);
	for (TInt y = by / 2; y < (by + bs) / 2; ++y)
		for (TInt x = aBoxX / 2; x < (aBoxX + bs) / 2; ++x)
			{
			aUV[y * aW + 2 * x] = 128;
			aUV[y * aW + 2 * x + 1] = 128;
			}
	}

// ---- what the VSU must be programmed to (Linux sun8i_vi_layer_update_coord, A64: 288 MHz module clock, 60 Hz, 1440 lines)
struct TExpect
	{
	TUint32 iHStep, iVStep, iYIn, iVds, iVsuOut;
	TInt iMode;
	};
static void Expect(TInt aSw, TInt aSh, TInt aDw, TInt aDh, TExpect& e)
	{
	const TUint32 ability = (288000u * 80u) / (1440u * 60u * (TUint32)Max(aSw, aDw) / 1000u);
	const TUint32 required = (TUint32)aSh * 100 / (TUint32)aDh;
	TUint32 vn = 0, vm = 0;
	TInt sh = aSh;
	e.iMode = 1;
	if (ability < required)
		{
		vm = (TUint32)aSh;
		vn = ability * (TUint32)aDh / 100;
		sh = (TInt)vn;
		e.iMode = 2;
		}
	e.iHStep = (((TUint32)aSw << 16) / (TUint32)aDw) << 4;
	e.iVStep = (((TUint32)sh << 16) / (TUint32)aDh) << 4;
	e.iYIn = ((TUint32)(sh - 1) << 16) | (TUint32)(aSw - 1);
	e.iVds = (vn << 16) | vm;
	e.iVsuOut = ((TUint32)(aDh - 1) << 16) | (TUint32)(aDw - 1);
	}

static TUint32 Rd(RPheDeOv& aOv, TUint32 aOff)
	{
	TUint32 v = 0xDEADBEEF;
	const TInt r = aOv.ReadReg(aOff, v);
	if (r != KErrNone)
		RDebug::Printf("PH: de2ov ReadReg %x r=%d", aOff, r);
	return v;
	}

// the checks of one Set: aBusY/aBusUV the planes' bus addresses
static void CheckRegs(RPheDeOv& aOv, const TPheDeOvSet& s, TUint32 aBusY, TUint32 aBusUV)
	{
	TExpect e;
	Expect(s.iSrcW, s.iSrcH, s.iDstW, s.iDstH, e);
	const TUint32 insize = ((TUint32)(s.iSrcH - 1) << 16) | (TUint32)(s.iSrcW - 1);
	const TUint32 outsize = e.iVsuOut;
	const TUint32 attr = Rd(aOv, 0x2000);
	Chk("vi.attr.en", attr & 1, 1);
	Chk("vi.attr.fmt(NV12=8)", (attr >> 8) & 0x1F, 8);
	Chk("vi.attr.rgbmode", (attr >> 15) & 1, 0);
	Chk("vi.size", Rd(aOv, 0x2004), insize);
	Chk("vi.ovl_size", Rd(aOv, 0x20E8), insize);
	Chk("vi.pitch0", Rd(aOv, 0x200C), (TUint32)s.iPitch);
	Chk("vi.pitch1", Rd(aOv, 0x2010), (TUint32)s.iPitch);
	Chk("vi.laddr0(Y)", Rd(aOv, 0x2018), aBusY);
	Chk("vi.laddr1(UV)", Rd(aOv, 0x201C), aBusUV);
	Chk("vi.vds_y", Rd(aOv, 0x20F8), e.iVds);
	Chk("vi.hds_y", Rd(aOv, 0x20F0), 0);
	// bit 4 (COEFF_RDY) is written 1 and self-clears once the DE has taken the coefficients (phone run 2026-10-16: reads 1)
	Chk("vsu.ctrl.en", Rd(aOv, 0x20000) & 1, 1);
	Chk("vsu.outsize", Rd(aOv, 0x20040), outsize);
	Chk("vsu.yinsize", Rd(aOv, 0x20080), e.iYIn);
	Chk("vsu.yhstep", Rd(aOv, 0x20088), e.iHStep);
	Chk("vsu.yvstep", Rd(aOv, 0x2008C), e.iVStep);
	Chk("vsu.chstep", Rd(aOv, 0x200C8), e.iHStep / 2);
	Chk("vsu.cvstep", Rd(aOv, 0x200CC), e.iVStep / 2);
	Chk("vsu.cinsize", Rd(aOv, 0x200C0), (((TUint32)((e.iYIn >> 16) + 1) / 2 - 1) << 16) | (((e.iYIn & 0xFFFF) + 1) / 2 - 1));
	// the phase register keeps 24 bits (phone run: written 0xfffc0000 reads 0x00fc0000)
	Chk("vsu.cvphase(-0.25)", Rd(aOv, 0x200D8) & 0xFFFFFF, (TUint32)(0 - (1 << 18)) & 0xFFFFFF);
	// the Y-H coefficient table at step 0.. is not zero (the tables were loaded)
	ChkTrue("vsu.coeff.loaded", Rd(aOv, 0x20200) != 0 && Rd(aOv, 0x20400) != 0 && Rd(aOv, 0x20800) != 0);
	// CCSC: YUV->RGB, limited range; BT.709 from 1280 wide up (row 0 col 2: 0x72B), else BT.601 (0x662)
	Chk("ccsc.en", Rd(aOv, 0xAA050) & 1, 1);
	Chk("ccsc.c00", Rd(aOv, 0xAA060), 0x4A8);
	Chk("ccsc.c02", Rd(aOv, 0xAA068), s.iSrcW >= 1280 ? 0x72B : 0x662);
	Chk("fcc.alpha", Rd(aOv, 0xAA090) >> 24, (TUint32)s.iAlpha);
	// the blender: pipe 2 = channel 0 (VI), enabled; the UI pipes (0 = channel 1, 1 = channel 2) untouched
	const TUint32 pc = Rd(aOv, 0x1000), rt = Rd(aOv, 0x1080);
	Chk("bld.pipe2.en", (pc >> 10) & 1, 1);
	Chk("bld.pipe0.en(UI)", (pc >> 8) & 1, 1);
	Chk("bld.pipe1.en(bars)", (pc >> 9) & 1, 1);
	Chk("bld.route.pipe2=VI", (rt >> 8) & 0xF, 0);
	Chk("bld.route.pipe0|1", rt & 0xFF, 0x21);
	Chk("bld.pipe2.isize", Rd(aOv, 0x1028), outsize);
	Chk("bld.pipe2.offset", Rd(aOv, 0x102C), ((TUint32)s.iDstY << 16) | (TUint32)s.iDstX);
	Chk("bld.pipe2.mode", Rd(aOv, 0x1098), 0x03010301);
	TPheDeOvInfo info;
	aOv.GetInfo(info);
	Chk("info.scalemode", (TUint32)info.iScaleMode, (TUint32)e.iMode);
	Chk("info.enabled", (TUint32)info.iEnabled, 1);
	}

// the double-buffer latch: GLB_DBUFFER reads back 1 until the DE took the new registers at a frame start
static void WaitLatch(RPheDeOv& aOv, const char* aTag)
	{
	const TUint32 t0 = User::NTickCount();
	TInt n = 0;
	while ((Rd(aOv, 0x8) & 1) && n < 200)
		{
		User::After(2000);
		++n;
		}
	RDebug::Printf("PH: de2ov latch %s after %u ms (GLB_DBUFFER %x)", aTag, User::NTickCount() - t0, Rd(aOv, 0x8));
	ChkTrue("latch", n < 200);
	}

TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	RPheDeOv ov;
	TInt r = ov.Open();
	RDebug::Printf("PH: de2ov open r=%d", r);
	if (r != KErrNone)
		{
		RDebug::Printf("PH: de2ov FAIL open");
		return 0;
		}
	TPheDeOvInfo info;
	r = ov.GetInfo(info);
	RDebug::Printf("PH: de2ov info r=%d panel %dx%d max src %dx%d enabled %d vblanks %u flips %u done %u", r, info.iPanelW,
		info.iPanelH, info.iMaxSrcW, info.iMaxSrcH, info.iEnabled, info.iVblanks, info.iFlips, info.iFlipsDone);
	ChkTrue("info.panel", info.iPanelW == 720 && info.iPanelH == 1440, info.iPanelW, info.iPanelH);
	RPheDeOv second;
	ChkErr("second open (one plane, one owner)", second.Open(), KErrInUse);

	// ---- the picture memory
	RPheVde ve;
	r = ve.Open();
	RChunk arena;
	if (r == KErrNone)
		r = ve.OpenArena(arena);
	TUint32 arenaPhys = 0;
	const TInt KFrameW = 1280, KFrameH = 720, KFrames = 8;
	const TInt KFrameBytes = (KFrameW * KFrameH * 3 / 2 + 0xFFF) & ~0xFFF;
	const TInt KBigOff = KFrames * KFrameBytes;						// the 1080p picture after the 8 frames
	const TInt KBigBytes = (1920 * 1080 * 3 / 2 + 0xFFF) & ~0xFFF;
	const TInt KArena = KBigOff + KBigBytes;
	if (r == KErrNone)
		r = ve.Commit(0, KArena, arenaPhys);
	RDebug::Printf("PH: de2ov arena r=%d %d KB phys %08x", r, KArena >> 10, arenaPhys);
	if (r != KErrNone)
		{
		RDebug::Printf("PH: de2ov FAIL arena");
		return 0;
		}
	TUint8* base = arena.Base();
	const TInt boxRange = KFrameW - KFrameH / 8;
	for (TInt f = 0; f < KFrames; ++f)
		Paint(base + f * KFrameBytes, base + f * KFrameBytes + KFrameW * KFrameH, KFrameW, KFrameH, f * boxRange / (KFrames - 1));
	Paint(base + KBigOff, base + KBigOff + 1920 * 1080, 1920, 1080, 960);
	RDebug::Printf("PH: de2ov frames painted (%d x %dx%d NV12 + one 1920x1080)", KFrames, KFrameW, KFrameH);

	// ---- 3/4: Set 1280x720 -> 720x405 at the panel centre, registers read back
	TPheDeOvSet s;
	Mem::FillZ(&s, sizeof(s));
	s.iY = base;
	s.iUV = base + KFrameW * KFrameH;
	s.iPitch = KFrameW;
	s.iSrcW = KFrameW; s.iSrcH = KFrameH;
	s.iDstW = 720; s.iDstH = 405; s.iDstX = 0; s.iDstY = (1440 - 405) / 2;
	s.iFormat = EPheDeOvNV12;
	s.iColor = EPheDeOvAuto;
	s.iFullRange = 0;
	s.iAlpha = 255;
	s.iRotation = EPheDeOvRot0;
	r = ov.Set(s);
	RDebug::Printf("PH: de2ov Set 1280x720 -> 720x405 r=%d", r);
	ChkErr("set", r, KErrNone);
	WaitLatch(ov, "set");
	CheckRegs(ov, s, arenaPhys, arenaPhys + KFrameW * KFrameH);

	// ---- 5: flips
	const TInt KFlips = 240;
	TPheDeOvInfo i0, i1;
	ov.GetInfo(i0);
	const TUint32 t0 = User::NTickCount();
	TInt failed = 0;
	for (TInt n = 1; n <= KFlips; ++n)
		{
		const TInt f = n % KFrames;
		TPheDeOvFlip fl;
		fl.iY = base + f * KFrameBytes;
		fl.iUV = fl.iY + KFrameW * KFrameH;
		TRequestStatus st;
		ov.Flip(fl, st);
		User::WaitForRequest(st);
		if (st.Int() != KErrNone)
			{
			if (++failed <= 3)
				RDebug::Printf("PH: de2ov flip %d r=%d", n, st.Int());
			}
		if (n == 1)
			Chk("flip.laddr0", Rd(ov, 0x2018), arenaPhys + f * KFrameBytes);
		}
	const TUint32 ms = User::NTickCount() - t0;
	ov.GetInfo(i1);
	const TUint32 vb = i1.iVblanks - i0.iVblanks;
	RDebug::Printf("PH: de2ov %d flips in %u ms (%u us each), vblanks %u, flips %u done %u, failed %d", KFlips, ms,
		ms * 1000 / KFlips, vb, i1.iFlips - i0.iFlips, i1.iFlipsDone - i0.iFlipsDone, failed);
	ChkTrue("flip.failed", failed == 0, failed);
	ChkTrue("flip.rate(<= 34 ms each, vblank paced >= 14 ms)", ms / KFlips >= 14 && ms / KFlips <= 34, (TInt)(ms / KFlips));
	ChkTrue("flip.vblanks>=flips", vb >= (TUint32)KFlips - 2, (TInt)vb, KFlips);
	ChkTrue("flip.done==flips", i1.iFlipsDone - i0.iFlipsDone >= (TUint32)KFlips - 1, (TInt)(i1.iFlipsDone - i0.iFlipsDone), KFlips);

	// ---- 6: coarse scaling (1080p -> 720x405: the VSU cannot do 1080 -> 405 lines at 60 Hz alone)
	TPheDeOvSet b = s;
	b.iY = base + KBigOff;
	b.iUV = base + KBigOff + 1920 * 1080;
	b.iPitch = 1920;
	b.iSrcW = 1920; b.iSrcH = 1080;
	r = ov.Set(b);
	ChkErr("set 1080p", r, KErrNone);
	WaitLatch(ov, "1080p");
	CheckRegs(ov, b, arenaPhys + KBigOff, arenaPhys + KBigOff + 1920 * 1080);
	User::After(1500000);									// the 1080p picture on screen for a moment
	// move + resize + half alpha: 1280x720 -> 360x202 near the top (an even size), alpha 128
	TPheDeOvSet m = s;
	m.iDstW = 360; m.iDstH = 202; m.iDstX = 180; m.iDstY = 200; m.iAlpha = 128;
	ChkErr("set move/resize/alpha", ov.Set(m), KErrNone);
	WaitLatch(ov, "move");
	CheckRegs(ov, m, arenaPhys, arenaPhys + KFrameW * KFrameH);
	User::After(1000000);

	// ---- 7: argument checks (nothing changes the plane)
	TPheDeOvSet bad = s;
	bad.iRotation = 1;
	ChkErr("rotation 90 -> NotSupported", ov.Set(bad), KErrNotSupported);
	bad = s; bad.iSrcW = 1279;
	ChkErr("odd source width", ov.Set(bad), KErrArgument);
	bad = s; bad.iDstX = 400;
	ChkErr("dest off the panel", ov.Set(bad), KErrArgument);
	bad = s; bad.iAlpha = 300;
	ChkErr("alpha 300", ov.Set(bad), KErrArgument);
	TUint8 onStack[64];
	bad = s; bad.iY = onStack;
	ChkErr("Y plane not in a shared chunk", ov.Set(bad), KErrBadHandle);
	bad = s; bad.iUV = base + KArena - 64;			// a UV plane running off the end of the committed arena
	ChkTrue("UV plane past the arena fails", ov.Set(bad) != KErrNone);
	TPheDeOvFlip fb;
	fb.iY = onStack; fb.iUV = onStack;
	TRequestStatus fst;
	ov.Flip(fb, fst);
	User::WaitForRequest(fst);
	ChkTrue("flip with bad pointers fails", fst.Int() != KErrNone, fst.Int());
	TUint32 dummy = 0;
	ChkErr("register outside the allow-list", ov.ReadReg(0x7000, dummy), KErrArgument);

	// ---- 8: Disable
	ChkErr("disable", ov.Disable(), KErrNone);
	User::After(100000);
	ov.GetInfo(info);
	Chk("off: info.enabled", (TUint32)info.iEnabled, 0);
	Chk("off: vi.attr.en", Rd(ov, 0x2000) & 1, 0);
	Chk("off: bld.pipe2.en", (Rd(ov, 0x1000) >> 10) & 1, 0);
	Chk("off: bld.pipe0.en(UI)", (Rd(ov, 0x1000) >> 8) & 1, 1);
	TPheDeOvFlip nf;
	nf.iY = base; nf.iUV = base + KFrameW * KFrameH;
	ov.Flip(nf, fst);
	User::WaitForRequest(fst);
	ChkErr("flip after disable", fst.Int(), KErrNotReady);

	arena.Close();
	ve.Close();
	second.Close();
	ov.Close();
	if (Fails == 0)
		RDebug::Printf("PH: de2ov PASS");
	else
		RDebug::Printf("PH: de2ov FAIL %d", Fails);
	delete cleanup;
	return 0;
	}
