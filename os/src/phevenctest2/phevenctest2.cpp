// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// phevenctest2.cpp -- OURS (VE encoder ladder rung 2, docs/phorepoc/ve-encoder-plan.md section 11): real-size NV12 frames
// (640x480, 1280x720), a 16-frame GOP (one IDR + 15 P frames; the VE does the motion search itself), fixed QP 28 and a simple
// frame-level rate control, decoded back with OUR VE decoder; per-frame bytes / encode time / PSNR and the fps.
// Register sequence = rung 1 (phevenctest, boot 686 PASS) + Bootlin's P-frame additions (see section 11).
// All output is "PH: venc2 ..." lines; the last one is "PH: venc2 PASS" or "PH: venc2 FAIL".
#include <e32base.h>
#include <e32debug.h>
#include <e32math.h>
#include <hal.h>

#include "ve_rphevde.h"
#include "ve_h264enc.h"

using namespace h264enc;

const TInt KGop = 16;						// frames per stream: 1 IDR + 15 P
const TInt KFixedQp = 28;
const TInt KBitstreamSize = 1 << 20;		// arena offset 0 (the encoder's output window, later the decoder's NAL copy)
const TInt KStreamCap = 8 << 20;			// heap: all encoded frames of one stream

static TInt gArenaSize = 64 << 20;

#define PS(x) ((x) / 100), ((x) % 100)

// ---------------------------------------------------------------- timing
static TUint32 gFreq;
static inline TUint32 Now() { return User::FastCounter(); }
static TInt Us(TUint32 aT0, TUint32 aT1) { return (TInt)(((TUint64)(aT1 - aT0) * 1000000u) / gFreq); }

// ---------------------------------------------------------------- the test content
// gradient background + a textured (checker) square that moves a few pixels per frame + a dark static bar; chroma ramps with a
// coloured square. Deterministic: the decoder-side compare regenerates frame k.
class TGen
	{
public:
	TGen(TInt aW, TInt aH) : iW(aW), iH(aH), iCol(new TInt[aW]), iRow(new TInt[aH])
		{
		for (TInt c = 0; c < aW; ++c)
			iCol[c] = 30 + (c * 150) / aW;
		for (TInt r = 0; r < aH; ++r)
			iRow[r] = (r * 60) / aH;
		}
	~TGen() { delete[] iCol; delete[] iRow; }
	void Make(TInt aK, TUint8* aY, TUint8* aUV) const
		{
		const TInt s = iW / 8;								// square size
		const TInt vx = iW / 160, vy = iH / 240;			// pixels per frame
		const TInt x0 = (16 + (aK * vx) % (iW - s - 32)) & ~1, y0 = (16 + (aK * vy) % (iH - s - 32)) & ~1;
		const TInt barY = iH * 3 / 4;
		for (TInt r = 0; r < iH; ++r)
			{
			TUint8* row = aY + r * iW;
			for (TInt c = 0; c < iW; ++c)
				row[c] = (TUint8)(iCol[c] + iRow[r]);
			if (r >= barY && r < barY + 4)
				Mem::Fill(row, iW, 30);
			if (r >= y0 && r < y0 + s)
				for (TInt c = x0; c < x0 + s; ++c)
					row[c] = ((((c - x0) >> 3) + ((r - y0) >> 3)) & 1) ? 225 : 70;
			}
		for (TInt r = 0; r < iH / 2; ++r)
			{
			TUint8* row = aUV + r * iW;
			for (TInt c = 0; c < iW / 2; ++c)
				{
				TInt cb = 96 + (c * 120) / (iW / 2), cr = 160 - (r * 100) / (iH / 2);
				if (2 * c >= x0 && 2 * c < x0 + s && 2 * r >= y0 && 2 * r < y0 + s)
					{
					cb = 200;
					cr = 90;
					}
				row[2 * c] = (TUint8)cb;
				row[2 * c + 1] = (TUint8)cr;
				}
			}
		}
	TInt iW, iH;
private:
	TInt* iCol;
	TInt* iRow;
	};

static TInt PsnrFromSse(TInt64 aSse, TInt aN)		// hundredths of a dB (9999 = identical)
	{
	if (!aSse)
		return 9999;
	TReal mse = (TReal)aSse / aN, l = 0;
	Math::Log(l, 255.0 * 255.0 / mse);				// Math::Log = log10
	return (TInt)(1000.0 * l);
	}

static TInt64 Sse(const TUint8* aA, TInt aStrideA, const TUint8* aB, TInt aStrideB, TInt aW, TInt aH)
	{
	TInt64 sse = 0;
	for (TInt r = 0; r < aH; ++r)
		{
		const TUint8* a = aA + r * aStrideA;
		const TUint8* b = aB + r * aStrideB;
		TInt row = 0;
		for (TInt c = 0; c < aW; ++c)
			{
			const TInt d = a[c] - b[c];
			row += d * d;
			}
		sse += row;
		}
	return sse;
	}

// ---------------------------------------------------------------- the hardware bit writer (rung 1, Bootlin variant)
class THwBits : public MBitSink
	{
public:
	THwBits(RPheVde& aVe) : iVe(aVe) {}
	void Put(uint32_t aValue, int aBits)
		{
		TUint32 st = 0;
		for (TInt i = 0; i < 1000; ++i)			// Bootlin cedrus_enc_h264.c: wait for STATUS bit 9 (PUT_BITS_READY)
			{
			iVe.ReadReg(KAvcStatus, st);
			if (st & (1u << 9))
				break;
			}
		iVe.WriteReg(KAvcBasicBits, aValue);
		iVe.WriteReg(KAvcTrigger, 0x1 | (aBits << 8));
		}
	void NoEmulationPrevention(bool aOff) { iVe.WriteReg(KAvcParam, aOff ? 0x80000000u : 0u); }
	RPheVde& iVe;
	};

// engine select (Bootlin cedrus_enc_format_coded_configure; rung 1 / boot 686)
const TUint32 KModeBase = 0x00130000u, KModeEnc = 1u << 7, KModeIsp = 1u << 6, KResetEncoder = 1u << 24;
const TInt KVeReset = 0x004;

static void EnterEncoderMode(RPheVde& aVe, TUint32 aModeBase = KModeBase)
	{
	TUint32 v = 0;
	aVe.ReadReg(KVeCtrl, v);
	aVe.WriteReg(KVeCtrl, (((v & ~0x00370000u) | aModeBase) & ~(KModeEnc | KModeIsp)) | 7);	// REC_WR_MODE (20) + DDR_MODE_BW (17:16)
	aVe.ReadReg(KVeReset, v);
	aVe.WriteReg(KVeReset, v | KResetEncoder);
	aVe.ReadReg(KVeReset, v);
	aVe.WriteReg(KVeReset, v & ~KResetEncoder);
	aVe.ReadReg(KVeCtrl, v);
	aVe.WriteReg(KVeCtrl, v | KModeEnc | KModeIsp | 7);
	}

// ---------------------------------------------------------------- one stream
struct TRc
	{
	TBool iFixed;
	TInt iQp0;				// first frame QP (fixed: 28) = the PPS pic_init_qp
	TInt iTargetI, iTargetP;	// bits per frame (rate control only)
	TUint32 iMePara;			// ME_PARA (0xB10)
	TUint32 iPara1Extra;		// extra PARA1 (0xB08) bits
	TUint32 iPara0Extra;		// extra PARA0 (0xB04) bits (deblock idc 13:12)
	TInt iDeblockIdc;			// slice header disable_deblocking_filter_idc
	TInt iFrames;				// frames in the stream (<= KGop)
	TBool iQuiet;				// no per-frame lines
	TUint32 iModeBase;			// VE_MODE bits 20,17,16 (default 0x130000 = REC_WR 2MB + DDR_MODE_BW_128, as the decoder)
	TInt iBufCtrl;				// VE_BUF_CTRL (0x50) value, -1 = leave; with a value, 0x54 / 0x58 get scratch DRAM buffers
	TInt iBitBuf;				// output window bytes (<= KBitstreamSize)
	TUint32 iIspCtrl;			// ISP_CTRL (0xA08)
	TBool iPlanar;				// stage + feed planar I420 (ISP format 24<<27) instead of NV12
	TBool iDump;				// dump the non-zero ISP/AVC/top-level registers after the last frame
	};

struct TRun
	{
	TInt iW, iH, iEncoded, iDecoded;
	TInt iBytes[KGop], iQp[KGop], iUs[KGop], iJobUs[KGop], iPy[KGop], iPc[KGop];
	TInt iOff[KGop + 1];
	TInt iTotalUs;
	TInt iErr;
	TUint8* iStream;
	};

// frame-level rate control: H.264 bits halve per +6 QP, so qp += 6*log2(bits/target) = 19.93*log10(ratio); clamp the step
static TInt RcAdjust(TInt aQp, TInt aBits, TInt aTarget)
	{
	if (aTarget <= 0 || aBits <= 0)
		return aQp;
	TReal r = (TReal)aBits / aTarget, l = 0;
	Math::Log(l, r);
	const TInt dq = l >= 0 ? (TInt)(l * 19.93 + 0.5) : -(TInt)(-l * 19.93 + 0.5);
	return Max(10, Min(46, aQp + Max(-4, Min(6, dq))));
	}

static void DumpVe(RPheVde& aVe, const char* aTag, TInt aFrom, TInt aTo);
static TInt Align(TInt a, TInt n) { return (a + n - 1) & ~(n - 1); }

static TInt EncodeStream(RPheVde& aVe, RChunk& aArena, TUint32 aPhys, const TGen& aGen, const TRc& aRc, TRun& aR)
	{
	const TInt W = aGen.iW, H = aGen.iH, mbw = W / 16, mbh = H / 16;
	TVeArenaMem mem(aArena, aPhys, gArenaSize, KBitstreamSize);
	// buffer sizes: Bootlin cedrus_enc_h264.c lines 468 (mb_info), 542-563 (subpixel, reconstruction)
	const TInt recLuma = Align(mbw, 2) * 16 * Align(mbh + 1, 4) * 16;
	const TInt recChroma = Align(mbw, 2) * 16 * Align((mbh + 1) / 2, 4) * 16;
	const TInt recSize = Align(recLuma + recChroma, 4096);
	const TInt subW = ((mbw + 47) * 2 / 3 & ~31) + Align(mbw, 32) * 2, subH = (mbh * 16 + 72) / 8;
	const TInt subSize = Align(Max(subW * subH, recLuma / 4), 4096);		// >= jemk's luma/4 as well
	const TInt mbInfoSize = ((mbw + 31) / 32) * 4096;
	const TInt frameBufSize = Align(Align(mbw, 4) * mbh * 8, 4096);
	TInt idx;
	TUint32 rec[2], sub[2], in[KGop];
	for (TInt i = 0; i < 2; ++i)
		{
		rec[i] = mem.Alloc(recSize, 4096, idx);
		sub[i] = mem.Alloc(subSize, 4096, idx);
		}
	const TUint32 lineBus = mem.Alloc(mbInfoSize, 4096, idx);
	const TUint32 frameBus = mem.Alloc(frameBufSize, 4096, idx);
	if (!rec[0] || !rec[1] || !sub[0] || !sub[1] || !lineBus || !frameBus)
		return KErrNoMemory;
	for (TInt i = 0; i < 2; ++i)
		{
		Mem::FillZ(mem.Cpu(rec[i]), recSize);
		Mem::FillZ(mem.Cpu(sub[i]), subSize);
		}
	Mem::FillZ(mem.Cpu(lineBus), mbInfoSize);
	Mem::FillZ(mem.Cpu(frameBus), frameBufSize);
	// stage every input frame in the (uncached) arena before the timed loop: a real capture path writes there directly
	TUint8* y = new TUint8[W * H];
	TUint8* uv = new TUint8[W * H / 2];
	if (!y || !uv)
		{
		delete[] y;
		delete[] uv;
		return KErrNoMemory;
		}
	for (TInt k = 0; k < aRc.iFrames; ++k)
		{
		in[k] = mem.Alloc(W * H * 3 / 2, 4096, idx);
		if (!in[k])
			{
			delete[] y;
			delete[] uv;
			return KErrNoMemory;
			}
		aGen.Make(k, y, uv);
		TUint8* d = mem.Cpu(in[k]);
		Mem::Copy(d, y, W * H);
		if (aRc.iPlanar)
			for (TInt i = 0; i < W * H / 4; ++i)
				{
				d[W * H + i] = uv[2 * i];
				d[W * H + W * H / 4 + i] = uv[2 * i + 1];
				}
		else
			Mem::Copy(d + W * H, uv, W * H / 2);
		}
	delete[] y;
	delete[] uv;
	Mem::FillZ(aArena.Base(), KBitstreamSize);

	EnterEncoderMode(aVe, aRc.iModeBase);
	if (aRc.iBufCtrl >= 0)
		{
		const TUint32 dblk = mem.Alloc(1 << 20, 4096, idx), ipred = mem.Alloc(1 << 20, 4096, idx);
		if (!dblk || !ipred)
			return KErrNoMemory;
		Mem::FillZ(mem.Cpu(dblk), 1 << 20);
		Mem::FillZ(mem.Cpu(ipred), 1 << 20);
		aVe.WriteReg(0x54, dblk);
		aVe.WriteReg(0x58, ipred);
		aVe.WriteReg(0x50, (TUint32)aRc.iBufCtrl);
		}
	static const TUint16 KZero[] = { 0xB0C, 0xB28, 0xB2C, 0xB30, 0xB34, 0xB38, 0xB3C, 0xB48, 0xB4C, 0xB50, 0xB5C, 0xBC4 };
	TVeJobAdapter job(aVe);
	THwBits sink(aVe);
	const TInt div48 = (mbw + 47) / 48;					// Bootlin line 1218: DIV_ROUND_UP(stride_mbs, 48)
	TInt qp = aRc.iQp0, qpP = aRc.iQp0;
	const TInt ppsQp = aRc.iQp0;
	TInt err = KErrNone;
	aR.iEncoded = 0;
	aR.iTotalUs = 0;
	aR.iOff[0] = 0;
	for (TInt k = 0; k < aRc.iFrames && err == KErrNone; ++k)
		{
		const TBool idr = k == 0;
		qp = idr ? aRc.iQp0 : (aRc.iFixed ? KFixedQp : qpP);
		const TInt cur = k & 1, ref = cur ^ 1;
		const TUint32 t0 = Now();
		// per-job state (Bootlin job_configure lines 1116-1255)
		aVe.WriteReg(KAvcStatus, 0x7);
		aVe.WriteReg(KAvcTrigger, 0);
		aVe.WriteReg(0xB90, 0);							// STM_BIT_LEN
		aVe.WriteReg(0xB54, 0);							// RESIDUAL_BITS
		aVe.WriteReg(0xB58, 0);							// HEADER_BITS
		aVe.WriteReg(KAvcVleOffset, 0);
		aVe.WriteReg(KAvcVleAddr, aPhys - KDramBus);
		aVe.WriteReg(KAvcVleEnd, aPhys - KDramBus + aRc.iBitBuf - 1);
		aVe.WriteReg(KAvcVleMax, (TUint32)aRc.iBitBuf * 8);
		THeaders hdr(sink, mbw, mbh, ppsQp);
		if (idr)
			{
			hdr.Sps();
			hdr.Pps();
			}
		hdr.SliceHeader(idr, k, qp - ppsQp, aRc.iDeblockIdc);
		aVe.WriteReg(KIspStride, (TUint32)mbw << 16);
		aVe.WriteReg(KIspSize, ((TUint32)mbw << 16) | mbh);
		aVe.WriteReg(KIspCtrl, aRc.iIspCtrl);
		aVe.WriteReg(KIspLuma, in[k]);
		aVe.WriteReg(KIspChroma, in[k] + W * H);
		if (aRc.iPlanar)
			aVe.WriteReg(0xA80, in[k] + W * H * 5 / 4);		// ISP_INPUT_CHROMA1 (V plane)
		aVe.WriteReg(KAvcMbInfo, lineBus);
		aVe.WriteReg(KAvcUnkBuf, frameBus);				// MV buffer (written back only if WB_MV_INFO_DIS clear)
		for (TUint i = 0; i < sizeof(KZero) / sizeof(KZero[0]); ++i)
			aVe.WriteReg(KZero[i], 0);
		aVe.WriteReg(KAvcRecLuma, rec[cur]);
		aVe.WriteReg(KAvcRecChroma, rec[cur] + recLuma);
		const TInt refSet = idr ? cur : ref;			// Bootlin 1150-1160: an I frame points REF0 at its own reconstruction
		aVe.WriteReg(KAvcRefLuma, rec[refSet]);
		aVe.WriteReg(KAvcRefChroma, rec[refSet] + recLuma);
		aVe.WriteReg(KAvcRefSluma, sub[refSet]);		// SUBPIX_ADDR_LAST (0xBB8), 1168-1178
		aVe.WriteReg(KAvcRecSluma, sub[cur]);			// SUBPIX_ADDR_NEW (0xBBC)
		TUint32 avcCtrl = 0;
		aVe.ReadReg(KAvcCtrl, avcCtrl);
		const TUint32 t1 = Now();
		job.Begin();
		job.Write(KAvcCtrl, avcCtrl | 0xF);
		job.Write(KAvcStatus, 0x7);
		job.Write(KAvcParam, (idr ? 0u : 0x10u) | aRc.iPara0Extra);			// PARA0: CAVLC, slice type I (0) / P (1<<4), frame_num 0, deblock on (1195-1216)
		job.Write(KAvcQp, (4u << 16) | ((TUint32)div48 << 10) | (TUint32)qp | aRc.iPara1Extra);	// PARA1: chroma qp offset 4, stride/48, fixed QP
		job.Write(KAvcMotionEst, aRc.iMePara);			// ME_PARA (default WB_MV_INFO_DIS | FME level 2, 1242-1245)
		job.Write(KAvcTrigger, 0x8);
		const TInt rr = job.Run(KAvcStatus, 0x7, KAvcCtrl, 0xF);
		const TUint32 t2 = Now();
		TUint32 status = 0, vle = 0;
		aVe.ReadReg(KAvcStatus, status);
		aVe.ReadReg(KAvcVleLength, vle);
		const TUint32 t3 = Now();
		const TInt bytes = (TInt)((vle + 7) / 8);
		aR.iUs[k] = Us(t0, t3);
		aR.iJobUs[k] = Us(t1, t2);
		aR.iTotalUs += aR.iUs[k];
		aR.iQp[k] = qp;
		aR.iBytes[k] = bytes;
		if (!(rr >= 0 && (rr & 3) == 1) || bytes <= 0 || aR.iOff[k] + bytes > KStreamCap)
			{
			RDebug::Printf("PH: venc2 f%02d ENCODE ERROR run r=%d status %08x vle %u bits", k, rr, status, vle);
			TUint32 v[4] = {0, 0, 0, 0};
			aVe.ReadReg(KVeCtrl, v[0]);
			aVe.ReadReg(KAvcCtrl, v[1]);
			aVe.ReadReg(KAvcStatus, v[2]);
			aVe.ReadReg(KAvcMotionEst, v[3]);
			RDebug::Printf("PH: venc2 f%02d regs ve_ctrl %08x avc_ctrl %08x status %08x me %08x", k, v[0], v[1], v[2], v[3]);
			err = KErrGeneral;
			break;
			}
		Mem::Copy(aR.iStream + aR.iOff[k], aArena.Base(), bytes);
		aR.iOff[k + 1] = aR.iOff[k] + bytes;
		++aR.iEncoded;
		if (!aRc.iFixed && !idr)
			qpP = RcAdjust(qp, bytes * 8, aRc.iTargetP);
		}
	if (aRc.iDump)
		{
		DumpVe(aVe, "encoder after run 000-0fc", 0x000, 0x100);
		DumpVe(aVe, "encoder after run a00-bfc", 0xA00, 0xC00);
		}
	aVe.WriteReg(KVeCtrl, 0x7);
	return err;
	}

// ---------------------------------------------------------------- decode back + per-frame PSNR
struct TDecCtx
	{
	TRun* iR;
	const TGen* iGen;
	TUint8 *iY, *iUV;
	h264::TVeH264* iDec;
	TVeArenaMem* iMem;
	TInt iCount;
	};

static void OnPicture(void* aCtx, const h264::TPicture& aPic)
	{
	TDecCtx& d = *(TDecCtx*)aCtx;
	const TInt i = d.iCount++;
	if (i >= KGop)
		return;
	const TInt W = d.iGen->iW, H = d.iGen->iH, st = d.iDec->iStride;
	d.iGen->Make(i, d.iY, d.iUV);
	d.iR->iPy[i] = PsnrFromSse(Sse(d.iMem->Cpu(aPic.iLumaBus), st, d.iY, W, W, H), W * H);
	d.iR->iPc[i] = PsnrFromSse(Sse(d.iMem->Cpu(aPic.iChromaBus), st, d.iUV, W, W, H / 2), W * H / 2);
	}

static TInt DecodeStream(RPheVde& aVe, RChunk& aArena, TUint32 aPhys, const TGen& aGen, TRun& aR)
	{
	TVeArenaMem mem(aArena, aPhys, gArenaSize, KBitstreamSize);
	h264::TParser* parser = new h264::TParser;
	TVeJobAdapter* job = new TVeJobAdapter(aVe);
	TDecCtx* ctx = new TDecCtx;
	const TInt W = aGen.iW, H = aGen.iH;
	ctx->iY = new TUint8[W * H];
	ctx->iUV = new TUint8[W * H / 2];
	h264::TVeH264* dec = parser && job && ctx && ctx->iY && ctx->iUV ? new h264::TVeH264(*parser, *job, mem) : NULL;
	if (!dec)
		return KErrNoMemory;
	ctx->iR = &aR;
	ctx->iGen = &aGen;
	ctx->iDec = dec;
	ctx->iMem = &mem;
	ctx->iCount = 0;
	dec->iOnPicture = OnPicture;
	dec->iCtx = ctx;
	TInt err = 0;
	for (TInt k = 0; k < aR.iEncoded; ++k)
		{
		const TUint8* s0 = aR.iStream + aR.iOff[k];
		const TInt len = aR.iOff[k + 1] - aR.iOff[k];
		size_t pos = 0, s = 0, e = 0;
		while (h264::NextNal(s0, len, pos, s, e))
			{
			const TUint8* nal = s0 + s;
			const TInt type = nal[0] & 0x1F;
			TInt rr = 0;
			if (type == h264::ENalSps)
				rr = parser->ParseSps(nal, e - s) ? 0 : -100;
			else if (type == h264::ENalPps)
				rr = parser->ParsePps(nal, e - s) ? 0 : -101;
			else if (type == h264::ENalSlice || type == h264::ENalIdr)
				{
				Mem::Copy(aArena.Base(), nal, e - s);
				Mem::FillZ(aArena.Base() + (e - s), 64);
				rr = dec->DecodeSlice(nal, e - s, aPhys - KDramBus);
				}
			if (rr < 0)
				{
				RDebug::Printf("PH: venc2 f%02d DECODE ERROR nal type %d len %d -> %d", k, type, (TInt)(e - s), rr);
				if (!err)
					err = rr;
				}
			}
		}
	dec->Flush();
	aR.iDecoded = ctx->iCount;
	RDebug::Printf("PH: venc2 decoder: %d frames fed, %d pictures out, %u slices, %u VE job errors, picture %ux%u r=%d",
		aR.iEncoded, ctx->iCount, dec->iSlices, dec->iJobErrors, dec->iWidth, dec->iHeight, err);
	aVe.WriteReg(KVeCtrl, 0x7);
	delete dec;
	delete[] ctx->iY;
	delete[] ctx->iUV;
	delete ctx;
	delete job;
	delete parser;
	return err;
	}

// ---------------------------------------------------------------- a run: encode + decode + report
struct TSummary
	{
	TBool iOk;				// every frame encoded and decoded
	TInt iMinY, iMinC, iAvgY, iFps100, iTotalBytes, iAvgP;
	TInt iJobI, iJobPAvg, iJobPMax, iJobPMin;
	};

static TInt RunOne(RPheVde& aVe, RChunk& aArena, TUint32 aPhys, const TGen& aGen, const char* aTag, const TRc& aRc, TSummary& aS)
	{
	TRun* r = new TRun;
	if (!r)
		return KErrNoMemory;
	Mem::FillZ(r, sizeof(TRun));
	r->iW = aGen.iW;
	r->iH = aGen.iH;
	r->iStream = new TUint8[KStreamCap];
	if (!r->iStream)
		{
		delete r;
		return KErrNoMemory;
		}
	RDebug::Printf("PH: venc2 %s begin: %dx%d, GOP %d (IDR + %d P), %s", aTag, r->iW, r->iH, aRc.iFrames, aRc.iFrames - 1,
		aRc.iFixed ? "fixed QP" : "rate control");
	const TInt er = EncodeStream(aVe, aArena, aPhys, aGen, aRc, *r);
	TInt dr = KErrNone;
	if (r->iEncoded > 0)
		dr = DecodeStream(aVe, aArena, aPhys, aGen, *r);
	TInt sumY = 0, sumP = 0, total = 0;
	aS.iMinY = aS.iMinC = 99999;
	for (TInt k = 0; k < r->iEncoded; ++k)
		{
		const TBool dec = k < r->iDecoded;
		if (!aRc.iQuiet)
		RDebug::Printf("PH: venc2 %s f%02d %c qp %d bytes %d enc_us %d job_us %d PSNR Y %d.%02d C %d.%02d", aTag, k, k ? 'P' : 'I',
			r->iQp[k], r->iBytes[k], r->iUs[k], r->iJobUs[k], PS(dec ? r->iPy[k] : 0), PS(dec ? r->iPc[k] : 0));
		total += r->iBytes[k];
		if (k)
			sumP += r->iBytes[k];
		if (dec)
			{
			aS.iMinY = Min(aS.iMinY, r->iPy[k]);
			aS.iMinC = Min(aS.iMinC, r->iPc[k]);
			sumY += r->iPy[k];
			}
		}
	aS.iOk = er == KErrNone && dr == KErrNone && r->iEncoded == aRc.iFrames && r->iDecoded == aRc.iFrames;
	aS.iTotalBytes = total;
	aS.iAvgP = r->iEncoded > 1 ? sumP / (r->iEncoded - 1) : 0;
	aS.iAvgY = r->iDecoded ? sumY / r->iDecoded : 0;
	aS.iFps100 = r->iTotalUs > 0 ? (TInt)((TInt64)r->iEncoded * 100000000 / r->iTotalUs) : 0;
	if (!r->iDecoded)
		aS.iMinY = aS.iMinC = 0;
	TInt jobUs = 0, sumJobP = 0;
	aS.iJobI = r->iEncoded ? r->iJobUs[0] : 0;
	aS.iJobPMax = 0;
	aS.iJobPMin = 1 << 30;
	for (TInt k = 1; k < r->iEncoded; ++k)
		{
		sumJobP += r->iJobUs[k];
		aS.iJobPMax = Max(aS.iJobPMax, r->iJobUs[k]);
		aS.iJobPMin = Min(aS.iJobPMin, r->iJobUs[k]);
		}
	aS.iJobPAvg = r->iEncoded > 1 ? sumJobP / (r->iEncoded - 1) : 0;
	if (r->iEncoded < 2)
		aS.iJobPMin = 0;
	for (TInt k = 0; k < r->iEncoded; ++k)
		jobUs += r->iJobUs[k];
	RDebug::Printf("PH: venc2 %s summary: encoded %d decoded %d, %d bytes (I %d, P avg %d), PSNR Y min %d.%02d avg %d.%02d C min %d.%02d, "
		"encode %d us total (job-only %d us) = %d.%02d fps, enc r=%d dec r=%d %s", aTag, r->iEncoded, r->iDecoded, total, r->iBytes[0],
		aS.iAvgP, PS(aS.iMinY), PS(aS.iAvgY), PS(aS.iMinC), r->iTotalUs, jobUs, PS(aS.iFps100), er, dr, aS.iOk ? "OK" : "FAIL");
	delete[] r->iStream;
	delete r;
	return KErrNone;
	}

// ---------------------------------------------------------------- speed variants (ve-encoder-plan.md section 13)
// ME_PARA bits (Bootlin cedrus_regs.h 803-822)
const TUint32 KMeWbMvDis = 1u << 16, KMeSplitMbDis = 1u << 15, KMeIntra4Dis = 1u << 14, KMeQpixSplitOff = 1u << 21,
	KMeQpixSmartOff = 1u << 20, KMeImeTimePrio = 1u << 5, KMeQsmtssDis = 1u << 3, KMeQsmtssRefineDis = 1u << 4, KMeMeDis = 1u << 8;
const TUint32 KPara1TfHisOutDis = 1u << 19;		// PARA1: temporal-filter history output disable (cedrus_regs.h 793)
const TUint32 KPara0DeblockOff = 1u << 12;		// PARA0 bits 13:12 = 1: deblocking off (cedrus_regs.h 776)
const TUint32 KMeFast = KMeWbMvDis | KMeSplitMbDis | KMeIntra4Dis | KMeQpixSplitOff | KMeQpixSmartOff | KMeImeTimePrio | KMeQsmtssDis |
	KMeQsmtssRefineDis;

struct TMeVariant
	{
	const char* iName;
	TUint32 iMe, iPara1, iPara0;
	TInt iDeblock;
	};
static const TMeVariant KMeVariants[] =
	{
	{ "base-fme2", KMeWbMvDis | (2u << 1), 0, 0, 0 },						// what rung 2 ran
	{ "fme0", KMeWbMvDis, 0, 0, 0 },
	{ "fast", KMeFast, 0, 0, 0 },
	{ "fast+tf", KMeFast, KPara1TfHisOutDis, 0, 0 },
	{ "fast+tf+nodeblk", KMeFast, KPara1TfHisOutDis, KPara0DeblockOff, 1 },
	{ "me-off", KMeFast | KMeMeDis, KPara1TfHisOutDis, 0, 0 }				// zero-motion floor: how much of the time is the search
	};
const TInt KMeCount = sizeof(KMeVariants) / sizeof(KMeVariants[0]);

static TRc MakeRc(TBool aFixed, const TMeVariant& aV, TInt aFrames)
	{
	TRc rc;
	rc.iFixed = aFixed;
	rc.iQp0 = aFixed ? KFixedQp : 30;
	rc.iTargetI = rc.iTargetP = 0;
	rc.iMePara = aV.iMe;
	rc.iPara1Extra = aV.iPara1;
	rc.iPara0Extra = aV.iPara0;
	rc.iDeblockIdc = aV.iDeblock;
	rc.iFrames = aFrames;
	rc.iQuiet = EFalse;
	rc.iModeBase = KModeBase;
	rc.iBufCtrl = -1;
	rc.iBitBuf = KBitstreamSize;
	rc.iIspCtrl = 0;
	rc.iPlanar = EFalse;
	rc.iDump = EFalse;
	return rc;
	}

// PLL_VE factor: VE clock = 24 MHz * N / M (VE_CLK divider 1); mainline cedrus runs the A64 at 402 MHz (cedrus.c variant sun50i_a64)
static TUint32 PllFor(TUint32 aOrig, TInt aN, TInt aM)
	{
	return (aOrig & ~((1u << 31) | (1u << 28) | 0x7F0Fu)) | ((TUint32)(aN - 1) << 8) | (TUint32)(aM - 1);
	}


// ---------------------------------------------------------------- diagnostics: register dumps
static void DumpVe(RPheVde& aVe, const char* aTag, TInt aFrom, TInt aTo)		// non-zero words of the VE window
	{
	TBuf8<200> b;
	for (TInt off = aFrom; off < aTo; off += 4)
		{
		TUint32 v = 0;
		aVe.ReadReg(off, v);
		if (!v)
			continue;
		b.AppendFormat(_L8("%03x=%08x "), off, v);
		if (b.Length() > 140)
			{
			b.ZeroTerminate();
			RDebug::Printf("PH: venc2 dump ve %s: %s", aTag, b.Ptr());
			b.Zero();
			}
		}
	if (b.Length())
		{
		b.ZeroTerminate();
		RDebug::Printf("PH: venc2 dump ve %s: %s", aTag, b.Ptr());
		}
	}

static void DumpPhys(RPheVde& aVe, const char* aTag, TUint32 aFrom, TUint32 aTo, TBool aSkipZero)
	{
	TBuf8<200> b;
	TInt firstErr = 0;
	for (TUint32 a = aFrom; a < aTo; a += 4)
		{
		TUint32 v = 0;
		const TInt r = aVe.PhysRead(a, v);
		if (r != KErrNone)
			{
			firstErr = r;
			break;
			}
		if (aSkipZero && !v)
			continue;
		b.AppendFormat(_L8("%03x=%08x "), a & 0xFFF, v);
		if (b.Length() > 140)
			{
			b.ZeroTerminate();
			RDebug::Printf("PH: venc2 dump %s: %s", aTag, b.Ptr());
			b.Zero();
			}
		}
	if (b.Length())
		{
		b.ZeroTerminate();
		RDebug::Printf("PH: venc2 dump %s: %s", aTag, b.Ptr());
		}
	if (firstErr)
		RDebug::Printf("PH: venc2 dump %s: PhysRead r=%d (kernel without the control?)", aTag, firstErr);
	}

// ---------------------------------------------------------------- sweep 2: the per-MB cost (section 14). All start from the "fast+tf" ME row.
const TInt KVar2Count = 15;
static const char* Var2Name(TInt i)
	{
	static const char* const n[KVar2Count] = { "ref", "mode=REC1MB+BW128", "mode=REC2MB+BW256", "mode=REC1MB+BW256", "mode=REC2MB+mode0", "mode=0",
		"bufctrl=0(int,int)", "bufctrl=0xA(ext,ext)", "bufctrl=5(mixed)", "bitbuf=64K", "isp=I420 planar", "isp=YVU420SP(chroma swapped)",
		"me=+TPIX/DEBLK_TO_DRAM", "me=+INTRA_PLAN_DIS+INTRA_PRED_DIS", "isp=BT709+int" };
	return n[i];
	}
static void ApplyVar2(TRc& aRc, TInt i)
	{
	switch (i)
		{
	case 1: aRc.iModeBase = 0x030000u; break;
	case 2: aRc.iModeBase = 0x120000u; break;
	case 3: aRc.iModeBase = 0x020000u; break;
	case 4: aRc.iModeBase = 0x100000u; break;
	case 5: aRc.iModeBase = 0; break;
	case 6: aRc.iBufCtrl = 0x00; break;
	case 7: aRc.iBufCtrl = 0x0A; break;
	case 8: aRc.iBufCtrl = 0x05; break;
	case 9: aRc.iBitBuf = 64 << 10; break;
	case 10: aRc.iPlanar = ETrue; aRc.iIspCtrl = 24u << 27; break;
	case 11: aRc.iIspCtrl = 4u << 27; break;
	case 12: aRc.iMePara |= (1u << 19) | (1u << 18); break;
	case 13: aRc.iMePara |= (1u << 13) | 1u; break;
	case 14: aRc.iIspCtrl = (1u << 1) | 1u; break;
		}
	}

static TInt MhzOf(TUint32 aPll) { return 24 * (((aPll >> 8) & 0x7F) + 1) / ((aPll & 0xF) + 1); }

TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	TInt f = 0;
	HAL::Get(HAL::EFastCounterFrequency, f);
	gFreq = f > 0 ? (TUint32)f : 1000000u;
	RPheVde ve;
	TInt r = ve.Open();
	RDebug::Printf("PH: venc2 open r=%d fastcounter %u Hz", r, gFreq);
	if (r != KErrNone)
		{
		RDebug::Printf("PH: venc2 FAIL");
		return 0;
		}
	TPheVdeInfo info;
	ve.GetInfo(info);
	const TUint32 origPll = info.iPllVe;
	RDebug::Printf("PH: venc2 info version=%08x irqs=%u PLL_VE=%08x (%d MHz)", info.iVersion, info.iIrqCount, origPll, MhzOf(origPll));
	RChunk arena;
	TUint32 phys = 0;
	r = ve.OpenArena(arena);
	TInt rc = r;
	if (r == KErrNone)
		{
		rc = KErrNoMemory;
		for (TInt mb = 64; mb >= 48 && rc != KErrNone; mb -= 16)
			{
			gArenaSize = mb << 20;
			rc = ve.Commit(0, gArenaSize, phys);
			}
		}
	RDebug::Printf("PH: venc2 arena r=%d commit r=%d %d MiB phys=%08x", r, rc, gArenaSize >> 20, phys);
	TBool decodeOk = ETrue, psnrOk = ETrue, fpsOk = EFalse;
	if (rc == KErrNone)
		{
		// ---- diagnostics: the SoC state around the VE (read only)
		DumpVe(ve, "idle 000-0fc", 0x000, 0x100);
		DumpPhys(ve, "CCU 018-0a0", 0x01C20018, 0x01C200A4, ETrue);
		DumpPhys(ve, "CCU 0f0-180 (MBUS_CLK is 15c)", 0x01C200F0, 0x01C20184, ETrue);
		DumpPhys(ve, "CCU bus gates/resets 2c0-2d8", 0x01C202C0, 0x01C202DC, ETrue);
		DumpPhys(ve, "SYSCON 00-1c", 0x01C00000, 0x01C00020, EFalse);
		DumpPhys(ve, "DRAMCOM (MBUS) 00-ac", 0x01C62000, 0x01C620B0, EFalse);

		TGen g720(1280, 720);
		TRc bestRc = MakeRc(ETrue, KMeVariants[0], KGop);
		TInt bestUs = 1 << 30, bestClk = 0;
		const char* bestName = "none";
		const TInt clocks[2][2] = { { 0, 0 }, { 67, 4 } };	// original, 402 MHz (432 is unstable: PSNR broke)
		TInt useClk = 0;
		for (TInt c = 0; c < 2; ++c)
			{
			TUint32 pllNow = 0;
			if (c)
				{
				const TInt sr = ve.SetPll(PllFor(origPll, clocks[c][0], clocks[c][1]) | (1u << 31), pllNow);
				RDebug::Printf("PH: venc2 clock: SetPll 402 MHz r=%d PLL_VE now %08x%s", sr, pllNow,
					sr == KErrNotSupported ? " (kernel without the SetPll control: staying at the boot clock)" : "");
				if (sr != KErrNone)
					break;
				useClk = 1;
				}
			const TInt mhz = c ? 402 : MhzOf(origPll);
			// sweep 1 (as section 13): ME effort rows
			for (TInt m = 0; m < KMeCount; ++m)
				{
				if (m != 0 && m != 3 && m != 4)
					continue;
				TRc rcv = MakeRc(ETrue, KMeVariants[m], 6);
				rcv.iQuiet = ETrue;
				TSummary s;
				TBuf8<64> tag;
				tag.Format(_L8("s1 %dMHz %s"), mhz, KMeVariants[m].iName);
				tag.ZeroTerminate();
				RunOne(ve, arena, phys, g720, (const char*)tag.Ptr(), rcv, s);
				RDebug::Printf("PH: venc2 VARIANT s1 clk=%d me=%s 720p: I job %d us, P job avg %d us (%d.%02d fps job-only), P bytes %d, PSNR Y min %d.%02d, %s",
					mhz, KMeVariants[m].iName, s.iJobI, s.iJobPAvg, PS(s.iJobPAvg ? (TInt)(100000000LL / s.iJobPAvg) : 0), s.iAvgP, PS(s.iMinY),
					s.iOk ? "decode OK" : "FAIL");
				}
			if (c != useClk)
				continue;
			// sweep 2 at this clock, base = the "fast+tf" ME row
			for (TInt v = 0; v < KVar2Count; ++v)
				{
				TRc rcv = MakeRc(ETrue, KMeVariants[3], 6);
				rcv.iQuiet = ETrue;
				rcv.iDump = (v == 0);
				ApplyVar2(rcv, v);
				TSummary s;
				TBuf8<80> tag;
				tag.Format(_L8("s2 %dMHz %s"), mhz, Var2Name(v));
				tag.ZeroTerminate();
				RunOne(ve, arena, phys, g720, (const char*)tag.Ptr(), rcv, s);
				const TBool valid = s.iOk && s.iMinY >= 3200;
				RDebug::Printf("PH: venc2 VARIANT s2 clk=%d %s 720p: I job %d us, P job avg %d (min %d max %d) us (%d.%02d fps job-only), P bytes %d, "
					"PSNR Y min %d.%02d, %s%s", mhz, Var2Name(v), s.iJobI, s.iJobPAvg, s.iJobPMin, s.iJobPMax,
					PS(s.iJobPAvg ? (TInt)(100000000LL / s.iJobPAvg) : 0), s.iAvgP, PS(s.iMinY), s.iOk ? "decode OK" : "FAIL",
					valid ? "" : " (not eligible)");
				if (valid && s.iJobPAvg > 0 && s.iJobPAvg < bestUs)
					{
					bestUs = s.iJobPAvg;
					bestRc = rcv;
					bestRc.iFrames = KGop;
					bestRc.iQuiet = EFalse;
					bestRc.iDump = EFalse;
					bestName = Var2Name(v);
					bestClk = c;
					}
				}
			}
		RDebug::Printf("PH: venc2 sweep best: %d MHz, %s (P job %d us); using it for the criteria runs", bestClk ? 402 : MhzOf(origPll), bestName,
			bestUs);
		DumpPhys(ve, "DRAMCOM after (MBUS) 00-ac", 0x01C62000, 0x01C620B0, EFalse);
		// ---- the criteria: 16-frame streams at the best settings (the clock is still the sweep's last = 402 when supported)
		const TInt sizes[2][2] = { { 640, 480 }, { 1280, 720 } };
		for (TInt s = 0; s < 2; ++s)
			{
			TGen gen(sizes[s][0], sizes[s][1]);
			TBuf8<48> tag;
			TSummary fixed, rcs;
			TRc fx = bestRc;
			fx.iFixed = ETrue;
			fx.iQp0 = KFixedQp;
			tag.Format(_L8("%dx%d-fixed28"), sizes[s][0], sizes[s][1]); tag.ZeroTerminate();
			RunOne(ve, arena, phys, gen, (const char*)tag.Ptr(), fx, fixed);
			TRc rcc = bestRc;
			rcc.iFixed = EFalse;
			rcc.iQp0 = 30;
			rcc.iTargetP = Max(800, fixed.iAvgP * 8 / 2);
			tag.Format(_L8("%dx%d-rc"), sizes[s][0], sizes[s][1]); tag.ZeroTerminate();
			RDebug::Printf("PH: venc2 %s target %d bits per P frame (half of the QP28 average %d bytes)", tag.Ptr(), rcc.iTargetP, fixed.iAvgP);
			RunOne(ve, arena, phys, gen, (const char*)tag.Ptr(), rcc, rcs);
			decodeOk = decodeOk && fixed.iOk && rcs.iOk;
			psnrOk = psnrOk && fixed.iOk && fixed.iMinY >= 3200;
			if (s == 1)
				fpsOk = fixed.iFps100 >= 3000;
			}
		if (origPll)
			{
			TUint32 pllNow = 0;
			ve.SetPll(origPll | (1u << 31), pllNow);
			}
		}
	else
		decodeOk = psnrOk = EFalse;
	const TBool pass = decodeOk && psnrOk && fpsOk;
	RDebug::Printf("PH: venc2 criteria: all frames decode %s, PSNR Y min >= 32 dB at QP 28 %s, 720p encode >= 30 fps %s",
		decodeOk ? "yes" : "NO", psnrOk ? "yes" : "NO", fpsOk ? "yes" : "NO");
	RDebug::Printf("PH: venc2 %s", pass ? "PASS" : "FAIL");
	arena.Close();
	ve.Close();
	delete cleanup;
	return 0;
	}
