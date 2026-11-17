// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// phecamtest.cpp -- OURS: a minimal CCamera client. Opens camera 0 through the ECam framework (ecam.dll -> the ROM's
// CCameraPlugin: the stock TestCamera plug-in = moving squares, or phecam = the camera driver's fake-sensor frames), starts the bitmap viewfinder and paints every
// frame into a full-screen window. "PH: camtest" lines count the frames. Boot test: SSM_TEST=phecamtest (x86).
#include <e32base.h>
#include <e32debug.h>
#include <w32std.h>
#include <fbs.h>
#include <ecam.h>
#include <f32file.h>
#include <imageconversion.h>
#include <ecamadvsettings.h>
#include <ecam/ecamadvsettingsintf.h>

// which camera(s), from the command line: (none) / "rear" = camera 0, "front" = camera 1 only, "both" = rear frames 1-199,
// then the front camera for frames 200-699. A still is taken at frame 100 of the first camera.
// More words test the camera-a64 plug-in's ECam features: "full" = the still in the largest capture size as EFormatExif
// (the plug-in's JPEG, written as is); "af" = a single autofocus at the picture's centre (frame 50, its status polled);
// "wb" = white balance daylight (frame 30); "ev" = exposure compensation +1 step (frame 30); "mirror" = a mirrored viewfinder.
static TInt TheFirstCamera = 0;
static TBool KTryFront = EFalse;
static TBool TheFull, TheAf, TheWb, TheEv, TheMirror;
static TInt TheSizeIndex;	// "sizeN": the capture-size index of the "full" still (0 = the largest)
static void ParseArgs()
	{
	TBuf<128> cmd;
	if (User::CommandLineLength() <= cmd.MaxLength())
		User::CommandLine(cmd);
	cmd.LowerCase();
	if (cmd.Find(_L("front")) >= 0)
		TheFirstCamera = 1;
	else if (cmd.Find(_L("both")) >= 0)
		KTryFront = ETrue;
	TheFull = cmd.Find(_L("full")) >= 0;
	{
	const TInt at = cmd.Find(_L("size"));
	if (at >= 0 && at + 4 < cmd.Length())
		TheSizeIndex = cmd[at + 4] - '0';
	}
	TheAf = cmd.Find(_L("af")) >= 0;
	TheWb = cmd.Find(_L("wb")) >= 0;
	TheEv = cmd.Find(_L("ev")) >= 0;
	TheMirror = cmd.Find(_L("mirror")) >= 0;
	RDebug::Printf("PH: camtest mode: first camera %d, then front %d; full %d af %d wb %d ev %d mirror %d", TheFirstCamera, KTryFront,
		TheFull, TheAf, TheWb, TheEv, TheMirror);
	}

// the camera-a64 plug-in's own interface (its TestCamera.h): the sensor's V4L2 controls
const TUid KPheCameraControlsUid = { 0xE7015C66 };
class MPheCameraControls
	{
public:
	virtual TInt SetCtrl(TUint32 aId, TInt aValue) = 0;
	virtual TInt GetCtrl(TUint32 aId, TInt& aValue) = 0;
	virtual TInt QueryCtrl(TUint32 aId, TAny* aInfo) = 0;
	virtual TInt FocusAt(const TPoint& aPoint, const TSize& aFrame) = 0;
	};
const TUint32 KV4l2CidAutoFocusStatus = 0x009a0900 + 30;	// V4L2_CID_AUTO_FOCUS_STATUS

// writes encoded bytes (EFormatExif / EFormatJpeg) to the next C:\Data\Images\phecam_<n>.jpg
static void SaveData(const TDesC8& aData)
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	fs.MkDirAll(_L("C:\\Data\\Images\\"));
	TFileName name;
	for (TInt n = 1; n < 10000; ++n)
		{
		name.Format(_L("C:\\Data\\Images\\phecam_%04d.jpg"), n);
		TEntry entry;
		if (fs.Entry(name, entry) == KErrNotFound)
			break;
		}
	RFile f;
	TInt r = f.Replace(fs, name, EFileWrite);
	if (r == KErrNone)
		{
		r = f.Write(aData);
		f.Close();
		}
	TBuf8<KMaxFileName> name8;
	name8.Copy(name);
	RDebug::Printf("PH: camtest still saved %S %d bytes r=%d %s", &name8, aData.Length(), r, r == KErrNone ? "PASS" : "FAIL");
	fs.Close();
	}

// a still from ImageReady -> C:\Data\Images\phecam_<n>.jpg through ICL (imageconversion + the JPEG codec plug-in)
class CJpegSaver : public CActive
	{
public:
	static CJpegSaver* NewL(CFbsBitmap* aBitmap)		// takes aBitmap
		{
		CJpegSaver* self = new (ELeave) CJpegSaver(aBitmap);
		CleanupStack::PushL(self);
		self->ConstructL();
		CleanupStack::Pop(self);
		return self;
		}
	~CJpegSaver() { Cancel(); delete iEncoder; delete iBitmap; iFs.Close(); }
private:
	CJpegSaver(CFbsBitmap* aBitmap) : CActive(EPriorityStandard), iBitmap(aBitmap) { CActiveScheduler::Add(this); }
	void ConstructL()
		{
		User::LeaveIfError(iFs.Connect());
		_LIT(KDir, "C:\\Data\\Images\\");
		TInt r = iFs.MkDirAll(KDir);
		if (r != KErrNone && r != KErrAlreadyExists)
			User::Leave(r);
		for (TInt n = 1; n < 10000; ++n)
			{
			iName.Format(_L("C:\\Data\\Images\\phecam_%04d.jpg"), n);
			TEntry entry;
			if (iFs.Entry(iName, entry) == KErrNotFound)
				break;
			}
		iEncoder = CImageEncoder::FileNewL(iFs, iName, _L8("image/jpeg"));
		iEncoder->Convert(&iStatus, *iBitmap);
		SetActive();
		}
	void RunL()
		{
		TBuf8<KMaxFileName> name8;
		name8.Copy(iName);
		TEntry entry;
		iFs.Entry(iName, entry);
		RDebug::Printf("PH: camtest still saved %S %d bytes r=%d %s", &name8, entry.iSize, iStatus.Int(), iStatus.Int() == KErrNone ? "PASS" : "FAIL");
		}
	void DoCancel() { if (iEncoder) iEncoder->Cancel(); }
	RFs iFs;
	TFileName iName;
	CImageEncoder* iEncoder;
	CFbsBitmap* iBitmap;
	};

class CCamTest : public CBase, public MCameraObserver
	{
public:
	CCamTest(RWsSession& aWs, CWsScreenDevice& aScreen, RWindow& aWin) : iWs(aWs), iScreen(aScreen), iWin(aWin) {}
	~CCamTest() { delete iSwitch; delete iSaver; delete iCam; delete iGc; }
	void StartL()
		{
		RDebug::Printf("PH: camtest cameras available %d", CCamera::CamerasAvailable());
		iIndex = TheFirstCamera;
		iCam = CCamera::NewL(*this, iIndex);
		iSwitch = new (ELeave) CAsyncCallBack(TCallBack(SwitchCb, this), CActive::EPriorityStandard);
		User::LeaveIfError(iScreen.CreateContext(iGc));
		iCam->Reserve();
		}
	// MCameraObserver
	void ReserveComplete(TInt aError)
		{
		RDebug::Printf("PH: camtest reserve %d", aError);
		if (aError == KErrNone)
			iCam->PowerOn();
		else
			CActiveScheduler::Stop();
		}
	void PowerOnComplete(TInt aError)
		{
		RDebug::Printf("PH: camtest poweron %d", aError);
		if (aError != KErrNone)
			{
			CActiveScheduler::Stop();
			return;
			}
		TCameraInfo info;
		iCam->CameraInfo(info);
		RDebug::Printf("PH: camtest info: hw %d options %x video fmts %x", info.iHardwareVersion.iMajor, info.iOptionsSupported, info.iVideoFrameFormatsSupported);
		if (TheMirror)
			{
			TRAPD(m, iCam->SetViewFinderMirrorL(ETrue));
			RDebug::Printf("PH: camtest mirror %d (now %d)", m, iCam->ViewFinderMirror());
			}
		TSize size = iScreen.SizeInPixels();
		TRAPD(e, iCam->StartViewFinderBitmapsL(size));
		RDebug::Printf("PH: camtest viewfinder start %d (size %dx%d)", e, size.iWidth, size.iHeight);
		if (e != KErrNone)
			CActiveScheduler::Stop();
		}
	void ViewFinderFrameReady(CFbsBitmap& aFrame)
		{
		++iFrames;
		if (iFrames <= 3 || iFrames % 50 == 0)
			RDebug::Printf("PH: camtest viewfinder frame %d (%dx%d)", iFrames, aFrame.SizeInPixels().iWidth, aFrame.SizeInPixels().iHeight);
		iWin.Invalidate();
		iWin.BeginRedraw();
		iGc->Activate(iWin);
		iGc->DrawBitmap(TRect(TPoint(0, 0), iScreen.SizeInPixels()), &aFrame);
		iGc->Deactivate();
		iWin.EndRedraw();
		iWs.Flush();
		if (iFrames == 30 && (TheWb || TheEv))
			Settings();
		if (iFrames == 50 && TheAf)
			Focus();
		if (TheAf && iFrames > 50 && iFrames <= 100 && iFrames % 10 == 0)
			FocusStatus();
		if (iFrames == 100 && TheFull)
			{
			FullStill();
			}
		else if (iFrames == 100)
			{
			// camera-plan.md "stills": one 640x480 still through ECam (the plug-in's capture path), saved as a JPEG in
			// C:\Data\Images (where the Photos/media gallery looks); the capture sizes are the sensor's modes, largest
			// first, so the viewfinder's size is looked up
			TRAPD(e, iCam->PrepareImageCaptureL(CCamera::EFormatFbsBitmapColor16M, SizeIndex(CCamera::EFormatFbsBitmapColor16M, TSize(480, 640))));
			RDebug::Printf("PH: camtest still prepare %d", e);
			if (e == KErrNone)
				iCam->CaptureImage();
			}
		// front/back (camera-plan.md): after 200 rear frames, the front camera (index 1) when the plug-in offers two; the
		// switch runs from a callback (the camera object is not deleted inside its own callback)
		if (iFrames == 200 && iIndex == 0 && CCamera::CamerasAvailable() > 1 && KTryFront)
			iSwitch->CallBack();
		if (iFrames == (KTryFront ? 700 : 400))	// rear only by default (front parked); with KTryFront: rear 1-199, front 200-699
			CActiveScheduler::Stop();
		}
	void ImageReady(CFbsBitmap* aBitmap, HBufC8* aData, TInt aError)
		{
		if (aData)
			{
			RDebug::Printf("PH: camtest still ready %d data %d bytes", aError, aData->Length());
			if (aError == KErrNone)
				SaveData(*aData);
			delete aData;
			}
		RDebug::Printf("PH: camtest still ready %d bitmap %08x", aError, (TUint)aBitmap);
		if (aError == KErrNone && aBitmap)
			{
			// boot 938: the JPEG was 5.8 KB = a nearly uniform picture -> what the bitmap really holds
			const TSize s = aBitmap->SizeInPixels();
			TRgb c[5];
			aBitmap->GetPixel(c[0], TPoint(s.iWidth / 2, s.iHeight / 2));
			aBitmap->GetPixel(c[1], TPoint(s.iWidth / 4, s.iHeight / 4));
			aBitmap->GetPixel(c[2], TPoint(3 * s.iWidth / 4, s.iHeight / 4));
			aBitmap->GetPixel(c[3], TPoint(s.iWidth / 4, 3 * s.iHeight / 4));
			aBitmap->GetPixel(c[4], TPoint(3 * s.iWidth / 4, 3 * s.iHeight / 4));
			RDebug::Printf("PH: camtest still %dx%d mode %d px %06x %06x %06x %06x %06x", s.iWidth, s.iHeight, aBitmap->DisplayMode(),
				c[0].Internal() & 0xFFFFFF, c[1].Internal() & 0xFFFFFF, c[2].Internal() & 0xFFFFFF, c[3].Internal() & 0xFFFFFF, c[4].Internal() & 0xFFFFFF);
			}
		if (aError == KErrNone && aBitmap && !iSaver)
			{
			TRAPD(e, iSaver = CJpegSaver::NewL(aBitmap));
			if (e != KErrNone)
				{
				RDebug::Printf("PH: camtest still save start %d FAIL", e);
				delete aBitmap;
				}
			}
		else
			delete aBitmap;
		}
	void FrameBufferReady(MFrameBuffer*, TInt) {}
	// the capture size index of aSize (0 = the largest, or the only one, when absent)
	TInt SizeIndex(CCamera::TFormat aFormat, const TSize& aSize)
		{
		TCameraInfo info;
		iCam->CameraInfo(info);
		for (TInt i = 0; i < info.iNumImageSizesSupported; ++i)
			{
			TSize s;
			iCam->EnumerateCaptureSizes(s, i, aFormat);
			if (s == aSize)
				return i;
			}
		return 0;
		}
	void FullStill()
		{
		TCameraInfo info;
		iCam->CameraInfo(info);
		for (TInt i = 0; i < info.iNumImageSizesSupported; ++i)
			{
			TSize s;
			iCam->EnumerateCaptureSizes(s, i, CCamera::EFormatExif);
			RDebug::Printf("PH: camtest capture size %d: %dx%d", i, s.iWidth, s.iHeight);
			}
		TRAPD(e, iCam->PrepareImageCaptureL(CCamera::EFormatExif, TheSizeIndex));
		RDebug::Printf("PH: camtest full still prepare (formats %x, size %d) %d", info.iImageFormatsSupported, TheSizeIndex, e);
		if (e == KErrNone)
			iCam->CaptureImage();
		}
	void Settings()
		{
		// the plug-in's advanced-settings interface itself (the image's ecam.dll does not export CCameraAdvancedSettings)
		MCameraAdvancedSettings* adv = (MCameraAdvancedSettings*)iCam->CustomInterface(KECamMCameraAdvancedSettingsUid);
		const TInt e = adv ? KErrNone : KErrNotSupported;
		TCameraInfo info;
		iCam->CameraInfo(info);
		if (TheWb)
			{
			TRAPD(w, iCam->SetWhiteBalanceL(CCamera::EWBDaylight));
			RDebug::Printf("PH: camtest white balance modes %x, daylight %d, now %x", info.iWhiteBalanceModesSupported, w, iCam->WhiteBalance());
			}
		if (TheEv && e == KErrNone)
			{
			TInt neg = 0, pos = 0;
			adv->GetExposureCompensationRangeInSteps(neg, pos);
			adv->SetExposureCompensation(1);
			TInt now = 0;
			TInt g = adv->GetExposureCompensation(now);
			RDebug::Printf("PH: camtest EV step %d range %d..%d, set +1 -> read %d (%d)", adv->ExposureCompensationStep(), neg, pos, now, g);
			}
		if (adv)
			adv->Release();
		}
	void Focus()
		{
		MPheCameraControls* c = (MPheCameraControls*)iCam->CustomInterface(KPheCameraControlsUid);
		TSize s = iScreen.SizeInPixels();
		TInt r = c ? c->FocusAt(TPoint(s.iWidth / 2, s.iHeight / 2), s) : KErrNotSupported;
		RDebug::Printf("PH: camtest focus at the centre -> %d", r);
		}
	void FocusStatus()
		{
		MPheCameraControls* c = (MPheCameraControls*)iCam->CustomInterface(KPheCameraControlsUid);
		TInt st = -1;
		TInt r = c ? c->GetCtrl(KV4l2CidAutoFocusStatus, st) : KErrNotSupported;
		RDebug::Printf("PH: camtest focus status frame %d: %d (%d)", iFrames, st, r);
		}
	static TInt SwitchCb(TAny* aSelf)
		{
		CCamTest* t = (CCamTest*)aSelf;
		t->iCam->StopViewFinder();
		t->iCam->PowerOff();
		t->iCam->Release();
		delete t->iCam;
		t->iCam = NULL;
		t->iIndex = 1;
		RDebug::Printf("PH: camtest switching to camera 1 (front)");
		TRAPD(e, t->iCam = CCamera::NewL(*t, 1));
		RDebug::Printf("PH: camtest camera 1 NewL %d", e);
		if (e == KErrNone)
			t->iCam->Reserve();
		else
			CActiveScheduler::Stop();
		return 0;
		}
	CAsyncCallBack* iSwitch;
	TInt iIndex;
	CCamera* iCam;
	CWindowGc* iGc;
	CJpegSaver* iSaver;
	TInt iFrames;
private:
	RWsSession& iWs;
	CWsScreenDevice& iScreen;
	RWindow& iWin;
	};

static void RunL()
	{
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
	User::LeaveIfError(group.Construct(0x43414D31, ETrue));
	group.SetOrdinalPosition(0, 20000);
	CleanupClosePushL(group);
	RWindow win(ws);
	User::LeaveIfError(win.Construct(group, 0x43414D32));
	CleanupClosePushL(win);
	win.SetExtent(TPoint(0, 0), screen->SizeInPixels());
	win.SetRequiredDisplayMode(EColor16MU);
	win.SetBackgroundColor(KRgbBlack);
	win.Activate();
	ws.Flush();
	CCamTest* t = new (ELeave) CCamTest(ws, *screen, win);
	CleanupStack::PushL(t);
	t->StartL();
	CActiveScheduler::Start();
	RDebug::Printf("PH: camtest done, %d frames %s", t->iFrames, t->iFrames > 0 ? "PASS" : "FAIL");
	CleanupStack::PopAndDestroy(5, &ws);		// t, win, group, screen, ws
	CleanupStack::PopAndDestroy(sched);
	}

TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	RDebug::Printf("PH: camtest start");
	ParseArgs();
	TRAPD(r, RunL());
	if (r != KErrNone)
		RDebug::Printf("PH: camtest leave %d FAIL", r);
	delete cleanup;
	return 0;
	}
