// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// phevdetest.cpp -- OURS (VDU rungs V0-V2): the VE driver test ("PH: ve" lines, forwarded by
// the kernel seam). V0: driver info, a register round trip, a contiguous arena commit. V1: one MPEG-2 intra picture.
// V2: a 25-frame IPB clip (ffmpeg testsrc, GOP 12, 2 B frames) with reference frames. Every decoded picture is compared
// with ffmpeg's decode (NV12, display order) by PSNR.
#include <e32base.h>
#include <e32debug.h>
#include <e32math.h>
#include <f32file.h>
#include <w32std.h>
#include <platform/cdsb.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "ve_rphevde.h"
#include "ve_mpeg2.h"

// The MPEG-2 decoder core (src/phevdec/ve_mpeg2) behind the interface this test always had: a Symbian-side wrapper that
// owns the job / memory adapters and copies each picture's slices into the arena (offset 0) before the decode.
typedef mpeg2::TPic TMpeg2Pic;
enum { EPicI = mpeg2::EPicI, EPicP = mpeg2::EPicP, EPicB = mpeg2::EPicB };
const TInt KEsMax = 1 << 20;					// arena offset 0: the bitstream of one picture

class TMpeg2Parser
	{
public:
	TMpeg2Parser(const TDesC8& aS) : iP(aS.Ptr(), aS.Length()) {}
	TBool Next(TMpeg2Pic& aP) { return iP.Next(aP); }
private:
	mpeg2::TParser iP;
	};

class CVeMpeg2
	{
public:
	CVeMpeg2(RPheVde& aVe, RChunk& aArena, TUint32 aPhys)
		: iArena(aArena), iPhys(aPhys), iJob(aVe), iMem(aArena, aPhys, 32 << 20, KEsMax), iDec(iJob, iMem) {}
	// decode aP's slices from aEs; returns the frame buffer index (>= 0) or an error; aStatus = the VE status
	TInt Decode(const TDesC8& aEs, const TMpeg2Pic& p, TInt& aStatus, TUint32& aMs)
		{
		const TInt esLen = p.iSliceEnd - p.iSliceAt;
		if (esLen <= 0 || esLen + 64 > KEsMax)
			return KErrNotSupported;
		TUint8* es = iArena.Base();
		Mem::Copy(es, aEs.Ptr() + p.iSliceAt, esLen);
		Mem::FillZ(es + esLen, 64);
		const TUint32 t0 = iJob.iMs;
		const TInt r = iDec.Decode(p, iPhys - KDramBus, aStatus);
		aMs = iJob.iMs - t0;
		return r == -2 ? KErrNotReady : r < 0 ? KErrNotSupported : r;
		}
	TUint8* Frame(TInt aIndex) const { return iArena.Base() + iDec.FrameMemIndex(aIndex); }
	TInt Stride() const { return iDec.Stride(); }
	TInt LumaSize() const { return iDec.LumaSize(); }
	void Reset() { iDec.Reset(); }
	void SetHeld(bool (*aFn)(void*, int), void* aCtx) { iDec.iHeld = aFn; iDec.iHeldCtx = aCtx; }
private:
	RChunk& iArena;
	TUint32 iPhys;
	TVeJobAdapter iJob;
	TVeArenaMem iMem;
	mpeg2::TVeMpeg2 iDec;
	};

// ---------------------------------------------------------------------------------------------------------------

static TInt ReadFile(RFs& aFs, const TDesC& aName, HBufC8*& aBuf)
	{
	RFile f;
	TInt r = f.Open(aFs, aName, EFileRead);
	if (r != KErrNone)
		return r;
	TInt size = 0;
	f.Size(size);
	aBuf = HBufC8::New(size);
	if (!aBuf)
		{
		f.Close();
		return KErrNoMemory;
		}
	TPtr8 p = aBuf->Des();
	r = f.Read(p);
	f.Close();
	return r;
	}

// PSNR in hundredths of a dB (9999 = identical)
static TInt Psnr(const TUint8* aA, const TUint8* aB, TInt aN)
	{
	TInt64 sse = 0;
	for (TInt i = 0; i < aN; ++i)
		{
		const TInt d = aA[i] - aB[i];
		sse += d * d;
		}
	if (!sse)
		return 9999;
	TReal mse = (TReal)sse / aN, l = 0;
	Math::Log(l, 255.0 * 255.0 / mse);
	return (TInt)(1000.0 * l);
	}

// Decode a whole stream; every picture against ffmpeg's frame at its display index. Returns the lowest PSNR.
static void RunClip(RPheVde& aVe, RChunk& aArena, TUint32 aPhys, const TDesC& aStream, const TDesC& aRef,
	const char* aTag)
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	HBufC8* es = NULL;
	HBufC8* ref = NULL;
	TInt r = ReadFile(fs, aStream, es);
	if (r == KErrNone)
		r = ReadFile(fs, aRef, ref);
	fs.Close();
	if (r != KErrNone)
		{
		RDebug::Printf("PH: ve %s files r=%d", aTag, r);
		delete es;
		delete ref;
		return;
		}
	CVeMpeg2* dec = new CVeMpeg2(aVe, aArena, aPhys);
	TMpeg2Parser parser(*es);
	TMpeg2Pic p;
	TInt n = 0, bad = 0, minY = 9999, minC = 9999;
	TUint32 total = 0, maxMs = 0;
	static const char KType[] = "?IPB";
	while (dec && parser.Next(p))
		{
		TInt st = 0;
		TUint32 ms = 0;
		const TInt f = dec->Decode(*es, p, st, ms);
		const TInt luma = dec->LumaSize(), frame = luma * 3 / 2;
		TInt py = 0, pc = 0;
		if (f >= 0 && (p.iDisplay + 1) * frame <= ref->Length())
			{
			const TUint8* rf = ref->Ptr() + p.iDisplay * frame;
			py = Psnr(dec->Frame(f), rf, luma);
			pc = Psnr(dec->Frame(f) + luma, rf + luma, luma / 2);
			}
		if (n < 4 || f < 0 || (st & 7) != 1)
			RDebug::Printf("PH: ve %s pic %d %c display %d status=%x %u ms PSNR Y %d.%02d C %d.%02d", aTag, n,
				KType[p.iType & 3], p.iDisplay, st, ms, py / 100, py % 100, pc / 100, pc % 100);
		if (f < 0 || (st & 7) != 1)
			++bad;
		minY = Min(minY, py), minC = Min(minC, pc);
		total += ms;
		maxMs = Max(maxMs, ms);
		++n;
		}
	aVe.WriteReg(0x000, 0x00000007);			// engine off
	TPheVdeInfo info;
	aVe.GetInfo(info);
	RDebug::Printf("PH: ve %s %d pictures %dx%d, %d failed, min PSNR Y %d.%02d C %d.%02d, VE %u ms total (max %u), irqs=%u %s",
		aTag, n, p.iWidth, p.iHeight, bad, minY / 100, minY % 100, minC / 100, minC % 100, total, maxMs, info.iIrqCount,
		n && !bad && minY >= 4000 && minC >= 4000 ? "PASS" : "FAIL");
	delete dec;
	delete es;
	delete ref;
	}

// ---------------------------------------------------------------------------------------------------------------
// V2 display: the clip on screen through direct screen access (NV12 -> XRGB on the CPU = the plan's first, debug-grade
// display path), looped, in display order: a reference picture is shown when the next one is decoded, B at once.

const TInt KPlaySeconds = 12;

class CVePlayer : public CActive, public MDirectScreenAccess
	{
public:
	CVePlayer(RPheVde& aVe, RChunk& aArena, TUint32 aPhys, const TDesC8& aEs)
		: CActive(EPriorityStandard), iVe(aVe), iArena(aArena), iPhys(aPhys), iEs(aEs), iPending(-1)
		{ CActiveScheduler::Add(this); }
	~CVePlayer() { Cancel(); CloseBitmap(); GlClose(); delete iTick; delete iRetry; delete iDsa; delete iDec; delete iParser; }
	void StartGlL(RWindow& aWin);		// G6: frames as NV12 EGLImages over the VE arena, drawn by the GPU
	void StartL(RWsSession& aWs, CWsScreenDevice& aScreen, RWindow& aWin)
		{
		iDsa = CDirectScreenAccess::NewL(aWs, aScreen, aWin, *this);
		BeginL();
		}
private:
	void BeginL()
		{
		iDsa->StartL();
		iRect = iDsa->DrawingRegion()->BoundingRect();
		if (iTries % 25 == 0 || !iRect.IsEmpty())
			RDebug::Printf("PH: ve play dsa region (%d,%d)-(%d,%d) try %d", iRect.iTl.iX, iRect.iTl.iY, iRect.iBr.iX,
				iRect.iBr.iY, iTries);
		if (iRect.IsEmpty())			// not composed yet: let go and ask again in 200 ms
			{
			iDsa->Cancel();
			if (!iRetry)
				iRetry = CPeriodic::NewL(CActive::EPriorityLow);
			if (++iTries < 1500)				// 5 min: the keyguard is up at boot (PhoneLab: key power unlocks)
				iRetry->Start(200000, 200000, TCallBack(Retry, this));
			return;
			}
		if (iRetry)
			iRetry->Cancel();
		iBmp = CDirectScreenBitmap::NewL();
		User::LeaveIfError(iBmp->Create(iRect, CDirectScreenBitmap::EDoubleBuffer));
		if (!iStarted)
			iStart.HomeTime(), iStarted = ETrue;
		Next();
		}
	static TInt Retry(TAny* aSelf)
		{
		CVePlayer* p = (CVePlayer*)aSelf;
		p->iRetry->Cancel();
		TRAP_IGNORE(p->BeginL());
		return 0;
		}
	void CloseBitmap()
		{
		if (iBmp)
			{
			iBmp->Close();
			delete iBmp;
			iBmp = NULL;
			}
		}
	void Restart(RDirectScreenAccess::TTerminationReasons) { CloseBitmap(); TRAP_IGNORE(BeginL()); }
	void AbortNow(RDirectScreenAccess::TTerminationReasons) { Cancel(); CloseBitmap(); }
	TInt DecodeNextShown();
	void Draw(TInt aFrame, const TAcceleratedBitmapInfo& aInfo);
	void Next();
	void RunL();
	void DoCancel() {}
	static TInt GlTick(TAny* aSelf);
	TBool GlImagesL();
	void GlClose();
	CPeriodic* iTick;
	EGLDisplay iDpy;
	EGLSurface iSurf;
	EGLContext iCtx;
	void* iImg[mpeg2::TVeMpeg2::KFrames];
	void* iFence[mpeg2::TVeMpeg2::KFrames];	// EGL_KHR_fence_sync per frame buffer: the GPU's last draw reading it
	TBool FenceBusy(TInt aIndex);
	void FenceWaitAll();
	static bool HeldThunk(void* aCtx, int aIndex);
	TInt iFenceWaits;
	GLuint iTex[mpeg2::TVeMpeg2::KFrames], iProg;
	TBool iGl;
	RPheVde& iVe;
	RChunk& iArena;
	TUint32 iPhys;
	const TDesC8& iEs;
	CDirectScreenAccess* iDsa;
	CDirectScreenBitmap* iBmp;
	TRect iRect;
	CVeMpeg2* iDec;
	TMpeg2Parser* iParser;
	TInt iPending;					// the last reference picture, shown when the next one is decoded
	TInt iShown, iVsyncs, iBad;
	TUint32 iVeMs, iCpuMs;
	TBool iStarted;
	CPeriodic* iRetry;
	TBool iLoop;
	TInt iTries;
	TTime iStart;
	};

// The next picture in display order (a frame buffer index), or an error.
TInt CVePlayer::DecodeNextShown()
	{
	for (TInt guard = 0; guard < 4; ++guard)
		{
		if (!iParser)
			{
			iParser = new TMpeg2Parser(iEs);
			if (!iDec)
				iDec = new CVeMpeg2(iVe, iArena, iPhys);
			if (!iParser || !iDec)
				return KErrNoMemory;
			if (iLoop)
				iDec->Reset(), iLoop = EFalse;
			}
		TMpeg2Pic p;
		if (!iParser->Next(p))					// end of the clip: show the last reference, then loop
			{
			const TInt last = iPending;
			iPending = -1;
			delete iParser, iParser = NULL;
			iLoop = ETrue;					// the decoder restarts its references at the next picture
			if (last >= 0)
				{
				// the buffers stay valid until the new decoder writes them (it starts at buffer 0, the last
				// reference may be there: copy semantics are not needed because Draw() runs before the next decode)
				return last;
				}
			continue;
			}
		TInt st = 0;
		TUint32 ms = 0;
		TInt f = iDec->Decode(iEs, p, st, ms);
		if (f == KErrNotSupported && iGl)			// -4: every free buffer still has a GPU draw pending
			{
			FenceWaitAll();
			f = iDec->Decode(iEs, p, st, ms);
			}
		iVeMs += ms;
		if (f < 0 || (st & 7) != 1)
			{
			++iBad;
			continue;
			}
		if (p.iType == EPicB)
			return f;
		const TInt show = iPending;
		iPending = f;
		if (show >= 0)
			return show;
		}
	return KErrCorrupt;
	}

// NV12 (BT.601 limited range) -> XRGB, centred, no scaling
void CVePlayer::Draw(TInt aFrame, const TAcceleratedBitmapInfo& aInfo)
	{
	const TUint32 t0 = User::NTickCount();
	const TInt w = 320, h = 240, stride = iDec->Stride();
	const TUint8* y0 = iDec->Frame(aFrame);
	const TUint8* uv0 = y0 + iDec->LumaSize();
	const TInt ox = Max(0, (aInfo.iSize.iWidth - w) / 2), oy = Max(0, (aInfo.iSize.iHeight - h) / 2);
	TUint8 yrow[320], uvrow[320];
	for (TInt y = 0; y < h && oy + y < aInfo.iSize.iHeight; ++y)
		{
		Mem::Copy(yrow, y0 + y * stride, w);	// one burst read of the uncached frame per row
		if (!(y & 1))
			Mem::Copy(uvrow, uv0 + (y / 2) * stride, w);
		TUint32* out = (TUint32*)(aInfo.iAddress + (oy + y) * aInfo.iLinePitch) + ox;
		for (TInt x = 0; x < w; ++x)
			{
			const TInt c = 298 * (yrow[x] - 16), d = uvrow[x & ~1] - 128, e = uvrow[x | 1] - 128;
			TInt r = (c + 409 * e + 128) >> 8, g = (c - 100 * d - 208 * e + 128) >> 8, b = (c + 516 * d + 128) >> 8;
			r = r < 0 ? 0 : r > 255 ? 255 : r;
			g = g < 0 ? 0 : g > 255 ? 255 : g;
			b = b < 0 ? 0 : b > 255 ? 255 : b;
			out[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
			}
		}
	iCpuMs += User::NTickCount() - t0;
	}

void CVePlayer::Next()
	{
	TAcceleratedBitmapInfo info;
	const TInt br = iBmp ? iBmp->BeginUpdate(info) : KErrNotReady;
	if (iVsyncs == 0 || br != KErrNone)
		RDebug::Printf("PH: ve play BeginUpdate r=%d addr=%08x %dx%d pitch %d shift %d", br, (TUint)info.iAddress,
			info.iSize.iWidth, info.iSize.iHeight, info.iLinePitch, info.iPixelShift);
	if (br != KErrNone || !info.iAddress)
		return;
	if (!(iVsyncs & 1))						// 25-30 fps: a new picture every second refresh
		{
		const TInt f = DecodeNextShown();
		if (f >= 0 && (1 << info.iPixelShift) == 32)
			{
			Draw(f, info);
			++iShown;
			}
		}
	iStatus = KRequestPending;
	iBmp->EndUpdate(iStatus);
	SetActive();
	}

void CVePlayer::RunL()
	{
	++iVsyncs;
	TTime now;
	now.HomeTime();
	const TInt ms = (TInt)(now.MicroSecondsFrom(iStart).Int64() / 1000);
	if (ms >= KPlaySeconds * 1000 || (iStatus.Int() != KErrNone && iStatus.Int() != -223))
		{
		RDebug::Printf("PH: ve play %d pictures in %d ms (fps x10 = %d), %d refreshes, %d failed, VE %u ms, NV12->RGB %u ms "
			"(%u us/picture), status %d", iShown, ms, ms ? iShown * 10000 / ms : 0, iVsyncs, iBad, iVeMs, iCpuMs,
			iShown ? iCpuMs * 1000 / iShown : 0, iStatus.Int());
		CActiveScheduler::Stop();
		return;
		}
	if (iVsyncs == 150)
		RDebug::Printf("PH: ve play running (screenshot now)");
	Next();
	}

// ---------------------------------------------------------------------------------------------------------------
// V3: H.264 (src/phevdec: the OURS parser + the VE decoder core) -- every picture's NV12 hashed (FNV-1a over the
// cropped Y rows then UV rows, as tools hash ffmpeg's NV12 output) and compared in display order.


struct TH264Out
	{
	TInt iCount;
	h264::TPicture iPics[64];
	};

static void OnPicture(void* aCtx, const h264::TPicture& aPic)
	{
	TH264Out& o = *(TH264Out*)aCtx;
	if (o.iCount < 64)
		o.iPics[o.iCount++] = aPic;
	}

static TUint32 Fnv(TUint32 aH, const TUint8* aP, TInt aN)
	{
	for (TInt i = 0; i < aN; ++i)
		aH = (aH ^ aP[i]) * 0x01000193u;
	return aH;
	}

static TUint32 HexWord(const TDesC8& aS, TInt aAt)
	{
	TUint32 v = 0;
	for (TInt i = 0; i < 8 && aAt + i < aS.Length(); ++i)
		{
		const TUint8 c = aS[aAt + i];
		v = (v << 4) | (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
		}
	return v;
	}

static void RunH264(RPheVde& aVe, RChunk& aArena, TUint32 aPhys, TInt aArenaSize, const TDesC& aName, TInt aW, TInt aH)
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	TFileName fn;
	fn.Format(_L("z:\\resource\\phevde\\%S.h264"), &aName);
	HBufC8* es = NULL;
	HBufC8* hashes = NULL;
	TInt r = ReadFile(fs, fn, es);
	fn.Format(_L("z:\\resource\\phevde\\%S.fnv"), &aName);
	if (r == KErrNone)
		r = ReadFile(fs, fn, hashes);
	fs.Close();
	TBuf8<16> tag;
	tag.Copy(aName);
	tag.ZeroTerminate();
	if (r != KErrNone)
		{
		RDebug::Printf("PH: ve v3 %s files r=%d", tag.Ptr(), r);
		delete es;
		delete hashes;
		return;
		}
	h264::TParser* parser = new h264::TParser;
	TVeJobAdapter* job = new TVeJobAdapter(aVe);
	TVeArenaMem* mem = new TVeArenaMem(aArena, aPhys, aArenaSize);
	TH264Out* out = new TH264Out;
	h264::TVeH264* dec = parser && job && mem && out ? new h264::TVeH264(*parser, *job, *mem) : NULL;
	if (!dec)
		{
		RDebug::Printf("PH: ve v3 no memory");
		return;
		}
	out->iCount = 0;
	job->iMs = 0;
	job->iMaxWrites = 0;
	dec->iOnPicture = OnPicture;
	dec->iCtx = out;
	const TUint8* buf = es->Ptr();
	size_t pos = 0, s = 0, e = 0;
	TInt nals = 0, errs = 0, firstErr = 0;
	const TUint32 t0 = User::NTickCount();
	while (h264::NextNal(buf, es->Length(), pos, s, e))
		{
		const TUint8* nal = buf + s;
		const TInt type = nal[0] & 0x1F;
		++nals;
		TInt rr = 0;
		if (type == h264::ENalSps)
			rr = parser->ParseSps(nal, e - s) ? 0 : -100;
		else if (type == h264::ENalPps)
			rr = parser->ParsePps(nal, e - s) ? 0 : -101;
		else if (type == h264::ENalSlice || type == h264::ENalIdr)
			{
			if ((TInt)(e - s) + 64 > (1 << 20))
				rr = -102;
			else
				{
				Mem::Copy(aArena.Base(), nal, e - s);		// the slice buffer at arena offset 0 (16-aligned)
				Mem::FillZ(aArena.Base() + (e - s), 64);
				rr = dec->DecodeSlice(nal, e - s, aPhys - KDramBus);
				}
			}
		if (rr < 0 && errs++ == 0)
			firstErr = rr, RDebug::Printf("PH: ve v3 %s nal %d type %d: error %d (VE status %x)", tag.Ptr(), nals, type, rr,
				dec->iLastStatus);
		}
	dec->Flush();
	const TUint32 t1 = User::NTickCount();
	// display order: (IDR period, POC)
	for (TInt a = 0; a < out->iCount; ++a)
		for (TInt b = a + 1; b < out->iCount; ++b)
			{
			const h264::TPicture& pa = out->iPics[a];
			const h264::TPicture& pb = out->iPics[b];
			if (pb.iIdrPeriod < pa.iIdrPeriod || (pb.iIdrPeriod == pa.iIdrPeriod && pb.iPoc < pa.iPoc))
				{
				h264::TPicture t = out->iPics[a];
				out->iPics[a] = out->iPics[b];
				out->iPics[b] = t;
				}
			}
	TInt same = 0;
	for (TInt i = 0; i < out->iCount; ++i)
		{
		const TUint8* y = mem->Cpu(out->iPics[i].iLumaBus);
		const TUint8* uv = mem->Cpu(out->iPics[i].iChromaBus);
		TUint32 hsh = 0x811c9dc5u;
		for (TInt row = 0; row < aH; ++row)
			hsh = Fnv(hsh, y + row * dec->iStride, aW);
		for (TInt row = 0; row < aH / 2; ++row)
			hsh = Fnv(hsh, uv + row * dec->iStride, aW);
		TInt at = 0;									// line i (LF or CRLF)
		for (TInt l = 0; l < i && at < hashes->Length(); ++at)
			if ((*hashes)[at] == '\n')
				++l;
		const TUint32 want = HexWord(*hashes, at);
		if (hsh == want)
			++same;
		else if (i - same < 4)
			RDebug::Printf("PH: ve v3 %s display %d (poc %d, decode #%d): fnv %08x want %08x, Y[0..3] %02x %02x %02x %02x",
				tag.Ptr(), i, out->iPics[i].iPoc, out->iPics[i].iBuffer, hsh, want, y[0], y[1], y[2], y[3]);
		if (hsh != want && i - same < 4 && out->iPics[i].iPicType == 1)
			{
			// per field-macroblock map of the luma: 'Z' = all 16x16 samples zero, '.' = not (the VE gave zero prediction)
			for (TInt par = 0; par < 2; ++par)
				for (TInt my = 0; my < aH / 32 && my < 12; ++my)
					{
					TBuf8<48> m;
					for (TInt mx = 0; mx < aW / 16 && mx < 40; ++mx)
						{
						TBool zero = ETrue;
						for (TInt r = 0; r < 16 && zero; ++r)
							{
							const TUint8* p = y + (2 * (16 * my + r) + par) * dec->iStride + 16 * mx;
							for (TInt x = 0; x < 16; ++x)
								if (p[x])
									{ zero = EFalse; break; }
							}
						m.Append(zero ? 'Z' : '.');
						}
					m.ZeroTerminate();
					RDebug::Printf("PH: ve v3 %s display %d %c%02d %s", tag.Ptr(), i, par ? 'B' : 'T', my, m.Ptr());
					}
			}
		}
	TPheVdeInfo info;
	aVe.GetInfo(info);
	RDebug::Printf("PH: ve v3 %s %dx%d: %d NALs, %u slices, %u pictures, %d errors (first %d), %u VE job errors; "
		"bit-exact %d/%d; VE %u ms, total %u ms, max %d writes/job, poll timeouts %u %s", tag.Ptr(), dec->iWidth,
		dec->iHeight, nals, dec->iSlices, dec->iPictures, errs, firstErr, dec->iJobErrors, same, out->iCount, job->iMs,
		t1 - t0, job->iMaxWrites, info.iPollTimeouts, out->iCount && same == out->iCount ? "PASS" : "FAIL");
	aVe.WriteReg(0x000, 0x00000007);
	delete dec;
	delete out;
	delete mem;
	delete job;
	delete parser;
	delete es;
	delete hashes;
	}

// ---------------------------------------------------------------------------------------------------------------
// G6 display: the VE's frame buffers ARE the textures -- one NV12 EGLImage per frame buffer over the arena chunk
// (egl_symbian.c: EGL_LINUX_DMA_BUF_EXT, the plane "fd" = the RChunk handle), sampled as GL_TEXTURE_EXTERNAL_OES (the
// state tracker splits NV12 into R8 + R8G8 and converts in the shader). No CPU copy, no CPU conversion.

const EGLint KEglLinuxDmaBuf = 0x3270, KEglFourcc = 0x3271, KEglP0Fd = 0x3272, KEglP0Off = 0x3273, KEglP0Pitch = 0x3274,
	KEglP1Fd = 0x3275, KEglP1Off = 0x3276, KEglP1Pitch = 0x3277;
const EGLint KFourccNV12 = 'N' | ('V' << 8) | ('1' << 16) | ('2' << 24);
const GLenum KGlTextureExternal = 0x8D65;
typedef void* (*TCreateImage)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
typedef EGLBoolean (*TDestroyImage)(EGLDisplay, void*);
typedef void (*TImageTargetTexture)(GLenum, void*);

static GLuint Shader(GLenum aType, const char* aSrc)
	{
	GLuint s = glCreateShader(aType);
	glShaderSource(s, 1, &aSrc, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
		{
		char log[200];
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		RDebug::Printf("PH: ve gl shader error: %s", log);
		}
	return s;
	}

void CVePlayer::StartGlL(RWindow& aWin)
	{
	iGl = ETrue;
	iDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	eglInitialize(iDpy, NULL, NULL);
	const EGLint cfgAttr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
	EGLConfig cfg;
	EGLint n = 0;
	eglChooseConfig(iDpy, cfgAttr, &cfg, 1, &n);
	iSurf = n ? eglCreateWindowSurface(iDpy, cfg, (EGLNativeWindowType)&aWin, NULL) : EGL_NO_SURFACE;
	const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	iCtx = n ? eglCreateContext(iDpy, cfg, EGL_NO_CONTEXT, ctxAttr) : EGL_NO_CONTEXT;
	const EGLBoolean cur = eglMakeCurrent(iDpy, iSurf, iSurf, iCtx);
	RDebug::Printf("PH: ve gl configs %d surface %08x context %08x current %d extensions: %s", n, (TUint)iSurf, (TUint)iCtx,
		cur, eglQueryString(iDpy, EGL_EXTENSIONS));
	User::LeaveIfError(cur ? KErrNone : KErrGeneral);
	static const char KVs[] = "attribute vec2 p; uniform vec2 sc; varying vec2 t;\n"
		"void main() { t = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); gl_Position = vec4(p * sc, 0.0, 1.0); }\n";
	static const char KFs[] = "#extension GL_OES_EGL_image_external : require\nprecision mediump float;\n"
		"uniform samplerExternalOES s; varying vec2 t;\nvoid main() { gl_FragColor = texture2D(s, t); }\n";
	iProg = glCreateProgram();
	glAttachShader(iProg, Shader(GL_VERTEX_SHADER, KVs));
	glAttachShader(iProg, Shader(GL_FRAGMENT_SHADER, KFs));
	glBindAttribLocation(iProg, 0, "p");
	glLinkProgram(iProg);
	GLint linked = 0;
	glGetProgramiv(iProg, GL_LINK_STATUS, &linked);
	RDebug::Printf("PH: ve gl program linked %d, GL_EXTENSIONS has image_external %d", linked,
		TPtrC8((const TUint8*)glGetString(GL_EXTENSIONS)).Find(_L8("GL_OES_EGL_image_external")) >= 0);
	glUseProgram(iProg);
	TSize sz = aWin.Size();
	// fit 320x240 into the window width, keep the aspect
	glUniform2f(glGetUniformLocation(iProg, "sc"), 1.0f, (sz.iWidth * 240.0f / 320.0f) / sz.iHeight);
	glUniform1i(glGetUniformLocation(iProg, "s"), 0);
	static const GLfloat KQuad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, KQuad);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, sz.iWidth, sz.iHeight);
	iTick = CPeriodic::NewL(CActive::EPriorityStandard);
	iStart.HomeTime();
	iStarted = ETrue;
	iTick->Start(1000, 8000, TCallBack(GlTick, this));	// 8 ms: faster than the VE+GPU, so the fences set the pace
	}

// The images need the decoder's geometry: made after the first decode.
TBool CVePlayer::GlImagesL()
	{
	if (iImg[0])
		return ETrue;
	TCreateImage create = (TCreateImage)eglGetProcAddress("eglCreateImageKHR");
	TImageTargetTexture target = (TImageTargetTexture)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!create || !target)
		{
		RDebug::Printf("PH: ve gl no eglCreateImageKHR (%08x) / glEGLImageTargetTexture2DOES (%08x)", (TUint)create,
			(TUint)target);
		return EFalse;
		}
	glGenTextures(mpeg2::TVeMpeg2::KFrames, iTex);
	iDec->SetHeld(HeldThunk, this);
	for (TInt i = 0; i < mpeg2::TVeMpeg2::KFrames; ++i)
		{
		const TInt base = iDec->Frame(i) - iArena.Base();
		const EGLint attr[] = { EGL_WIDTH, 320, EGL_HEIGHT, 240, KEglFourcc, KFourccNV12,
			KEglP0Fd, iArena.Handle(), KEglP0Off, base, KEglP0Pitch, iDec->Stride(),
			KEglP1Fd, iArena.Handle(), KEglP1Off, base + iDec->LumaSize(), KEglP1Pitch, iDec->Stride(), EGL_NONE };
		iImg[i] = create(iDpy, EGL_NO_CONTEXT, KEglLinuxDmaBuf, NULL, attr);
		glBindTexture(KGlTextureExternal, iTex[i]);
		if (iImg[i])
			target(KGlTextureExternal, iImg[i]);
		glTexParameteri(KGlTextureExternal, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(KGlTextureExternal, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(KGlTextureExternal, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(KGlTextureExternal, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		RDebug::Printf("PH: ve gl image %d at +%x: %08x egl error %x gl error %x", i, base, (TUint)iImg[i], eglGetError(),
			glGetError());
		}
	return iImg[0] != NULL;
	}

typedef void* (*TCreateSync)(EGLDisplay, EGLenum, const EGLint*);
typedef EGLBoolean (*TDestroySync)(EGLDisplay, void*);
typedef EGLint (*TClientWait)(EGLDisplay, void*, EGLint, EGLTimeKHR);
static const EGLenum KSyncFence = 0x30F9;		// EGL_SYNC_FENCE_KHR

// True while the GPU may still read frame buffer aIndex (its fence has not signalled): the VE must not decode into it.
TBool CVePlayer::FenceBusy(TInt aIndex)
	{
	void* f = iFence[aIndex];
	if (!f)
		return EFalse;
	TClientWait wait = (TClientWait)eglGetProcAddress("eglClientWaitSyncKHR");
	TDestroySync destroy = (TDestroySync)eglGetProcAddress("eglDestroySyncKHR");
	if (wait && wait(iDpy, f, 0, 0) == 0x30F5)		// EGL_TIMEOUT_EXPIRED_KHR
		return ETrue;
	if (destroy)
		destroy(iDpy, f);
	iFence[aIndex] = NULL;
	return EFalse;
	}

// Every buffer busy (decoder returned -4): block until the oldest draws finish.
void CVePlayer::FenceWaitAll()
	{
	TClientWait wait = (TClientWait)eglGetProcAddress("eglClientWaitSyncKHR");
	TDestroySync destroy = (TDestroySync)eglGetProcAddress("eglDestroySyncKHR");
	for (TInt i = 0; i < mpeg2::TVeMpeg2::KFrames; ++i)
		if (iFence[i])
			{
			++iFenceWaits;
			if (wait)
				wait(iDpy, iFence[i], 1, EGL_FOREVER_KHR);		// EGL_SYNC_FLUSH_COMMANDS_BIT_KHR
			if (destroy)
				destroy(iDpy, iFence[i]);
			iFence[i] = NULL;
			}
	}

bool CVePlayer::HeldThunk(void* aCtx, int aIndex)
	{
	return ((CVePlayer*)aCtx)->FenceBusy(aIndex);
	}

TInt CVePlayer::GlTick(TAny* aSelf)
	{
	CVePlayer& p = *(CVePlayer*)aSelf;
	TTime now;
	now.HomeTime();
	const TInt ms = (TInt)(now.MicroSecondsFrom(p.iStart).Int64() / 1000);
	const TInt f = p.DecodeNextShown();
	if (f >= 0 && p.GlImagesL())
		{
		const TUint32 t0 = User::NTickCount();
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glBindTexture(KGlTextureExternal, p.iTex[f]);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		if (p.iShown == 100)				// check one pixel against the CPU conversion of the same frame
			{
			TSize sz;
			EGLint w = 0, h = 0;
			eglQuerySurface(p.iDpy, p.iSurf, EGL_WIDTH, &w);
			eglQuerySurface(p.iDpy, p.iSurf, EGL_HEIGHT, &h);
			TUint8 px[4] = { 0, 0, 0, 0 };
			glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
			const TUint8* y0 = p.iDec->Frame(f);
			const TUint8* uv0 = y0 + p.iDec->LumaSize();
			const TInt yy = y0[120 * p.iDec->Stride() + 160], u = uv0[60 * p.iDec->Stride() + 160] - 128,
				v = uv0[60 * p.iDec->Stride() + 161] - 128, c = 298 * (yy - 16);
			RDebug::Printf("PH: ve gl centre pixel GPU %d,%d,%d CPU %d,%d,%d", px[0], px[1], px[2],
				Max(0, Min(255, (c + 409 * v + 128) >> 8)), Max(0, Min(255, (c - 100 * u - 208 * v + 128) >> 8)),
				Max(0, Min(255, (c + 516 * u + 128) >> 8)));
			(void)sz;
			}
		// A per-buffer fence instead of glFinish: the VE may decode into any other buffer at once, and into this
		// one as soon as the fence signals. Made before the swap, so it covers this draw (the swap's own flush
		// leaves nothing pending for a later fence).
		TCreateSync createSync = (TCreateSync)eglGetProcAddress("eglCreateSyncKHR");
		TDestroySync destroySync = (TDestroySync)eglGetProcAddress("eglDestroySyncKHR");
		if (createSync)
			{
			if (p.iFence[f] && destroySync)
				destroySync(p.iDpy, p.iFence[f]);
			p.iFence[f] = createSync(p.iDpy, KSyncFence, NULL);
			}
		else
			p.iFence[f] = NULL;
		eglSwapBuffers(p.iDpy, p.iSurf);
		if (!createSync)
			glFinish();
		p.iCpuMs += User::NTickCount() - t0;
		++p.iShown;
		if (p.iShown == 150)
			RDebug::Printf("PH: ve gl play running (screenshot now)");
		}
	if (ms >= KPlaySeconds * 1000 || (f >= 0 && !p.iImg[0]))
		{
		RDebug::Printf("PH: ve gl play %d pictures in %d ms (fps x10 = %d), %d failed, VE %u ms, GPU draw+swap %u ms "
			"(%u us/picture), %d fence waits", p.iShown, ms, ms ? p.iShown * 10000 / ms : 0, p.iBad, p.iVeMs, p.iCpuMs,
			p.iShown ? p.iCpuMs * 1000 / p.iShown : 0, p.iFenceWaits);
		p.iTick->Cancel();
		CActiveScheduler::Stop();
		}
	return 1;
	}

void CVePlayer::GlClose()
	{
	if (!iGl)
		return;
	TDestroyImage destroy = (TDestroyImage)eglGetProcAddress("eglDestroyImageKHR");
	TDestroySync destroySync = (TDestroySync)eglGetProcAddress("eglDestroySyncKHR");
	for (TInt i = 0; i < mpeg2::TVeMpeg2::KFrames; ++i)
		if (iFence[i] && destroySync)
			destroySync(iDpy, iFence[i]), iFence[i] = NULL;
	glDeleteTextures(mpeg2::TVeMpeg2::KFrames, iTex);
	for (TInt i = 0; i < mpeg2::TVeMpeg2::KFrames; ++i)
		if (iImg[i] && destroy)
			destroy(iDpy, iImg[i]);
	eglMakeCurrent(iDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(iDpy, iCtx);
	eglDestroySurface(iDpy, iSurf);
	eglTerminate(iDpy);
	iGl = EFalse;
	}

static void PlayL(RPheVde& aVe, RChunk& aArena, TUint32 aPhys, TBool aGl)
	{
	RFs fs;
	User::LeaveIfError(fs.Connect());
	HBufC8* es = NULL;
	const TInt r = ReadFile(fs, _L("z:\\resource\\phevde\\ipb.m2v"), es);
	fs.Close();
	User::LeaveIfError(r);
	CleanupStack::PushL(es);
	CActiveScheduler* sched = new (ELeave) CActiveScheduler;
	CleanupStack::PushL(sched);
	CActiveScheduler::Install(sched);
	RWsSession ws;
	User::LeaveIfError(ws.Connect());
	CleanupClosePushL(ws);
	CWsScreenDevice* screen = new (ELeave) CWsScreenDevice(ws);
	CleanupStack::PushL(screen);
	User::LeaveIfError(screen->Construct());
	RWindowGroup group(ws);
	User::LeaveIfError(group.Construct(0x56455031, ETrue));
	group.SetOrdinalPosition(0, 1000);			// above the home screen / Phone that start after us
	CleanupClosePushL(group);
	RWindow win(ws);
	User::LeaveIfError(win.Construct(group, 0x56455032));
	CleanupClosePushL(win);
	win.SetExtent(TPoint(0, 0), screen->SizeInPixels());
	win.SetRequiredDisplayMode(EColor16MU);
	win.SetBackgroundColor(KRgbBlack);
	win.Activate();
	ws.Flush();
	RDebug::Printf("PH: ve play window up");
	CVePlayer* player = new (ELeave) CVePlayer(aVe, aArena, aPhys, *es);
	CleanupStack::PushL(player);
	if (aGl)
		player->StartGlL(win);
	else
		player->StartL(ws, *screen, win);
	CActiveScheduler::Start();
	CleanupStack::PopAndDestroy(7, es);
	}

TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	RPheVde ve;
	TInt r = ve.Open();
	RDebug::Printf("PH: ve open r=%d", r);
	if (r != KErrNone)
		return 0;
	TPheVdeInfo info;
	r = ve.GetInfo(info);
	RDebug::Printf("PH: ve info r=%d version=%08x irqs=%u pll=%08x sram=%08x arena max %d MB", r, info.iVersion,
		info.iIrqCount, info.iPllVe, info.iSramCtrl, info.iArenaMaxSize >> 20);
	// VE_CTRL (0x000): select the MPEG engine (0) and back to none (7); the engine field must read back
	TUint32 ctrl = 0, sel = 0, back = 0;
	ve.ReadReg(0x000, ctrl);
	ve.WriteReg(0x000, (ctrl & ~0xFu) | 0x0);
	ve.ReadReg(0x000, sel);
	ve.WriteReg(0x000, ctrl);
	ve.ReadReg(0x000, back);
	RDebug::Printf("PH: ve VE_CTRL %08x -> select MPEG %08x -> restored %08x %s", ctrl, sel, back,
		(sel & 0xF) == 0 && back == ctrl ? "OK" : "BAD");
	RChunk arena;
	r = ve.OpenArena(arena);
	TUint32 phys = 0;
	const TInt size = 32 << 20;					// MPEG-2 frames + every H.264 test picture (no reuse)
	TInt rc = r == KErrNone ? ve.Commit(0, size, phys) : r;
	TBool ok = EFalse;
	if (rc == KErrNone)
		{
		volatile TUint32* p = (volatile TUint32*)arena.Base();
		p[0] = 0x5A5A1234;
		p[size / 4 - 1] = 0xA5A54321;
		ok = p[0] == 0x5A5A1234 && p[size / 4 - 1] == 0xA5A54321;
		}
	RDebug::Printf("PH: ve arena open r=%d commit %d MB r=%d phys=%08x rw %s", r, size >> 20, rc, phys, ok ? "OK" : "BAD");
	if (ok)
		{
		RunClip(ve, arena, phys, _L("z:\\resource\\phevde\\t320.m2v"), _L("z:\\resource\\phevde\\t320.nv12"), "v1");
		RunClip(ve, arena, phys, _L("z:\\resource\\phevde\\ipb.m2v"), _L("z:\\resource\\phevde\\ipb.nv12"), "v2");
		RunH264(ve, arena, phys, size, _L("hbase"), 320, 240);
		RunH264(ve, arena, phys, size, _L("hmain"), 352, 288);
		RunH264(ve, arena, phys, size, _L("hh8x8"), 640, 360);
		RunH264(ve, arena, phys, size, _L("hcqm"), 640, 360);
		RunH264(ve, arena, phys, size, _L("hhigh"), 640, 360);
		// interlaced: MBAFF frame pictures (cropped), and PAFF field pictures from
		// tests/h264_parser/paffgen.py (mixed frame / field pictures, B fields, field MMCO + list modification)
		RunH264(ve, arena, phys, size, _L("hmbaffm"), 320, 240);
		RunH264(ve, arena, phys, size, _L("hmbaffh"), 640, 360);
		RunH264(ve, arena, phys, size, _L("hpaff1"), 96, 64);
		RunH264(ve, arena, phys, size, _L("hpaff2"), 96, 64);
		RunH264(ve, arena, phys, size, _L("hpaff3"), 96, 64);
		RunH264(ve, arena, phys, size, _L("hpaff4"), 96, 64);
		RunH264(ve, arena, phys, size, _L("hpaffl"), 320, 256);
		TRAPD(gr, PlayL(ve, arena, phys, ETrue));		// G6: the GPU samples the VE frames (zero copy)
		RDebug::Printf("PH: ve gl play r=%d", gr);
		}
	arena.Close();
	ve.Close();
	RDebug::Printf("PH: ve done");
	delete cleanup;
	return 0;
	}
