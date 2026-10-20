// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// phevenctest.cpp -- OURS (VE encoder one-frame probe, docs/phorepoc/ve-encoder-plan.md): encodes ONE 64x64 NV12 intra
// picture on the A64 Video Engine's H.264 encoder (register map from research/ve-h264-enc-regs.md, unproven on the A64),
// dumps the bitstream (size, first bytes, CRC-32) and decodes it back with OUR VE decoder (src/phevdec, bit-exact in V3b),
// comparing with the source by PSNR. All output is "PH: venc ..." lines; SSM_TEST=phevenctest stages it at boot.
// A variant loop covers the three uncertain encodings (engine select, bit-count in the trigger, ISP colour code).
#include <e32base.h>
#include <e32debug.h>
#include <e32math.h>

#include "ve_rphevde.h"
#include "ve_h264enc.h"

using namespace h264enc;

const TInt KW = 64, KH = 64, KMbW = KW / 16, KMbH = KH / 16, KQp = 28;
const TInt KBitstreamSize = 1 << 20;		// arena offset 0
const TInt KArenaSize = 8 << 20;

struct TVariant
	{
	TBool iBootlin;				// engine select as Bootlin's cedrus encoder (VE_MODE bits 7+6 + reset pulse), else jemk's 0xB
	TBool iTrigMinus1;			// bit-write trigger carries n-1 (else n)
	TInt iIspFmt;				// ISP_CTRL colour code << 29 for NV12
	};

// the hardware bit writer: BASIC_BITS then TRIGGER (1 | n << 8); AVC_PARAM bit 31 = no emulation prevention
class THwBits : public MBitSink
	{
public:
	THwBits(RPheVde& aVe, const TVariant& aV) : iVe(aVe), iV(aV), iWrites(0) {}
	void Put(uint32_t aValue, int aBits)
		{
		TUint32 st = 0;
		for (TInt i = 0; i < 1000; ++i)			// Bootlin: wait for STATUS bit 9 (PUT_BITS_READY) before each put
			{
			iVe.ReadReg(KAvcStatus, st);
			if (st & (1u << 9))
				break;
			}
		iVe.WriteReg(KAvcBasicBits, aValue);
		iVe.WriteReg(KAvcTrigger, 0x1 | (((iV.iTrigMinus1 ? aBits - 1 : aBits)) << 8));
		iWrites += 2;
		}
	void NoEmulationPrevention(bool aOff) { iVe.WriteReg(KAvcParam, aOff ? 0x80000000u : 0u); }
	RPheVde& iVe;
	TVariant iV;
	TInt iWrites;
	};

static TInt Psnr(const TUint8* aA, const TUint8* aB, TInt aN)		// hundredths of a dB (9999 = identical)
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

static void HexDump(const char* aTag, const TUint8* aP, TInt aN)
	{
	for (TInt at = 0; at < aN; at += 32)
		{
		TBuf8<128> b;
		const TInt n = Min(32, aN - at);
		for (TInt i = 0; i < n; ++i)
			b.AppendFormat(_L8("%02x"), aP[at + i]);
		b.ZeroTerminate();
		RDebug::Printf("PH: venc %s +%03x %s", aTag, at, b.Ptr());
		}
	}

static void ReadRegs(RPheVde& aVe, const char* aTag)
	{
	static const TUint16 KRegs[] = { KVeCtrl, KVeVersion, KIspSize, KIspStride, KIspCtrl, KIspLuma, KIspChroma, KAvcParam,
		KAvcQp, KAvcMotionEst, KAvcCtrl, KAvcStatus, KAvcUnkBuf, KAvcVleAddr, KAvcVleEnd, KAvcVleOffset, KAvcVleMax,
		KAvcVleLength, KAvcRecLuma, KAvcRecChroma, KAvcRecSluma, KAvcMbInfo };
	TBuf8<200> b;
	for (TUint i = 0; i < sizeof(KRegs) / sizeof(KRegs[0]); ++i)
		{
		TUint32 v = 0;
		aVe.ReadReg(KRegs[i], v);
		b.AppendFormat(_L8("%03x=%08x "), KRegs[i], v);
		if (b.Length() > 150 || i == sizeof(KRegs) / sizeof(KRegs[0]) - 1)
			{
			b.ZeroTerminate();
			RDebug::Printf("PH: venc regs %s: %s", aTag, b.Ptr());
			b.Zero();
			}
		}
	}

// source picture: smooth gradient + a bright square + a dark bar, chroma ramps
static void MakeSource(TUint8* aY, TUint8* aUV)
	{
	for (TInt y = 0; y < KH; ++y)
		for (TInt x = 0; x < KW; ++x)
			{
			TInt v = 40 + x * 2 + y;
			if (x >= 16 && x < 32 && y >= 16 && y < 32)
				v = 220;
			if (y >= 48 && y < 52)
				v = 30;
			aY[y * KW + x] = (TUint8)v;
			}
	for (TInt y = 0; y < KH / 2; ++y)
		for (TInt x = 0; x < KW / 2; ++x)
			{
			aUV[y * KW + 2 * x] = (TUint8)(96 + x * 2);			// Cb
			aUV[y * KW + 2 * x + 1] = (TUint8)(160 - y * 2);	// Cr
			}
	}

// decode the Annex B stream with the VE decoder; returns the PSNR pair via aPsnrY/aPsnrC, <0 on a failure
struct TOut
	{
	TInt iCount;
	h264::TPicture iPic;
	};
static void OnPicture(void* aCtx, const h264::TPicture& aPic)
	{
	TOut& o = *(TOut*)aCtx;
	if (!o.iCount++)
		o.iPic = aPic;
	}

static TInt DecodeBack(RPheVde& aVe, RChunk& aArena, TUint32 aPhys, TVeArenaMem& aMem, const TUint8* aStream, TInt aLen,
	const TUint8* aSrcY, const TUint8* aSrcUV, TInt& aPsnrY, TInt& aPsnrC)
	{
	h264::TParser* parser = new h264::TParser;
	TVeJobAdapter* job = new TVeJobAdapter(aVe);
	TOut* out = new TOut;
	h264::TVeH264* dec = parser && job && out ? new h264::TVeH264(*parser, *job, aMem) : NULL;
	if (!dec)
		return KErrNoMemory;
	out->iCount = 0;
	dec->iOnPicture = OnPicture;
	dec->iCtx = out;
	size_t pos = 0, s = 0, e = 0;
	TInt nals = 0, err = 0;
	while (h264::NextNal(aStream, aLen, pos, s, e))
		{
		const TUint8* nal = aStream + s;
		const TInt type = nal[0] & 0x1F;
		++nals;
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
		RDebug::Printf("PH: venc dec nal %d type %d len %d -> %d", nals, type, (TInt)(e - s), rr);
		if (rr < 0 && !err)
			err = rr;
		}
	dec->Flush();
	TInt r = err ? err : (out->iCount == 1 ? KErrNone : KErrCorrupt);
	if (out->iCount >= 1)
		{
		const TUint8* y = aMem.Cpu(out->iPic.iLumaBus);
		const TUint8* uv = aMem.Cpu(out->iPic.iChromaBus);
		TUint8 row[KW];
		TUint8* ycopy = new TUint8[KW * KH];
		TUint8* uvcopy = new TUint8[KW * KH / 2];
		if (ycopy && uvcopy)
			{
			for (TInt i = 0; i < KH; ++i)
				{
				Mem::Copy(row, y + i * dec->iStride, KW);
				Mem::Copy(ycopy + i * KW, row, KW);
				}
			for (TInt i = 0; i < KH / 2; ++i)
				{
				Mem::Copy(row, uv + i * dec->iStride, KW);
				Mem::Copy(uvcopy + i * KW, row, KW);
				}
			aPsnrY = Psnr(ycopy, aSrcY, KW * KH);
			aPsnrC = Psnr(uvcopy, aSrcUV, KW * KH / 2);
			RDebug::Printf("PH: venc dec picture %ux%u Y[0..3] %02x %02x %02x %02x (source %02x %02x %02x %02x)", dec->iWidth,
				dec->iHeight, ycopy[0], ycopy[1], ycopy[2], ycopy[3], aSrcY[0], aSrcY[1], aSrcY[2], aSrcY[3]);
			}
		delete[] ycopy;
		delete[] uvcopy;
		}
	RDebug::Printf("PH: venc dec %d NALs, %d pictures, %u slices, %u VE job errors, r=%d", nals, out->iCount, dec->iSlices,
		dec->iJobErrors, r);
	aVe.WriteReg(KVeCtrl, 0x7);
	delete dec;
	delete out;
	delete job;
	delete parser;
	return r;
	}

// Engine select. Bootlin's cedrus encoder (cedrus_enc_format_coded_configure): VE_MODE &= ~(ENC 1<<7 | ISP 1<<6),
// |= DEC_DISABLED (7); VE_RESET (0x004) |= ENCODER_RESET (1<<24), then &= ~it; VE_MODE |= ENC | ISP | DEC_DISABLED.
// jemk's engine id 0xB (low nibble) is a DECODER mode number on this block (mode 3 = RV), not an encoder select.
const TUint32 KModeBase = 0x00130000u;	// DDR mode + rec-write bits, as the decoder writes
const TUint32 KModeEnc = 1u << 7, KModeIsp = 1u << 6;
const TInt KVeReset = 0x004;
const TUint32 KResetEncoder = 1u << 24;

static void EnterEncoderMode(RPheVde& aVe)
	{
	TUint32 v = 0;
	aVe.ReadReg(KVeCtrl, v);
	aVe.WriteReg(KVeCtrl, ((v | KModeBase) & ~(KModeEnc | KModeIsp)) | 7);
	aVe.ReadReg(KVeReset, v);
	aVe.WriteReg(KVeReset, v | KResetEncoder);
	aVe.ReadReg(KVeReset, v);
	aVe.WriteReg(KVeReset, v & ~KResetEncoder);
	aVe.ReadReg(KVeCtrl, v);
	aVe.WriteReg(KVeCtrl, v | KModeEnc | KModeIsp | 7);
	}

// write a pattern to plain storage registers of the ISP / AVC blocks and read it back
static void ProbeEncRegs(RPheVde& aVe, const char* aTag)
	{
	static const TUint16 KProbe[] = { KIspStride, KAvcVleOffset, KAvcVleMax };
	TUint32 ctrl = 0, rst = 0;
	aVe.ReadReg(KVeCtrl, ctrl);
	aVe.ReadReg(KVeReset, rst);
	TBuf8<160> b;
	for (TUint i = 0; i < sizeof(KProbe) / sizeof(KProbe[0]); ++i)
		{
		TUint32 old = 0, back = 0;
		aVe.ReadReg(KProbe[i], old);
		aVe.WriteReg(KProbe[i], 0x5A5A5A5Au);
		aVe.ReadReg(KProbe[i], back);
		aVe.WriteReg(KProbe[i], old);
		b.AppendFormat(_L8("%03x: wrote 5a5a5a5a read %08x | "), KProbe[i], back);
		}
	b.ZeroTerminate();
	RDebug::Printf("PH: venc probe %s: VE_CTRL %08x VE_RESET %08x %s", aTag, ctrl, rst, b.Ptr());
	}

// one encode with a variant; returns 1 on PASS, 0 otherwise
static TInt EncodeOnce(RPheVde& aVe, RChunk& aArena, TUint32 aPhys, const TVariant& aV, TInt aTry)
	{
	TVeArenaMem mem(aArena, aPhys, KArenaSize, KBitstreamSize);
	TInt idx;
	const TInt mbw = KMbW, mbh = KMbH;
	const TUint32 luma = (((mbw * 16) + 31) & ~31) * (((mbh * 16) + 31) & ~31);
	const TUint32 chroma = (((mbw * 16) + 31) & ~31) * (((mbh * 8) + 31) & ~31);
	const TUint32 inBus = mem.Alloc(KW * KH * 3 / 2, 4096, idx);
	const TUint32 recBus = mem.Alloc(luma + chroma + luma / 4, 4096, idx);
	const TUint32 frameBus = mem.Alloc(4096, 4096, idx);
	const TUint32 lineBus = mem.Alloc(4096, 4096, idx);
	if (!inBus || !recBus || !frameBus || !lineBus)
		return 0;
	TUint8* base = aArena.Base();
	Mem::FillZ(base, KBitstreamSize);
	Mem::FillZ(mem.Cpu(recBus), luma + chroma + luma / 4);
	Mem::FillZ(mem.Cpu(frameBus), 4096);
	Mem::FillZ(mem.Cpu(lineBus), 4096);
	TUint8* src = mem.Cpu(inBus);
	MakeSource(src, src + KW * KH);
	TUint8 srcY[KW * KH], srcUV[KW * KH / 2];
	Mem::Copy(srcY, src, KW * KH);
	Mem::Copy(srcUV, src + KW * KH, KW * KH / 2);

	RDebug::Printf("PH: venc try %d: engine select %s, trigger %s, isp fmt %d", aTry, aV.iBootlin ? "Bootlin (bits 7+6)" : "jemk 0xB",
		aV.iTrigMinus1 ? "n-1" : "n", aV.iIspFmt);
	TUint32 ctrl = 0;
	aVe.ReadReg(KVeCtrl, ctrl);
	if (aV.iBootlin)
		EnterEncoderMode(aVe);
	else
		aVe.WriteReg(KVeCtrl, 0x0013000Bu);
	TUint32 sel = 0;
	aVe.ReadReg(KVeCtrl, sel);
	RDebug::Printf("PH: venc VE_CTRL %08x -> %08x", ctrl, sel);
	ProbeEncRegs(aVe, aV.iBootlin ? "after Bootlin select" : "after jemk select");
	aVe.WriteReg(KAvcStatus, 0x7);
	aVe.WriteReg(KAvcTrigger, 0);
	aVe.WriteReg(0xB90, 0);					// STM_BIT_LEN
	aVe.WriteReg(0xB54, 0);					// RESIDUAL_BITS
	aVe.WriteReg(0xB58, 0);					// HEADER_BITS
	// the bitstream window
	aVe.WriteReg(KAvcVleOffset, 0);
	aVe.WriteReg(KAvcVleAddr, aPhys - KDramBus);
	aVe.WriteReg(KAvcVleEnd, aPhys - KDramBus + KBitstreamSize - 1);
	aVe.WriteReg(KAvcVleMax, (TUint32)KBitstreamSize * 8);
	// the headers through the hardware bit writer
	THwBits sink(aVe, aV);
	THeaders hdr(sink, mbw, mbh, KQp);
	hdr.Sps();
	hdr.Pps();
	hdr.IdrSliceHeader();
	// the input picture + buffers
	aVe.WriteReg(KIspStride, (TUint32)(KW / 16) << 16);
	aVe.WriteReg(KIspSize, ((TUint32)mbw << 16) | mbh);
	aVe.WriteReg(KIspCtrl, (TUint32)aV.iIspFmt << 29);
	aVe.WriteReg(KIspLuma, inBus);
	aVe.WriteReg(KIspChroma, inBus + KW * KH);
	aVe.WriteReg(KAvcRecLuma, recBus);
	aVe.WriteReg(KAvcRecChroma, recBus + luma);
	aVe.WriteReg(KAvcRecSluma, recBus + luma + chroma);
	aVe.WriteReg(KAvcMbInfo, lineBus);
	aVe.WriteReg(KAvcUnkBuf, frameBus);
	if (aV.iBootlin)
		{
		// the rest of Bootlin's job_configure: registers zeroed per job (rate control, dynamic ME, statistics, deblock, refresh)
		static const TUint16 KZero[] = { 0xB0C, 0xB28, 0xB2C, 0xB30, 0xB34, 0xB38, 0xB3C, 0xB48, 0xB4C, 0xB50, 0xB5C, 0xBC4 };
		for (TUint i = 0; i < sizeof(KZero) / sizeof(KZero[0]); ++i)
			aVe.WriteReg(KZero[i], 0);
		aVe.WriteReg(0xBA0, recBus);				// REF0 = REC for an I frame
		aVe.WriteReg(0xBA4, recBus + luma);
		}
	TUint32 avcCtrl = 0;
	aVe.ReadReg(KAvcCtrl, avcCtrl);
	ReadRegs(aVe, "before start");
	// the start: one job, the trigger last
	TVeJobAdapter job(aVe);
	job.Begin();
	job.Write(KAvcCtrl, avcCtrl | 0xF);
	job.Write(KAvcStatus, 0x7);
	job.Write(KAvcParam, 0);				// CAVLC, I slice, emulation prevention on
	// Bootlin PARA1: chroma qp offset [18:16]=4, stride_mbs/48 rounded up [13:10]=1, fixed QP [5:0]; ME_PARA: WB_MV_INFO_DIS | FME level 2
	if (aV.iBootlin)
		{
		job.Write(KAvcQp, (4u << 16) | (1u << 10) | KQp);
		job.Write(KAvcMotionEst, (1u << 16) | (2u << 1));
		}
	else
		{
		job.Write(KAvcQp, (4u << 16) | (KQp << 8) | KQp);
		job.Write(KAvcMotionEst, 0x104);
		}
	job.Write(KAvcTrigger, 0x8);
	const TInt rr = job.Run(KAvcStatus, 0x7, KAvcCtrl, 0xF);
	TUint32 status = 0, vle = 0;
	aVe.ReadReg(KAvcStatus, status);
	aVe.ReadReg(KAvcVleLength, vle);
	RDebug::Printf("PH: venc run r=%d (%s) status %08x vle_length %u bits, %u ms", rr, rr == KErrTimedOut ? "timed out" : "done",
		status, vle, job.iMs);
	ReadRegs(aVe, "after run");
	TInt pass = 0;
	const TInt bytes = (TInt)((vle + 7) / 8);
	const TBool ok = rr >= 0 && (rr & 3) == 1 && bytes > 0 && bytes < KBitstreamSize;
	if (bytes > 0 && bytes < KBitstreamSize)
		{
		const TUint8* es = base;
		RDebug::Printf("PH: venc stream %d bytes, crc32 %08x, first %02x %02x %02x %02x %02x %02x %02x %02x", bytes,
			Crc32(es, bytes), es[0], es[1], es[2], es[3], es[4], es[5], es[6], es[7]);
		HexDump("bytes", es, Min(bytes, 256));
		// scan for the end: the last non-zero byte in the buffer, in case VLE_LENGTH undercounts
		TInt last = KBitstreamSize - 1;
		while (last > 0 && !es[last])
			--last;
		RDebug::Printf("PH: venc buffer's last non-zero byte at %d (VLE_LENGTH says %d)", last, bytes - 1);
		TUint8* copy = new TUint8[bytes + 8];
		if (copy)
			{
			Mem::Copy(copy, es, bytes);
			Mem::FillZ(copy + bytes, 8);
			TInt py = 0, pc = 0;
			const TInt dr = DecodeBack(aVe, aArena, aPhys, mem, copy, bytes, srcY, srcUV, py, pc);
			RDebug::Printf("PH: venc decode-back r=%d PSNR Y %d.%02d dB C %d.%02d dB", dr, py / 100, py % 100, pc / 100, pc % 100);
			pass = ok && dr == KErrNone && py >= 3000;
			delete[] copy;
			}
		}
	aVe.WriteReg(KVeCtrl, 0x7);
	RDebug::Printf("PH: venc try %d %s", aTry, pass ? "PASS" : "FAIL");
	return pass;
	}

TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	RPheVde ve;
	TInt r = ve.Open();
	RDebug::Printf("PH: venc open r=%d", r);
	if (r != KErrNone)
		return 0;
	TPheVdeInfo info;
	ve.GetInfo(info);
	RDebug::Printf("PH: venc info version=%08x irqs=%u pll=%08x sram=%08x", info.iVersion, info.iIrqCount, info.iPllVe,
		info.iSramCtrl);
	RChunk arena;
	TUint32 phys = 0;
	r = ve.OpenArena(arena);
	TInt rc = r == KErrNone ? ve.Commit(0, KArenaSize, phys) : r;
	RDebug::Printf("PH: venc arena r=%d commit r=%d phys=%08x", r, rc, phys);
	TInt pass = 0, tries = 0;
	if (rc == KErrNone)
		{
		ve.WriteReg(KVeCtrl, 0x7);
		// diagnostic before the variant loop: do the encoder blocks answer once the encoder mode is selected?
		ProbeEncRegs(ve, "decoder-idle (VE_CTRL 7)");
		ve.WriteReg(KVeCtrl, 0x0013000Bu);
		ProbeEncRegs(ve, "jemk mode 0xB");
		EnterEncoderMode(ve);
		ProbeEncRegs(ve, "Bootlin encoder mode");
		ve.WriteReg(KVeCtrl, 0x7);
		for (TInt isp = 0; isp < 2 && !pass; ++isp)
			for (TInt trig = 0; trig < 2 && !pass; ++trig)
				for (TInt raw = 1; raw >= 0 && !pass; --raw)
					{
					TVariant v;
					v.iBootlin = raw;
					v.iTrigMinus1 = trig;
					v.iIspFmt = isp;
					pass = EncodeOnce(ve, arena, phys, v, ++tries);
					}
		}
	RDebug::Printf("PH: venc %s after %d tries", pass ? "PASS" : "FAIL", tries);
	arena.Close();
	ve.Close();
	RDebug::Printf("PH: venc done");
	delete cleanup;
	return 0;
	}
