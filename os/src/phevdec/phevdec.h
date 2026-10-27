/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// phevdec.h -- OURS (VDU rung V4): the DevVideo decoder hardware device for the Allwinner A64
// Video Engine. An ECOM plug-in on interface 0x101FB4BE (KUidDevVideoDecoderHwDevice) implementing
// CMMFVideoDecodeHwDevice, found and driven by CMMFDevVideoPlay (devvideo.dll) like any 2010 decoder.
//
// Formats (SetInputFormatL):
//   video/H264                    Annex B byte stream (00 00 01 / 00 00 00 01 start codes), SPS + PPS in band, frame
//                                 pictures, 4:2:0 8-bit (Baseline / Main / High, B frames, weighted prediction, 8x8, CQM)
//   video/mpeg, video/mpeg2, video/mp2v   MPEG-2 video elementary stream, frame pictures, 4:2:0
//   Data unit: EDuElementaryStream with
//     EDuArbitraryStreamSection   any chunking of the byte stream (a file read in blocks): a trailing NAL / picture is
//                                 held until the next start code arrives or InputEnd()
//     EDuCodedPicture             each buffer holds whole access unit(s) (one frame, SPS/PPS may precede it): decoded at
//                                 once, no waiting for the next start code
//   aDataInOrder must be ETrue (decoding order).
// Output: TVideoPicture, iData.iDataFormat = EYuvRawData, YUV 4:2:0 semi-planar (NV12: a Y plane, then interleaved U V at
//   the same pitch), iRawData = the picture in VE memory (an uncached chunk, owned by the device), iDataSize = (pitch,
//   16-aligned height), iCropRect = the displayed area (SPS cropping for H.264). Pictures are delivered in DISPLAY order
//   (H.264: bumped by (IDR period, picture order count) through a DPB-sized reorder queue; MPEG-2: references held back
//   until the next reference), timestamp = the presentation timestamp of the input buffer holding the picture's start
//   (when the input has none: a 25 fps counter). The client returns each picture with ReturnPicture().
//
// Threading: the device runs in the creating (client) thread; decoding is done one NAL / picture per active-object
// step (priority EPriorityLow), each step a synchronous VE job through RPheVde. Needs an active scheduler.
#ifndef PHEVDEC_H
#define PHEVDEC_H

#include <e32base.h>
#include <mmf/devvideo/videoplayhwdevice.h>
#ifdef PHE_SW
// PHE_SW (Path C x86, 2026-10-16): no Video Engine -- the same DevVideo decoder device, decoding in software through the
// separate LGPL pheffmpeg.dll (PheVid* ordinals). Everything VE-specific (rphevde / ve_h264 / ve_mpeg2 / the arena pool) is out.
#include "h264_parser.h"
#else
#include "rphevde.h"
#include "ve_rphevde.h"
#include "ve_mpeg2.h"
#endif

#ifdef PHE_MP4V
const TInt KPheVdecImplUid = 0xE70157FD;		// phevdecsw.dll (UID3 0xE70157FC): MPEG-4 Part 2 only, next to the VE decoder
#else
const TInt KPheVdecImplUid = 0xE70157C3;		// the decoder implementation (dll UID3 0xE70157C2)
#endif

#ifndef PHE_SW
// the VE arena: bump allocation with recycling of same-sized blocks that nobody holds
class CPheVePool : public h264::MVeMemory
	{
public:
	CPheVePool(RChunk& aArena, TUint32 aPhys, TInt aSize, TInt aReserve);
	uint32_t Alloc(uint32_t aSize, uint32_t aAlign, int& aIndex);
	void Hold(uint32_t aBus, TBool aHeld);
	TUint8* Cpu(TUint32 aBus) const { return iArena.Base() + (aBus + KDramBus - iPhys); }
	bool (*iBusy)(void* aCtx, uint32_t aBus);	// the decoder still needs the block; NULL = never recycle
	void* iBusyCtx;
private:
	struct TEntry
		{
		TUint32 iBus, iSize;
		TInt iIndex;
		TBool iHeld;
		};
	enum { KMaxEntries = 160 };
	RChunk& iArena;
	TUint32 iPhys;
	TInt iSize, iNext, iCount;
	TEntry iEntries[KMaxEntries];
	};
#endif

class CPheStep;

class CPheVideoDecoder : public CMMFVideoDecodeHwDevice
	{
public:
	static CMMFVideoDecodeHwDevice* NewL(TAny* aInitParams);
	~CPheVideoDecoder();

	// from CMMFVideoHwDevice
	TAny* CustomInterface(TUid aInterface);

	// from CMMFVideoPlayHwDevice
	CPostProcessorInfo* PostProcessorInfoLC();
	void GetOutputFormatListL(RArray<TUncompressedVideoFormat>& aFormats);
	void SetOutputFormatL(const TUncompressedVideoFormat& aFormat);
	void SetClockSource(MMMFClockSource* aClock);
	void SetVideoDestScreenL(TBool aScreen);
	void SetPostProcessTypesL(TUint32 aPostProcCombination);
	void SetInputCropOptionsL(const TRect& aRect);
	void SetYuvToRgbOptionsL(const TYuvToRgbOptions& aOptions, const TYuvFormat& aYuvFormat, TRgbFormat aRgbFormat);
	void SetYuvToRgbOptionsL(const TYuvToRgbOptions& aOptions);
	void SetRotateOptionsL(TRotationType aRotationType);
	void SetScaleOptionsL(const TSize& aTargetSize, TBool aAntiAliasFiltering);
	void SetOutputCropOptionsL(const TRect& aRect);
	void SetPostProcSpecificOptionsL(const TDesC8& aOptions);
	void Initialize();
	void CommitL();
	void Revert();
	void StartDirectScreenAccessL(const TRect& aVideoRect, CFbsScreenDevice& aScreenDevice, const TRegion& aClipRegion);
	void SetScreenClipRegion(const TRegion& aRegion);
	void SetPauseOnClipFail(TBool aPause);
	void AbortDirectScreenAccess();
	TBool IsPlaying();
	void Redraw();
	void Start();
	void Stop();
	void Pause();
	void Resume();
	void SetPosition(const TTimeIntervalMicroSeconds& aPlaybackPosition);
	void FreezePicture(const TTimeIntervalMicroSeconds& aTimestamp);
	void ReleaseFreeze(const TTimeIntervalMicroSeconds& aTimestamp);
	TTimeIntervalMicroSeconds PlaybackPosition();
	TUint PictureBufferBytes();
	void GetPictureCounters(CMMFDevVideoPlay::TPictureCounters& aCounters);
	void SetComplexityLevel(TUint aLevel);
	TUint NumComplexityLevels();
	void GetComplexityLevelInfo(TUint aLevel, CMMFDevVideoPlay::TComplexityLevelInfo& aInfo);
	void ReturnPicture(TVideoPicture* aPicture);
	TBool GetSnapshotL(TPictureData& aPictureData, const TUncompressedVideoFormat& aFormat);
	void GetTimedSnapshotL(TPictureData* aPictureData, const TUncompressedVideoFormat& aFormat,
		const TTimeIntervalMicroSeconds& aPresentationTimestamp);
	void GetTimedSnapshotL(TPictureData* aPictureData, const TUncompressedVideoFormat& aFormat,
		const TPictureId& aPictureId);
	void CancelTimedSnapshot();
	void GetSupportedSnapshotFormatsL(RArray<TUncompressedVideoFormat>& aFormats);
	void InputEnd();

	// from CMMFVideoDecodeHwDevice
	CVideoDecoderInfo* VideoDecoderInfoLC();
	TVideoPictureHeader* GetHeaderInformationL(TVideoDataUnitType aDataUnitType,
		TVideoDataUnitEncapsulation aEncapsulation, TVideoInputBuffer* aDataUnit);
	void ReturnHeader(TVideoPictureHeader* aHeader);
	void SetInputFormatL(const CCompressedVideoFormat& aFormat, TVideoDataUnitType aDataUnitType,
		TVideoDataUnitEncapsulation aEncapsulation, TBool aDataInOrder);
	void SynchronizeDecoding(TBool aSynchronize);
	void SetBufferOptionsL(const CMMFDevVideoPlay::TBufferOptions& aOptions);
	void GetBufferOptions(CMMFDevVideoPlay::TBufferOptions& aOptions);
	void SetHrdVbvSpec(THrdVbvSpecification aHrdVbvSpec, const TDesC8& aHrdVbvParams);
	void SetOutputDevice(CMMFVideoPostProcHwDevice* aDevice);
	TTimeIntervalMicroSeconds DecodingPosition();
	TUint PreDecoderBufferBytes();
	void GetBitstreamCounters(CMMFDevVideoPlay::TBitstreamCounters& aCounters);
	TUint NumFreeBuffers();
	TVideoInputBuffer* GetBufferL(TUint aBufferSize);
	void WriteCodedDataL(TVideoInputBuffer* aBuffer);
	void ConfigureDecoderL(const TVideoPictureHeader& aVideoPictureHeader);
	void SetProxy(MMMFDevVideoPlayProxy& aProxy);

	// the decode step (CPheStep::RunL)
	void Step();

private:
	enum TCodec { ENoCodec, EH264, EMpeg2, EMpeg4 };
	enum { KMaxInput = 12, KMaxOut = 40, KPreDecoderMax = 8 << 20, KSliceBuf = 1 << 20, KArenaMb = 64 };
	struct TInBuf
		{
		TVideoInputBuffer iBuf;
		HBufC8* iData;
		TBool iBusy;
		};
	struct TChunk
		{
		TInt64 iAbs;			// absolute stream offset of the chunk's first byte
		TInt64 iPts;			// microseconds, -1 = none
		};
	struct TOutPic
		{
		TVideoPicture iPic;
		TPtr8 iPtr;
		TInt iState;			// 0 free, 1 queued for reordering, 2 delivered to the client
		TUint8* iMem;			// PHE_SW: the NV12 picture (heap, owned)
		TInt iMemSize;
		TUint32 iBus;			// H.264: the luma bus address (the pool key)
		TInt iFrame;			// MPEG-2: the frame buffer index
		TUint32 iPeriod;
		TInt32 iPoc;
		TInt64 iPts;
		TOutPic() : iPtr(NULL, 0, 0), iMem(NULL), iMemSize(0) {}
		};

	CPheVideoDecoder();
	void ConstructStateL();
	void Schedule();
#ifdef PHE_SW
	TBool StepSw();
	void ReceiveSw();
#else
	TBool StepH264();
	TBool StepMpeg2();
#endif
	void FinishStream();
	TInt64 PtsAt(TInt64 aAbs) const;
	void PruneChunks();
	void Compact();
	TOutPic* NewOut();
	TInt HeldCount() const;
	void Deliver(TOutPic* aOut);
#ifndef PHE_SW
	void DrainH264(TBool aAll);
	void OnH264Picture(const h264::TPicture& aPic);
	static void OnH264PictureThunk(void* aCtx, const h264::TPicture& aPic);
	static bool BusyThunk(void* aCtx, uint32_t aBus);
	static bool FrameHeldThunk(void* aCtx, int aIndex);
#endif
	void Fatal(TInt aError);
	void SetupFormat(TUncompressedVideoFormat& aFormat) const;

	MMMFDevVideoPlayProxy* iProxy;
	CPheStep* iStep;
	TCodec iCodec;
	TBool iComplete;				// EDuCodedPicture: each buffer holds complete units
	TBool iInitialised, iFailed, iFinal, iEndSent, iPaused, iPlaying, iBlocked, iWantBuffers;
#ifdef PHE_SW
	// the software decoder (pheffmpeg.dll, loaded on Initialize)
	RLibrary iFf;
	void* iVid;
	TBool iDrainSent;
	TBool iPreallocDone;
	void* (*iFfOpen)(int, int);
	int (*iFfSend)(void*, const unsigned char*, int, long long);
	int (*iFfRecv)(void*, unsigned char*, int, int, int*, int*, long long*);
	void (*iFfClose)(void*);
#else
	// the VE
	RPheVde iVe;
	RChunk iArena;
	TUint32 iPhys;
	TInt iArenaSize;
	TVeJobAdapter* iJob;
	CPheVePool* iPool;
	h264::TParser* iH264Parser;
	h264::TVeH264* iH264;
	h264::TSliceHeader* iSh;			// scratch (several KB: not on the client thread's small stack)
	mpeg2::TParser* iM2Parser;
	mpeg2::TVeMpeg2* iM2;
#endif
	// input
	TInBuf iIn[KMaxInput];
	TInt iNumIn;
	RBuf8 iStream;
	TInt64 iStreamAbs;
	TInt iCursor;
	RArray<TChunk> iChunks;
	TInt iMinBuffers, iMaxInputSize;
	// output
	TOutPic iOut[KMaxOut];
	TInt iMaxHeld, iDepth;
	TInt64 iCurPts, iSynthPts;
	TBool iHavePts;
	TRect iCurCrop;
	TInt iPicSeq;
	TOutPic* iM2Pending;
	TUint32 iLastPeriod;
	// statistics
	CMMFDevVideoPlay::TPictureCounters iPicCounters;
	CMMFDevVideoPlay::TBitstreamCounters iBitCounters;
	TInt iErrors, iFirstError, iTrace;
	TTimeIntervalMicroSeconds iDecodePos;
	TVideoPictureHeader iHeader;
	CMMFDevVideoPlay::TBufferOptions iBufferOptions;
	};

class CPheStep : public CActive
	{
public:
	CPheStep(CPheVideoDecoder& aOwner) : CActive(EPriorityLow), iOwner(aOwner) { CActiveScheduler::Add(this); }
	~CPheStep() { Cancel(); }
	void Go()
		{
		if (!IsActive())
			{
			iStatus = KRequestPending;
			SetActive();
			TRequestStatus* s = &iStatus;
			User::RequestComplete(s, KErrNone);
			}
		}
private:
	void RunL() { iOwner.Step(); }
	void DoCancel() {}
	CPheVideoDecoder& iOwner;
	};

#endif
