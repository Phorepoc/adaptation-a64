// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// phecamsource.cpp -- OURS (camera-plan.md C1). See phecamsource.h.
#include <hal.h>
#include <e32math.h>
#include <f32file.h>
#include <e32debug.h>
#include <pixelformats.h>
#include "phecamsource.h"

_LIT(KPheCamPdd, "camerasc.pdd");
_LIT(KPheCamLdd, "ecamerasc.ldd");

const TInt KPheCamBuffers = 4;
const TInt KPheCamW = 320, KPheCamH = 240, KPheCamRate = 15;
#ifdef PHE_CAM_RAW
const TInt KRawLines = 2;		// the raw dump is (2W x 2H) bytes
const TInt KBayer = 0;			// BGGR (libmegapixels' OV5640 modes)
#else
const TInt KRawLines = 1;
#endif

CPheCamSource::CPheCamSource() : CActive(EPriorityLow)
	{
	CActiveScheduler::Add(this);
	}

CPheCamSource* CPheCamSource::NewL(TInt aUnit)
	{
	CPheCamSource* s = new (ELeave) CPheCamSource;
	CleanupStack::PushL(s);
	s->ConstructL(aUnit);
	CleanupStack::Pop(s);
	return s;
	}

CPheCamSource::~CPheCamSource()
	{
	Cancel();
	if (iStarted)
		iCam.Stop();
	iChunk.Close();
	if (iOpen)
		iCam.Close();
	delete iBitmap;
	}

void CPheCamSource::ConstructL(TInt aUnit)
	{
	// the driver pair is built into the kernel image; its file names are the built-in device records of the ROM
	TInt r = User::LoadPhysicalDevice(KPheCamPdd);
	if (r != KErrNone && r != KErrAlreadyExists)
		User::Leave(r);
	r = User::LoadLogicalDevice(KPheCamLdd);
	if (r != KErrNone && r != KErrAlreadyExists)
		User::Leave(r);
	User::LeaveIfError(iCam.Open(aUnit));
	iOpen = ETrue;
	TCameraConfigV02Buf cfgBuf;
	iCam.GetCamConfig(ECamCaptureModeViewFinder, cfgBuf);
	TCameraConfigV02& cfg = cfgBuf();
#ifdef PHE_CAM_RAW
	cfg.iFrameSize.iWidth = KPheCamW * 2;	// raw8: the 640x480 mode carries (2W) x (2H) Bayer = W*2 bytes x 2H lines (camera-plan.md "RAW")
	cfg.iFrameSize.iHeight = KPheCamH * 2;
#else
	cfg.iFrameSize.iWidth = KPheCamW;
	cfg.iFrameSize.iHeight = KPheCamH;
#endif
	cfg.iFrameSize.iMinFrameRate = 1;
	cfg.iFrameSize.iMaxFrameRate = 30;
	cfg.iPixelFormat.iPixelFormat = EUidPixelFormatYUV_422Interleaved;
	cfg.iPixelFormat.iNumFrameSizes = 1;
	cfg.iPixelFormat.iPixelWidthInBytes = 2;
	cfg.iFrameRate = KPheCamRate;
	cfg.iPixelWidthInBytes = 2;
	r = iCam.SetCamConfig(ECamCaptureModeViewFinder, cfgBuf);
	RDebug::Printf("PH: phecam SetCamConfig r=%d", r);
	User::LeaveIfError(r);
	r = iCam.SetBufConfigChunkCreate(ECamCaptureModeViewFinder, KPheCamBuffers, iChunk);
	RDebug::Printf("PH: phecam buffers r=%d chunk size %d", r, r == KErrNone ? iChunk.Size() : 0);
	User::LeaveIfError(r);
	User::LeaveIfError(iCam.SetCaptureMode(ECamCaptureModeViewFinder));
#ifdef PHE_CAM_RAW
	iGainR = iGainB = 256;
	for (TInt i = 0; i < 256; ++i) { TReal g; Math::Pow(g, i / 255.0, 1.0 / 1.6); iGamma[i] = (TUint8)(g * 255.0 + 0.5); }
#endif
	iW = KPheCamW;
	iH = KPheCamH;
	iBitmap = new (ELeave) CFbsBitmap;
#ifdef __MARM__
	// the PinePhone's rear OV5640 is mounted landscape (DT rotation = 90): the bitmap is the frame transposed (sensor column -> screen row; checked against a reference photo, boot 940)
	// clockwise, portrait like the screen (Convert)
	User::LeaveIfError(iBitmap->Create(TSize(iH, iW), EColor16MU));
#else
	User::LeaveIfError(iBitmap->Create(TSize(iW, iH), EColor16MU));
#endif
	User::LeaveIfError(iCam.Start());
	iStarted = ETrue;
	iCam.NotifyNewImage(iStatus);
	SetActive();
	RDebug::Printf("PH: phecam started %dx%d YUV422 viewfinder", iW, iH);
	}

void CPheCamSource::DoCancel()
	{
	iCam.NotifyNewImageCancel();
	}

void CPheCamSource::RunL()
	{
	const TInt id = iStatus.Int();
	if (id >= 0)
		{
		TInt offset = -1;
		if (iCam.BufferIdToOffset(ECamCaptureModeViewFinder, id, offset) == KErrNone && offset >= 0)
			{
			const TUint32 t0 = User::FastCounter();
			Convert(iChunk.Base() + offset);
			const TUint32 t1 = User::FastCounter();
			// PHOREPOC timing (2026-10-18): convert cost vs frame interval, every 50 frames (fast counter ticks -> us)
			TInt f = 0; HAL::Get(HALData::EFastCounterFrequency, f);
			iTConv += t1 - t0; if (iTLast) iTGap += t0 - iTLast; iTLast = t0;
			if (++iTN == 50 && f > 0)
				{
				RDebug::Printf("PH: phecam timing: convert %d us/frame, interval %d us/frame (%d fps)", (TInt)((TUint64)iTConv * 1000000 / f / 50),
					(TInt)((TUint64)iTGap * 1000000 / f / 49), iTGap ? (TInt)((TUint64)f * 49 / iTGap) : 0);
				iTConv = iTGap = 0; iTN = 0;
				}
			}
		iCam.ReleaseBuffer(id);
		}
	else if (iLoggedErr++ < 3)
		RDebug::Printf("PH: phecam NotifyNewImage -> %d", id);
	if (id != KErrCancel && id != KErrAbort)
		{
		iCam.NotifyNewImage(iStatus);
		SetActive();
		}
	}

// The template sensor labels its frames EUidPixelFormatYUV_422Interleaved but its logo is a 24-bit BGR image that YUVtoYUV565 (camerasc_sensor.cpp) just packs
// 5:6:5 -- blue in bits 15-11, green in 10-5, red in 4-0 -- so the frames are really BGR565 and are decoded as that here. -> X8R8G8B8.
void CPheCamSource::Convert(const TUint8* aYuv)
	{
	// 2026-10-18 (daylight colours wrong): frame 60's raw YUYV bytes to C:\Data\phecam_raw.yuv (iW x iH x 2), once per run,
	// for an offline look at the sensor's actual Y/U/V values
	if (iFrames == 59)
		{
		RFs fs;
		if (fs.Connect() == KErrNone)
			{
			RFile f;
			if (f.Replace(fs, _L("C:\\Data\\phecam_raw.yuv"), EFileWrite) == KErrNone)
				{
				f.Write(TPtrC8(aYuv, iW * iH * 2 * KRawLines));
				f.Close();
				RDebug::Printf("PH: phecam raw frame saved %dx%d", iW, iH);
				}
			fs.Close();
			}
		}
	iBitmap->LockHeap();
	TUint32* out = (TUint32*)iBitmap->DataAddress();
	const TInt wordsPerLine = iBitmap->DataStride() / 4;
#if defined(PHE_CAM_RAW) && defined(__MARM__)
	// 2026-10-18: Bayer raw8 (2W x 2H) -> one RGB pixel per 2x2 quad, as Megapixels does it in software (the sensor's own
	// YUV path swaps R/G on the PinePhone). Phase KBayer: 0 = BGGR (the reference's order), 1 = GBRG, 2 = GRBG, 3 = RGGB.
	// Grey-world white balance: this frame's channel means set the next frame's R/B gains (1/256 steps); a light gamma.
	{
	const TInt rw = iW * 2;
	TUint32 sumR = 0, sumG = 0, sumB = 0;
	for (TInt y = 0; y < iH; ++y)
		{
		const TUint8* l0 = aYuv + (2 * y) * rw;
		const TUint8* l1 = l0 + rw;
		for (TInt x = 0; x < iW; ++x)
			{
			const TInt a = l0[2 * x], b = l0[2 * x + 1], c = l1[2 * x], d = l1[2 * x + 1];
			TInt r, g, bl;
			switch (KBayer)
				{
			case 0: bl = a; g = (b + c) >> 1; r = d; break;
			case 1: g = (a + d) >> 1; bl = b; r = c; break;
			case 2: g = (a + d) >> 1; r = b; bl = c; break;
			default: r = a; g = (b + c) >> 1; bl = d; break;
				}
			sumR += r; sumG += g; sumB += bl;
			r = (r * iGainR) >> 8; bl = (bl * iGainB) >> 8;
			r = r > 255 ? 255 : r; bl = bl > 255 ? 255 : bl;
			out[(iW - 1 - x) * wordsPerLine + (iH - 1 - y)] = 0xFF000000u | (iGamma[r] << 16) | (iGamma[g] << 8) | iGamma[bl];
			}
		}
	if (sumR && sumB)
		{
		iGainR = (TInt)((TUint64)sumG * 256 / sumR); iGainB = (TInt)((TUint64)sumG * 256 / sumB);
		iGainR = iGainR > 1024 ? 1024 : iGainR; iGainB = iGainB > 1024 ? 1024 : iGainB;
		}
	iBitmap->UnlockHeap();
	if (++iFrames <= 3 || iFrames % 100 == 0)
		RDebug::Printf("PH: phecam raw frame %d means R %d G %d B %d gains %d %d", iFrames, (TInt)(sumR / (iW * iH)),
			(TInt)(sumG / (iW * iH)), (TInt)(sumB / (iW * iH)), iGainR, iGainB);
	return;
	}
#endif
	for (TInt y = 0; y < iH; ++y)
		{
		const TUint8* in = aYuv + y * iW * 2;
		TUint32* row = out + y * wordsPerLine;
#ifdef __MARM__
		// the PinePhone: the real OV5640 behind CameraSc (a64camera.cpp) sends true packed YUYV (Y0 U Y1 V, 0x4300 = 0x30):
		// BT.601 studio range -> RGB
		for (TInt x = 0; x + 1 < iW; x += 2, in += 4)
			{
			const TInt u = in[1] - 128, v = in[3] - 128;
			for (TInt k = 0; k < 2; ++k)
				{
				const TInt c = 298 * ((k ? in[2] : in[0]) - 16);
				TInt r = (c + 409 * v + 128) >> 8, g = (c - 100 * u - 208 * v + 128) >> 8, b = (c + 516 * u + 128) >> 8;
				r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
				out[(iW - 1 - (x + k)) * wordsPerLine + (iH - 1 - y)] = 0xFF000000u | (r << 16) | (g << 8) | b;	// 90 cw + vertical flip = the transpose (boot 940 vs the reference photo: floor top-left -> bottom-left, shelf bottom-right -> top-right)
				}
			}
		continue;
#endif
		for (TInt x = 0; x < iW; ++x, in += 2)
			{
			const TUint32 p = in[0] | (in[1] << 8);
			const TUint32 b = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, r = p & 0x1F;
			row[x] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
			}
		}
	iBitmap->UnlockHeap();
	if (++iFrames <= 3)
		RDebug::Printf("PH: phecam frame %d converted", iFrames);
	}
