// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// phlink.cpp -- OURS (PinePhone USB step 2): the Phorepoc debug link -- a
// KERNEL-SIDE USB function on top of the real USB client PIL (no LDD, no user side): a CDC ACM serial port, which
// Windows 10+ binds to its in-box usbser.sys (a COM port, no driver to install), Linux to cdc_acm (/dev/ttyACM0).
//  * the kernel log (the seam's board log -- the same bytes the SD log holds, from the first line of the boot)
//    streams out on the bulk IN endpoint;
//  * what the PC types comes in on bulk OUT: 'R' = reboot now (the A64 watchdog), anything else is echoed into the
//    log ("phlink: rx ...").
// Kernel-side on purpose: the link must work while there is no user side yet, or a broken one.
//
// The function = two interfaces, registered through the PIL's LDD-facing API (SetInterface, SetCSInterface-
// DescriptorBlock, SetupRead/WriteBuffer) with two DBase "clients" as the LDD would: interface 0 = Communications
// (ACM, one interrupt IN endpoint, never written) + the CDC functional descriptors, interface 1 = Data (bulk OUT +
// bulk IN). The ACM class requests reach the comm client through its endpoint-0 read: SET_LINE_CODING and
// SET_CONTROL_LINE_STATE are accepted, GET_LINE_CODING answers 115200 8N1, the rest are stalled.

#include <drivers/usbc.h>
#include <kernel/kern_priv.h>
#include <e32keys.h>
#include "syborg_priv.h"
#include "phlz4.h"
extern "C" void A64CpuInputBoost();	// power/a64_cpufreq.cpp: input -> the top allowed CPU clock for 1 s

extern "C" void ekseam_board_log(unsigned c);
extern "C" const char* ekseam_board_log_buf(unsigned* aLen);
extern "C" TInt A64SlotUploadBegin(TUint aLen, TUint aSum);
extern "C" TInt A64SlotUploadData(const TUint8* aData, TInt aLen);
extern "C" TInt A64SlotUploadEnd(TUint* aSum);
extern "C" void A64UsbLinkUp();
// the delta upload (a64_glue.cpp "DELTA UPLOAD"): 'Y' block hashes, 'X' patch a run, 'Z' commit
extern "C" TInt A64SlotHashBlocks(TUint aLen, TUint32* aOut, TInt aMax);
extern "C" TInt A64SlotPatchBegin(TUint aOff, TUint aLen);
extern "C" TInt A64SlotPatchEnd();
extern "C" TInt A64SlotCommit(TUint aLen, TUint32 aHash, TUint32* aGot);
extern "C" void A64WatchdogReboot();
extern "C" TInt A64SlotRearm();
extern "C" TInt A64DisplayCaptureSource(TInt aWhich, TLinAddr* aLin, TInt* aW, TInt* aH);
extern "C" void A64BarsProgress(const char* aLabel, TUint64 aDone, TUint64 aTotal);	// a64_bars.cpp: the top bar
extern "C" void A64BarsProgressEnd();
extern "C" void A64BarsNotify(TInt aLevel, const char* aText);
extern "C" TBool A64BarsPanelTouch(TInt aX, TInt aY, TInt aAction);
extern "C" void A64BarsMenuKey();

// Protocol v2 (PhoneLab, tools/phorepoc-arm/phonelab): bulk data goes out as binary frames between log chunks --
// 16-byte header {A5 5A 'P' 'F', type, 0, seq LE16, len LE32, byte sum of the payload LE32} + payload. The log is
// text, so the magic never occurs in it. Types: 1 = screenshot begin {id, w | h << 16, stride, format 1 = XRGB8888},
// 2 = screenshot rows {first row LE16, rows LE16, RLE}, 3 = screenshot end {id, rows}. RLE over 24-bit RGB pixels
// (3 bytes R G B): a token t < 0x80 = t + 1 literal pixels follow; t >= 0x80 = the next pixel repeated t - 0x80 + 2
// times.
// Protocol v3 adds the LIVE VIEW ('L' which flags): changed 32x32 tiles only, each LZ4
// block compressed. 8 = begin {id, w | h << 16, tile size, flags (bit 0 = keyframe: every tile)}, 9 = tiles {count LE16,
// 0 LE16, then count x {tx u8, ty u8, clen LE16 (bit 15 = stored raw), data}} (tile = w x h 24-bit RGB R G B, rows
// packed; edge tiles are clipped), 10 = end {id, tiles sent, 0}. The shot ops above are unchanged.
const TInt KProtoVersion = 3;
const TInt KFrameMax = 4096;
enum { EFrameShotBegin = 1, EFrameShotRows = 2, EFrameShotEnd = 3, EFrameLiveBegin = 8, EFrameLiveTiles = 9, EFrameLiveEnd = 10, EFrameBlobData = 6, EFrameBlobEnd = 7 };
// Blobs (type 6 = the next bytes, 7 = end {kind, total length}): 'Q' = a profile snapshot (kind 1, a64_glue.cpp
// A64ProfileSnapshot: text lines)
extern "C" TInt A64ProfileSnapshot(TUint8* aBuf, TInt aMax);
extern "C" void A64HotStart();
extern "C" void A64ThreadDump();						// 'D' (a64_glue.cpp): the thread dump into the log
extern "C" TInt A64HotStop(TUint8* aBuf, TInt aMax);	// 'H' 0: the hotspots histogram (kind 2)
// 'F' = the file service (PhoneLab Project panel): the request goes to the user-side helper through the PhFs.A64 LDD
// (phfs_ldd.cpp; contract phfs_api.h), its answer streams back as blob kind 4 {status LE32, data}
extern "C" TInt PhFsSubmit(TInt aOp, const TUint8* aData, TInt aLen);
extern "C" void PhFsInstall();
const TInt KFsMaxRequest = 4096 + 512;
const TInt KBlobMax = 256 * 1024;
static TUint8 BlobBuf[KBlobMax];
static TUint8 FsBuf[KFsMaxRequest];
// the OS picture is copied here when a capture starts (r34): the rows are streamed over ~160 ms, and the scan-out buffer is
// recycled by the compositor meanwhile (a seam in animated content). 368 words = the 64-byte-aligned 360x640 pitch.
static TUint32 ShotSnap[368 * 640];

// live view state (one link, one viewer): the hash of each tile as last SENT (and acked by the next request), the
// composed 32-row band being tiled, and the tile coder's scratch
const TInt KLiveTile = 32;
const TInt KLiveMaxW = 720, KLiveMaxH = 1440;
const TInt KLiveMaxTiles = ((KLiveMaxW + KLiveTile - 1) / KLiveTile) * ((KLiveMaxH + KLiveTile - 1) / KLiveTile);
static TUint32 LiveRef[KLiveMaxTiles];		// committed when the frame carrying the tile went out
static TUint32 LiveNew[KLiveMaxTiles];
static TUint32 LiveBand[KLiveTile * KLiveMaxW];
static TUint8 LiveRaw[KLiveTile * KLiveTile * 3];
static TUint8 LiveComp[PHLZ4_BOUND(KLiveTile * KLiveTile * 3)];
static TPhLz4Table LiveTable;
static TBool LiveRefOk;						// LiveRef is a complete picture of what the PC holds
static TInt LiveWhich = -1, LiveW, LiveH;	// ... of this source

static void PTr(const char* aText)
	{
	while (*aText)
		ekseam_board_log((unsigned char)*aText++);
	}
static void PTrHex(TUint32 aValue)
	{
	for (TInt i = 7; i >= 0; --i)
		ekseam_board_log("0123456789abcdef"[(aValue >> (i * 4)) & 0xF]);
	}

class DPhClient : public DBase {};

class DPhLink : public DBase
	{
public:
	DPhLink();
	TInt Start();
private:
	static void Ep0ReadDfc(TAny* aPtr);
	static void Ep0WriteDfc(TAny* aPtr);
	static void TxDfc(TAny* aPtr);
	static void RxDfc(TAny* aPtr);
	static void PollTimerFn(TAny* aPtr);
	static void PollDfcFn(TAny* aPtr);
	void PostEp0Read();
	void HandleSetup(const TUint8* aSetup);
	void Kick();
	void Reboot();
	void PeekPoke(TUint32 aAddr, TUint32 aArg);
	void Command(TUint32 aA, TUint32 aB);
	void ShotStart(TUint32 aWhich);
	TBool ShotFrame();
	TBool LiveFrame();
	void LiveSent();
	void LiveStart(TUint32 aWhich, TUint32 aFlags);
	TBool ShotSource(TUint32 aWhich);
	void Seal(TUint8 aType, TInt aN);
	TBool BlobFrame();
	TBool NextFrame() { iTxIsBlob = !iShotStage; return iShotStage ? (iLive ? LiveFrame() : ShotFrame()) : BlobFrame(); }
	void Received(const TUint8* aData, TInt aLen);
	void FsDispatch();
	void FsFail(TInt aError);
	static void FsDfcFn(TAny* aPtr);
public:
	TBool FsReplyBuf(TUint8*& aBuf, TInt& aMax);
	void FsReplyDone(TInt aLen);
private:
	enum TUpState { EUpIdle, EUpHeader, EUpData, EUpFs };
private:
	TUpState iUpState;
	TUint8 iUpHdr[8];
	TUint8 iCmd;
	TInt iUpHdrFill;
	TUint iUpLen, iUpLeft;
	TBool iUpOk;
	TBool iUpPatch;						// the data being received is an 'X' run (delta), not a 'U' image
	TUint iUpOff;
	TInt iFsOp, iFsLen, iFsFill;		// 'F': the request being received (FsBuf)
	TBool iFsOver;						// ... and it was too big: drained, answered KErrTooBig
	TDfc iFsDfc;						// a reply is staged in BlobBuf: start sending it
	TUint iDeltaTotal, iDeltaDone;		// 'G': the bytes the whole delta will send (one progress bar for all runs)
	DUsbClientController* iCtrl;
	DThread* iThread;
	DPhClient iComm;
	DPhClient iData;
	TInt iRealEp[2][6];
	TUsbcRequestCallback* iEp0Read;
	TUsbcRequestCallback* iEp0Write;
	TUsbcRequestCallback* iTx;
	TUsbcRequestCallback* iRx;
	TUint8 iEp0Buf[64];
	TUsbcPacketArray iEp0Index[2], iEp0Size[2];
	TUint8 iEp0TxBuf[8];
	TBool iEp0ExpectData;
	TUint8 iLineCoding[7];
	TUint8 iRxBuf[512] __attribute__((aligned(4)));			// one bulk OUT packet: 64 B at full speed, 512 B at high speed
	TUsbcPacketArray iRxIndex[2], iRxSize[2];
	TBool iTxBusy, iRxBusy;
	TUint iSent;
	TInt iTxLen;
	NTimer iPollTimer;
	TDfc iPollDfc;
	// screenshot streaming ('S'): one frame at a time, alternating with the log
	TBool iTxIsLog;
	TInt iShotStage;		// 0 = idle, 1 = begin frame next, 2 = rows, 3 = end frame next
	TUint32 iShotId;
	TInt iShotW, iShotH, iShotRow;
	TInt iShotNextStage, iShotNextRow;	// applied when the frame in flight is sent (a failed one is rebuilt)
	const TUint32* iShotPix;
	const TUint32* iShotPanel;			// which 2: the panel layer composited over iShotPix (the OS picture)
	TInt ShotPitch() const { return (iShotW == 360 && iShotH == 640) ? 368 : iShotW; }	// pixels per row of the OS picture (the screen buffer pitch is 64-byte aligned: 1472 B, svpfb_patch.py)
	TUint32 iShotRowBuf[360];
	// live view ('L'): iShotStage / iShotRow (= the next TILE) are shared with the shot machinery
	TBool iLive, iLiveKey;
	TInt iLiveWhich, iLiveNtx, iLiveTiles, iLiveBandTy, iLiveLo, iLiveHi, iLiveSentTiles;
	// blob streaming ('Q')
	TBool iTxIsBlob;
	TInt iBlobStage, iBlobNextStage;	// 0 idle, 1 data frames, 2 end frame
	TInt iBlobLen, iBlobOff, iBlobNextOff;
	TUint32 iBlobKind;
	TUint16 iFrameSeq;
	TUint8 iFrame[KFrameMax];
	};

DPhLink::DPhLink()
	: iUpState(EUpIdle), iCmd(0), iUpHdrFill(0), iUpLen(0), iUpLeft(0), iUpOk(EFalse), iUpPatch(EFalse), iUpOff(0), iFsOp(0), iFsLen(0), iFsFill(0), iFsOver(EFalse), iFsDfc(FsDfcFn, this, 1), iDeltaTotal(0), iDeltaDone(0), iCtrl(NULL), iThread(NULL), iEp0Read(NULL), iEp0Write(NULL), iTx(NULL), iRx(NULL), iEp0ExpectData(EFalse),
	  iTxBusy(EFalse), iRxBusy(EFalse), iSent(0), iTxLen(0), iPollTimer(PollTimerFn, this),
	  iPollDfc(PollDfcFn, this, 1), iTxIsLog(EFalse), iShotStage(0), iShotId(0), iShotW(0), iShotH(0), iShotRow(0),
	  iShotNextStage(0), iShotNextRow(0), iShotPix(NULL), iShotPanel(NULL), iLive(EFalse), iLiveKey(EFalse), iLiveWhich(0), iLiveNtx(0), iLiveTiles(0), iLiveBandTy(-1), iLiveLo(0), iLiveHi(0), iLiveSentTiles(0), iFrameSeq(0), iTxIsBlob(EFalse), iBlobStage(0), iBlobNextStage(0), iBlobLen(0), iBlobOff(0), iBlobNextOff(0), iBlobKind(0)
	{
	static const TUint8 KDefaultCoding[7] = {0x00, 0xC2, 0x01, 0x00, 0, 0, 8};	// 115200, 1 stop, no parity, 8
	memcpy(iLineCoding, KDefaultCoding, sizeof(iLineCoding));
	for (TInt i = 0; i < 2; ++i)
		for (TInt j = 0; j < 6; ++j)
			iRealEp[i][j] = -1;
	}

TInt DPhLink::Start()
	{
	iCtrl = DUsbClientController::UsbcControllerPointer(0);
	if (!iCtrl)
		{
		PTr("phlink: no USB device controller\n");
		return KErrNotFound;
		}
	iThread = &Kern::CurrentThread();
	TDfcQue* q = iCtrl->DfcQ(0);
	iPollDfc.SetDfcQ(q);
	iFsDfc.SetDfcQ(q);

	// interface 0: CDC Communications, ACM -- one interrupt IN endpoint (notifications; we never send any)
	TUsbcClassInfo commClass(0x02, 0x02, 0x01);
	TUsbcEndpointInfo commEp[1] = { TUsbcEndpointInfo(KUsbEpTypeInterrupt, KUsbEpDirIn, 16, 16) };
	TInt r = iCtrl->SetInterface(&iComm, iThread, 0, commClass, NULL, 1, commEp, &iRealEp[0], 0);
	if (r != KErrNone)
		{
		PTr("phlink: SetInterface(comm) r="); PTrHex((TUint32)r); PTr("\n");
		return r;
		}
	// CDC functional descriptors: Header 1.10, Call Management (no call mgmt, data ifc 1), ACM (line coding +
	// control line state), Union (master 0, slave 1)
	static const TUint8 KCdcDesc[19] =
		{
		0x05, 0x24, 0x00, 0x10, 0x01,
		0x05, 0x24, 0x01, 0x00, 0x01,
		0x04, 0x24, 0x02, 0x02,
		0x05, 0x24, 0x06, 0x00, 0x01
		};
	TBuf8<19> cdc;
	cdc.Copy(KCdcDesc, sizeof(KCdcDesc));
	r = iCtrl->SetCSInterfaceDescriptorBlock(iThread, &iComm, 0, cdc, cdc.Length());
	if (r != KErrNone)
		{
		PTr("phlink: SetCSInterfaceDescriptorBlock r="); PTrHex((TUint32)r); PTr("\n");
		return r;
		}

	// interface 1: CDC Data -- bulk OUT (logical 1) + bulk IN (logical 2)
	TUsbcClassInfo dataClass(0x0A, 0x00, 0x00);
	TUsbcEndpointInfo dataEp[2] =
		{
		TUsbcEndpointInfo(KUsbEpTypeBulk, KUsbEpDirOut, 64),
		TUsbcEndpointInfo(KUsbEpTypeBulk, KUsbEpDirIn, 64)
		};
	r = iCtrl->SetInterface(&iData, iThread, 0, dataClass, NULL, 2, dataEp, &iRealEp[1], 0);
	if (r != KErrNone)
		{
		PTr("phlink: SetInterface(data) r="); PTrHex((TUint32)r); PTr("\n");
		return r;
		}
	PTr("phlink: real eps comm="); PTrHex((TUint32)iRealEp[0][1]);
	PTr(" out="); PTrHex((TUint32)iRealEp[1][1]); PTr(" in="); PTrHex((TUint32)iRealEp[1][2]); PTr("\n");

	iEp0Read = new TUsbcRequestCallback(&iComm, 0, Ep0ReadDfc, this, q, 1);
	iEp0Write = new TUsbcRequestCallback(&iComm, 0, Ep0WriteDfc, this, q, 1);
	iTx = new TUsbcRequestCallback(&iData, 2, TxDfc, this, q, 1);
	iRx = new TUsbcRequestCallback(&iData, 1, RxDfc, this, q, 1);
	if (!iEp0Read || !iEp0Write || !iTx || !iRx)
		return KErrNoMemory;
	iEp0Read->iRealEpNum = 0;
	iEp0Write->iRealEpNum = 0;
	iTx->iRealEpNum = iRealEp[1][2];
	iRx->iRealEpNum = iRealEp[1][1];
	PostEp0Read();

	r = iCtrl->PowerUpUdc();
	if (r != KErrNone)
		{
		PTr("phlink: PowerUpUdc r="); PTrHex((TUint32)r); PTr("\n");
		return r;
		}
	r = iCtrl->UsbConnect();
	PTr("phlink: up, UsbConnect r="); PTrHex((TUint32)r); PTr("\n");
	iPollTimer.OneShot(NKern::TimerTicks(50));
	return KErrNone;
	}

void DPhLink::PostEp0Read()
	{
	iEp0Read->SetRxBufferInfo(iEp0Buf, 0, iEp0Index, iEp0Size, sizeof(iEp0Buf));
	iEp0Read->SetTransferDirection(EControllerRead);
	TInt r = iCtrl->SetupReadBuffer(*iEp0Read);
	if (r != KErrNone)
		{
		PTr("phlink: ep0 read r="); PTrHex((TUint32)r); PTr("\n");
		}
	}

void DPhLink::Ep0ReadDfc(TAny* aPtr)
	{
	DPhLink* self = static_cast<DPhLink*>(aPtr);
	TUsbcRequestCallback& rq = *self->iEp0Read;
	const TInt n = self->iEp0Size[0];
	if (rq.iError != KErrNone)
		{
		self->iEp0ExpectData = EFalse;
		}
	else if (self->iEp0ExpectData)
		{
		// the data stage of SET_LINE_CODING
		memcpy(self->iLineCoding, self->iEp0Buf, n < 7 ? n : 7);
		self->iEp0ExpectData = EFalse;
		self->iCtrl->SendEp0StatusPacket(&self->iComm);
		}
	else if (n >= 8)
		{
		self->HandleSetup(self->iEp0Buf);
		}
	self->PostEp0Read();
	}

void DPhLink::HandleSetup(const TUint8* aSetup)
	{
	const TUint8 req = aSetup[1];
	const TUint16 len = (TUint16)(aSetup[6] | (aSetup[7] << 8));
	switch (req)
		{
	case 0x20:									// SET_LINE_CODING: 7 bytes follow
		iEp0ExpectData = ETrue;
		break;
	case 0x21:									// GET_LINE_CODING
		memcpy(iEp0TxBuf, iLineCoding, 7);
		iEp0Write->SetTxBufferInfo(iEp0TxBuf, 0, len < 7 ? len : 7);
		iEp0Write->SetTransferDirection(EControllerWrite);
		iEp0Write->iZlpReqd = EFalse;
		(void)iCtrl->SetupWriteBuffer(*iEp0Write);
		break;
	case 0x22:									// SET_CONTROL_LINE_STATE (DTR/RTS)
		PTr("phlink: control line state "); PTrHex(aSetup[2]); PTr("\n");
		iCtrl->SendEp0StatusPacket(&iComm);
		Kick();
		break;
	default:
		iCtrl->Ep0Stall(&iComm);
		break;
		}
	}

void DPhLink::Ep0WriteDfc(TAny*)
	{
	}

// stream the log: one chunk in flight at a time
void DPhLink::Kick()
	{
	if (iCtrl->GetDeviceStatus() != EUsbcDeviceStateConfigured)
		return;
	A64UsbLinkUp();			// the host configured us (a64_glue.cpp: the A64_USB_GUARD's "link seen")
	if (!iRxBusy)
		{
		iRx->SetRxBufferInfo(iRxBuf, 0, iRxIndex, iRxSize, sizeof(iRxBuf));
		iRx->SetTransferDirection(EControllerRead);
		if (iCtrl->SetupReadBuffer(*iRx) == KErrNone)
			iRxBusy = ETrue;
		}
	if (iTxBusy)
		return;
	// HIGH SPEED (2026-10-12): an upload receiving while the log streams out froze the link (boots 381/383/385, always
	// during boot's heavy log; the same push on a settled phone: 91 MB in 17.8 s). Until the IN/OUT interaction in the
	// PSL is understood, the log waits while an image comes in (the 256 KB ring keeps the newest lines).
	// A file request ('F', up to 4.5 KB in) is an upload too: receiving one while the log streamed froze the link the
	// same way, one core spinning (2026-10-20, ph package install's 3 KB writes during the installer's log).
	if (iUpState == EUpData || iUpState == EUpFs)
		return;
	unsigned len = 0;
	const char* buf = ekseam_board_log_buf(&len);
	// the log is a 256 KB ring (a64_glue.cpp): len = bytes ever logged; a whole ring behind = skip to the oldest kept
	const TUint KRing = 0x40000;
	if (len - iSent > KRing - 0x1000)
		iSent = len - (KRing - 0x1000);
	// a screenshot in progress takes every other transfer (all of them while the log is quiet)
	if ((iShotStage || iBlobStage) && (iSent >= len || iTxIsLog) && NextFrame())
		{
		iTxIsLog = EFalse;
		iTx->SetTxBufferInfo(iFrame, 0, iTxLen);
		iTx->SetTransferDirection(EControllerWrite);
		iTx->iZlpReqd = EFalse;
		if (iCtrl->SetupWriteBuffer(*iTx) == KErrNone)
			iTxBusy = ETrue;
		return;
		}
	if (iSent >= len)
		return;
	iTxIsLog = ETrue;
	TInt n = (TInt)(len - iSent);
	const TUint at = iSent & (KRing - 1);
	if (n > 1024)
		n = 1024;
	if ((TUint)n > KRing - at)
		n = (TInt)(KRing - at);
	iTxLen = n;
	iTx->SetTxBufferInfo((TUint8*)(buf + at), 0, n);
	iTx->SetTransferDirection(EControllerWrite);
	iTx->iZlpReqd = EFalse;
	if (iCtrl->SetupWriteBuffer(*iTx) == KErrNone)
		iTxBusy = ETrue;
	}

void DPhLink::TxDfc(TAny* aPtr)
	{
	DPhLink* self = static_cast<DPhLink*>(aPtr);
	self->iTxBusy = EFalse;
	if (self->iTx->iError == KErrNone)
		{
		if (self->iTxIsLog)
			self->iSent += self->iTx->iTxBytes;
		else if (self->iTxIsBlob)
			{
			self->iBlobStage = self->iBlobNextStage;
			self->iBlobOff = self->iBlobNextOff;
			}
		else
			{
			if (self->iLive)
				self->LiveSent();
			self->iShotStage = self->iShotNextStage;
			self->iShotRow = self->iShotNextRow;
			}
		}
	self->Kick();
	}

void DPhLink::RxDfc(TAny* aPtr)
	{
	DPhLink* self = static_cast<DPhLink*>(aPtr);
	self->iRxBusy = EFalse;
	if (self->iRx->iError == KErrNone)
		self->Received(self->iRxBuf, self->iRxSize[0]);
	self->Kick();
	}

// the command stream: 'R' = reboot; 'U' + len (LE32) + sum (LE32, the word sum of the data zero-padded to 512) +
// len bytes = upload a new image into slot B (a64_glue.cpp: written, read back, checked, marked PENDING; the next
// boot tries it). Anything else is echoed into the log.
void DPhLink::Received(const TUint8* aData, TInt aLen)
	{
	TInt i = 0;
	while (i < aLen)
		{
		if (iUpState == EUpFs)
			{
			TInt n = aLen - i;
			if (n > iFsLen - iFsFill)
				n = iFsLen - iFsFill;
			if (!iFsOver)
				memcpy(FsBuf + iFsFill, aData + i, n);
			iFsFill += n;
			i += n;
			if (iFsFill == iFsLen)
				{
				iUpState = EUpIdle;
				FsDispatch();
				}
			continue;
			}
		if (iUpState == EUpData)
			{
			TInt n = aLen - i;
			if ((TUint)n > iUpLeft)
				n = (TInt)iUpLeft;
			if (A64SlotUploadData(aData + i, n) != KErrNone && iUpOk)
				{
				iUpOk = EFalse;
				PTr("phlink: upload: SD write FAILED\n");
				}
			i += n;
			iUpLeft -= n;
			if (((iUpLen - iUpLeft) & 0xFFFF) == 0 && iUpLeft)
				{	// a delta shows ONE bar over all its runs ('G' announced the total; user, 2026-10-09)
				if (iUpPatch && iDeltaTotal)
					A64BarsProgress("DELTA", iDeltaDone + (iUpLen - iUpLeft), iDeltaTotal);
				else if (!iUpPatch)
					A64BarsProgress("UPLOAD", iUpLen - iUpLeft, iUpLen);
				}
			if (((iUpLen - iUpLeft) & 0x3FFFF) == 0 && iUpLeft)
				{
				PTr("phlink: upload "); PTrHex(iUpLen - iUpLeft); PTr("\n");
				}
			if (iUpLeft == 0 && iUpPatch)
				{
				const TInt r = iUpOk ? A64SlotPatchEnd() : KErrCorrupt;
				PTr(r == KErrNone ? "phlink: PATCH OK" : "phlink: PATCH FAILED");
				PTr(" off="); PTrHex(iUpOff); PTr(" len="); PTrHex(iUpLen); PTr("\n");
				iDeltaDone += iUpLen;
				if (iDeltaTotal)
					A64BarsProgress("DELTA", iDeltaDone, iDeltaTotal);
				iUpState = EUpIdle;
				}
			else if (iUpLeft == 0)
				{
				TUint sum = 0;
				const TInt r = iUpOk ? A64SlotUploadEnd(&sum) : KErrCorrupt;
				PTr(r == KErrNone ? "phlink: UPLOAD OK" : "phlink: UPLOAD FAILED");
				PTr(" len="); PTrHex(iUpLen); PTr(" sum="); PTrHex(sum); PTr("\n");
				A64BarsProgressEnd();
				A64BarsNotify(r == KErrNone ? 1 : 3, r == KErrNone ? "Upload OK: image B ready" : "Upload FAILED (checksum or SD write)");
				iUpState = EUpIdle;
				}
			continue;
			}
		const TUint8 c = aData[i++];
		if (iUpState == EUpHeader)
			{
			iUpHdr[iUpHdrFill++] = c;
			if (iUpHdrFill == 8 && iCmd == 'X')
				{	// a delta run: off, len, then len bytes
				iUpOff = iUpHdr[0] | (iUpHdr[1] << 8) | (iUpHdr[2] << 16) | ((TUint)iUpHdr[3] << 24);
				iUpLen = iUpHdr[4] | (iUpHdr[5] << 8) | (iUpHdr[6] << 16) | ((TUint)iUpHdr[7] << 24);
				const TInt r = A64SlotPatchBegin(iUpOff, iUpLen);
				iUpLeft = iUpLen;
				iUpOk = (r == KErrNone);
				iUpPatch = ETrue;
				iUpState = EUpData;
				if (r != KErrNone)
					{ PTr("phlink: patch begin r="); PTrHex((TUint32)r); PTr("\n"); }
				continue;
				}
			if (iUpHdrFill == 8 && iCmd == 'F')
				{	// the file service: payload length, op, then the payload
				iFsLen = (TInt)(iUpHdr[0] | (iUpHdr[1] << 8) | (iUpHdr[2] << 16) | ((TUint)iUpHdr[3] << 24));
				iFsOp = (TInt)(iUpHdr[4] | (iUpHdr[5] << 8) | (iUpHdr[6] << 16) | ((TUint)iUpHdr[7] << 24));
				iFsFill = 0;
				iFsOver = iFsLen < 0 || iFsLen > KFsMaxRequest;
				if (iFsOver && iFsLen < 0)
					iFsLen = 0;
				iUpState = EUpFs;
				if (iFsLen == 0)
					{
					iUpState = EUpIdle;
					FsDispatch();
					}
				continue;
				}
			if (iUpHdrFill == 8 && iCmd != 'U')
				{
				iUpState = EUpIdle;
				Command(iUpHdr[0] | (iUpHdr[1] << 8) | (iUpHdr[2] << 16) | ((TUint)iUpHdr[3] << 24),
					iUpHdr[4] | (iUpHdr[5] << 8) | (iUpHdr[6] << 16) | ((TUint)iUpHdr[7] << 24));
				continue;
				}
			if (iUpHdrFill == 8)
				{
				iUpLen = iUpHdr[0] | (iUpHdr[1] << 8) | (iUpHdr[2] << 16) | ((TUint)iUpHdr[3] << 24);
				const TUint sum = iUpHdr[4] | (iUpHdr[5] << 8) | (iUpHdr[6] << 16) | ((TUint)iUpHdr[7] << 24);
				const TInt r = A64SlotUploadBegin(iUpLen, sum);
				PTr("phlink: upload begin len="); PTrHex(iUpLen); PTr(" r="); PTrHex((TUint32)r); PTr("\n");
				iUpLeft = iUpLen;
				iUpOk = (r == KErrNone);
				iUpPatch = EFalse;
				iUpState = EUpData;
				A64BarsProgress("UPLOAD", 0, iUpLen);
				}
			continue;
			}
		if (c == 'R')
			Reboot();
		else if (c == 'B')
			{
			const TInt r = A64SlotRearm();
			PTr("phlink: slot B re-armed r="); PTrHex((TUint32)r); PTr("\n");
			if (r == KErrNone)
				Reboot();
			}
		else if (c == 'D')						// PhoneLab "threads": every thread's state + wait object + user pc/lr
			{
			A64ThreadDump();
			}
		else if (c == 'V')
			{
			PTr("phlink: V proto="); PTrHex(KProtoVersion); PTr(" caps=shot,live,key,touch,panel,composite,profile,hotspots,threads,delta,fs\n");
			}
		else if (c == 'U' || c == 'P' || c == 'W' || c == 'S' || c == 'L' || c == 'K' || c == 'T' || c == 'Q' || c == 'H'
			|| c == 'X' || c == 'Y' || c == 'Z' || c == 'G' || c == 'F')
			{
			iCmd = c;
			iUpState = EUpHeader;
			iUpHdrFill = 0;
			}
		else if (c >= 0x20 && c < 0x7F)
			{
			PTr("phlink: rx '"); ekseam_board_log(c); PTr("'\n");
			}
		}
	}

// The 8-byte-argument commands (two LE32 words a, b):
//   'P' addr count / 'W' addr value = PeekPoke (below)
//   'S' which 0     = screenshot (which: 0 = the OS picture, 1 = the panel layer), streamed as frames
//   'K' scan code, 0 = key down / 1 = key up (TStdScanCode, e32keys.h)
//   'T' x | y << 16 (OS screen pixels), 0 = down / 1 = move / 2 = up -- a touch, as a64touch.cpp delivers it
void DPhLink::Command(TUint32 aA, TUint32 aB)
	{
	switch (iCmd)
		{
	case 'P':
	case 'W':
		PeekPoke(aA, aB);
		break;
	case 'S':
		ShotStart(aA);
		break;
	case 'L':
		LiveStart(aA, aB);
		break;
	case 'Q':									// a profile snapshot (PhoneLab "profile"), streamed as a blob
		if (iBlobStage)
			{ PTr("phlink: Q busy\n"); break; }
		iBlobLen = A64ProfileSnapshot(BlobBuf, KBlobMax);
		iBlobKind = 1;
		iBlobOff = 0;
		iBlobStage = iBlobLen ? 1 : 2;
		Kick();
		break;
	case 'H':									// hotspots (PhoneLab "hotspots"): 1 = start sampling, 0 = stop + send
		if (aA)
			{ A64HotStart(); PTr("phlink: H started\n"); break; }
		if (iBlobStage)
			{ PTr("phlink: Q busy\n"); break; }
		iBlobLen = A64HotStop(BlobBuf, KBlobMax);
		iBlobKind = 2;
		iBlobOff = 0;
		iBlobStage = iBlobLen ? 1 : 2;
		Kick();
		break;
	case 'Y':									// delta: the hash of each 64 KB block of slot B over [0, a) -> blob kind 3
		{
		if (iBlobStage)
			{ PTr("phlink: Y busy\n"); break; }
		const TInt n = A64SlotHashBlocks(aA, (TUint32*)BlobBuf, KBlobMax / 4);
		PTr("phlink: Y blocks="); PTrHex((TUint32)n); PTr("\n");
		iBlobLen = n > 0 ? n * 4 : 0;
		iBlobKind = 3;
		iBlobOff = 0;
		iBlobStage = iBlobLen ? 1 : 2;
		Kick();
		break;
		}
	case 'G':									// delta: a = the bytes all 'X' runs will carry (the progress bar's total)
		iDeltaTotal = aA;
		iDeltaDone = 0;
		A64BarsProgress("DELTA", 0, aA ? aA : 1);
		PTr("phlink: G total="); PTrHex(aA); PTr("\n");
		break;
	case 'Z':									// delta: commit a = length, b = hash of the block hashes -> PENDING
		{
		TUint32 got = 0;
		A64BarsProgress("VERIFY", 0, 1);
		const TInt r = A64SlotCommit(aA, aB, &got);
		A64BarsProgressEnd();
		iDeltaTotal = 0;
		PTr(r == KErrNone ? "phlink: COMMIT OK" : "phlink: COMMIT FAILED");
		PTr(" len="); PTrHex(aA); PTr(" hash="); PTrHex(got); PTr("\n");
		A64BarsNotify(r == KErrNone ? 1 : 3, r == KErrNone ? "Delta upload OK: image B ready" : "Delta upload FAILED");
		break;
		}
	case 'K':
		{
		TRawEvent ev;
		ev.Set(aB ? TRawEvent::EKeyUp : TRawEvent::EKeyDown, (TInt)aA);
		A64CpuInputBoost();
		Kern::AddEvent(ev);
		PTr("phlink: K "); PTrHex(aA); PTr(aB ? " up\n" : " down\n");
		break;
		}
	case 'T':
		if (aB & 0x10)
			{	// panel pixels (720x1440): the bars first (golden overlay, soft key bar), as a finger would hit them
			TInt px = (TInt)(aA & 0xFFFF), py = (TInt)(aA >> 16);
			const TInt act = (TInt)(aB & 3);
			if (py >= 1360)
				{
				static TInt held;
				if (act == 0)
					{
					held = px < 240 ? EStdKeyYes : (px < 480 ? EStdKeyApplication0 : EStdKeyNo);
#ifdef A64_GOLDEN
					if (held == EStdKeyApplication0) { A64BarsMenuKey(); held = 0; }
#endif
					}
				if (held && act != 1)
					{
					TRawEvent ev;
					ev.Set(act == 0 ? TRawEvent::EKeyDown : TRawEvent::EKeyUp, held);
					A64CpuInputBoost();
					Kern::AddEvent(ev);
					}
				if (act == 2) held = 0;
				PTr("phlink: T panel soft key "); PTrHex(aA); PTr("\n");
				break;
				}
			if (A64BarsPanelTouch(px, py, act))
				{
				if (act != 1) { PTr("phlink: T panel overlay "); PTrHex(aA); PTr("\n"); }
				break;
				}
			px /= 2; py = (py - 80) / 2;				// -> OS pixels (360x640 doubled, under the 80 px top bar)
			if (py < 0 || py >= 640) break;
			aA = (TUint32)px | ((TUint32)py << 16);
			aB = (TUint32)act;
			}
		{
		const TRawEvent::TType t = aB == 0 ? TRawEvent::EButton1Down : (aB == 1 ? TRawEvent::EPointerMove : TRawEvent::EButton1Up);
		TRawEvent ev;
		ev.Set(t, (TInt)(aA & 0xFFFF), (TInt)(aA >> 16), 0);	// Z = 0: see a64touch.cpp
		A64CpuInputBoost();
		Kern::AddEvent(ev);
		if (aB != 1)
			{ PTr("phlink: T "); PTrHex(aA); PTr(aB ? " up\n" : " down\n"); }
		break;
		}
		}
	}

// The picture comes from a64_display.cpp already mapped (mapping RAM from this DFC hung the phone).
TBool DPhLink::ShotSource(TUint32 aWhich)
	{
	TLinAddr lin = 0;
	TInt w = 0, h = 0;
	iShotPanel = NULL;
	if (aWhich == 2)
		{	// the whole screen as the user sees it, at half the panel's resolution (360x720): the panel layer where it is
			// opaque (the bars, the golden overlay), the OS picture where it is see-through (alpha 0)
		TLinAddr plin = 0, olin = 0;
		TInt pw = 0, ph = 0, ow = 0, oh = 0;
		if (A64DisplayCaptureSource(1, &plin, &pw, &ph) != KErrNone || !plin || pw != 720 || ph != 1440
			|| A64DisplayCaptureSource(0, &olin, &ow, &oh) != KErrNone || !olin || ow != 360 || oh != 640)
			return EFalse;
		iShotPanel = (const TUint32*)plin;
		lin = olin; w = 360; h = 720;
		}
	else if (A64DisplayCaptureSource((TInt)aWhich, &lin, &w, &h) != KErrNone || !lin || w <= 0 || h <= 0)
		return EFalse;
	if (w == 360 && (h == 640 || (aWhich == 2 && h == 720)))
		{
		memcpy(ShotSnap, (const TAny*)lin, sizeof(ShotSnap));
		lin = (TLinAddr)ShotSnap;
		}
	iShotPix = (const TUint32*)lin;
	iShotW = w;
	iShotH = h;
	return ETrue;
	}

void DPhLink::ShotStart(TUint32 aWhich)
	{
	if (iShotStage)
		{ PTr("phlink: S busy\n"); return; }
	if (!ShotSource(aWhich))
		{ PTr("phlink: S no picture\n"); return; }
	iLive = EFalse;
	iShotRow = 0;
	++iShotId;
	iShotStage = 1;
	PTr("phlink: S id="); PTrHex(iShotId); PTr(" size="); PTrHex(((TUint32)iShotW << 16) | (TUint32)iShotH); PTr("\n");
	Kick();
	}

// 'L' which flags (bit 0 = keyframe): the live view. The request IS the ack of the previous frame; a PC that lost
// anything asks with the keyframe bit (the first time / another source / an interrupted frame: the reference is
// dropped here anyway).
void DPhLink::LiveStart(TUint32 aWhich, TUint32 aFlags)
	{
	if (iShotStage)
		{ PTr("phlink: L busy\n"); return; }
	if (!ShotSource(aWhich) || iShotW > KLiveMaxW || iShotH > KLiveMaxH)
		{ PTr("phlink: L no picture\n"); return; }
	iLive = ETrue;
	iLiveWhich = (TInt)aWhich;
	iLiveKey = (aFlags & 1) || !LiveRefOk || LiveWhich != iLiveWhich || LiveW != iShotW || LiveH != iShotH;
	iLiveNtx = (iShotW + KLiveTile - 1) / KLiveTile;
	iLiveTiles = iLiveNtx * ((iShotH + KLiveTile - 1) / KLiveTile);
	iLiveBandTy = -1;
	iLiveLo = iLiveHi = 0;
	iLiveSentTiles = 0;
	iShotRow = 0;
	++iShotId;
	iShotStage = 1;
	Kick();
	}

static inline void Put32(TUint8* aP, TUint32 aV)
	{
	aP[0] = (TUint8)aV; aP[1] = (TUint8)(aV >> 8); aP[2] = (TUint8)(aV >> 16); aP[3] = (TUint8)(aV >> 24);
	}

static TInt RleRow(const volatile TUint32* aPix, TInt aW, TUint8* aOut)
	{
	TUint8* o = aOut;
	TInt i = 0;
	while (i < aW)
		{
		const TUint32 p = aPix[i] & 0xFFFFFF;
		TInt run = 1;
		while (i + run < aW && run < 129 && (aPix[i + run] & 0xFFFFFF) == p)
			++run;
		if (run >= 2)
			{
			*o++ = (TUint8)(0x80 + run - 2);
			*o++ = (TUint8)(p >> 16); *o++ = (TUint8)(p >> 8); *o++ = (TUint8)p;
			i += run;
			continue;
			}
		TInt j = i + 1;
		while (j < aW && j - i < 128 && !(j + 1 < aW && (aPix[j] & 0xFFFFFF) == (aPix[j + 1] & 0xFFFFFF)))
			++j;
		*o++ = (TUint8)(j - i - 1);
		for (TInt k = i; k < j; ++k)
			{
			const TUint32 q = aPix[k];
			*o++ = (TUint8)(q >> 16); *o++ = (TUint8)(q >> 8); *o++ = (TUint8)q;
			}
		i = j;
		}
	return (TInt)(o - aOut);
	}

// Build the next screenshot frame into iFrame (iTxLen); the state moves on in TxDfc once it is sent.
TBool DPhLink::ShotFrame()
	{
	TUint8* f = iFrame;
	TUint8* pl = f + 16;
	TInt n = 0;
	TUint8 type = 0;
	iShotNextRow = iShotRow;
	if (iShotStage == 1)
		{
		type = EFrameShotBegin;
		Put32(pl, iShotId); Put32(pl + 4, ((TUint32)iShotH << 16) | (TUint32)iShotW);
		Put32(pl + 8, (TUint32)iShotW * 4); Put32(pl + 12, 1);
		n = 16;
		iShotNextStage = 2;
		}
	else if (iShotStage == 2)
		{
		type = EFrameShotRows;
		const TInt worst = iShotW * 3 + (iShotW + 127) / 128 + 1;
		TInt row = iShotRow, rows = 0;
		n = 4;
		while (row < iShotH && 16 + n + worst <= KFrameMax)
			{
			const TUint32* src = iShotPix + row * ShotPitch();
			if (iShotPanel)
				{
				const volatile TUint32* pp = iShotPanel + (row * 2) * 720;
				const TInt oy = row - 40;					// the OS picture starts 80 panel rows down
				for (TInt x = 0; x < 360; ++x)
					{
					TUint32 p = pp[x * 2];
					if (!(p >> 24))
						p = (oy >= 0 && oy < 640) ? iShotPix[oy * 368 + x] : 0;
					iShotRowBuf[x] = p;
					}
				src = iShotRowBuf;
				}
			n += RleRow(src, iShotW, pl + n);
			++row;
			++rows;
			}
		if (!rows)
			{ PTr("phlink: S row too wide\n"); iShotStage = 0; return EFalse; }
		pl[0] = (TUint8)iShotRow; pl[1] = (TUint8)(iShotRow >> 8); pl[2] = (TUint8)rows; pl[3] = (TUint8)(rows >> 8);
		iShotNextRow = row;
		iShotNextStage = row < iShotH ? 2 : 3;
		}
	else if (iShotStage == 3)
		{
		type = EFrameShotEnd;
		Put32(pl, iShotId); Put32(pl + 4, (TUint32)iShotH);
		n = 8;
		iShotNextStage = 0;
		}
	else
		return EFalse;
	TUint32 sum = 0;
	for (TInt i = 0; i < n; ++i)
		sum += pl[i];
	f[0] = 0xA5; f[1] = 0x5A; f[2] = 'P'; f[3] = 'F';
	f[4] = type; f[5] = 0; f[6] = (TUint8)iFrameSeq; f[7] = (TUint8)(iFrameSeq >> 8);
	++iFrameSeq;
	Put32(f + 8, (TUint32)n);
	Put32(f + 12, sum);
	iTxLen = 16 + n;
	return ETrue;
	}

void DPhLink::Seal(TUint8 aType, TInt aN)
	{
	TUint8* f = iFrame;
	const TUint8* pl = f + 16;
	TUint32 sum = 0;
	for (TInt i = 0; i < aN; ++i)
		sum += pl[i];
	f[0] = 0xA5; f[1] = 0x5A; f[2] = 'P'; f[3] = 'F';
	f[4] = aType; f[5] = 0; f[6] = (TUint8)iFrameSeq; f[7] = (TUint8)(iFrameSeq >> 8);
	++iFrameSeq;
	Put32(f + 8, (TUint32)aN);
	Put32(f + 12, sum);
	iTxLen = 16 + aN;
	}

static TUint32 TileHash(const TUint32* aBand, TInt aStride, TInt aX0, TInt aTw, TInt aTh)
	{
	TUint32 h = 0x811C9DC5u;
	for (TInt y = 0; y < aTh; ++y)
		{
		const TUint32* p = aBand + y * aStride + aX0;
		for (TInt x = 0; x < aTw; ++x)
			{
			h = (h ^ (p[x] & 0xFFFFFF)) * 0x01000193u;
			h ^= h >> 15;
			}
		}
	return h;
	}

// The next live frame. Stage 1 = begin, 2 = tiles from tile iShotRow on (as many changed ones as fit), 3 = end. A tile is
// "changed" when its hash differs from the one last sent (LiveRef) or the frame is a keyframe.
TBool DPhLink::LiveFrame()
	{
	TUint8* pl = iFrame + 16;
	const TInt w = iShotW, h = iShotH;
	iShotNextRow = iShotRow;
	if (iShotStage == 1)
		{
		Put32(pl, iShotId); Put32(pl + 4, ((TUint32)h << 16) | (TUint32)w);
		Put32(pl + 8, KLiveTile); Put32(pl + 12, iLiveKey ? 1 : 0);
		Seal(EFrameLiveBegin, 16);
		iShotNextStage = 2;
		return ETrue;
		}
	if (iShotStage == 2)
		{
		TInt n = 4, count = 0, t = iShotRow;
		while (t < iLiveTiles)
			{
			const TInt tx = t % iLiveNtx, ty = t / iLiveNtx;
			if (ty != iLiveBandTy)
				{	// compose the 32 source rows of this tile row (the panel layer is read once, here)
				for (TInt r = 0; r < KLiveTile && ty * KLiveTile + r < h; ++r)
					{
					const TInt row = ty * KLiveTile + r;
					TUint32* out = LiveBand + r * w;
					if (iShotPanel)
						{
						const volatile TUint32* pp = iShotPanel + (row * 2) * 720;
						const TInt oy = row - 40;
						for (TInt x = 0; x < w; ++x)
							{
							TUint32 p = pp[x * 2];
							if (!(p >> 24))
								p = (oy >= 0 && oy < 640) ? iShotPix[oy * 368 + x] : 0;
							out[x] = p;
							}
						}
					else
						{
						const TUint32* src = iShotPix + row * ShotPitch();
						for (TInt x = 0; x < w; ++x)
							out[x] = src[x];
						}
					}
				iLiveBandTy = ty;
				}
			const TInt x0 = tx * KLiveTile;
			const TInt tw = (w - x0 < KLiveTile) ? w - x0 : KLiveTile;
			const TInt th = (h - ty * KLiveTile < KLiveTile) ? h - ty * KLiveTile : KLiveTile;
			const TUint32 hs = TileHash(LiveBand, w, x0, tw, th);
			LiveNew[t] = hs;
			if (iLiveKey || hs != LiveRef[t])
				{
				TUint8* o = LiveRaw;
				for (TInt y = 0; y < th; ++y)
					{
					const TUint32* p = LiveBand + y * w + x0;
					for (TInt x = 0; x < tw; ++x)
						{
						const TUint32 q = p[x];
						*o++ = (TUint8)(q >> 16); *o++ = (TUint8)(q >> 8); *o++ = (TUint8)q;
						}
					}
				const TInt rawLen = tw * th * 3;
				TInt c = PhLz4Compress(LiveRaw, rawLen, LiveComp, &LiveTable);
				const TUint8* data = LiveComp;
				TUint cl = (TUint)c;
				if (c >= rawLen)
					{ data = LiveRaw; c = rawLen; cl = (TUint)rawLen | 0x8000; }
				if (16 + n + 4 + c > KFrameMax)
					break;							// full: the next frame starts at this tile
				pl[n] = (TUint8)tx; pl[n + 1] = (TUint8)ty; pl[n + 2] = (TUint8)cl; pl[n + 3] = (TUint8)(cl >> 8);
				for (TInt i = 0; i < c; ++i)
					pl[n + 4 + i] = data[i];
				n += 4 + c;
				++count;
				}
			++t;
			}
		if (count)
			{
			pl[0] = (TUint8)count; pl[1] = (TUint8)(count >> 8); pl[2] = 0; pl[3] = 0;
			iLiveLo = iShotRow; iLiveHi = t;
			iLiveSentTiles += count;
			iShotNextRow = t;
			iShotNextStage = t < iLiveTiles ? 2 : 3;
			Seal(EFrameLiveTiles, n);
			return ETrue;
			}
		iLiveLo = iLiveHi = 0;						// nothing (more) changed: straight to the end frame
		iShotStage = 3;
		}
	if (iShotStage == 3)
		{
		Put32(pl, iShotId); Put32(pl + 4, (TUint32)iLiveSentTiles); Put32(pl + 8, 0);
		Seal(EFrameLiveEnd, 12);
		iShotNextStage = 0;
		return ETrue;
		}
	return EFalse;
	}

// The frame just built went out (TxDfc): commit its tiles' hashes; the reference is complete only after the end frame.
// (iShotStage is still the stage of the frame that was sent; LiveFrame may have moved a tile-less stage 2 on to 3.)
void DPhLink::LiveSent()
	{
	if (iShotStage == 1)
		LiveRefOk = EFalse;
	else if (iShotStage == 2)
		{
		for (TInt t = iLiveLo; t < iLiveHi; ++t)
			LiveRef[t] = LiveNew[t];
		}
	else if (iShotStage == 3)
		{
		LiveRefOk = ETrue;
		LiveWhich = iLiveWhich;
		LiveW = iShotW; LiveH = iShotH;
		}
	}

TBool DPhLink::BlobFrame()
	{
	TUint8* f = iFrame;
	TUint8* pl = f + 16;
	TInt n = 0;
	TUint8 type;
	if (iBlobStage == 1)
		{
		type = EFrameBlobData;
		n = iBlobLen - iBlobOff;
		if (n > KFrameMax - 16)
			n = KFrameMax - 16;
		memcpy(pl, BlobBuf + iBlobOff, n);
		iBlobNextOff = iBlobOff + n;
		iBlobNextStage = iBlobNextOff < iBlobLen ? 1 : 2;
		}
	else if (iBlobStage == 2)
		{
		type = EFrameBlobEnd;
		Put32(pl, iBlobKind); Put32(pl + 4, (TUint32)iBlobLen);
		n = 8;
		iBlobNextOff = iBlobOff;
		iBlobNextStage = 0;
		}
	else
		return EFalse;
	TUint32 sum = 0;
	for (TInt i = 0; i < n; ++i)
		sum += pl[i];
	f[0] = 0xA5; f[1] = 0x5A; f[2] = 'P'; f[3] = 'F';
	f[4] = type; f[5] = 0; f[6] = (TUint8)iFrameSeq; f[7] = (TUint8)(iFrameSeq >> 8);
	++iFrameSeq;
	Put32(f + 8, (TUint32)n);
	Put32(f + 12, sum);
	iTxLen = 16 + n;
	return ETrue;
	}

// 'P' + addr + count (LE32 each) = log count words from physical addr; 'W' + addr + value = write one word and log
// the readback. Physical memory is mapped a 1 MB window at a time, kept (never closed: a Close per access rebooted
// the phone, 2026-10-06), up to KWins windows.
void DPhLink::PeekPoke(TUint32 aAddr, TUint32 aArg)
	{
	const TInt KWins = 16;
	static TUint32 base[KWins];
	static TLinAddr lin[KWins];
	TUint32 n = iCmd == 'W' ? 1 : (aArg ? aArg : 1);
	if (n > 64)
		n = 64;
	aAddr &= ~3u;
	for (TUint32 k = 0; k < n; ++k, aAddr += 4)
		{
		const TUint32 b = aAddr & ~0xFFFFFu;
		TInt w = 0;
		while (w < KWins && lin[w] && base[w] != b)
			++w;
		if (w == KWins)
			{ PTr("phlink: peek: out of windows\n"); return; }
		if (!lin[w])
			{
			DPlatChunkHw* ch = NULL;
			if (DPlatChunkHw::New(ch, b, 0x100000, EMapAttrSupRw | EMapAttrFullyBlocking) != KErrNone)
				{ PTr("phlink: peek: map failed "); PTrHex(b); PTr("\n"); return; }
			base[w] = b;
			lin[w] = ch->LinearAddress();
			}
		volatile TUint32* r = (volatile TUint32*)(lin[w] + (aAddr - b));
		if (iCmd == 'W')
			*r = aArg;
		PTr(iCmd == 'W' ? "phlink: W " : "phlink: P "); PTrHex(aAddr); PTr(" = "); PTrHex(*r); PTr("\n");
		}
	}

// the A64 watchdog (timer block at 0x01C20C00, WDOG0 at +0xA0: CTRL +0x10, CFG +0x14, MODE +0x18; Linux
// sunxi_wdt.c sun6i_wdt_reg): CFG = 1 (whole system), MODE = enable with interval 0 (0.5 s)
void DPhLink::Reboot()
	{
	PTr("phlink: reboot requested\n");
	A64WatchdogReboot();					// a64_glue.cpp: also stops its feeding
	}

void DPhLink::PollTimerFn(TAny* aPtr)
	{
	static_cast<DPhLink*>(aPtr)->iPollDfc.Add();
	}

void DPhLink::PollDfcFn(TAny* aPtr)
	{
	DPhLink* self = static_cast<DPhLink*>(aPtr);
	self->Kick();
	self->iPollTimer.OneShot(NKern::TimerTicks(50));
	}

// ---- the file service ('F'): requests out to the helper (PhFs.A64), answers back as blob kind 4 ----
// FsBuf holds a complete request; it goes to the helper through the LDD, or the PC gets a bare status at once.
void DPhLink::FsDispatch()
	{
	if (iFsOver)
		{ FsFail(KErrTooBig); return; }
	if (iBlobStage)
		{ FsFail(KErrInUse); return; }
	const TInt r = PhFsSubmit(iFsOp, FsBuf, iFsLen);
	if (r != KErrNone)
		FsFail(r);							// KErrNotReady = phfs.exe is not running
	}

// DFC context: answer with just a status
void DPhLink::FsFail(TInt aError)
	{
	if (iBlobStage)
		return;								// another blob is in flight: the PC times out
	Put32(BlobBuf, (TUint32)aError);
	iBlobLen = 4;
	iBlobKind = 4;
	iBlobOff = 0;
	iBlobStage = 1;
	Kick();
	}

// the LDD (a client thread) assembles the reply in BlobBuf, then FsReplyDone starts the transfer from the DFC queue
TBool DPhLink::FsReplyBuf(TUint8*& aBuf, TInt& aMax)
	{
	if (iBlobStage)
		return EFalse;
	aBuf = BlobBuf;
	aMax = KBlobMax;
	return ETrue;
	}

void DPhLink::FsReplyDone(TInt aLen)
	{
	iBlobLen = aLen;
	iBlobKind = 4;
	iBlobOff = 0;
	iBlobStage = aLen ? 1 : 2;
	iFsDfc.Enque();
	}

void DPhLink::FsDfcFn(TAny* aPtr)
	{
	static_cast<DPhLink*>(aPtr)->Kick();
	}

static DPhLink* TheLink;

extern "C" TUint8* PhLinkFsReplyBuf(TInt* aMax)
	{
	TUint8* b = NULL;
	return TheLink && TheLink->FsReplyBuf(b, *aMax) ? b : NULL;
	}

extern "C" void PhLinkFsReplyDone(TInt aLen)
	{
	if (TheLink)
		TheLink->FsReplyDone(aLen);
	}

// a failure from outside the DFC thread (the helper died with a request in hand)
extern "C" void PhLinkFsFail(TInt aError)
	{
	TUint8* b;
	TInt max;
	if (TheLink && TheLink->FsReplyBuf(b, max))
		{
		Put32(b, (TUint32)aError);
		TheLink->FsReplyDone(4);
		}
	}

extern "C" TInt PhLinkStart()
	{
	DPhLink* link = new DPhLink;
	if (!link)
		return KErrNoMemory;
	TheLink = link;
	PhFsInstall();
	return link->Start();
	}
