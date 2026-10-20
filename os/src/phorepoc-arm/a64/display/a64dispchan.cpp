// OURS (GPU plan G1a, docs/phorepoc/gpu-lima-research.md): the PinePhone's RDisplayChannel PDD -- the NGA display
// path (OpenWF-C composes, this driver flips the DE to each composed buffer on the panel's vblank). Split out of
// a64fb.cpp (2026-10-09, user decision): a64fb.cpp stays the MINIMAL SAFE display driver -- the legacy framebuffer
// the golden image runs, and the pattern for a first display on a new board; this file is the optional NGA layer
// on top of it (link_ekern.sh A64_DISPLAY=legacy leaves it out). Uses DLcdPowerHandler's composition RAM + DFC queue.

#include <videodriver.h>
#include "platform.h"
#include <nkern.h>
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <kernel/kpower.h>
#include <kernel/cache.h>
#include <syborg_priv.h>
#include "svpframebuffer.h"

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);
extern "C" TUint32 A64FbLegacyBase();		// a64fb.cpp: the legacy video RAM (FB_BASE)

// DDisplayPddA64: the PinePhone's RDisplayChannel PDD (docs/phorepoc/gpu-lima-research.md, G1a). The buffer
// bookkeeping is Syborg's (DDisplayPddSyborg above, the 2010 reference shape); the difference is WHEN a post
// completes: A64DisplayFlip (a64_display.cpp) points the DE at the buffer, and the vblank after the DE latched it
// queues iVSyncDfc. Two composition buffers in the contiguous RAM DLcdPowerHandler allocated for them.

extern "C" TBool A64DisplayFlip(TUint32 aPhys, TLinAddr aLin, TInt aW, TInt aH);
extern "C" void A64DisplaySetVblankDfc(TDfc* aDfc);
extern "C" TUint32 A64DisplayFlipForced();
extern "C" void A64DisplayChannelOwns(TBool aOwns);
// DEBUG (2026-10-09, the NGA boot death): force the SD log out at each step (ekseam_board_panic only flushes it)
extern "C" void ekseam_board_panic();
#ifdef EKSEAM_FLUSH_ON_PROC		// (KDEFS=-DEKSEAM_FLUSH_ON_PROC, the debug build only)
static void A64DbgFlush() { ekseam_board_panic(); }
#else
static void A64DbgFlush() {}
#endif


class DDisplayPddA64 : public DDisplayPdd
	{
public:
	DDisplayPddA64();
	~DDisplayPddA64();
	virtual TInt SetLegacyMode() { return KErrNone; }
	virtual TInt SetGceMode() { return PostCompositionBuffer(&iLdd->iCompositionBuffer[0]); }
	virtual TInt SetRotation(RDisplayChannel::TDisplayRotation aRotation);
	virtual TInt PostUserBuffer(TBufferNode* aNode);
	virtual TInt PostCompositionBuffer(TBufferNode* aNode);
	virtual TInt PostLegacyBuffer();
	virtual TInt CloseMsg();
	virtual TInt CreateChannelSetup(TInt aUnit);
	virtual TBool PostPending() { return iPendingBuffer != NULL; }
	virtual TDfcQue* DfcQ(TInt) { return iDfcQ; }
private:
	void DropPending(TBool aCancelUser);
	void Show(TBufferNode* aNode, TUint32 aPhys, TLinAddr aLin);
	void Post(TBufferNode* aNode, TUint32 aPhys, TLinAddr aLin);
	static void VSyncDfcFn(TAny* aSelf);
private:
	TDfcQue* iDfcQ;
	TBufferNode* iPendingBuffer;		// the ARMED flip: the DE may latch it at any frame start, so it is never cancelled
	TBufferNode* iQueuedBuffer;		// the post behind it (one deep: a newer post replaces it -- a mailbox)
	TUint32 iQueuedPhys;
	TLinAddr iQueuedLin;
	TBufferNode* iActiveBuffer;
	DChunk* iChunk;
	TDfc iVSyncDfc;
	NTimer iPaceTimer;			// posts before the vblank IRQ runs: complete one frame (16 ms) later, not at once
	static void PaceTimerFn(TAny* aSelf);
	TUint iPosts;
	TUint32 iFirstPostTick;
	};

DDisplayPddA64::DDisplayPddA64()
	: iDfcQ(NULL), iPendingBuffer(NULL), iQueuedBuffer(NULL), iQueuedPhys(0), iQueuedLin(0), iActiveBuffer(NULL), iChunk(NULL),
	  iVSyncDfc(&VSyncDfcFn, this, KVSyncDfcPriority), iPaceTimer(PaceTimerFn, this)
	{
	}

void DDisplayPddA64::PaceTimerFn(TAny* aSelf)
	{
	((DDisplayPddA64*)aSelf)->iVSyncDfc.Add();		// (timer callback context)
	}

DDisplayPddA64::~DDisplayPddA64()
	{
	iPaceTimer.Cancel();
	A64DisplaySetVblankDfc(NULL);
	iVSyncDfc.Cancel();
	DChunk* chunk = (DChunk*)__e32_atomic_swp_ord_ptr(&iChunk, 0);
	if (chunk)
		Kern::ChunkClose(chunk);
	}

// A newer post replaces a pending one (Syborg's rule): a pending user buffer's request is cancelled.
void DDisplayPddA64::DropPending(TBool aCancelUser)
	{
	if (!iPendingBuffer)
		return;
	iPendingBuffer->iState = EBufferFree;
	if (iPendingBuffer->iType == EBufferTypeUser)
		{
		if (aCancelUser)
			RequestComplete(RDisplayChannel::EReqPostUserBuffer, KErrCancel);
		}
	else
		iPendingBuffer->iFree = ETrue;
	iPendingBuffer = NULL;
	}

void DDisplayPddA64::Show(TBufferNode* aNode, TUint32 aPhys, TLinAddr aLin)
	{
	aNode->iState = EBufferPending;
	iPendingBuffer = aNode;
	if (iPosts == 0)
		iFirstPostTick = NKern::TickCount();
	if (iPosts++ < 2)
		{
		ekseam_puts("[EKERN][disp] pdd post "); ekseam_puthex(aPhys); A64DbgFlush();
		}
	const TInt w = iLdd->iDisplayInfo.iNormal.iWidth, h = iLdd->iDisplayInfo.iNormal.iHeight;
	if (!A64DisplayFlip(aPhys, aLin, w, h))
		iPaceTimer.OneShot(NKern::TimerTicks(16));	// shown now (display not up yet): complete a frame later -- at
													// once let a composer spin at full speed (2026-10-09)
	}

// A post never blocks the poster and never cancels an ARMED flip (2026-10-09; Syborg dropped the pending one, which
// races the DE's latch at the next frame start: the buffer would go back to "free" while being scanned). While a
// flip is armed the post waits in a one-deep queue and is shown at that flip's vblank; a newer post replaces a
// queued one (its buffer goes back to free, a user buffer's request is cancelled). With KDisplayCBMax = 4 the
// compositor (src/nga/openwfc_ri_display, which does not wait for its posts) always finds a free buffer: one on
// screen, one armed, one queued, one free -- so libWFC's one-compose-at-a-time rule no longer drops every other
// submit waiting for a vblank (34 fps -> the display's rate).
void DDisplayPddA64::Post(TBufferNode* aNode, TUint32 aPhys, TLinAddr aLin)
	{
	if (!iPendingBuffer)
		{
		Show(aNode, aPhys, aLin);
		return;
		}
	if (iQueuedBuffer && iQueuedBuffer != aNode)
		{
		iQueuedBuffer->iState = EBufferFree;
		if (iQueuedBuffer->iType == EBufferTypeUser)
			RequestComplete(RDisplayChannel::EReqPostUserBuffer, KErrCancel);
		else
			iQueuedBuffer->iFree = ETrue;
		}
	aNode->iState = EBufferPending;
	iQueuedBuffer = aNode;
	iQueuedPhys = aPhys;
	iQueuedLin = aLin;
	}

// the compositor already composed the rotated scene into the native-orientation buffer: record the rotation (the LDD
// reports it through ECtrlCurrentRotation and derives "changed" from it), nothing to do at the DE
TInt DDisplayPddA64::SetRotation(RDisplayChannel::TDisplayRotation aRotation)
	{
	if (!(aRotation & iLdd->iDisplayInfo.iAvailableRotations))
		return KErrNotSupported;
	ekseam_puts("[EKERN][disp] SetRotation "); ekseam_puthex((unsigned)aRotation);
	iLdd->iCurrentRotation = aRotation;
	return KErrNone;
	}

TInt DDisplayPddA64::PostUserBuffer(TBufferNode* aNode)
	{
	Post(aNode, Epoc::LinearToPhysical(aNode->iAddress), 0);	// (no capture of user buffers yet)
	return KErrNone;
	}

TInt DDisplayPddA64::PostCompositionBuffer(TBufferNode* aNode)
	{
	aNode->iFree = EFalse;
	Post(aNode, aNode->iPhysicalAddress, aNode->iAddress);
	return KErrNone;
	}

TInt DDisplayPddA64::PostLegacyBuffer()
	{
	if (iQueuedBuffer)
		{
		iQueuedBuffer->iState = EBufferFree;
		iQueuedBuffer->iFree = ETrue;
		iQueuedBuffer = NULL;
		}
	DropPending(ETrue);
	TBufferNode* node = &iLdd->iLegacyBuffer[0];
	node->iFree = EFalse;
	Show(node, A64FbLegacyBase(), 0);					// the legacy video RAM, as DLcdPowerHandler programmed it (FB_BASE)
	return KErrNone;
	}

TInt DDisplayPddA64::CloseMsg()
	{
	// the channel's statistics (the flip test's result: posts / ms ~ the panel rate when each waits for its post)
	ekseam_puts("[EKERN][disp] channel closed: posts="); ekseam_puthex(iPosts);
	ekseam_puts("[EKERN][disp]   ms since the first post="); ekseam_puthex(NKern::TickCount() - iFirstPostTick);
	ekseam_puts("[EKERN][disp]   forced completions="); ekseam_puthex(A64DisplayFlipForced());
	iPendingBuffer = NULL;
	iQueuedBuffer = NULL;
	iActiveBuffer = NULL;
	iPaceTimer.Cancel();
	iVSyncDfc.Cancel();
	// hand the screen back to the window server's legacy buffer: the composition chunk goes away with the channel
	// (and with it the mapping PhoneLab's screenshot was reading). No completion DFC for this flip: the PDD may be
	// deleted before the vblank.
	A64DisplaySetVblankDfc(NULL);
	A64DisplayChannelOwns(EFalse);
	A64DisplayFlip(A64FbLegacyBase(), 0, iLdd->iDisplayInfo.iNormal.iWidth, iLdd->iDisplayInfo.iNormal.iHeight);
	return KErrNone;
	}

TInt DDisplayPddA64::CreateChannelSetup(TInt aUnit)
	{
	DLcdPowerHandler* lcd = DLcdPowerHandler::pLcd;
	ekseam_puts("[EKERN][disp] display channel setup, composition RAM "); ekseam_puthex(lcd->iCompositionPhysical); A64DbgFlush();
	const TVideoInfoV01& vi = lcd->iVideoInfo;
	iLdd->iUnit = aUnit;
	// NORMAL + 90 + 270, deliberately NOT 180 (sensors S2b, 2026-10-10, boots 215/216). Two consumers, two tests:
	// (1) the window server's display policy accepts a rotated wsini screen mode only if the display lists that
	// rotation (CDisplayPolicy intersects the mode's rotations with the display's; normal-only = the landscape mode
	// cannot be configured and the screen went black on boot 216); (2) the OpenWF reference compositor takes the
	// HARDWARE-rotation path only if ALL FOUR rotations are listed (owfscreen.cpp OWF_Screen_Rotation_Supported:
	// "(supported & 0xF) == 0xF"), composing a flipped-size buffer the DE cannot rotate (the UI froze on boot 215);
	// with fewer it rotates the scene itself into the panel's native 360x640 buffer. SetRotation is then bookkeeping.
	iLdd->iDisplayInfo.iAvailableRotations = RDisplayChannel::ERotationNormal | RDisplayChannel::ERotation90CW
		| RDisplayChannel::ERotation270CW;
	iLdd->iDisplayInfo.iNormal.iOffsetBetweenLines = vi.iOffsetBetweenLines;
	iLdd->iDisplayInfo.iNormal.iHeight = vi.iSizeInPixels.iHeight;
	iLdd->iDisplayInfo.iNormal.iWidth = vi.iSizeInPixels.iWidth;
	// the 90/270-degree geometry (sensors S2b, boot 216): the OpenWF compositor sizes its internal target from the
	// larger of normal/flipped and composes a rotated scene through the flipped size -- all zero = a black screen
	iLdd->iDisplayInfo.iFlipped.iWidth = vi.iSizeInPixels.iHeight;
	iLdd->iDisplayInfo.iFlipped.iHeight = vi.iSizeInPixels.iWidth;
	iLdd->iDisplayInfo.iFlipped.iOffsetBetweenLines = vi.iSizeInPixels.iHeight * 4;
	iLdd->iDisplayInfo.iNumCompositionBuffers = KDisplayCBMax;
	// the buffers are 4 bytes a pixel (XRGB8888, iOffsetBetweenLines = 4 * width) whatever the legacy mode reports
	iLdd->iDisplayInfo.iBitsPerPixel = 32;
	iLdd->iDisplayInfo.iRefreshRateHz = 60;
	iLdd->iDisplayInfo.iPixelFormat = EUidPixelFormatXRGB_8888;
	iLdd->iCurrentRotation = RDisplayChannel::ERotationNormal;

	// the composition buffers: a shared chunk over DLcdPowerHandler's contiguous allocation. EMapAttrBufferedC = the
	// legacy video RAM's attributes (the DE scans it, nobody flushes) -- one memory type for every mapping of this RAM
	// (an alias of another type hangs the phone: phonelab.md; Syborg uses EMapAttrFullyBlocking here)
	const TUint size = KDisplayCBMax * Kern::RoundToPageSize(lcd->iSize);	// (3: lddinc/display.h; svpfb_patch.py allocates them)
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = size;
	info.iMapAttr = EMapAttrBufferedC;
	info.iOwnsMemory = EFalse;
	info.iDestroyedDfc = 0;
	DChunk* chunk = 0;
	TLinAddr lin = 0;
	TUint32 attr = 0;
	TInt r = Kern::ChunkCreate(info, chunk, lin, attr);
	if (r == KErrNone)
		{
		r = Kern::ChunkCommitPhysical(chunk, 0, size, lcd->iCompositionPhysical);
		if (r != KErrNone)
			Kern::ChunkClose(chunk);
		}
	if (r != KErrNone)
		return r;
	iChunk = chunk;
	ekseam_puts("[EKERN][disp] composition chunk committed lin="); ekseam_puthex(lin); A64DbgFlush();
	// The composition RAM comes straight from the page allocator and a warm reset (every push) keeps RAM: the first flips of
	// a new boot would show the previous boot's last composed frames, and any area libWFC never redraws keeps them (the
	// "imprint under Haze after golden -> slot B"). Black it, and clean the cache, before the DE can be pointed at it.
	memclr((TAny*)lin, size);
	Cache::SyncMemoryBeforeDmaWrite(lin, size);
	ekseam_puts("[EKERN][disp] composition buffers cleared\n"); A64DbgFlush();
	for (TInt i = 0; i < KDisplayCBMax; ++i)
		{
		TBufferNode& b = iLdd->iCompositionBuffer[i];
		b.iType = EBufferTypeComposition;
		b.iBufferId = i;
		b.iFree = ETrue;
		b.iState = EBufferFree;
		b.iAddress = lin + i * lcd->iSize;
		b.iPhysicalAddress = lcd->iCompositionPhysical + i * lcd->iSize;
		b.iChunk = chunk;
		b.iHandle = 0;
		b.iOffset = i * lcd->iSize;
		b.iSize = lcd->iSize;
		b.iPendingRequest = 0;
		}
	iLdd->iCompositionBuffIdx = 0;
	iDfcQ = lcd->iDfcQ;						// all display hardware access on DLcdPowerHandler's DFC thread
	iVSyncDfc.SetDfcQ(iDfcQ);
	A64DisplaySetVblankDfc(&iVSyncDfc);
	A64DisplayChannelOwns(ETrue);			// legacy FB_BASE writes (PowerUpLcd on every wake) no longer take the DE
	ekseam_puts("[EKERN][disp] display channel open (DDisplayPddA64, vblank-paced posts)\n"); A64DbgFlush();
	return KErrNone;
	}

// The previous active buffer is released, the pending one becomes active (Syborg's VSyncDfcFn, unchanged logic).
void DDisplayPddA64::VSyncDfcFn(TAny* aSelf)
	{
	DDisplayPddA64* self = (DDisplayPddA64*)aSelf;
	DDisplayLdd* ldd = self->iLdd;
	if (self->iPosts <= 2)
		ekseam_puts("[EKERN][disp] pdd post done\n"); A64DbgFlush();
	if (self->iActiveBuffer)
		{
		if (self->iActiveBuffer->iType == EBufferTypeUser)
			self->RequestComplete(RDisplayChannel::EReqPostUserBuffer, KErrNone);
		else
			self->iActiveBuffer->iFree = ETrue;
		self->iActiveBuffer->iState = EBufferFree;
		// a GetCompositionBuffer that found no free buffer was handed the active one: it is free to draw now
		TInt idx = ldd->iPendingIndex[RDisplayChannel::EReqGetCompositionBuffer];
		TClientRequest* req = ldd->iPendingReq[RDisplayChannel::EReqGetCompositionBuffer][idx].iTClientReq;
		if (req && req->IsReady())
			{
			self->iActiveBuffer->iState = EBufferCompose;
			self->RequestComplete(RDisplayChannel::EReqGetCompositionBuffer, KErrNone);
			}
		self->iActiveBuffer = NULL;
		}
	if (self->iPendingBuffer)
		{
		self->iActiveBuffer = self->iPendingBuffer;
		self->iActiveBuffer->iState = EBufferActive;
		self->iPendingBuffer = NULL;
		self->RequestComplete(RDisplayChannel::EReqWaitForPost, KErrNone);
		}
	// the queued post takes the flip just freed: armed now, latched at the next frame start
	if (self->iQueuedBuffer)
		{
		TBufferNode* next = self->iQueuedBuffer;
		self->iQueuedBuffer = NULL;
		self->Show(next, self->iQueuedPhys, self->iQueuedLin);
		}
	}

class DDisplayPddFactoryA64 : public DDisplayPddFactory
	{
public:
	virtual TInt Create(DBase*& aChannel, TInt, const TDesC8*, const TVersion&)
		{
		ekseam_puts("[EKERN][disp] DDisplayPddA64 created\n"); A64DbgFlush();
		aChannel = new DDisplayPddA64;
		return aChannel ? KErrNone : KErrNoMemory;
		}
	};

// The display channel (NGA, rung G1a): Syborg's PDD completes every post at once (its device has no vblank) and
// re-points the DE through FB_BASE -> A64DisplaySetSource, whose capture mapping (EMapAttrBufferedC) aliased the
// strongly-ordered composition chunk and hung the phone hard on the first channel open (2026-10-09, the
// phonelab.md alias lesson again). The board glue (tools/kernel/ekern_fb_glue.cpp) installs this factory instead.
extern "C" DPhysicalDevice* ekseam_display_pdd_factory()
	{
	return new DDisplayPddFactoryA64;
	}
