// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// bench_main.cpp -- OURS (pheencbench): the PinePhone H.264 encode benchmark driver. For each resolution it builds UI-like source frames, times the
// colour conversion (scalar / NEON, XRGB8888 and RGB565 -> NV12 / I420, plus NEON straight into the VE arena) and then encodes N frames with
//   hw    the A64 Video Engine H.264 encoder (402 MHz), fixed QP 28 and rate control
//   sw1   OpenH264 on the A53, 1 thread, fixed QP 28 and rate control
//   sw4   OpenH264, 4 threads (4 slices)
// logging "[encbench] ..." lines (RDebug). CPU load = a spinner thread per core at the lowest priority counts the idle time. Docs:
// SSM_TEST=pheencbench.
#include <e32std.h>
#include <e32debug.h>
#include <hal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "bench_common.h"
#include "enc_hw.h"

using namespace pb;

const int KFrames = 120;			// frames per encode run
const int KMaxSeconds = 20;			// a run stops after this long (reported frames= says how many ran)
const int KStreamCap = 24 << 20;
const int KSettleSeconds = 20;		// wait for the boot/UI to settle before measuring

extern "C" uint32_t ph_ticks(void) { return User::FastCounter(); }
static uint32_t gFreq = 24000000;
extern "C" uint32_t ph_freq(void) { return gFreq; }
extern "C" void ph_log(const char* aLine) { RDebug::Printf("%s", aLine); }

static void Log(const char* aFmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void Log(const char* aFmt, ...)
	{
	char buf[300];
	int n = snprintf(buf, sizeof(buf), "[encbench] ");
	va_list ap;
	va_start(ap, aFmt);
	vsnprintf(buf + n, sizeof(buf) - n, aFmt, ap);
	va_end(ap);
	RDebug::Printf("%s", buf);
	}

// ---------------------------------------------------------------- CPU load: the CPU time of every thread in the system (RThread::GetCpuTime)
// (v1 used lowest-priority spinner threads; boot 870 died with "Program closed: encspin3" and logged nothing -- no threads of our own now)
static const TInt KMaxThr = 256;
struct TSnap
	{
	TInt iCount;
	TUint64 iId[KMaxThr];
	TInt64 iUs[KMaxThr];
	};
static TInt gCpuErr = 0;			// first GetCpuTime error (non-zero: cpu_pct reads -1)

static void Snapshot(TSnap& aS)
	{
	aS.iCount = 0;
	if (gCpuErr)
		return;				// the kernel has no thread CPU time (boot 873: r=-5): do not spend time enumerating threads
	TFindThread ft;
	TFullName name;
	while (aS.iCount < KMaxThr && ft.Next(name) == KErrNone)
		{
		RThread t;
		if (t.Open(ft) != KErrNone)
			continue;
		TTimeIntervalMicroSeconds cpu(0);
		const TInt r = t.GetCpuTime(cpu);
		if (r != KErrNone && !gCpuErr)
			gCpuErr = r;
		aS.iId[aS.iCount] = t.Id();
		aS.iUs[aS.iCount] = cpu.Int64();
		++aS.iCount;
		t.Close();
		}
	}

// busy microseconds between two snapshots (threads created in between count from zero)
static TInt64 BusyUs(const TSnap& aA, const TSnap& aB)
	{
	TInt64 sum = 0;
	for (TInt i = 0; i < aB.iCount; ++i)
		{
		TInt64 before = 0;
		for (TInt j = 0; j < aA.iCount; ++j)
			if (aA.iId[j] == aB.iId[i])
				before = aA.iUs[j];
		if (aB.iUs[i] > before)
			sum += aB.iUs[i] - before;
		}
	return sum;
	}

struct TLoad
	{
	TSnap* iS0;
	TUint32 iT0;
	};
static TSnap* gMark;				// end-of-loop snapshot taken by the encoder before it tears its worker threads down
static TUint32 gMarkTicks;
extern "C" void ph_cpu_mark(void)
	{
	if (!gMark)
		gMark = new TSnap;
	if (gMark)
		{
		Snapshot(*gMark);
		gMarkTicks = ph_ticks();
		}
	}
static TLoad LoadBegin()
	{
	TLoad l;
	l.iS0 = new TSnap;
	if (l.iS0)
		Snapshot(*l.iS0);
	if (gMark)
		{
		delete gMark;
		gMark = 0;
		}
	l.iT0 = ph_ticks();
	return l;
	}
// busy cores x 100 (100 = one core fully busy), or -1 when the kernel gives no thread CPU time
static TInt LoadEnd(TLoad& aL)
	{
	TInt res = -1;
	TSnap* now = gMark;
	TUint32 t1 = gMarkTicks;
	if (!now)
		{
		now = new TSnap;
		if (now)
			Snapshot(*now);
		t1 = ph_ticks();
		}
	const TInt64 wall = UsOf(t1 - aL.iT0);
	if (aL.iS0 && now && !gCpuErr && wall > 0)
		res = (TInt)(BusyUs(*aL.iS0, *now) * 100 / wall);
	if (now != gMark)
		delete now;
	delete aL.iS0;
	aL.iS0 = 0;
	return res;
	}

// ---------------------------------------------------------------- helpers
static TInt UsPerCall(TUint32 aTicks, TInt aReps) { return (TInt)(UsOf(aTicks) / aReps); }

struct TRes
	{
	int iW, iH;
	};
static const TRes KRes[] = { { 180, 320 }, { 360, 640 }, { 640, 480 }, { 720, 1440 }, { 1280, 720 } };
static const int KResCount = sizeof(KRes) / sizeof(KRes[0]);

static int TargetKbps(int aW, int aH)
	{
	const int k = (int)((int64_t)aW * aH * 30 * 8 / 100 / 1000);		// 0.08 bit per pixel per frame at 30 fps
	return k < 150 ? 150 : k;
	}

struct TSet
	{
	int iW, iH, iAW, iAH;
	uint32_t* iXrgb[KRing];
	uint16_t* i565[KRing];
	uint8_t* iNv12[KRing];			// stride iAW, MB-padded (hardware input)
	uint8_t* iI420[KRing];			// stride iW (software input)
	};

static bool BuildSet(TSet& s, int aW, int aH, bool aStress = false)
	{
	memset(&s, 0, sizeof(s));
	s.iW = aW;
	s.iH = aH;
	s.iAW = (aW + 15) & ~15;
	s.iAH = (aH + 15) & ~15;
	for (int k = 0; k < KRing; ++k)
		{
		s.iXrgb[k] = (uint32_t*)malloc((size_t)aW * aH * 4);
		s.i565[k] = (uint16_t*)malloc((size_t)aW * aH * 2);
		s.iNv12[k] = (uint8_t*)malloc((size_t)s.iAW * s.iAH * 3 / 2);
		s.iI420[k] = (uint8_t*)malloc((size_t)aW * aH * 3 / 2);
		if (!s.iXrgb[k] || !s.i565[k] || !s.iNv12[k] || !s.iI420[k])
			return false;
		if (aStress)
			GenStress(aW, aH, k, s.iXrgb[k]);
		else
			GenXrgb(aW, aH, k, s.iXrgb[k]);
		ToRgb565(s.iXrgb[k], aW, aH, s.i565[k]);
		uint8_t* nv = s.iNv12[k];
		Csc(EXrgb, s.iXrgb[k], aW * 4, aW, aH, nv, s.iAW, nv + s.iAW * s.iAH, 0, s.iAW, true);
		PadNv12(nv, nv + s.iAW * s.iAH, aW, aH, s.iAW, s.iAH);
		uint8_t* i4 = s.iI420[k];
		Csc(EXrgb, s.iXrgb[k], aW * 4, aW, aH, i4, aW, i4 + aW * aH, i4 + aW * aH + (aW / 2) * (aH / 2), aW / 2, true);
		}
	return true;
	}

static void FreeSet(TSet& s)
	{
	for (int k = 0; k < KRing; ++k)
		{
		free(s.iXrgb[k]);
		free(s.i565[k]);
		free(s.iNv12[k]);
		free(s.iI420[k]);
		}
	}

// ---------------------------------------------------------------- colour conversion benchmark
static int gCscNv12NeonUs, gCscI420NeonUs, gCscNv12ArenaUs;		// XRGB figures kept for the end-to-end column

static void BenchCsc(TSet& s, THw& aHw)
	{
	const int w = s.iW, h = s.iH;
	const int reps = Max(5, Min(60, (int)(4000000 / ((int64_t)w * h))));
	uint8_t* ynv = (uint8_t*)malloc((size_t)s.iAW * s.iAH * 3 / 2);
	uint8_t* y420 = (uint8_t*)malloc((size_t)w * h * 3 / 2);
	if (!ynv || !y420)
		{
		Log("csc res=%dx%d ERROR out of memory", w, h);
		free(ynv);
		free(y420);
		return;
		}
	{	// self-check: the NEON kernels must match the scalar ones (BT.601, same rounding); a mismatch count is logged
	uint8_t* ref = (uint8_t*)malloc((size_t)w * h * 3 / 2);
	if (ref)
		{
		for (int fmt = 0; fmt < 2; ++fmt)
			{
			const TSrc sf = fmt ? ERgb565 : EXrgb;
			const void* src = fmt ? (const void*)s.i565[0] : (const void*)s.iXrgb[0];
			const int ss = w * (fmt ? 2 : 4);
			Csc(sf, src, ss, w, h, ref, w, ref + w * h, ref + w * h + (w / 2) * (h / 2), w / 2, false);
			Csc(sf, src, ss, w, h, y420, w, y420 + w * h, y420 + w * h + (w / 2) * (h / 2), w / 2, true);
			int diff = 0, maxd = 0;
			for (int i = 0; i < w * h * 3 / 2; ++i)
				{
				const int d = ref[i] > y420[i] ? ref[i] - y420[i] : y420[i] - ref[i];
				if (d)
					++diff;
				if (d > maxd)
					maxd = d;
				}
			Log("csc_check res=%dx%d src=%s neon_vs_scalar: %d differing bytes, max diff %d %s", w, h, fmt ? "rgb565" : "xrgb8888", diff, maxd,
				maxd <= 1 ? "OK" : "MISMATCH");
			}
		free(ref);
		}
	}
	for (int fmt = 0; fmt < 2; ++fmt)
		{
		const TSrc sf = fmt ? ERgb565 : EXrgb;
		int us[5] = { 0, 0, 0, 0, -1 };			// nv12_c, nv12_neon, i420_c, i420_neon, nv12_neon into the VE arena
		for (int v = 0; v < 4; ++v)
			{
			const bool neon = v & 1, planar = v & 2;
			const uint32_t t0 = ph_ticks();
			for (int i = 0; i < reps; ++i)
				{
				const void* src = fmt ? (const void*)s.i565[i % KRing] : (const void*)s.iXrgb[i % KRing];
				const int ss = w * (fmt ? 2 : 4);
				if (planar)
					Csc(sf, src, ss, w, h, y420, w, y420 + w * h, y420 + w * h + (w / 2) * (h / 2), w / 2, neon);
				else
					Csc(sf, src, ss, w, h, ynv, s.iAW, ynv + s.iAW * s.iAH, 0, s.iAW, neon);
				}
			us[v] = UsPerCall(ph_ticks() - t0, reps);
			}
		if (aHw.Ok() && s.iAW * s.iAH * 3 / 2 <= aHw.ScratchSize())
			{
			uint8_t* d = aHw.Scratch();
			const uint32_t t0 = ph_ticks();
			for (int i = 0; i < reps; ++i)
				{
				const void* src = fmt ? (const void*)s.i565[i % KRing] : (const void*)s.iXrgb[i % KRing];
				Csc(sf, src, w * (fmt ? 2 : 4), w, h, d, s.iAW, d + s.iAW * s.iAH, 0, s.iAW, true);
				}
			us[4] = UsPerCall(ph_ticks() - t0, reps);
			}
		if (!fmt)
			{
			gCscNv12NeonUs = us[1];
			gCscI420NeonUs = us[3];
			gCscNv12ArenaUs = us[4];
			}
		Log("csc res=%dx%d src=%s us_per_frame: nv12_c=%d nv12_neon=%d i420_c=%d i420_neon=%d nv12_neon_into_ve_arena=%d reps=%d hw_csc=none(ISP takes NV12/I420 only)",
			w, h, fmt ? "rgb565" : "xrgb8888", us[0], us[1], us[2], us[3], us[4], reps);
		}
	free(ynv);
	free(y420);
	}

// ---------------------------------------------------------------- one encode run + report
static void Report(const char* aEnc, const char* aMode, const TSet& s, const TRunResult& r, const TDecodeResult* aD, int aCpu, int aCscUs)
	{
	if (r.iErr || r.iFrames <= 0)
		{
		Log("enc=%s res=%dx%d mode=%s ERROR %d after %d frames", aEnc, s.iW, s.iH, aMode, r.iErr, r.iFrames);
		if (r.iFrames <= 0)
			return;
		}
	const TInt64 usPer = r.iUs / r.iFrames;
	const TInt fps100 = r.iUs > 0 ? (TInt)((TInt64)r.iFrames * 100000000 / r.iUs) : 0;
	const TInt kbps = (TInt)((TInt64)r.iBytes * 8 * 30 / r.iFrames / 1000);
	const TInt e2e100 = (TInt)(100000000 / (usPer + aCscUs));
	char psnr[48];
	if (aD && aD->iSampled > 0 && !aD->iErr)
		snprintf(psnr, sizeof(psnr), "%d.%02d(min %d.%02d,%d/%d)", aD->iAvgY / 100, aD->iAvgY % 100, aD->iMinY / 100, aD->iMinY % 100, aD->iSampled,
			aD->iPictures);
	else if (aD)
		snprintf(psnr, sizeof(psnr), "n/a(dec_err=%d,pics=%d)", aD->iErr, aD->iPictures);
	else
		snprintf(psnr, sizeof(psnr), "n/a");
	Log("enc=%s res=%dx%d mode=%s frames=%d fps=%d.%02d ms_per_frame=%d.%02d max_ms=%d cpu_pct=%d kbps30=%d idr_bytes=%d bytes=%d psnr_y=%s e2e_fps_with_csc=%d.%02d",
		aEnc, s.iW, s.iH, aMode, r.iFrames, fps100 / 100, fps100 % 100, (TInt)(usPer / 1000), (TInt)((usPer % 1000) / 10), (TInt)(r.iMaxFrameUs / 1000),
		aCpu >= 0 ? aCpu : r.iCpuPct, kbps, r.iFirstBytes, r.iBytes, psnr, e2e100 / 100, e2e100 % 100);
	}

// one resolution. pass 0: colour conversion + hardware + software 1 thread; pass 1: software 4 threads (LAST, so that a thread problem cannot
// cost the other results)
static void RunRes(int ri, int pass, THw& hw, uint8_t* stream, int* off)
	{
	const int w = KRes[ri].iW, h = KRes[ri].iH;
	if (pass >= 1 && w == 180)
		return;				// the striped 4-thread run is pointless at 180x320 (80-line stripes: no speed-up) and was the hang of boot 879
	TSet s;
	if (!BuildSet(s, w, h))
		{
		Log("res=%dx%d ERROR out of memory building the source set", w, h);
		FreeSet(s);
		return;
		}
	const int kbps = TargetKbps(w, h);
	if (pass == 0)
		{
		Log("res=%dx%d (hw coded %dx%d) target=%d kbps at 30 fps", w, h, s.iAW, s.iAH, kbps);
		BenchCsc(s, hw);
		}
	else
		{
		// the end-to-end column of the 4-thread rows needs the NEON I420 conversion time of this size: measure it quickly again
		uint8_t* tmp = (uint8_t*)malloc((size_t)w * h * 3 / 2);
		gCscI420NeonUs = 0;
		if (tmp)
			{
			const uint32_t t0 = ph_ticks();
			for (int i = 0; i < 5; ++i)
				Csc(EXrgb, s.iXrgb[i % KRing], w * 4, w, h, tmp, w, tmp + w * h, tmp + w * h + (w / 2) * (h / 2), w / 2, true);
			gCscI420NeonUs = UsPerCall(ph_ticks() - t0, 5);
			free(tmp);
			}
		}
	for (int mode = 0; mode < 2; ++mode)
		{
		if ((pass == 1 && mode != 0) || (pass == 2 && mode != 1))
			continue;
		const int qp = mode ? 0 : 28;
		char mname[24];
		if (mode)
			snprintf(mname, sizeof(mname), "rc%dkbps", kbps);
		else
			snprintf(mname, sizeof(mname), "fixqp28");
		if (pass == 0 && hw.Ok())
			{
			Log("run hw402 res=%dx%d mode=%s", w, h, mname);
			TRunResult r;
			TLoad l = LoadBegin();
			hw.Encode(w, h, qp, kbps, KFrames, KMaxSeconds, s.iNv12, stream, KStreamCap, off, r);
			const TInt cpu = LoadEnd(l);
			TDecodeResult d;
			if (r.iFrames > 1)
				hw.DecodePsnr(stream, off, r.iFrames, w, h, s.iNv12, s.iAW, 10, d);
			else
				memset(&d, 0, sizeof(d));
			Report("hw402", mname, s, r, r.iFrames > 1 ? &d : 0, cpu, gCscNv12ArenaUs > 0 ? gCscNv12ArenaUs : gCscNv12NeonUs);
			}
		const int th = pass == 0 ? 1 : 4;
		Log("run sw%dt res=%dx%d mode=%s", th, w, h, mname);
		TRunResult r;
		TLoad l = LoadBegin();
		SwEncode(w, h, qp, kbps, th, KFrames, KMaxSeconds, s.iI420, stream, KStreamCap, off, r);
		const TInt cpu = LoadEnd(l);
		TDecodeResult d;
		const bool dec = th == 1 && hw.Ok() && r.iFrames > 1 && !r.iErr;		// stripe-threaded rows are not one stream
		if (dec)
			hw.DecodePsnr(stream, off, r.iFrames, w, h, s.iI420, w, 10, d);
		char en[16];
		snprintf(en, sizeof(en), "sw%dt", th);
		Report(en, mname, s, r, dec ? &d : 0, cpu, gCscI420NeonUs);
		}
	FreeSet(s);
	if (pass == 0 && w == 1280 && hw.Ok())
		{
		// (d) of boot 873: the parked "11 fps 720p" figure used a full-frame gradient + checker square; this UI content ran 20x faster per macroblock.
		// The old content again, fixed QP 28, both encoders, so the difference is attributed by measurement
		TSet st;
		if (BuildSet(st, w, h, true))
			{
			Log("run hw402-stress res=%dx%d mode=fixqp28 (gradient + checker content of the first VE benchmark)", w, h);
			TRunResult r;
			hw.Encode(w, h, 28, kbps, KFrames, KMaxSeconds, st.iNv12, stream, KStreamCap, off, r);
			TDecodeResult d;
			if (r.iFrames > 1)
				hw.DecodePsnr(stream, off, r.iFrames, w, h, st.iNv12, st.iAW, 10, d);
			else
				memset(&d, 0, sizeof(d));
			Report("hw402-stress", "fixqp28", st, r, r.iFrames > 1 ? &d : 0, -1, 0);
			TRunResult r2;
			SwEncode(w, h, 28, kbps, 1, KFrames, KMaxSeconds, st.iI420, stream, KStreamCap, off, r2);
			Report("sw1t-stress", "fixqp28", st, r2, 0, -1, 0);
			}
		FreeSet(st);
		}
	}

int main()
	{
	TInt f = 0;
	HAL::Get(HAL::EFastCounterFrequency, f);
	if (f > 0)
		gFreq = (TUint32)f;
	TInt cpus = 4;
	HAL::Get(HAL::ENumCpus, cpus);
	const TUint32 tAll = ph_ticks();
#ifdef __ARM_NEON
	const char* neon = "intrinsics";
#else
	const char* neon = "none";
#endif
	// first line before anything else can go wrong; the SSM_TEST starts during boot, so wait for the UI to settle (the numbers are
	// otherwise polluted by boot activity) -- a line every 5 s shows the exe is alive
	Log("begin v5 fastcounter=%u Hz cpus=%d frames=%d maxsec=%d csc_neon=%s openh264=2.4.1(BSD-2,NEON asm .S in the archive) hw=A64 VE 402 MHz",
		gFreq, cpus, KFrames, KMaxSeconds, neon);
	for (int i = 0; i < KSettleSeconds / 5; ++i)
		{
		User::After(5000000);
		Log("settling %d/%d s", (i + 1) * 5, KSettleSeconds);
		}
	{
	TSnap* sn = new TSnap;
	if (sn)
		{
		Snapshot(*sn);
		Log("cpu: %d threads in the system, GetCpuTime r=%d%s", sn->iCount, gCpuErr, gCpuErr ? " (cpu_pct will read -1)" : "");
		delete sn;
		}
	}
	THw hw;
	char msg[160];
	const TInt hr = hw.Open(msg, sizeof(msg));
	Log("hw: %s", msg);
	if (hr != KErrNone)
		Log("hw: hardware legs skipped");
	uint8_t* stream = (uint8_t*)malloc(KStreamCap);
	int* off = (int*)malloc(sizeof(int) * (KFrames + 2));
	if (!stream || !off)
		{
		Log("FAIL out of memory");
		return 0;
		}
	for (int pass = 0; pass < 3; ++pass)
		{
		if (pass >= 1)
			Log("pass %d: OpenH264, 4 stripe threads, %s", pass + 1, pass == 1 ? "fixed QP 28 (all sizes)" : "rate control (all sizes)");
		for (int ri = 0; ri < KResCount; ++ri)
			RunRes(ri, pass, hw, stream, off);
		}
	SwShutdown();
	hw.Close();
	Log("DONE total_s=%d", (TInt)(UsOf(ph_ticks() - tAll) / 1000000));
	return 0;
	}
