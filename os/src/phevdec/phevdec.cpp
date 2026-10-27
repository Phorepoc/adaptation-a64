// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// phevdec.cpp -- OURS (VDU rung V4): the DevVideo decoder hw device on the A64 Video Engine. See phevdec.h for the
// contract (formats, data units, output pictures, threading).
#include <e32debug.h>
#include <hal.h>
#include <ecom/implementationproxy.h>
#include "phevdec.h"
#include <mm/mmcleanup.h>

#define PHTRACE(args) do { if (iTrace < 0) { ++iTrace; RDebug::Printf args; } } while (0)


_LIT(KPheManufacturer, "Phorepoc");
_LIT(KPheIdentifier, "A64 Video Engine decoder (H.264 / MPEG-2)");
_LIT8(KMimeH264, "video/H264");
_LIT8(KMimeMpeg, "video/mpeg");
_LIT8(KMimeMpeg2, "video/mpeg2");
_LIT8(KMimeMp2v, "video/mp2v");
_LIT8(KMimeMp4v, "video/mp4v-es");

// ---------------------------------------------------------------------------------------------------------------
// CPheVePool

CPheVePool::CPheVePool(RChunk& aArena, TUint32 aPhys, TInt aSize, TInt aReserve)
	: iBusy(NULL), iBusyCtx(NULL), iArena(aArena), iPhys(aPhys), iSize(aSize), iNext(aReserve), iCount(0)
	{
	}

uint32_t CPheVePool::Alloc(uint32_t aSize, uint32_t aAlign, int& aIndex)
	{
	for (TInt i = 0; iBusy && i < iCount; ++i)	// a block of the same size that nobody needs any more
		{
		TEntry& e = iEntries[i];
		if (e.iSize == aSize && !e.iHeld && !iBusy(iBusyCtx, e.iBus))
			{
			aIndex = e.iIndex;
			return e.iBus;
			}
		}
	const TInt at = (iNext + (TInt)aAlign - 1) & ~((TInt)aAlign - 1);
	if (at + (TInt)aSize > iSize || iCount >= KMaxEntries)
		return 0;
	iNext = at + (TInt)aSize;
	TEntry& e = iEntries[iCount++];
	e.iBus = iPhys + at - KDramBus;
	e.iSize = aSize;
	e.iIndex = at;
	e.iHeld = EFalse;
	aIndex = at;
	return e.iBus;
	}

void CPheVePool::Hold(uint32_t aBus, TBool aHeld)
	{
	for (TInt i = 0; i < iCount; ++i)
		if (iEntries[i].iBus == aBus)
			iEntries[i].iHeld = aHeld;
	}


// ---------------------------------------------------------------------------------------------------------------
// construction

CMMFVideoDecodeHwDevice* CPheVideoDecoder::NewL(TAny* /*aInitParams*/)
	{
	CPheVideoDecoder* d = new (ELeave) CPheVideoDecoder;
	CleanupStack::PushL(d);
	d->iStep = new (ELeave) CPheStep(*d);
	CleanupStack::Pop(d);
	return d;			// (single inheritance: the same address as the CMMFVideoDecodeHwDevice)
	}

CPheVideoDecoder::CPheVideoDecoder() : iMaxHeld(8), iDepth(1)
	{
	iMaxInputSize = 256 * 1024;
	iMinBuffers = 4;
	}

CPheVideoDecoder::~CPheVideoDecoder()
	{
	delete iStep;
	if (iVe.Handle())
		{
		iVe.CancelAll();
		iVe.WriteReg(0x000, 0x00000007);		// engine off
		}
	delete iH264;
	delete iSh;
	delete iH264Parser;
	delete iM2;
	delete iM2Parser;
	delete iPool;
	delete iJob;
	iArena.Close();
	iVe.Close();
	iStream.Close();
	iChunks.Close();
	for (TInt i = 0; i < KMaxInput; ++i)
		delete iIn[i].iData;
	}

TAny* CPheVideoDecoder::CustomInterface(TUid /*aInterface*/)
	{
	// MMMFVideoSurfaceSupport (0x1028340E, zero-copy surfaces) is not implemented yet: the MMF controller falls back to
	// the raw-picture path (V4 follow-up)
	return NULL;
	}

void CPheVideoDecoder::SetProxy(MMMFDevVideoPlayProxy& aProxy)
	{
	iProxy = &aProxy;
	}

// ---------------------------------------------------------------------------------------------------------------
// information

void CPheVideoDecoder::SetupFormat(TUncompressedVideoFormat& aFormat) const
	{
	Mem::FillZ(&aFormat, sizeof(aFormat));
	aFormat.iDataFormat = EYuvRawData;
	aFormat.iYuvFormat.iCoefficients = EYuvBt601Range0;
	aFormat.iYuvFormat.iPattern = EYuv420Chroma1;
	aFormat.iYuvFormat.iDataLayout = EYuvDataSemiPlanar;
	aFormat.iYuvFormat.iAspectRatioNum = 1;
	aFormat.iYuvFormat.iAspectRatioDenom = 1;
	}

CPostProcessorInfo* CPheVideoDecoder::PostProcessorInfoLC()
	{
	RArray<TUncompressedVideoFormat> formats;
	CleanupClosePushL(formats);
	TUncompressedVideoFormat f;
	SetupFormat(f);
	User::LeaveIfError(formats.Append(f));
	RArray<TUint32> combs;
	CleanupClosePushL(combs);
	User::LeaveIfError(combs.Append(0));				// no post-processing: the picture as decoded
	RArray<TScaleFactor> scales;
	CleanupClosePushL(scales);
	TYuvToRgbCapabilities caps;
	Mem::FillZ(&caps, sizeof(caps));
	CPostProcessorInfo* info = CPostProcessorInfo::NewL(TUid::Uid(KPheVdecImplUid), KPheManufacturer, KPheIdentifier,
		TVersion(1, 0, 0), formats.Array(), combs.Array(), ETrue, EFalse, caps, 0, EFalse, scales.Array(), EFalse);
	CleanupStack::PopAndDestroy(3, &formats);
	CleanupStack::PushL(info);
	return info;
	}

void CPheVideoDecoder::GetOutputFormatListL(RArray<TUncompressedVideoFormat>& aFormats)
	{
	TUncompressedVideoFormat f;
	SetupFormat(f);
	User::LeaveIfError(aFormats.Append(f));
	}

void CPheVideoDecoder::SetOutputFormatL(const TUncompressedVideoFormat& aFormat)
	{
	if (aFormat.iDataFormat != EYuvRawData || aFormat.iYuvFormat.iDataLayout != EYuvDataSemiPlanar)
		User::Leave(KErrNotSupported);
	}

CVideoDecoderInfo* CPheVideoDecoder::VideoDecoderInfoLC()
	{
	RPointerArray<CCompressedVideoFormat> formats;
	CleanupResetAndDestroyPushL(formats);
	const TDesC8* mimes[] = { &KMimeH264, &KMimeMpeg, &KMimeMpeg2, &KMimeMp2v };
	for (TUint i = 0; i < sizeof(mimes) / sizeof(mimes[0]); ++i)
		{
		CCompressedVideoFormat* f = CCompressedVideoFormat::NewL(*mimes[i]);
		CleanupStack::PushL(f);
		User::LeaveIfError(formats.Append(f));
		CleanupStack::Pop(f);
		}
	RArray<TPictureRateAndSize> rates;
	CleanupClosePushL(rates);
	TPictureRateAndSize r;
	r.iPictureRate = 60.0;
	r.iPictureSize = TSize(1920, 1088);
	User::LeaveIfError(rates.Append(r));
	r.iPictureRate = 30.0;
	r.iPictureSize = TSize(4096, 2304);
	User::LeaveIfError(rates.Append(r));
	CVideoDecoderInfo* info = CVideoDecoderInfo::NewL(TUid::Uid(KPheVdecImplUid), KPheManufacturer, KPheIdentifier,
		TVersion(1, 0, 0), formats.Array(), ETrue, EFalse, TSize(4096, 2304), 60 * 1000 * 1000, rates.Array(), EFalse,
		EFalse);
	CleanupStack::PopAndDestroy(2, &formats);
	CleanupStack::PushL(info);
	return info;
	}

// A header from the first data: H.264 -> the first SPS, MPEG-2 -> the sequence header. NULL = not enough data yet.
TVideoPictureHeader* CPheVideoDecoder::GetHeaderInformationL(TVideoDataUnitType /*aDataUnitType*/,
	TVideoDataUnitEncapsulation /*aEncapsulation*/, TVideoInputBuffer* aDataUnit)
	{
	if (!aDataUnit)
		User::Leave(KErrArgument);
	const TUint8* buf = aDataUnit->iData.Ptr();
	const size_t len = aDataUnit->iData.Length();
	Mem::FillZ(&iHeader, sizeof(iHeader));
	iHeader.iProfile = iHeader.iLevel = iHeader.iVersion = -1;
	iHeader.iIsRandomAccessible = ETrue;
	if (iCodec == EMpeg4)
		return NULL;						// (not parsed: the VOL header goes to libavcodec in band)
	if (iCodec != EMpeg2)
		{
		size_t pos = 0, s = 0, e = 0;
		while (h264::NextNal(buf, len, pos, s, e))
			{
			if ((buf[s] & 0x1F) != h264::ENalSps || pos + 3 > len)
				continue;
			h264::TParser* p = new (ELeave) h264::TParser;
			CleanupStack::PushL(p);
			TVideoPictureHeader* result = NULL;
			if (p->ParseSps(buf + s, e - s))
				{
				for (uint32_t id = 0; id < h264::KMaxSps; ++id)
					if (const h264::TSps* sps = p->Sps(id))
						{
						TUint32 ux = 2, uy = 2 * (2 - (sps->iFrameMbsOnly ? 1 : 0));
						iHeader.iProfile = sps->iProfileIdc;
						iHeader.iLevel = sps->iLevelIdc;
						iHeader.iSizeInMemory = TSize(sps->Width(), sps->Height());
						iHeader.iDisplayedRect = TRect(0, 0, sps->Width(), sps->Height());
						if (sps->iFrameCropping)
							iHeader.iDisplayedRect = TRect(sps->iCropLeft * ux, sps->iCropTop * uy,
								sps->Width() - sps->iCropRight * ux, sps->Height() - sps->iCropBottom * uy);
						result = &iHeader;
						break;
						}
				}
			CleanupStack::PopAndDestroy(p);
			if (result)
				{
				iCodec = EH264;
				return result;
				}
			}
		if (iCodec == EH264)
			return NULL;
		}
	// MPEG-2: a sequence header (00 00 01 B3 + 12 bit width, 12 bit height)
	for (size_t i = 0; i + 8 <= len; ++i)
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 && buf[i + 3] == 0xB3)
			{
			const TInt w = (buf[i + 4] << 4) | (buf[i + 5] >> 4), h = ((buf[i + 5] & 0xF) << 8) | buf[i + 6];
			iHeader.iSizeInMemory = TSize((w + 15) & ~15, (h + 15) & ~15);
			iHeader.iDisplayedRect = TRect(0, 0, w, h);
			iCodec = EMpeg2;
			return &iHeader;
			}
	return NULL;
	}

void CPheVideoDecoder::ReturnHeader(TVideoPictureHeader* /*aHeader*/)
	{
	}

void CPheVideoDecoder::ConfigureDecoderL(const TVideoPictureHeader& /*aVideoPictureHeader*/)
	{
	// nothing to configure: the stream's own SPS / sequence header carries everything
	}

void CPheVideoDecoder::SetInputFormatL(const CCompressedVideoFormat& aFormat, TVideoDataUnitType aDataUnitType,
	TVideoDataUnitEncapsulation aEncapsulation, TBool aDataInOrder)
	{
	if (iInitialised)
		User::Leave(KErrNotReady);
	TPtrC8 mime = aFormat.MimeType();
	const TInt semi = mime.Locate(';');
	if (semi != KErrNotFound)
		mime.Set(mime.Left(semi));
	TCodec codec = ENoCodec;
	if (mime.CompareF(KMimeH264) == 0)
		codec = EH264;
	else if (mime.CompareF(KMimeMpeg) == 0 || mime.CompareF(KMimeMpeg2) == 0 || mime.CompareF(KMimeMp2v) == 0)
		codec = EMpeg2;
	if (codec == ENoCodec || aEncapsulation != EDuElementaryStream || !aDataInOrder
		|| (aDataUnitType != EDuArbitraryStreamSection && aDataUnitType != EDuCodedPicture))
		User::Leave(KErrNotSupported);
	iCodec = codec;
	iComplete = aDataUnitType == EDuCodedPicture;
	}

void CPheVideoDecoder::SynchronizeDecoding(TBool /*aSynchronize*/)
	{
	// pictures are decoded as soon as data arrives and a free buffer exists; timestamps are passed through
	}

void CPheVideoDecoder::SetBufferOptionsL(const CMMFDevVideoPlay::TBufferOptions& aOptions)
	{
	if (aOptions.iMinNumInputBuffers > (TUint)KMaxInput)
		User::Leave(KErrNotSupported);
	iBufferOptions = aOptions;
	if (aOptions.iMinNumInputBuffers > (TUint)iMinBuffers)
		iMinBuffers = aOptions.iMinNumInputBuffers;
	if (aOptions.iMaxInputBufferSize)
		iMaxInputSize = aOptions.iMaxInputBufferSize;
	}

void CPheVideoDecoder::GetBufferOptions(CMMFDevVideoPlay::TBufferOptions& aOptions)
	{
	aOptions = iBufferOptions;
	aOptions.iMinNumInputBuffers = iMinBuffers;
	aOptions.iMaxInputBufferSize = iMaxInputSize;
	}

void CPheVideoDecoder::SetHrdVbvSpec(THrdVbvSpecification /*aHrdVbvSpec*/, const TDesC8& /*aHrdVbvParams*/)
	{
	}

void CPheVideoDecoder::SetOutputDevice(CMMFVideoPostProcHwDevice* aDevice)
	{
	// a post-processor chained after the decoder (the NGA one, 0x10273417, for surfaces) takes the pictures via
	// WritePictureL: not wired yet -- without it the pictures go to the client (MdvppNewPicture)
	(void)aDevice;
	}

// ---------------------------------------------------------------------------------------------------------------
// post-processing, display, timing: none (raw pictures to the client)

void CPheVideoDecoder::SetClockSource(MMMFClockSource* /*aClock*/) {}
void CPheVideoDecoder::SetVideoDestScreenL(TBool /*aScreen*/) {}

void CPheVideoDecoder::SetPostProcessTypesL(TUint32 aPostProcCombination)
	{
	if (aPostProcCombination != 0)
		User::Leave(KErrNotSupported);
	}

void CPheVideoDecoder::SetInputCropOptionsL(const TRect&) { User::Leave(KErrNotSupported); }
void CPheVideoDecoder::SetYuvToRgbOptionsL(const TYuvToRgbOptions&, const TYuvFormat&, TRgbFormat) { User::Leave(KErrNotSupported); }
void CPheVideoDecoder::SetYuvToRgbOptionsL(const TYuvToRgbOptions&) { User::Leave(KErrNotSupported); }
void CPheVideoDecoder::SetRotateOptionsL(TRotationType) { User::Leave(KErrNotSupported); }
void CPheVideoDecoder::SetScaleOptionsL(const TSize&, TBool) { User::Leave(KErrNotSupported); }
void CPheVideoDecoder::SetOutputCropOptionsL(const TRect&) { User::Leave(KErrNotSupported); }
void CPheVideoDecoder::SetPostProcSpecificOptionsL(const TDesC8&) { User::Leave(KErrNotSupported); }
void CPheVideoDecoder::CommitL() {}
void CPheVideoDecoder::Revert() {}
void CPheVideoDecoder::StartDirectScreenAccessL(const TRect&, CFbsScreenDevice&, const TRegion&) { User::Leave(KErrNotSupported); }
void CPheVideoDecoder::SetScreenClipRegion(const TRegion&) {}
void CPheVideoDecoder::SetPauseOnClipFail(TBool) {}
void CPheVideoDecoder::AbortDirectScreenAccess() {}
TBool CPheVideoDecoder::IsPlaying() { return iPlaying; }
void CPheVideoDecoder::Redraw() {}
void CPheVideoDecoder::Start() { iPlaying = ETrue; iPaused = EFalse; Schedule(); }
void CPheVideoDecoder::Stop() { iPlaying = EFalse; }
void CPheVideoDecoder::Pause() { iPaused = ETrue; }
void CPheVideoDecoder::Resume() { iPaused = EFalse; iPlaying = ETrue; Schedule(); }
void CPheVideoDecoder::SetPosition(const TTimeIntervalMicroSeconds&) {}
void CPheVideoDecoder::FreezePicture(const TTimeIntervalMicroSeconds&) {}
void CPheVideoDecoder::ReleaseFreeze(const TTimeIntervalMicroSeconds&) {}
TTimeIntervalMicroSeconds CPheVideoDecoder::PlaybackPosition() { return iDecodePos; }
TTimeIntervalMicroSeconds CPheVideoDecoder::DecodingPosition() { return iDecodePos; }

TUint CPheVideoDecoder::PictureBufferBytes()
	{
	TUint n = 0;
	for (TInt i = 0; i < KMaxOut; ++i)
		if (iOut[i].iState)
			n += iOut[i].iPtr.Length();
	return n;
	}

void CPheVideoDecoder::GetPictureCounters(CMMFDevVideoPlay::TPictureCounters& aCounters)
	{
	aCounters = iPicCounters;
	iPicCounters = CMMFDevVideoPlay::TPictureCounters();
	}

void CPheVideoDecoder::SetComplexityLevel(TUint) {}
TUint CPheVideoDecoder::NumComplexityLevels() { return 1; }

void CPheVideoDecoder::GetComplexityLevelInfo(TUint /*aLevel*/, CMMFDevVideoPlay::TComplexityLevelInfo& aInfo)
	{
	Mem::FillZ(&aInfo, sizeof(aInfo));
	}

TBool CPheVideoDecoder::GetSnapshotL(TPictureData&, const TUncompressedVideoFormat&) { return EFalse; }
void CPheVideoDecoder::GetTimedSnapshotL(TPictureData*, const TUncompressedVideoFormat&, const TTimeIntervalMicroSeconds&) { User::Leave(KErrNotSupported); }
void CPheVideoDecoder::GetTimedSnapshotL(TPictureData*, const TUncompressedVideoFormat&, const TPictureId&) { User::Leave(KErrNotSupported); }
void CPheVideoDecoder::CancelTimedSnapshot() {}
void CPheVideoDecoder::GetSupportedSnapshotFormatsL(RArray<TUncompressedVideoFormat>&) {}

TUint CPheVideoDecoder::PreDecoderBufferBytes()
	{
	return iStream.Length() - iCursor;
	}

void CPheVideoDecoder::GetBitstreamCounters(CMMFDevVideoPlay::TBitstreamCounters& aCounters)
	{
	aCounters = iBitCounters;
	iBitCounters = CMMFDevVideoPlay::TBitstreamCounters();
	}

// ---------------------------------------------------------------------------------------------------------------
// Initialize: the VE channel, the arena, the decoder core

void CPheVideoDecoder::ConstructStateL()
	{
	if (iCodec == ENoCodec)
		User::Leave(KErrNotReady);
	User::LeaveIfError(iVe.Open());
	TPheVdeInfo info;
	User::LeaveIfError(iVe.GetInfo(info));
	User::LeaveIfError(iVe.OpenArena(iArena));
	TInt size = KArenaMb << 20;
	if (size > info.iArenaMaxSize)
		size = info.iArenaMaxSize;
	TInt r = KErrNoMemory;
	for (; size >= (8 << 20); size >>= 1)		// physically contiguous RAM: take the largest that exists
		{
		r = iVe.Commit(0, size, iPhys);
		if (r == KErrNone)
			break;
		}
	User::LeaveIfError(r);
	iArenaSize = size;
	iJob = new (ELeave) TVeJobAdapter(iVe);
	iPool = new (ELeave) CPheVePool(iArena, iPhys, iArenaSize, KSliceBuf);	// offset 0 = the coded-data buffer
	if (iCodec == EH264)
		{
		iSh = new (ELeave) h264::TSliceHeader;
		iH264Parser = new (ELeave) h264::TParser;
		iH264 = new (ELeave) h264::TVeH264(*iH264Parser, *iJob, *iPool);
		iH264->iOnPicture = OnH264PictureThunk;
		iH264->iCtx = this;
		iPool->iBusy = BusyThunk;
		iPool->iBusyCtx = this;
		}
	else
		{
		iM2Parser = new (ELeave) mpeg2::TParser(NULL, 0);
		iM2 = new (ELeave) mpeg2::TVeMpeg2(*iJob, *iPool, 8);
		iM2->iHeld = FrameHeldThunk;
		iM2->iHeldCtx = this;
		iMaxHeld = 6;							// 8 frame buffers: 2 references + the client's pictures + 1 to decode into
		}
	iStream.CreateL(64 * 1024);
	}

void CPheVideoDecoder::Initialize()
	{
	TRAPD(err, ConstructStateL());
	iInitialised = err == KErrNone;
	iProxy->MdvppInitializeComplete(this, err);
	}

// ---------------------------------------------------------------------------------------------------------------
// input

TUint CPheVideoDecoder::NumFreeBuffers()
	{
	TUint n = 0;
	for (TInt i = 0; i < iNumIn; ++i)
		n += iIn[i].iBusy ? 0 : 1;
	return n + (iNumIn < iMinBuffers ? iMinBuffers - iNumIn : 0);
	}

TVideoInputBuffer* CPheVideoDecoder::GetBufferL(TUint aBufferSize)
	{
	if (aBufferSize > (TUint)iMaxInputSize && aBufferSize > (TUint)(4 << 20))
		User::Leave(KErrTooBig);
	if (PreDecoderBufferBytes() > (TUint)KPreDecoderMax)
		{
		iWantBuffers = ETrue;					// flow control: the decoder is far behind; told when it caught up
		return NULL;
		}
	TInBuf* b = NULL;
	for (TInt i = 0; i < iNumIn && !b; ++i)
		if (!iIn[i].iBusy)
			b = &iIn[i];
	if (!b && iNumIn < iMinBuffers)
		b = &iIn[iNumIn++];
	if (!b)
		{
		iWantBuffers = ETrue;				// the client is told (MdvppNewBuffers) when one is free again
		return NULL;
		}
	if (!b->iData || (TUint)b->iData->Des().MaxLength() < aBufferSize)
		{
		RDebug::Printf("PH: dvp input buffer %d: %u B (asked %u, max input %d, backlog %u)", (TInt)(b - iIn), (aBufferSize > (TUint)iMaxInputSize ? aBufferSize : (TUint)iMaxInputSize), aBufferSize, iMaxInputSize, PreDecoderBufferBytes());
		HBufC8* d = HBufC8::NewL((aBufferSize > (TUint)iMaxInputSize ? aBufferSize : (TUint)iMaxInputSize));
		delete b->iData;
		b->iData = d;
		}
	b->iBuf = TVideoInputBuffer();
	TPtr8 p = b->iData->Des();
	p.SetLength(0);
	b->iBuf.iData.Set(p);
	b->iBuf.iUser = b;
	b->iBusy = ETrue;
	return &b->iBuf;
	}

void CPheVideoDecoder::WriteCodedDataL(TVideoInputBuffer* aBuffer)
	{
	if (!iInitialised || iFailed)
		User::Leave(KErrNotReady);
	if (!aBuffer || !aBuffer->iUser)
		User::Leave(KErrArgument);
	if (iFinal)
		User::Leave(KErrNotReady);
	TInBuf* b = (TInBuf*)aBuffer->iUser;
	PHTRACE(("PH: dvp.pl write b=%x len=%d stream=%d/%d", (TUint)b, aBuffer->iData.Length(), iStream.Length(), iStream.MaxLength()));
	const TInt add = aBuffer->iData.Length();
	if (add > 0)
		{
		if (iStream.Length() + add > iStream.MaxLength())
			iStream.ReAllocL(Max(iStream.Length() + add, iStream.MaxLength() * 2));
		TChunk c;
		c.iAbs = iStreamAbs + iStream.Length();
		c.iPts = (aBuffer->iOptions & TVideoInputBuffer::EPresentationTimestamp) ? aBuffer->iPresentationTimestamp.Int64()
			: (aBuffer->iOptions & TVideoInputBuffer::EDecodingTimestamp) ? aBuffer->iDecodingTimestamp.Int64() : -1;
		if (iChunks.Count() == 0 || iChunks[iChunks.Count() - 1].iPts != c.iPts || c.iPts >= 0)
			User::LeaveIfError(iChunks.Append(c));
		iStream.Append(aBuffer->iData);
		++iBitCounters.iTotalPackets;
		}
	b->iBusy = EFalse;
	Schedule();
	}

void CPheVideoDecoder::InputEnd()
	{
	iFinal = ETrue;
	Schedule();
	}

TInt64 CPheVideoDecoder::PtsAt(TInt64 aAbs) const
	{
	TInt64 pts = -1;
	for (TInt i = 0; i < iChunks.Count() && iChunks[i].iAbs <= aAbs; ++i)
		pts = iChunks[i].iPts;
	return pts;
	}

// forget chunks wholly behind the cursor (keep the one containing it)
void CPheVideoDecoder::PruneChunks()
	{
	const TInt64 cur = iStreamAbs + iCursor;
	TInt keep = 0;
	while (keep + 1 < iChunks.Count() && iChunks[keep + 1].iAbs <= cur)
		++keep;
	while (keep-- > 0)
		iChunks.Remove(0);
	}

void CPheVideoDecoder::Compact()
	{
	if (iCursor >= 256 * 1024 || (iCursor > 0 && iCursor == iStream.Length()))
		{
		iStream.Delete(0, iCursor);
		iStreamAbs += iCursor;
		iCursor = 0;
		PruneChunks();
		}
	}

// ---------------------------------------------------------------------------------------------------------------
// output

CPheVideoDecoder::TOutPic* CPheVideoDecoder::NewOut()
	{
	for (TInt i = 0; i < KMaxOut; ++i)
		if (!iOut[i].iState)
			{
			TOutPic* o = &iOut[i];
			Mem::FillZ(&o->iPic, sizeof(o->iPic));
			o->iState = 1;
			o->iFrame = -1;
			o->iBus = 0;
			return o;
			}
	return NULL;
	}

TInt CPheVideoDecoder::HeldCount() const
	{
	TInt n = 0;
	for (TInt i = 0; i < KMaxOut; ++i)
		n += iOut[i].iState ? 1 : 0;
	return n;
	}

bool CPheVideoDecoder::BusyThunk(void* aCtx, uint32_t aBus)
	{
	CPheVideoDecoder* d = (CPheVideoDecoder*)aCtx;
	return d->iH264 && d->iH264->InUse(aBus);
	}

bool CPheVideoDecoder::FrameHeldThunk(void* aCtx, int aIndex)
	{
	CPheVideoDecoder* d = (CPheVideoDecoder*)aCtx;
	for (TInt i = 0; i < KMaxOut; ++i)
		if (d->iOut[i].iState && d->iOut[i].iFrame == aIndex)
			return true;
	return false;
	}


void CPheVideoDecoder::Deliver(TOutPic* aOut)
	{
	aOut->iState = 2;
	TVideoPicture& p = aOut->iPic;
	p.iOptions |= TVideoPicture::ETimestamp;
	if (aOut->iPts >= 0)
		{
		p.iTimestamp = TTimeIntervalMicroSeconds(aOut->iPts);
		iSynthPts = aOut->iPts;
		iHavePts = ETrue;
		}
	else
		{
		iSynthPts += 40000;							// no input timestamps: 25 fps in display order
		p.iTimestamp = TTimeIntervalMicroSeconds(iSynthPts);
		}
	iDecodePos = p.iTimestamp;
	++iPicCounters.iPicturesDecoded;
	++iPicCounters.iPicturesDisplayed;
	++iPicCounters.iTotalPictures;
	PHTRACE(("PH: dvp.pl deliver poc %d ts %d bus %x", aOut->iPoc, (TInt)aOut->iPts, aOut->iBus));
	iProxy->MdvppNewPicture(&p);		// (the client may return the picture from inside this call)
	}

void CPheVideoDecoder::ReturnPicture(TVideoPicture* aPicture)
	{
	TOutPic* o = aPicture ? (TOutPic*)aPicture->iUser : NULL;
	PHTRACE(("PH: dvp.pl return %x", (TUint)o));
	if (!o || o < &iOut[0] || o >= &iOut[KMaxOut] || o->iState != 2)
		return;
	if (o->iBus && iPool)
		iPool->Hold(o->iBus, EFalse);
	o->iState = 0;
	o->iFrame = -1;
	if (iBlocked)
		{
		iBlocked = EFalse;
		Schedule();
		}
	}

void CPheVideoDecoder::OnH264PictureThunk(void* aCtx, const h264::TPicture& aPic)
	{
	((CPheVideoDecoder*)aCtx)->OnH264Picture(aPic);
	}

// A picture was decoded (decode order). Queue it, and bump (deliver) in display order: (IDR period, POC).
void CPheVideoDecoder::OnH264Picture(const h264::TPicture& aPic)
	{
	TOutPic* o = NewOut();
	PHTRACE(("PH: dvp.pl picture decoded poc %d period %d out %x", aPic.iPoc, aPic.iIdrPeriod, (TUint)o));
	if (!o)
		{
		Fatal(KErrOverflow);
		return;
		}
	const TInt luma = iH264->iStride * iH264->iAlignedHeight;
	o->iBus = aPic.iLumaBus;
	o->iPeriod = aPic.iIdrPeriod;
	o->iPoc = aPic.iPoc;
	o->iPts = iCurPts;
	o->iPtr.Set(iPool->Cpu(aPic.iLumaBus), luma * 3 / 2, luma * 3 / 2);
	iPool->Hold(aPic.iLumaBus, ETrue);
	TVideoPicture& p = o->iPic;
	p.iData.iDataFormat = EYuvRawData;
	p.iData.iDataSize = TSize(iH264->iStride, iH264->iAlignedHeight);
	p.iData.iRawData = &o->iPtr;
	p.iOptions = TVideoPicture::ECropRect;
	p.iCropRect = iCurCrop;
	p.iUser = o;
	iLastPeriod = aPic.iIdrPeriod;
	DrainH264(EFalse);
	}

// deliver the smallest queued picture while the queue is deeper than the reorder depth (or older than the newest IDR
// period); aAll = flush everything
void CPheVideoDecoder::DrainH264(TBool aAll)
	{
	for (;;)
		{
		TOutPic* best = NULL;
		TInt queued = 0;
		for (TInt i = 0; i < KMaxOut; ++i)
			{
			TOutPic* o = &iOut[i];
			if (o->iState != 1)
				continue;
			++queued;
			if (!best || o->iPeriod < best->iPeriod || (o->iPeriod == best->iPeriod && o->iPoc < best->iPoc))
				best = o;
			}
		if (!best || !(aAll || queued > iDepth || best->iPeriod < iLastPeriod))
			return;
		Deliver(best);
		}
	}


void CPheVideoDecoder::Fatal(TInt aError)
	{
	if (iFailed)
		return;
	iFailed = ETrue;
	iProxy->MdvppFatalError(this, aError);
	}

// ---------------------------------------------------------------------------------------------------------------
// decoding

void CPheVideoDecoder::Schedule()
	{
	if (iStep && iInitialised && !iFailed)
		iStep->Go();
	}

void CPheVideoDecoder::Step()
	{
	PHTRACE(("PH: dvp.pl step len=%d cur=%d final=%d held=%d", iStream.Length(), iCursor, iFinal, HeldCount()));
	if (iFailed || !iInitialised)
		return;
	if (iWantBuffers && NumFreeBuffers() > 0 && PreDecoderBufferBytes() < (TUint)(KPreDecoderMax / 2))
		{
		iWantBuffers = EFalse;
		iProxy->MdvppNewBuffers();			// a buffer is available again (the client's GetBufferL returned NULL)
		if (iFailed)
			return;
		}
	if (iPaused)
		return;
	if (HeldCount() >= iMaxHeld && !(iFinal && iStream.Length() == iCursor))
		{
		iBlocked = ETrue;				// the client holds the pictures: ReturnPicture() resumes
		return;
		}
	const TBool more = iCodec == EH264 ? StepH264() : StepMpeg2();
	Compact();
	if (iFailed)
		return;
	if (more)
		Schedule();
	else if (iFinal && !iEndSent && !iBlocked)
		FinishStream();
	}

void CPheVideoDecoder::FinishStream()
	{
	iEndSent = ETrue;
	if (iCodec == EH264)
		{
		iH264->Flush();
		DrainH264(ETrue);
		}
	else if (iM2Pending)
		{
		TOutPic* p = iM2Pending;
		iM2Pending = NULL;
		Deliver(p);
		}
	iProxy->MdvppStreamEnd();
	}

// One NAL of the byte stream. Returns ETrue when it consumed something (call again).
TBool CPheVideoDecoder::StepH264()
	{
	const TUint8* buf = iStream.Ptr();
	const size_t len = iStream.Length();
	size_t pos = iCursor, s = 0, e = 0;
	const size_t before = pos;
	if (!h264::NextNal(buf, len, pos, s, e))
		{
		if (pos != before)
			{
			iCursor = pos;			// start codes with nothing in between
			return ETrue;
			}
		return EFalse;
		}
	const TBool terminated = pos + 3 <= len;
	if (!terminated && !iComplete && !iFinal)
		return EFalse;				// the NAL may continue in the next buffer
	const TUint8* nal = buf + s;
	const size_t nl = e - s;
	const TInt type = nal[0] & 0x1F;
	PHTRACE(("PH: dvp.pl nal type %d len %d at %d term %d", type, (TInt)nl, (TInt)s, terminated));
	const TInt64 pts = PtsAt(iStreamAbs + (TInt64)s);
	TInt rr = 0;
	if (type == h264::ENalSps)
		rr = iH264Parser->ParseSps(nal, nl) ? 0 : -100;
	else if (type == h264::ENalPps)
		rr = iH264Parser->ParsePps(nal, nl) ? 0 : -101;
	else if (type == h264::ENalSlice || type == h264::ENalIdr)
		{
		// pre-read what the picture's first slice says about display geometry and reordering; it becomes current when
		// the PREVIOUS picture has been reported (that happens inside DecodeSlice)
		TRect crop = iCurCrop;
		TInt depth = iDepth;
		TBool newPicture = EFalse;
		h264::TSliceHeader& sh = *iSh;
		if (iH264Parser->ParseSliceHeader(nal, nl, sh) && sh.iFirstMbInSlice == 0)
			{
			newPicture = ETrue;
			const h264::TPps* pps = iH264Parser->Pps(sh.iPpsId);
			const h264::TSps* sps = pps ? iH264Parser->Sps(pps->iSpsId) : NULL;
			if (sps)
				{
				const TUint32 ux = 2, uy = 2 * (2 - (sps->iFrameMbsOnly ? 1 : 0));
				crop = TRect(0, 0, sps->Width(), sps->Height());
				if (sps->iFrameCropping)
					crop = TRect(sps->iCropLeft * ux, sps->iCropTop * uy, sps->Width() - sps->iCropRight * ux,
						sps->Height() - sps->iCropBottom * uy);
				// the DPB size from the level (A.3.1 Table A-1, MaxDpbMbs): output is bumped only when it would be full
				static const TUint16 KLevels[][2] = { { 9, 396 }, { 10, 396 }, { 11, 900 }, { 12, 2376 }, { 13, 2376 },
					{ 20, 2376 }, { 21, 4752 }, { 22, 8100 }, { 30, 8100 }, { 31, 18000 }, { 32, 20480 }, { 40, 32768 },
					{ 41, 32768 }, { 42, 34816 }, { 50, 110400 }, { 51, 184320 }, { 52, 184320 } };
				TUint maxDpbMbs = 184320;
				for (TUint k = 0; k < sizeof(KLevels) / sizeof(KLevels[0]); ++k)
					if (KLevels[k][0] == sps->iLevelIdc)
						maxDpbMbs = KLevels[k][1];
				const TUint mbs = sps->iPicWidthInMbs * sps->FrameHeightInMbs();
				depth = mbs ? maxDpbMbs / mbs : 1;
				if (depth < (TInt)sps->iMaxNumRefFrames) depth = (TInt)sps->iMaxNumRefFrames;
				depth = Max(1, Min(16, depth));
				}
			}
		if (nl + 64 > (size_t)KSliceBuf)
			rr = -102;
		else
			{
			if (depth != iDepth)
				{
				iDepth = depth;
				iMaxHeld = depth + 4;
				}
			Mem::Copy(iArena.Base(), nal, nl);			// the coded-data buffer at arena offset 0 (16-byte aligned)
			Mem::FillZ(iArena.Base() + nl, 64);
			rr = iH264->DecodeSlice(nal, nl, iPhys - KDramBus);
			if (newPicture)								// pictures are reported when the next one starts
				{
				iCurCrop = crop;
				iCurPts = pts;
				}
			}
		}
	PHTRACE(("PH: dvp.pl nal done rr=%d", rr));
	if (rr < 0)
		{
		if (iErrors++ == 0)
			iFirstError = rr;
		++iBitCounters.iLostPackets;
		if (rr == -3)
			Fatal(KErrNoMemory);
		}
	iCursor = pos;
	return ETrue;
	}

// One picture of the MPEG-2 stream.
TBool CPheVideoDecoder::StepMpeg2()
	{
	iM2Parser->SetBuffer(iStream.Ptr(), iStream.Length(), iCursor);
	mpeg2::TParser saved(*iM2Parser);
	mpeg2::TPic p;
	if (!iM2Parser->Next(p))
		{
		*iM2Parser = saved;
		if (iFinal)
			iCursor = iStream.Length();			// trailing bytes without a picture
		return EFalse;
		}
	if (!p.iTerminated && !iComplete && !iFinal)
		{
		*iM2Parser = saved;						// the picture may continue in the next buffer
		return EFalse;
		}
	const TInt esLen = p.iSliceEnd - p.iSliceAt;
	if (esLen + 64 > KSliceBuf)
		{
		++iErrors;
		iCursor = p.iSliceEnd;
		return ETrue;
		}
	Mem::Copy(iArena.Base(), iStream.Ptr() + p.iSliceAt, esLen);
	Mem::FillZ(iArena.Base() + esLen, 64);
	TInt status = 0;
	const TInt f = iM2->Decode(p, iPhys - KDramBus, status);
	if (f == -4)
		{
		*iM2Parser = saved;						// every frame buffer is held by the client
		iBlocked = ETrue;
		return EFalse;
		}
	iCursor = p.iSliceEnd;
	if (f == -3)
		{
		Fatal(KErrNoMemory);
		return EFalse;
		}
	if (f < 0)
		{
		if (f != -2 && iErrors++ == 0)			// -2: leading B frames of an open GOP have no references: dropped
			iFirstError = f;
		++iBitCounters.iLostPackets;
		return ETrue;
		}
	if ((status & 7) != 1)
		{
		if (iErrors++ == 0)
			iFirstError = status;
		}
	TOutPic* o = NewOut();
	if (!o)
		{
		Fatal(KErrOverflow);
		return EFalse;
		}
	const TInt stride = iM2->Stride(), luma = iM2->LumaSize();
	o->iFrame = f;
	o->iPts = PtsAt(iStreamAbs + p.iPicAt);
	o->iPtr.Set(iPool->Cpu(iM2->FrameLumaBus(f)), luma * 3 / 2, luma * 3 / 2);
	TVideoPicture& vp = o->iPic;
	vp.iData.iDataFormat = EYuvRawData;
	vp.iData.iDataSize = TSize(stride, luma / stride);
	vp.iData.iRawData = &o->iPtr;
	vp.iOptions = TVideoPicture::ECropRect;
	vp.iCropRect = TRect(0, 0, p.iWidth, p.iHeight);
	vp.iUser = o;
	// display order: a reference (I / P) waits for the next reference, a B picture goes out at once
	if (p.iType == mpeg2::EPicB)
		Deliver(o);
	else
		{
		TOutPic* prev = iM2Pending;
		iM2Pending = o;
		if (prev)
			Deliver(prev);
		}
	return ETrue;
	}


// ---------------------------------------------------------------------------------------------------------------
// ECOM

const TImplementationProxy KImplementationTable[] =
	{
	IMPLEMENTATION_PROXY_ENTRY(KPheVdecImplUid, CPheVideoDecoder::NewL)
	};

EXPORT_C const TImplementationProxy* ImplementationGroupProxy(TInt& aTableCount)
	{
	aTableCount = sizeof(KImplementationTable) / sizeof(TImplementationProxy);
	return KImplementationTable;
	}
