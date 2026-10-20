// enc_sw.cpp -- OURS (pheencbench): the software H.264 encoder leg = Cisco OpenH264 2.4.1 (BSD-2-Clause), Constrained Baseline / CAVLC,
// low-complexity real-time settings, NEON kernels from the archive (tools/codecpack/openh264_enc.sh). No Symbian types here: the I420 input
// frames come in as plain pointers.
//
// Threads: OpenH264's own multi-threading (iMultipleThreadIdc > 1: a thread pool + slice tasks + semaphores) HANGS inside EncodeFrame under
// Open C pthreads (boot 873/875: "initialised, encoding" is the last line, all CPUs idle; not understood). So "N threads" here = N horizontal
// stripes of the picture, each its own single-threaded encoder instance on its own pthread (mutex + condvar barrier per frame: the primitives
// FFmpeg's frame threads use on the same phone). The stripes are independent streams (a real deployment would use slices of ONE stream, or
// tiles of N streams): bytes, speed and CPU load are representative, the output is not one decodable stream (no PSNR for the threaded rows).
#include "bench_common.h"
#include "wels/codec_api.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

// function-local statics in the archive (OpenH264 is built with thread-safe statics; this exe's own code is not): a guard that does not
// hold its mutex across the initialiser (a nested static would otherwise deadlock on itself); the ARM EABI guard is 4 bytes
static pthread_mutex_t gGuardMutex = PTHREAD_MUTEX_INITIALIZER;
struct TGuardOwner { volatile unsigned char* iGuard; pthread_t iThread; };
static TGuardOwner gOwners[16];
static void OwnerSet(volatile unsigned char* aG, bool aAdd)
	{
	for (int i = 0; i < 16; ++i)
		if (aAdd ? !gOwners[i].iGuard : gOwners[i].iGuard == aG)
			{
			gOwners[i].iGuard = aAdd ? aG : 0;
			gOwners[i].iThread = pthread_self();
			return;
			}
	}
extern "C" int __cxa_guard_acquire(long long* aGuard)
	{
	volatile unsigned char* g = (volatile unsigned char*)aGuard;
	for (;;)
		{
		pthread_mutex_lock(&gGuardMutex);
		if (g[0])
			{
			pthread_mutex_unlock(&gGuardMutex);
			return 0;
			}
		if (!g[1])
			{
			g[1] = 1;
			OwnerSet(g, true);
			pthread_mutex_unlock(&gGuardMutex);
			return 1;
			}
		bool mine = false;
		for (int i = 0; i < 16; ++i)
			if (gOwners[i].iGuard == g && pthread_equal(gOwners[i].iThread, pthread_self()))
				mine = true;
		pthread_mutex_unlock(&gGuardMutex);
		if (mine)
			return 0;
		usleep(1000);
		}
	}
extern "C" void __cxa_guard_release(long long* aGuard)
	{
	volatile unsigned char* g = (volatile unsigned char*)aGuard;
	pthread_mutex_lock(&gGuardMutex);
	g[0] = 1;
	g[1] = 0;
	OwnerSet(g, false);
	pthread_mutex_unlock(&gGuardMutex);
	}
extern "C" void __cxa_guard_abort(long long* aGuard)
	{
	volatile unsigned char* g = (volatile unsigned char*)aGuard;
	pthread_mutex_lock(&gGuardMutex);
	g[1] = 0;
	OwnerSet(g, false);
	pthread_mutex_unlock(&gGuardMutex);
	}
extern "C" void __cxa_pure_virtual()
	{
	abort();
	}

namespace pb {

// a single-threaded encoder instance for a aW x aH picture
static ISVCEncoder* MakeEncoder(int aW, int aH, int aQp, int aKbps)
	{
	ISVCEncoder* enc = 0;
	if (WelsCreateSVCEncoder(&enc) != 0 || !enc)
		return 0;
	SEncParamExt p;
	enc->GetDefaultParams(&p);
	p.iUsageType = CAMERA_VIDEO_REAL_TIME;
	p.iPicWidth = aW;
	p.iPicHeight = aH;
	p.iTargetBitrate = aKbps * 1000;
	p.iMaxBitrate = aKbps * 1000 * 2;
	p.iRCMode = aQp > 0 ? RC_OFF_MODE : RC_BITRATE_MODE;
	p.fMaxFrameRate = 30.0f;
	p.iTemporalLayerNum = 1;
	p.iSpatialLayerNum = 1;
	p.iComplexityMode = LOW_COMPLEXITY;
	p.iEntropyCodingModeFlag = 0;				// CAVLC (= the hardware encoder's stream)
	p.bEnableFrameSkip = false;					// never drop: the benchmark counts every frame
	p.bEnableDenoise = false;
	p.bEnableBackgroundDetection = false;
	p.bEnableAdaptiveQuant = false;
	p.bEnableSceneChangeDetect = false;			// no surprise IDRs
	p.bEnableLongTermReference = false;
	p.uiIntraPeriod = 0;						// IDR only at the start
	p.iMultipleThreadIdc = 1;
	p.iMinQp = aQp > 0 ? aQp : 12;
	p.iMaxQp = aQp > 0 ? aQp : 42;
	SSpatialLayerConfig& sl = p.sSpatialLayers[0];
	sl.iVideoWidth = aW;
	sl.iVideoHeight = aH;
	sl.fFrameRate = 30.0f;
	sl.iSpatialBitrate = aKbps * 1000;
	sl.iMaxSpatialBitrate = aKbps * 1000 * 2;
	sl.iDLayerQp = aQp > 0 ? aQp : 26;
	sl.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
	if (enc->InitializeExt(&p) != 0)
		{
		WelsDestroySVCEncoder(enc);
		return 0;
		}
	int vfmt = videoFormatI420;
	enc->SetOption(ENCODER_OPTION_DATAFORMAT, &vfmt);
	return enc;
	}

// encode one frame (planes already offset to the stripe); returns the bytes, copying them to aOut (may be NULL / too small: counted only)
static int EncodeOne(ISVCEncoder* aEnc, int aW, int aH, const uint8_t* aY, int aYs, const uint8_t* aU, const uint8_t* aV, int aCs, long long aTs,
	uint8_t* aOut, int aOutCap, int& aErr)
	{
	SSourcePicture pic;
	memset(&pic, 0, sizeof(pic));
	pic.iColorFormat = videoFormatI420;
	pic.iPicWidth = aW;
	pic.iPicHeight = aH;
	pic.iStride[0] = aYs;
	pic.iStride[1] = pic.iStride[2] = aCs;
	pic.pData[0] = (unsigned char*)aY;
	pic.pData[1] = (unsigned char*)aU;
	pic.pData[2] = (unsigned char*)aV;
	pic.uiTimeStamp = aTs;
	SFrameBSInfo info;
	memset(&info, 0, sizeof(info));
	const int rv = aEnc->EncodeFrame(&pic, &info);
	if (rv != cmResultSuccess)
		{
		aErr = 100 + rv;
		return 0;
		}
	int bytes = 0;
	if (info.eFrameType != videoFrameTypeSkip)
		for (int l = 0; l < info.iLayerNum; ++l)
			{
			const SLayerBSInfo& lb = info.sLayerInfo[l];
			int lbytes = 0;
			for (int j = 0; j < lb.iNalCount; ++j)
				lbytes += lb.pNalLengthInByte[j];
			if (aOut && bytes + lbytes <= aOutCap)
				memcpy(aOut + bytes, lb.pBsBuf, lbytes);
			bytes += lbytes;
			}
	return bytes;
	}

// ---------------------------------------------------------------- the stripe threads
// Persistent: four pthreads are created ONCE (the first threaded run) and reused by every later run; boot 879: the first create/encode/join cycle
// worked and the second one hung after "stripe threads started" (cause unknown; creating and joining threads per run is what is avoided now).
// Each worker has its own mutex + two condvars and is only ever signalled (never broadcast), the way FFmpeg's frame threads use pthreads on this phone.
const int KMaxStripes = 4;
struct TWorker
	{
	pthread_mutex_t iMutex;
	pthread_cond_t iStart, iDone;
	int iGen, iDoneGen;
	bool iQuit, iStarted;
	pthread_t iThread;
	// per run
	ISVCEncoder* iEnc;
	int iW, iH, iFullH, iY0, iBytes, iErr;
	const uint8_t* iSrc;
	long long iTs;
	};
static TWorker gWorker[KMaxStripes];

static void* WorkerMain(void* aArg)
	{
	TWorker* w = (TWorker*)aArg;
	int seen = 0;
	for (;;)
		{
		pthread_mutex_lock(&w->iMutex);
		while (w->iGen == seen && !w->iQuit)
			pthread_cond_wait(&w->iStart, &w->iMutex);
		if (w->iQuit)
			{
			pthread_mutex_unlock(&w->iMutex);
			break;
			}
		seen = w->iGen;
		const uint8_t* src = w->iSrc;
		const long long ts = w->iTs;
		ISVCEncoder* enc = w->iEnc;
		const int W = w->iW, H = w->iH, cw = W / 2, full = w->iFullH, y0 = w->iY0;
		pthread_mutex_unlock(&w->iMutex);
		const uint8_t* y = src + y0 * W;
		const uint8_t* u = src + W * full + (y0 / 2) * cw;
		const uint8_t* v = src + W * full + cw * (full / 2) + (y0 / 2) * cw;
		int err = 0;
		const int bytes = EncodeOne(enc, W, H, y, W, u, v, cw, ts, 0, 0, err);
		pthread_mutex_lock(&w->iMutex);
		w->iBytes = bytes;
		w->iErr = err;
		w->iDoneGen = seen;
		pthread_cond_signal(&w->iDone);
		pthread_mutex_unlock(&w->iMutex);
		}
	return 0;
	}

static bool StartPool()
	{
	static bool tried = false, ok = false;
	if (tried)
		return ok;
	tried = true;
	ok = true;
	for (int i = 0; i < KMaxStripes; ++i)
		{
		TWorker& w = gWorker[i];
		memset(&w, 0, sizeof(w));
		pthread_mutex_init(&w.iMutex, 0);
		pthread_cond_init(&w.iStart, 0);
		pthread_cond_init(&w.iDone, 0);
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setstacksize(&at, 512 * 1024);
		if (pthread_create(&w.iThread, &at, WorkerMain, &w) != 0)
			ok = false;
		else
			w.iStarted = true;
		pthread_attr_destroy(&at);
		}
	ph_log("[encbench] sw: stripe worker threads started (persistent)");
	return ok;
	}

void SwShutdown()
	{
	for (int i = 0; i < KMaxStripes; ++i)
		{
		TWorker& w = gWorker[i];
		if (!w.iStarted)
			continue;
		pthread_mutex_lock(&w.iMutex);
		w.iQuit = true;
		pthread_cond_signal(&w.iStart);
		pthread_mutex_unlock(&w.iMutex);
		pthread_join(w.iThread, 0);
		w.iStarted = false;
		}
	}

static void SwEncodeStripes(int aW, int aH, int aQp, int aKbps, int aThreads, int aFrames, int aMaxSeconds, const uint8_t* const* aRing, TRunResult& aR)
	{
	const int n = aThreads > KMaxStripes ? KMaxStripes : aThreads;
	int err = StartPool() ? 0 : -4;
	const int hs = ((aH / n) + 15) & ~15;
	for (int i = 0; i < n && !err; ++i)
		{
		TWorker& w = gWorker[i];
		w.iW = aW;
		w.iFullH = aH;
		w.iY0 = i * hs;
		w.iH = i == n - 1 ? aH - w.iY0 : hs;
		if (w.iH < 16)
			{
			err = -3;
			break;
			}
		w.iEnc = MakeEncoder(aW, w.iH, aQp, (int)((long long)aKbps * w.iH / aH) + 1);
		if (!w.iEnc)
			err = -2;
		}
	ph_log("[encbench] sw: stripe encoders created");
	aR.iErr = err;
	int total = 0, nf = 0;
	if (!err)
		{
		const uint32_t t0 = Ticks();
		uint32_t tl = t0;
		int64_t maxUs = 0;
		for (; nf < aFrames; ++nf)
			{
			for (int i = 0; i < n; ++i)
				{
				TWorker& w = gWorker[i];
				pthread_mutex_lock(&w.iMutex);
				w.iSrc = aRing[nf % KRing];
				w.iTs = (long long)nf * 33;
				++w.iGen;
				pthread_cond_signal(&w.iStart);
				pthread_mutex_unlock(&w.iMutex);
				}
			int bytes = 0;
			for (int i = 0; i < n; ++i)
				{
				TWorker& w = gWorker[i];
				pthread_mutex_lock(&w.iMutex);
				while (w.iDoneGen != w.iGen)
					pthread_cond_wait(&w.iDone, &w.iMutex);
				bytes += w.iBytes;
				if (w.iErr && !aR.iErr)
					aR.iErr = w.iErr;
				pthread_mutex_unlock(&w.iMutex);
				}
			total += bytes;
			if (nf == 0)
				{
				aR.iFirstBytes = bytes;
				ph_log("[encbench] sw: first frame done");
				}
			const uint32_t tn = Ticks();
			const int64_t us = UsOf(tn - tl);
			if (us > maxUs)
				maxUs = us;
			tl = tn;
			if (aR.iErr || (nf > 3 && UsOf(tn - t0) > (int64_t)aMaxSeconds * 1000000))
				{
				++nf;
				break;
				}
			}
		aR.iUs = UsOf(Ticks() - t0);
		aR.iMaxFrameUs = maxUs;
		}
	ph_log("[encbench] sw: loop done, releasing encoders");
	for (int i = 0; i < n; ++i)
		if (gWorker[i].iEnc)
			{
			gWorker[i].iEnc->Uninitialize();
			WelsDestroySVCEncoder(gWorker[i].iEnc);
			gWorker[i].iEnc = 0;
			}
	aR.iFrames = nf;
	aR.iBytes = total;
	aR.iCpuPct = -1;
	}

// aQp > 0: fixed QP (rate control off); else rate control at aKbps. aRing: KRing I420 planes (Y w*h, U, V contiguous).
// aThreads == 1: one encoder, the stream is stored in aStream/aOff (aOff[n+1] entries); > 1: stripe threads (nothing stored)
void SwEncode(int aW, int aH, int aQp, int aKbps, int aThreads, int aFrames, int aMaxSeconds, const uint8_t* const* aRing, uint8_t* aStream,
	int aStreamCap, int* aOff, TRunResult& aR)
	{
	memset(&aR, 0, sizeof(aR));
	aR.iQpUsed = aQp;
	aR.iCpuPct = -1;
	if (aThreads > 1)
		{
		SwEncodeStripes(aW, aH, aQp, aKbps, aThreads, aFrames, aMaxSeconds, aRing, aR);
		return;
		}
	ISVCEncoder* enc = MakeEncoder(aW, aH, aQp, aKbps);
	if (!enc)
		{
		aR.iErr = -2;
		return;
		}
	const uint32_t t0 = Ticks();
	uint32_t tl = t0;
	int64_t maxUs = 0;
	int total = 0;
	aOff[0] = 0;
	int n = 0;
	for (; n < aFrames; ++n)
		{
		const uint8_t* src = aRing[n % KRing];
		int err = 0;
		const int bytes = EncodeOne(enc, aW, aH, src, aW, src + aW * aH, src + aW * aH + (aW / 2) * (aH / 2), aW / 2, (long long)n * 33,
			aStream + total, aStreamCap - total, err);
		if (err)
			{
			aR.iErr = err;
			break;
			}
		if (total + bytes > aStreamCap)
			{
			aR.iErr = 2;
			break;
			}
		total += bytes;
		aOff[n + 1] = total;
		const uint32_t tn = Ticks();
		const int64_t us = UsOf(tn - tl);
		if (us > maxUs)
			maxUs = us;
		tl = tn;
		if (n == 0)
			aR.iFirstBytes = bytes;
		if (n > 3 && UsOf(tn - t0) > (int64_t)aMaxSeconds * 1000000)
			{
			++n;
			break;
			}
		}
	aR.iUs = UsOf(Ticks() - t0);
	aR.iFrames = n;
	aR.iBytes = total;
	aR.iMaxFrameUs = maxUs;
	aR.iCpuPct = 100;		// one thread: the loop is the CPU load (assumed; GetCpuTime is not supported by the kernel)
	enc->Uninitialize();
	WelsDestroySVCEncoder(enc);
	}

}	// namespace pb
