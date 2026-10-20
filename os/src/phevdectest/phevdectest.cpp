// phevdectest.cpp -- OURS (VDU rung V4, docs/phorepoc/vdu-plan.md): drives the phevdec DevVideo plug-in THROUGH the public
// DevVideo API (CMMFDevVideoPlay), exactly as an MMF video controller would:
//   FindDecodersL(mime) -> SelectDecoderL -> VideoDecoderInfoLC -> SetInputFormatL -> GetOutputFormatListL /
//   SetOutputFormatL -> Initialize (MdvpoInitComplete) -> Start -> GetBufferL / WriteCodedDataL (MdvpoNewBuffers when it
//   returned NULL) -> InputEnd; pictures arrive by MdvpoNewPictures -> NextPictureL -> ReturnPicture; MdvpoStreamEnd.
// Streams (in ROM, from phevdetest's data): five H.264 clips -- every picture's NV12 (cropped Y rows, then UV rows)
// FNV-1a-hashed against ffmpeg's decode, in display order -- and the IPB MPEG-2 clip (PSNR against ffmpeg's NV12).
// "PH: dvp" lines: pictures, bit-exact count, ms.
#include <e32base.h>
#include <e32debug.h>
#include <e32math.h>
#include <f32file.h>
#include <mmf/devvideo/devvideoplay.h>

_LIT8(KMimeH264, "video/H264");
_LIT8(KMimeMpeg, "video/mpeg");
const TInt KPlugin = 0xE70157C3;

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

// One clip through the DevVideo API. Pictures are checked as they arrive; the scheduler runs until the stream ends.
class CClipTest : public CBase, public MMMFDevVideoPlayObserver
	{
public:
	CClipTest(const TDesC8& aMime, const TDesC& aName, TBool aH264, TInt aChunk)
		: iMime(aMime), iName(aName), iH264(aH264), iChunk(aChunk), iMinY(9999), iMinC(9999) {}
	~CClipTest();
	void RunL();							// everything; leaves on an API error
	// MMMFDevVideoPlayObserver
	void MdvpoNewBuffers() { iNeedBuffer = EFalse; Feed(); }
	void MdvpoReturnPicture(TVideoPicture*) {}
	void MdvpoSupplementalInformation(const TDesC8&, const TTimeIntervalMicroSeconds&, const TPictureId&) {}
	void MdvpoPictureLoss() {}
	void MdvpoPictureLoss(const TArray<TPictureId>&) {}
	void MdvpoSliceLoss(TUint, TUint, const TPictureId&) {}
	void MdvpoReferencePictureSelection(const TDesC8&) {}
	void MdvpoTimedSnapshotComplete(TInt, TPictureData*, const TTimeIntervalMicroSeconds&, const TPictureId&) {}
	void MdvpoNewPictures();
	void MdvpoFatalError(TInt aError) { iError = aError; Stop(); }
	void MdvpoInitComplete(TInt aError);
	void MdvpoStreamEnd() { iEnded = ETrue; Stop(); }
private:
	void Feed();
	static TInt FeedCb(TAny* aSelf) { ((CClipTest*)aSelf)->Feed(); return 0; }
	static TInt TimeoutCb(TAny* aSelf) { ((CClipTest*)aSelf)->iError = KErrTimedOut; ((CClipTest*)aSelf)->Stop(); return 0; }
	void Stop() { if (iRunning) CActiveScheduler::Stop(); }
	void Check(TVideoPicture* aPic);

	TPtrC8 iMime;
	TBuf<16> iName;
	TBool iH264;
	TInt iChunk;
	CMMFDevVideoPlay* iDvp;
	THwDeviceId iHw;
	HBufC8* iEs;						// the coded stream
	HBufC8* iRef;						// .fnv hashes (H.264) / NV12 reference frames (MPEG-2)
	TInt iOffset, iChunks;
	TBool iNeedBuffer, iInputDone, iEnded;
	TInt iError, iInit;
	TBool iRunning;
	CAsyncCallBack* iFeeder;
	// results
	TInt iPictures, iSame, iWidth, iHeight, iMinY, iMinC, iBadCrop;
	TInt64 iFirstTs[3];
	TUint32 iStartTick;
	};

CClipTest::~CClipTest()
	{
	delete iFeeder;
	delete iDvp;
	delete iEs;
	delete iRef;
	}

void CClipTest::RunL()
	{
	RFs fs;
	User::LeaveIfError(fs.Connect());
	CleanupClosePushL(fs);
	TFileName fn;
	if (iH264)
		{
		fn.Format(_L("z:\\resource\\phevde\\%S.h264"), &iName);
		User::LeaveIfError(ReadFile(fs, fn, iEs));
		fn.Format(_L("z:\\resource\\phevde\\%S.fnv"), &iName);
		User::LeaveIfError(ReadFile(fs, fn, iRef));
		}
	else
		{
		fn.Format(_L("z:\\resource\\phevde\\%S.m2v"), &iName);
		User::LeaveIfError(ReadFile(fs, fn, iEs));
		fn.Format(_L("z:\\resource\\phevde\\%S.nv12"), &iName);
		User::LeaveIfError(ReadFile(fs, fn, iRef));
		}
	CleanupStack::PopAndDestroy(&fs);

	TBuf8<16> tag;
	tag.Copy(iName);
	tag.ZeroTerminate();
	iDvp = CMMFDevVideoPlay::NewL(*this);
	// find: by MIME type; our plug-in must be among the decoders
	RArray<TUid> decoders;
	CleanupClosePushL(decoders);
	TRAPD(fr, iDvp->FindDecodersL(iMime, 0, decoders, ETrue));
	TBool ours = EFalse;
	for (TInt i = 0; i < decoders.Count(); ++i)
		ours |= decoders[i].iUid == KPlugin;
	RDebug::Printf("PH: dvp %s find '%S' r=%d decoders=%d ours=%s", tag.Ptr(), &iMime, fr, decoders.Count(), ours ? "yes" : "NO");
	User::LeaveIfError(fr);
	if (!ours)
		User::Leave(KErrNotFound);
	CleanupStack::PopAndDestroy(&decoders);
	iHw = iDvp->SelectDecoderL(TUid::Uid(KPlugin));
	CVideoDecoderInfo* info = iDvp->VideoDecoderInfoLC(TUid::Uid(KPlugin));
	TBuf8<64> ident;
	ident.Copy(info->Identifier().Left(60));		// (RDebug::Printf %S is 8-bit: a 16-bit descriptor printed as "A")
	ident.ZeroTerminate();
	RDebug::Printf("PH: dvp %s decoder '%s' formats=%d accel=%d maxsize=%dx%d", tag.Ptr(), ident.Ptr(),
		info->SupportedFormats().Count(), info->Accelerated(), info->MaxPictureSize().iWidth,
		info->MaxPictureSize().iHeight);
	CleanupStack::PopAndDestroy(info);

	// the header of the first data (before Initialize): the picture size
	CCompressedVideoFormat* fmt = CCompressedVideoFormat::NewL(iMime);
	CleanupStack::PushL(fmt);
	iDvp->SetInputFormatL(iHw, *fmt, EDuArbitraryStreamSection, EDuElementaryStream, ETrue);
	CleanupStack::PopAndDestroy(fmt);
	RArray<TUncompressedVideoFormat> outs;
	CleanupClosePushL(outs);
	iDvp->GetOutputFormatListL(iHw, outs);
	RDebug::Printf("PH: dvp %s output formats=%d first: raw=%d layout=%x", tag.Ptr(), outs.Count(),
		outs.Count() ? outs[0].iDataFormat == EYuvRawData : -1, outs.Count() ? outs[0].iYuvFormat.iDataLayout : 0);
	if (!outs.Count())
		User::Leave(KErrNotFound);
	iDvp->SetOutputFormatL(iHw, outs[0]);
	CleanupStack::PopAndDestroy(&outs);
	{
	TVideoInputBuffer hdrBuf;
	const TInt n = Min(iEs->Length(), 4096);
	TPtr8 p((TUint8*)iEs->Ptr(), n, n);
	hdrBuf.iData.Set(p);
	TVideoPictureHeader* h = iDvp->GetHeaderInformationL(EDuArbitraryStreamSection, EDuElementaryStream, &hdrBuf);
	if (h)
		{
		RDebug::Printf("PH: dvp %s header: profile %d level %d memory %dx%d displayed %dx%d", tag.Ptr(), h->iProfile, h->iLevel,
			h->iSizeInMemory.iWidth, h->iSizeInMemory.iHeight, h->iDisplayedRect.Width(), h->iDisplayedRect.Height());
		iDvp->ReturnHeader(h);
		}
	else
		RDebug::Printf("PH: dvp %s header: none in the first %d bytes", tag.Ptr(), n);
	}

	iFeeder = new (ELeave) CAsyncCallBack(TCallBack(FeedCb, this), CActive::EPriorityStandard);
	iStartTick = User::NTickCount();
	iInit = KRequestPending;
	iDvp->Initialize();						// (the plug-in completes synchronously: MdvpoInitComplete starts the feed)
	if (iError == KErrNone)
		{
		iRunning = ETrue;
		CPeriodic* watchdog = CPeriodic::NewL(CActive::EPriorityLow);
		CleanupStack::PushL(watchdog);
		watchdog->Start(60 * 1000000, 60 * 1000000, TCallBack(TimeoutCb, this));
		CActiveScheduler::Start();			// runs the feed + the decode steps to stream end / error
		CleanupStack::PopAndDestroy(watchdog);
		iRunning = EFalse;
		}
	const TUint32 ms = User::NTickCount() - iStartTick;

	CMMFDevVideoPlay::TPictureCounters pc;
	iDvp->GetPictureCounters(pc);
	const TBool pass = iError == KErrNone && iEnded && iPictures > 0 &&
		(iH264 ? iSame == iPictures : iMinY >= 4000 && iMinC >= 4000);
	if (iH264)
		RDebug::Printf("PH: dvp %s %dx%d: %d pictures, bit-exact %d/%d, ts[0..2] %d %d %d us, %d chunks of %d, counters decoded %u"
			" displayed %u, error %d, %u ms %s", tag.Ptr(), iWidth, iHeight, iPictures, iSame, iPictures, (TInt)iFirstTs[0],
			(TInt)iFirstTs[1], (TInt)iFirstTs[2], iChunks, iChunk, pc.iPicturesDecoded, pc.iPicturesDisplayed, iError, ms,
			pass ? "PASS" : "FAIL");
	else
		RDebug::Printf("PH: dvp %s %dx%d: %d pictures, min PSNR Y %d.%02d C %d.%02d, ts[0..2] %d %d %d us, %d chunks of %d, counters"
			" decoded %u displayed %u, error %d, %u ms %s", tag.Ptr(), iWidth, iHeight, iPictures, iMinY / 100, iMinY % 100,
			iMinC / 100, iMinC % 100, (TInt)iFirstTs[0], (TInt)iFirstTs[1], (TInt)iFirstTs[2], iChunks, iChunk,
			pc.iPicturesDecoded, pc.iPicturesDisplayed, iError, ms, pass ? "PASS" : "FAIL");
	}

void CClipTest::MdvpoInitComplete(TInt aError)
	{
	iInit = aError;
	RDebug::Printf("PH: dvp init complete r=%d", aError);
	if (aError != KErrNone)
		{
		iError = aError;
		Stop();
		return;
		}
	RDebug::Printf("PH: dvp start decoder");
	iDvp->Start();
	RDebug::Printf("PH: dvp feed");
	iFeeder->CallBack();
	}

// write the next chunk of the stream; one chunk per callback
void CClipTest::Feed()
	{
	if (iInputDone || iNeedBuffer || iError != KErrNone)
		return;
	if (iOffset >= iEs->Length())
		{
		iInputDone = ETrue;
		iDvp->InputEnd();
		return;
		}
	const TInt n = Min(iChunk, iEs->Length() - iOffset);
	TVideoInputBuffer* b = NULL;
	TRAPD(r, b = iDvp->GetBufferL(n));
	if (r != KErrNone)
		{
		iError = r;
		Stop();
		return;
		}
	if (!b)
		{
		iNeedBuffer = ETrue;				// MdvpoNewBuffers resumes the feed
		return;
		}
	if (iChunks < 4)
		RDebug::Printf("PH: dvp chunk %d got buffer %x", iChunks, (TUint)b);
	b->iData.Copy(iEs->Ptr() + iOffset, n);
	b->iOptions = TVideoInputBuffer::EPresentationTimestamp;
	b->iPresentationTimestamp = TTimeIntervalMicroSeconds((TInt64)iChunks * 1000);
	if (iChunks < 4)
		RDebug::Printf("PH: dvp write chunk %d len %d", iChunks, b->iData.Length());
	TRAP(r, iDvp->WriteCodedDataL(b));
	if (iChunks < 4)
		RDebug::Printf("PH: dvp wrote r=%d", r);
	if (r != KErrNone)
		{
		iError = r;
		Stop();
		return;
		}
	iOffset += n;
	++iChunks;
	iFeeder->CallBack();
	}

void CClipTest::MdvpoNewPictures()
	{
	if (iPictures < 3)
		RDebug::Printf("PH: dvp new pictures (have %d)", iPictures);
	for (;;)
		{
		TVideoPicture* pic = NULL;
		TRAPD(r, pic = iDvp->NextPictureL());
		if (r != KErrNone || !pic)
			return;
		if (iPictures < 3)
			RDebug::Printf("PH: dvp got picture %x fmt %x raw %x size %dx%d opt %x", (TUint)pic, pic->iData.iDataFormat,
				(TUint)pic->iData.iRawData, pic->iData.iDataSize.iWidth, pic->iData.iDataSize.iHeight, pic->iOptions);
		Check(pic);
		if (iPictures <= 3)
			RDebug::Printf("PH: dvp checked %d", iPictures);
		iDvp->ReturnPicture(pic);
		}
	}

void CClipTest::Check(TVideoPicture* aPic)
	{
	const TInt i = iPictures++;
	if (i < 3)
		iFirstTs[i] = aPic->iTimestamp.Int64();
	const TPictureData& d = aPic->iData;
	if (d.iDataFormat != EYuvRawData || !d.iRawData)
		{
		++iBadCrop;
		return;
		}
	const TUint8* y = d.iRawData->Ptr();
	const TInt stride = d.iDataSize.iWidth, ah = d.iDataSize.iHeight;
	const TRect crop = aPic->iOptions & TVideoPicture::ECropRect ? aPic->iCropRect : TRect(0, 0, stride, ah);
	const TInt w = crop.Width(), h = crop.Height();
	iWidth = w, iHeight = h;
	const TUint8* uv = y + stride * ah;
	if (iH264)
		{
		TUint32 hsh = 0x811c9dc5u;
		for (TInt row = 0; row < h; ++row)
			hsh = Fnv(hsh, y + (crop.iTl.iY + row) * stride + crop.iTl.iX, w);
		for (TInt row = 0; row < h / 2; ++row)
			hsh = Fnv(hsh, uv + (crop.iTl.iY / 2 + row) * stride + crop.iTl.iX, w);
		TInt at = 0;									// line i (LF or CRLF)
		for (TInt l = 0; l < i && at < iRef->Length(); ++at)
			if ((*iRef)[at] == '\n')
				++l;
		const TUint32 want = HexWord(*iRef, at);
		if (hsh == want)
			++iSame;
		else if (i - iSame < 4)
			RDebug::Printf("PH: dvp picture %d: fnv %08x want %08x, ts %d", i, hsh, want, (TInt)aPic->iTimestamp.Int64());
		}
	else
		{
		const TInt frame = w * h * 3 / 2;
		if ((i + 1) * frame <= iRef->Length() && stride == w)
			{
			const TUint8* rf = iRef->Ptr() + i * frame;
			const TInt py = Psnr(y, rf, w * h), pc = Psnr(uv, rf + w * h, w * h / 2);
			iMinY = Min(iMinY, py), iMinC = Min(iMinC, pc);
			if (i < 4)
				RDebug::Printf("PH: dvp picture %d: PSNR Y %d.%02d C %d.%02d ts %d", i, py / 100, py % 100, pc / 100, pc % 100,
					(TInt)aPic->iTimestamp.Int64());
			}
		else
			iMinY = iMinC = 0;
		}
	}

static void RunClip(const TDesC8& aMime, const TDesC& aName, TBool aH264, TInt aChunk)
	{
	CClipTest* t = new CClipTest(aMime, aName, aH264, aChunk);
	if (!t)
		return;
	TRAPD(r, t->RunL());
	if (r != KErrNone)
		RDebug::Printf("PH: dvp leave %d", r);
	delete t;
	}

TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	CActiveScheduler* sched = new CActiveScheduler;
	CActiveScheduler::Install(sched);
	RDebug::Printf("PH: dvp start");
	RunClip(KMimeH264, _L("hmain"), ETrue, 4096);
	RunClip(KMimeH264, _L("hmain"), ETrue, 777);		// NALs split at odd places
	RunClip(KMimeH264, _L("hbase"), ETrue, 4096);
	RunClip(KMimeH264, _L("hh8x8"), ETrue, 4096);
	RunClip(KMimeH264, _L("hcqm"), ETrue, 4096);
	RunClip(KMimeH264, _L("hhigh"), ETrue, 4096);
	RunClip(KMimeH264, _L("hmbaffm"), ETrue, 4096);			// interlaced: MBAFF frames, PAFF field pictures
	RunClip(KMimeH264, _L("hmbaffh"), ETrue, 4096);
	RunClip(KMimeH264, _L("hpaff2"), ETrue, 4096);
	RunClip(KMimeH264, _L("hpaff3"), ETrue, 777);
	RunClip(KMimeH264, _L("hpaffl"), ETrue, 4096);
	RunClip(KMimeMpeg, _L("ipb"), EFalse, 4096);
	RDebug::Printf("PH: dvp done");
	delete sched;
	delete cleanup;
	return 0;
	}
