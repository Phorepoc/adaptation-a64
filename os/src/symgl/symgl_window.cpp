// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// symgl_window.cpp -- OURS (2026-10-09, GPU plan G2/G5): the Symbian side of Mesa's EGL window surfaces
// (tools/mesa/symbian/include/symgl_native.h is the seam; egl_symbian.c + sym_frontend.c call it).
//
// NGA path (the window server composes surfaces): the window gets a SurfaceManager surface of SYMGL_NBUF buffers as
// its BACKGROUND SURFACE (the recipe of adapt.SimulatorGraphics guestegl.cpp eglCreateWindowSurface/eglSwapBuffers).
// GL renders STRAIGHT INTO the buffers (no copy): acquire() hands out the next buffer once the compositor has
// stopped reading it (the "displayed" notification of the frame that replaced it on screen), present() submits a
// finished buffer and registers that notification. Three buffers = the renderer can be two frames ahead of the
// display.
//
// The update notifications live on a PRESENTER THREAD per window (2026-10-12): it owns the RSurfaceUpdateSession, submits
// the queued buffers one at a time and waits for each "displayed". Outstanding requests on the APP's thread were stray
// signals to its event loop -- Qt's dispatcher qFatal'd "Caught Symbian stray signal" (King Oddball), a plain
// CActiveScheduler would panic E32USER-CBase 46. The app thread only queues (present) and waits on a semaphore (acquire).
//
// Fallback (no SurfaceManager: a non-NGA ROM): copy into an EColor16MU CFbsBitmap and BitBlt it into the window.
#include <e32std.h>
#include <e32debug.h>
#include <w32std.h>
#include <fbs.h>
#include <graphics/surface.h>
#include <graphics/surfacemanager.h>
#include <graphics/surfaceupdateclient.h>
#include <graphics/surfaceconfiguration.h>
#include <pixelformats.h>

#include "symgl_native.h"

namespace
	{
	const TInt KNbuf = SYMGL_NBUF;

	struct TWinState
		{
		RWindow* iWindow;
		// NGA surface path
		TBool iSurfaceTried, iSurfaceOk;
		RSurfaceManager iSm;
		RChunk iChunk;
		TSurfaceId iSurfaceId;
		TSize iSurfaceSize;
		TInt iStride;
		TInt iOffset[KNbuf];
		TUint iSubmitNo;				// the next submit's number; its buffer = iSubmitNo % KNbuf
		TTimeStamp iWhen;				// (the presenter's "displayed" time stamp)
		TInt64 iNextPresentUs;			// the swap interval (NTickCount-based)
		// the presenter thread: the app queues buffer numbers (iQueue, single producer / single consumer), iWork counts them;
		// iFree counts the buffers the app may render into (KNbuf - 1 at start; +1 per displayed submit)
		RThread iPresenter;
		RSemaphore iWork, iFree;
		volatile TInt iQueue[KNbuf];
		volatile TUint iQHead, iQTail;
		volatile TBool iQuit;
		TInt iScreen;
		// fallback path
		CWsScreenDevice* iDevice;
		CWindowGc* iGc;
		CFbsBitmap* iBitmap;
		};
	const TInt KMaxWindows = 8;
	TWinState gWins[KMaxWindows];

	TWinState* Find(void* aNative, TBool aCreate)
		{
		TWinState* freeSlot = NULL;
		for (TInt i = 0; i < KMaxWindows; ++i)
			{
			if (gWins[i].iWindow == aNative)
				return &gWins[i];
			if (!gWins[i].iWindow && !freeSlot)
				freeSlot = &gWins[i];
			}
		if (!aCreate || !freeSlot)
			return NULL;
		Mem::FillZ(freeSlot, sizeof(*freeSlot));
		new (&freeSlot->iSm) RSurfaceManager;
		new (&freeSlot->iChunk) RChunk;
		// the TPckgBuf timestamps MUST be constructed: a zeroed one is a descriptor of type 0 the update server cannot
		// write the "displayed" time into, and it then completes every NotifyWhenDisplayed at once with
		// KErrBadDescriptor (-38) -- which left the renderer unpaced against the compositor (torn frames, 2026-10-09)
		new (&freeSlot->iWhen) TTimeStamp;
		new (&freeSlot->iPresenter) RThread;
		new (&freeSlot->iWork) RSemaphore;
		new (&freeSlot->iFree) RSemaphore;
		freeSlot->iSurfaceId = TSurfaceId::CreateNullId();
		freeSlot->iWindow = static_cast<RWindow*>(aNative);
		return freeSlot;
		}

	// the completion codes seen (diagnosis: "[symgl] displayed: ok N overflow N notvisible N other N" every 120 submits)
	TUint gDispOk, gDispOverflow, gDispNotVisible, gDispOther;
	TInt gDispOtherCode;

	void CountDisplayed(TInt r)
		{
		if (r == KErrNone) ++gDispOk;
		else if (r == KErrOverflow) ++gDispOverflow;
		else if (r == KErrNotVisible) ++gDispNotVisible;
		else { ++gDispOther; gDispOtherCode = r; }
		}

	// the presenter: submit each queued buffer and wait until it is displayed (then the buffer before it is free again)
	TInt PresenterMain(TAny* aState)
		{
		TWinState& st = *static_cast<TWinState*>(aState);
		RSurfaceUpdateSession sus;
		const TInt cr = sus.Connect();
		RThread::Rendezvous(cr);
		if (cr != KErrNone)
			return cr;
		for (;;)
			{
			st.iWork.Wait();
			if (st.iQuit)
				break;
			const TInt buf = st.iQueue[st.iQTail % KNbuf];
			++st.iQTail;
			TRequestStatus disp;
			sus.NotifyWhenDisplayed(disp, st.iWhen);
			if (sus.SubmitUpdate(st.iScreen, st.iSurfaceId, buf) != KErrNone)
				sus.CancelAllUpdateNotifications();
			User::WaitForRequest(disp);
			CountDisplayed(disp.Int());
			st.iFree.Signal();
			}
		sus.Close();
		return KErrNone;
		}

	void StopPresenter(TWinState& aSt)
		{
		if (aSt.iPresenter.Handle() == KNullHandle)
			return;
		TRequestStatus dead;
		aSt.iPresenter.Logon(dead);
		aSt.iQuit = ETrue;
		aSt.iWork.Signal();
		User::WaitForRequest(dead);			// (synchronous: consumed here, never a stray signal)
		aSt.iPresenter.Close();
		}

	TInt StartPresenter(TWinState& aSt)
		{
		aSt.iQHead = aSt.iQTail = 0;
		aSt.iQuit = EFalse;
		TInt r = aSt.iWork.CreateLocal(0);
		if (r == KErrNone)
			r = aSt.iFree.CreateLocal(KNbuf - 1);
		if (r == KErrNone)
			r = aSt.iPresenter.Create(KNullDesC, PresenterMain, 0x2000, &User::Allocator(), &aSt);
		if (r != KErrNone)
			return r;
		TRequestStatus started;
		aSt.iPresenter.Rendezvous(started);
		aSt.iPresenter.SetPriority(EPriorityMore);
		aSt.iPresenter.Resume();
		User::WaitForRequest(started);
		return started.Int();
		}

	void CloseSurface(TWinState& aSt)
		{
		StopPresenter(aSt);
		aSt.iWork.Close();
		aSt.iFree.Close();
		if (aSt.iSurfaceOk)
			{
			RWindow& win = *aSt.iWindow;
			win.RemoveBackgroundSurface(ETrue);
			win.Session()->UnregisterSurface(win.ScreenNumber(), aSt.iSurfaceId);
			}
		aSt.iChunk.Close();
		if (!aSt.iSurfaceId.IsNull())
			aSt.iSm.CloseSurface(aSt.iSurfaceId);
		aSt.iSm.Close();
		aSt.iSurfaceId = TSurfaceId::CreateNullId();
		aSt.iSurfaceOk = EFalse;
		}

	// a KNbuf-buffer XRGB8888 surface of aSize as the window's background (guestegl.cpp's attributes)
	TInt OpenSurface(TWinState& aSt, const TSize& aSize)
		{
		TInt r = aSt.iSm.Open();
		if (r != KErrNone)
			return r;
		RSurfaceManager::TSurfaceCreationAttributesBuf buf;
		RSurfaceManager::TSurfaceCreationAttributes& a = buf();
		a.iSize = aSize;
		a.iBuffers = KNbuf;
		a.iPixelFormat = EUidPixelFormatXRGB_8888;
		// rows cover a 16-pixel-aligned width: the Mali renders 16x16 tiles, and Mesa's lima refuses an imported
		// linear buffer whose stride is below that (360 -> 368 pixels = 1472 bytes)
		a.iStride = 4 * ((aSize.iWidth + 15) & ~15);
		a.iOffsetToFirstBuffer = 0;
		a.iAlignment = RSurfaceManager::EPageAligned;
		a.iHintCount = 0;
		a.iSurfaceHints = NULL;
		// each buffer holds a 16-row-aligned height too: the Mali writes WHOLE 16x16 tiles, so a 360-row landscape surface
		// gets rows up to 368 -- past a tightly packed buffer into an unmapped page (MMU write faults on both PPs, the
		// unrendered tiles = King Oddball's black band, blocks and flicker, 2026-10-12)
		a.iOffsetBetweenBuffers = a.iStride * ((aSize.iHeight + 15) & ~15);
		a.iContiguous = ETrue;
		// CACHED: the reader (libWFC composing on another core) is a CPU; uncached made its per-pixel source read the
		// hottest code on the phone (OWF_Image_SourceFormatConversion, 46% of the CPU). softpipe writes with the CPU
		// too; the Mali's writes get CPU cache maintenance from the kernel driver (RMaliChannel::CacheSync)
		a.iCacheAttrib = RSurfaceManager::ECached;
		a.iMappable = ETrue;
		if ((r = aSt.iSm.CreateSurface(buf, aSt.iSurfaceId)) != KErrNone)
			return r;
		if ((r = aSt.iSm.MapSurface(aSt.iSurfaceId, aSt.iChunk)) != KErrNone)
			return r;
		for (TInt b = 0; b < KNbuf; ++b)
			if ((r = aSt.iSm.GetBufferOffset(aSt.iSurfaceId, b, aSt.iOffset[b])) != KErrNone)
				return r;
		RWindow& win = *aSt.iWindow;
		if ((r = win.Session()->RegisterSurface(win.ScreenNumber(), aSt.iSurfaceId)) != KErrNone)
			return r;
		TSurfaceConfiguration cfg;
		cfg.SetSurfaceId(aSt.iSurfaceId);
		if ((r = win.SetBackgroundSurface(cfg, ETrue)) != KErrNone)
			return r;
		win.Session()->Flush();
		aSt.iSurfaceSize = aSize;
		aSt.iStride = a.iStride;
		aSt.iSubmitNo = 0;
		aSt.iScreen = win.ScreenNumber();
		if ((r = StartPresenter(aSt)) != KErrNone)
			return r;
		aSt.iSurfaceOk = ETrue;
		return KErrNone;
		}

	void PresentBitmap(TWinState& aSt, const TUint8* aSrc, TInt aSrcStride, TInt aW, TInt aH)
		{
		RWindow& win = *aSt.iWindow;
		if (!aSt.iDevice)
			{
			if (RFbsSession::GetSession() == NULL && RFbsSession::Connect() != KErrNone)
				return;
			aSt.iDevice = new CWsScreenDevice(*win.Session());
			if (!aSt.iDevice || aSt.iDevice->Construct() != KErrNone || aSt.iDevice->CreateContext(aSt.iGc) != KErrNone)
				{
				delete aSt.iDevice;
				aSt.iDevice = NULL;
				return;
				}
			}
		const TSize size(aW, aH);
		if (!aSt.iBitmap || aSt.iBitmap->SizeInPixels() != size)
			{
			delete aSt.iBitmap;
			aSt.iBitmap = new CFbsBitmap;
			if (!aSt.iBitmap || aSt.iBitmap->Create(size, EColor16MU) != KErrNone)
				{
				delete aSt.iBitmap;
				aSt.iBitmap = NULL;
				return;
				}
			}
		CFbsBitmap& bmp = *aSt.iBitmap;
		bmp.BeginDataAccess();
		TUint8* dst = reinterpret_cast<TUint8*>(bmp.DataAddress());
		const TInt dstStride = CFbsBitmap::ScanLineLength(aW, EColor16MU);
		for (TInt y = 0; y < aH; ++y)
			Mem::Copy(dst + y * dstStride, aSrc + y * aSrcStride, aW * 4);
		bmp.EndDataAccess();
		CWindowGc& gc = *aSt.iGc;
		gc.Activate(win);
		win.Invalidate();
		win.BeginRedraw();
		gc.BitBlt(TPoint(0, 0), &bmp);
		win.EndRedraw();
		gc.Deactivate();
		win.Session()->Flush();
		}
	}

extern "C" int symgl_window_size(void* aNative, unsigned* aWidth, unsigned* aHeight)
	{
	if (!aNative)
		return 0;
	const TSize s = static_cast<RWindow*>(aNative)->Size();
	if (s.iWidth <= 0 || s.iHeight <= 0)
		return 0;
	*aWidth = s.iWidth;
	*aHeight = s.iHeight;
	return 1;
	}

extern "C" int symgl_window_surface(void* aNative, unsigned aWidth, unsigned aHeight, symgl_surface* aOut)
	{
	TWinState* st = Find(aNative, ETrue);
	if (!st)
		return 0;
	const TSize size(aWidth, aHeight);
	if (st->iSurfaceOk && st->iSurfaceSize != size)
		{
		CloseSurface(*st);				// the window was resized: a new surface of the new size
		st->iSurfaceTried = EFalse;
		}
	if (!st->iSurfaceTried)
		{
		st->iSurfaceTried = ETrue;
		const TInt r = OpenSurface(*st, size);
		// (PH: = forwarded to PhoneLab; once per surface. The window's screen position: the landscape black-band work)
		const TPoint pos = st->iWindow->AbsPosition();
		RDebug::Printf("PH: symgl window surface %dx%d at (%d,%d) x%d: %s (r=%d)", aWidth, aHeight, pos.iX, pos.iY, KNbuf,
			r == KErrNone ? "NGA background surface" : "no SurfaceManager -- bitmap blit", r);
		if (r != KErrNone)
			CloseSurface(*st);
		}
	if (!st->iSurfaceOk)
		return 0;
	aOut->chunk_handle = st->iChunk.Handle();
	aOut->base = st->iChunk.Base();
	aOut->chunk_size = st->iChunk.Size();
	aOut->width = aWidth;
	aOut->height = aHeight;
	aOut->stride = st->iStride;
	aOut->nbuf = KNbuf;
	for (TInt b = 0; b < KNbuf; ++b)
		aOut->offset[b] = st->iOffset[b];
	aOut->buf_size = st->iStride * ((aHeight + 15) & ~15);	// the tile-aligned rows (see OpenSurface)
	return 1;
	}

// Submit k goes into buffer k % N; the app may render into a buffer once the presenter has seen the submit after the one
// that last held it displayed (iFree: KNbuf - 1 buffers ahead). Plus a swap interval: presents are at most 60/s
// (unpaced, the Mali drew 343 triangles/s of which the display showed 60).
const TInt KSwapIntervalUs = 16667;
extern "C" int symgl_window_acquire(void* aNative)
	{
	TWinState* st = Find(aNative, EFalse);
	if (!st || !st->iSurfaceOk)
		return 0;
	st->iFree.Wait();
	const TInt64 now = (TInt64)User::NTickCount() * 1000;
	if (st->iNextPresentUs > now)
		User::After((TInt)(st->iNextPresentUs - now));
	return st->iSubmitNo % KNbuf;
	}

extern "C" void symgl_window_present(void* aNative, int aBuf)
	{
	TWinState* st = Find(aNative, EFalse);
	if (!st || !st->iSurfaceOk)
		return;
	st->iQueue[st->iQHead % KNbuf] = aBuf;
	++st->iQHead;
	st->iWork.Signal();
	++st->iSubmitNo;
	if (st->iSubmitNo % 120 == 0)
		RDebug::Printf("[symgl] displayed: ok %u overflow %u notvisible %u other %u (last other code %d; submits %u)",
			gDispOk, gDispOverflow, gDispNotVisible, gDispOther, gDispOtherCode, st->iSubmitNo);
	// the swap interval runs from this present, or from the previous deadline when we are keeping up (no drift)
	const TInt64 now = (TInt64)User::NTickCount() * 1000;
	st->iNextPresentUs = (st->iNextPresentUs + KSwapIntervalUs > now ? st->iNextPresentUs : now) + KSwapIntervalUs;
	}


extern "C" void symgl_window_present_pixels(void* aNative, const void* aPixels, unsigned aStride, unsigned aWidth,
	unsigned aHeight)
	{
	TWinState* st = Find(aNative, ETrue);
	if (st)
		PresentBitmap(*st, static_cast<const TUint8*>(aPixels), aStride, aWidth, aHeight);
	}

extern "C" void symgl_window_release(void* aNative)
	{
	TWinState* st = Find(aNative, EFalse);
	if (!st)
		return;
	CloseSurface(*st);
	delete st->iBitmap;
	delete st->iGc;
	delete st->iDevice;
	st->iBitmap = NULL;
	st->iGc = NULL;
	st->iDevice = NULL;
	st->iWindow = NULL;
	}

extern "C" void symgl_log(const char* aMsg)
	{
	RDebug::Printf("%s", aMsg);
	}

// G6 (egl_symbian.c, EGLImage from a chunk): the committed size of a chunk handle of this process, 0 if not a chunk.
// The handle stays the caller's (no Close).
extern "C" unsigned symgl_chunk_size(int aChunkHandle)
	{
	RChunk c;
	c.SetHandle(aChunkHandle);
	const TInt size = c.Size();
	return size > 0 ? (unsigned)size : 0;
	}

// ---------------------------------------------------------------------------------------------------------------
// G6b: RSgImage -> GPU-importable memory (egl_symbian.c: eglCreateImageKHR(EGL_NATIVE_PIXMAP_KHR) and
// eglCreatePixmapSurface on an RSgImage*, the EGL_SYMBIAN_PIXMAP_TYPE_RSGIMAGE / EGL_NOK_pixmap_type_rsgimage contract).
//
// The ROM's sgresource.dll is the 2010 UMA software provider (graphicsresourceimplementation: sgdriver / sgimage over the
// sgdevice LDD + the sgextension kernel extension). Every RSgImage is one shared chunk made by SgExtension::CreateResource
// (TChunkCreateInfo::ESharedKernelMultiple, own pages, cached when the image is CPU-accessed) and mapped into the process;
// the provider publishes only its CPU address, through the adapter interface MSgImage_Sw (sgimage_sw.h, UID 0x102858F0:
// DataAddress / DataStride; "supported if the image is or can be stored in system memory"). The Mali's MMU needs no
// contiguous memory -- the kernel driver maps the chunk's pages (RMaliChannel::ImportAddress finds the chunk from that
// address, Kern::OpenSharedChunk(thread, address)) -- so this bridge IS the adapter: handle -> (address, stride, format).
// The chunk keeps its CPU cache attribute; the callers do the RMaliChannel::CacheSync around GPU work.
#include <sgresource/sgresource.h>
#include <sgresource/sgimage.h>
#include <sgresource/sgimage_sw.h>

extern "C" int symgl_sgimage_info(void* aImage, symgl_sgimage* aOut)
	{
	if (!aImage || !aOut)
		return 0;
	RSgImage* image = static_cast<RSgImage*>(aImage);
	if (image->IsNull())
		return 0;
	TSgImageInfo info;
	if (image->GetInfo(info) != KErrNone || info.iSizeInPixels.iWidth <= 0 || info.iSizeInPixels.iHeight <= 0)
		return 0;
	switch (info.iPixelFormat)
		{
		case ESgPixelFormatARGB_8888:
		case ESgPixelFormatARGB_8888_PRE:
			aOut->format = SYMGL_SGFMT_ARGB8888;
			break;
		case ESgPixelFormatXRGB_8888:
			aOut->format = SYMGL_SGFMT_XRGB8888;
			break;
		case ESgPixelFormatRGB_565:
			aOut->format = SYMGL_SGFMT_RGB565;
			break;
		default:
			return 0;				// (A8: no GL render target / sampler format we import)
		}
	TAny* itf = NULL;
	if (image->GetInterface(TUid::Uid(MSgImage_Sw::EInterfaceUid), itf) != KErrNone || !itf)
		return 0;					// not in system memory
	const MSgImage_Sw* sw = static_cast<const MSgImage_Sw*>(itf);
	TAny* data = sw->DataAddress();
	const TInt stride = sw->DataStride();
	if (!data || stride <= 0)		// (a negative stride = bottom-up rows: never made by this provider)
		return 0;
	aOut->base = data;
	aOut->stride = (unsigned)stride;
	aOut->width = (unsigned)info.iSizeInPixels.iWidth;
	aOut->height = (unsigned)info.iSizeInPixels.iHeight;
	aOut->usage = (unsigned)info.iUsage;
	return 1;
	}
