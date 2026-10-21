/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// rphedeov.h -- OURS (DE2 overlay plan): the user-side API of phedeov, our kernel driver
// for the Allwinner A64 Display Engine 2 VI (video) channel -- the plane that scans an NV12 picture out directly, with its own
// scaler (VSU) and YUV->RGB converter (CCSC), blended over the UI. Built into ekern (a64deov.cpp), like phevde.
//   GetInfo  -> panel size, source limits, the overlay state, vblank / flip counters
//   Set      -> program the plane for a picture (source pointers, size, destination rectangle, alpha, colour standard)
//               and show it at the next vblank; also used to move / resize
//   Flip     -> (async) point the plane at the next picture of the same geometry; completes at the vblank that latched it
//   Disable  -> hide the plane (the buffers stay referenced until the next vblank passed)
//   ReadReg  -> one mixer-0 register (VI channel, blender, VI scaler windows) for tests
// The picture is NOT given as a physical address (never trusted from user mode): the caller passes the ADDRESS of the first
// byte of the Y plane and of the UV plane in a shared chunk it owns (the VE arena of RPheVde, a physically contiguous uncached
// chunk); the driver resolves the chunk, checks the whole plane is contiguous, and holds a reference to it while it is shown.
#ifndef RPHEDEOV_H
#define RPHEDEOV_H

#include <e32cmn.h>
#ifndef __KERNEL_MODE__
#include <e32std.h>
#endif

_LIT(KPheDeOvLddName, "PheDeOv");

enum TPheDeOvFormat { EPheDeOvNV12 = 0, EPheDeOvNV21 = 1 };		// YUV 4:2:0 semi-planar (UV / VU interleaved)
enum TPheDeOvColor { EPheDeOvBT601 = 0, EPheDeOvBT709 = 1, EPheDeOvAuto = 2 };	// Auto: BT.709 from 1280 wide up, else BT.601
enum TPheDeOvRotation { EPheDeOvRot0 = 0 };		// the DE2 has no rotator: 90/180/270 (a G2D pass) is rung V6b -> KErrNotSupported

struct TPheDeOvInfo
	{
	TInt iPanelW, iPanelH;			// the DE canvas (the panel's native pixels)
	TInt iMaxSrcW, iMaxSrcH;		// VI limits
	TInt iEnabled;					// the plane is shown
	TUint32 iVblanks;				// TCON0 frame counter since boot
	TUint32 iFlips;					// Set/Flip calls that reached the hardware
	TUint32 iFlipsDone;				// ... that completed on a vblank
	TInt iScaleMode;				// 0 = none/off, 1 = fine scaling only, 2 = vertical coarse, 3 = horizontal coarse, 4 = both
	};

// Set: the source Y/UV plane addresses are in the CALLER's address space (a shared chunk).
struct TPheDeOvSet
	{
	const TUint8* iY;				// first byte of the (cropped) source, Y plane
	const TUint8* iUV;				// first byte of the matching UV row/column (even x, even y of the Y origin)
	TInt iPitch;					// bytes per line of both planes
	TInt iSrcW, iSrcH;				// source size in pixels (even)
	TInt iDstX, iDstY, iDstW, iDstH;	// destination rectangle on the PANEL (720x1440)
	TInt iFormat;					// TPheDeOvFormat
	TInt iColor;					// TPheDeOvColor
	TInt iFullRange;				// 0 = limited (16..235), 1 = full
	TInt iAlpha;					// 0..255 plane alpha
	TInt iRotation;					// TPheDeOvRotation
	};

struct TPheDeOvFlip
	{
	const TUint8* iY;				// same geometry as the last Set, the next picture
	const TUint8* iUV;
	};

enum TPheDeOvControl { EPheDeOvCtrlGetInfo, EPheDeOvCtrlSet, EPheDeOvCtrlDisable, EPheDeOvCtrlReadReg };
enum TPheDeOvRequest { EPheDeOvReqFlip, EPheDeOvReqCount };
const TInt KPheDeOvMajorVersion = 1;

// The registers ReadReg may see, as offsets from the mixer-0 base (DE + 0x100000): the mixer proper (0..0x5FFF: global,
// blender, VI channel at 0x2000, UI channels, the CCSC at 0xAA050 is outside -- see KPheDeOvRegCcsc) and the VI scaler.
const TUint32 KPheDeOvRegCcsc = 0xAA050;		// ReadReg also allows 0xAA000..0xAA0FF (FCC global alpha 0xAA090, CCSC 0xAA050..0xAA07F)
const TUint32 KPheDeOvRegVsu = 0x20000;			// VI scaler 0x20000..0x20FFF

#ifndef __KERNEL_MODE__
class RPheDeOv : public RBusLogicalChannel
	{
public:
	inline static TVersion VersionRequired() { return TVersion(KPheDeOvMajorVersion, 0, 0); }
	inline TInt Open() { return DoCreate(KPheDeOvLddName, VersionRequired(), KNullUnit, NULL, NULL, EOwnerThread); }
	inline TInt GetInfo(TPheDeOvInfo& aInfo)
		{ TPckg<TPheDeOvInfo> p(aInfo); return DoControl(EPheDeOvCtrlGetInfo, &p); }
	inline TInt Set(const TPheDeOvSet& aSet)
		{ TPckgC<TPheDeOvSet> p(aSet); return DoControl(EPheDeOvCtrlSet, (TAny*)&p); }
	inline TInt Disable() { return DoControl(EPheDeOvCtrlDisable); }
	inline TInt ReadReg(TUint32 aOffInMixer, TUint32& aVal)
		{ TPckg<TUint32> p(aVal); return DoControl(EPheDeOvCtrlReadReg, (TAny*)aOffInMixer, &p); }
	inline void Flip(const TPheDeOvFlip& aFlip, TRequestStatus& aStatus)
		{ DoRequest(EPheDeOvReqFlip, aStatus, (TAny*)&aFlip); }
	inline void CancelFlip() { DoCancel(0xFFFFFFFF); }
	};
#endif

#endif
