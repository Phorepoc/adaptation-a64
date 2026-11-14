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

// the front GC2145 is parked (camera-plan.md): rear only. ETrue = switch to camera 1 after 200 frames (needs a front-enabled kernel + phecam)
static const TBool KTryFront = EFalse;

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
		iCam = CCamera::NewL(*this, 0);
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
		if (iFrames == 100)
			{
			// camera-plan.md "stills": one 640x480 still through ECam (the plug-in's capture path), saved as a JPEG in
			// C:\Data\Images (where the Photos/media gallery looks)
			TRAPD(e, iCam->PrepareImageCaptureL(CCamera::EFormatFbsBitmapColor16M, 0));
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
		delete aData;
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
	TRAPD(r, RunL());
	if (r != KErrNone)
		RDebug::Printf("PH: camtest leave %d FAIL", r);
	delete cleanup;
	return 0;
	}
