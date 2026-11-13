/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// a64camhw_api.h -- the user/kernel contract of "CamHw.A64" (a64camhw.cpp): the thin, generic hardware channel the
// camera server (the camera-a64 Catalogue package, a user-side process) drives the A64 camera block through. The
// camera's logic (sensor drivers, CSI programming, buffer queue) runs in that server; this channel only gives it, for
// the camera's own resources and nothing else:
//   - the CSI register window (0x01CB0000, 4 KiB) as a shared chunk mapped into the server;
//   - physically contiguous, uncached DMA buffers as shared chunks (the CSI has no IOMMU), with their physical address;
//   - the camera's clocks and reset in the CCU, the CSI pin function (PE0-PE11), the sensor GPIOs (PE12/PE13 = the
//     bit-banged camera I2C, PE16/PE17 = GC2145 reset/enable, PD3/PC0 = OV5640 reset/powerdown);
//   - the camera's AXP803 rails (A64RailSet, power/a64_rail.cpp);
//   - the CSI interrupt: a request that completes once per interrupt (ISR masks the line, a DFC completes the
//     request); the server runs its handler and re-enables the line with EControlIrqEnable.
// The channel needs ECapabilityCommDD and one client at a time.
#ifndef A64CAMHW_API_H
#define A64CAMHW_API_H
#include <e32cmn.h>
#include <e32ver.h>
#ifndef __KERNEL_MODE__
#include <e32std.h>
#endif

_LIT(KA64CamHwLddName, "CamHw.A64");

// clocks (the Linux sun50i-a64-ccu dt-binding numbers, which the server's device tree uses) and the reset
enum TA64CamClock { EA64ClkBusCsi = 50, EA64ClkDramCsi = 96, EA64ClkCsiMisc = 103, EA64ClkCsiSclk = 104, EA64ClkCsiMclk = 105 };
enum TA64CamReset { EA64RstBusCsi = 27 };
// rails: power/a64_rail.h
enum TA64CamGpioOp { EA64GpioInput = 0, EA64GpioOutput = 1, EA64GpioOpenDrain = 2 };

struct TA64CamHwGpio { TUint8 iBank, iPin, iOp, iValue; };	// bank: 0 = PA ... 4 = PE
struct TA64CamHwDma { TUint32 iSize; TUint32 iPhys; };		// in: size; out: the physical address

class RA64CamHw : public RBusLogicalChannel
	{
public:
	enum { EMajorVersion = 1 };
	enum TControl
		{
		EControlMapCsi = 0,		// returns a chunk handle: the CSI register window
		EControlAllocDma = 1,		// a1 = TPckg<TA64CamHwDma>*; returns a chunk handle (closing it frees the memory)
		EControlClock = 2,		// a1 = clock id, a2 = 1 on / 0 off
		EControlClockRate = 3,		// a1 = clock id, a2 = rate in Hz (SCLK, MCLK); returns KErrNone
		EControlClockGetRate = 4,	// a1 = clock id, a2 = TUint32* out
		EControlReset = 5,		// a1 = reset id, a2 = 1 assert / 0 release
		EControlGpio = 6,		// a1 = TPckg<TA64CamHwGpio>*; for EA64GpioInput returns the level
		EControlRail = 7,		// a1 = TA64Rail, a2 = 1 on / 0 off
		EControlIrqEnable = 8,		// a2 = 1 unmask / 0 mask the CSI interrupt
		EControlPinmux = 9		// the CSI pins to their CSI function
		};
	enum TRequest { ERequestIrq = 0, ERequestCount = 1 };
#ifndef __KERNEL_MODE__
	inline TInt Open() { return DoCreate(KA64CamHwLddName, TVersion(EMajorVersion, 0, 0), KNullUnit, NULL, NULL, EOwnerProcess); }
	inline TInt MapCsi(RChunk& aChunk) { TInt h = DoControl(EControlMapCsi); return h < 0 ? h : aChunk.SetReturnedHandle(h); }
	inline TInt AllocDma(RChunk& aChunk, TUint32 aSize, TUint32& aPhys)
		{
		TA64CamHwDma d = { aSize, 0 };
		TPckg<TA64CamHwDma> p(d);
		TInt h = DoControl(EControlAllocDma, &p);
		if (h < 0)
			return h;
		aPhys = d.iPhys;
		return aChunk.SetReturnedHandle(h);
		}
	inline TInt Clock(TInt aId, TBool aOn) { return DoControl(EControlClock, (TAny*)aId, (TAny*)aOn); }
	inline TInt SetClockRate(TInt aId, TUint32 aHz) { return DoControl(EControlClockRate, (TAny*)aId, (TAny*)aHz); }
	inline TInt ClockRate(TInt aId, TUint32& aHz) { return DoControl(EControlClockGetRate, (TAny*)aId, &aHz); }
	inline TInt Reset(TInt aId, TBool aAssert) { return DoControl(EControlReset, (TAny*)aId, (TAny*)aAssert); }
	inline TInt Gpio(TA64CamHwGpio& aGpio) { TPckg<TA64CamHwGpio> p(aGpio); return DoControl(EControlGpio, &p); }
	inline TInt Rail(TInt aRail, TBool aOn) { return DoControl(EControlRail, (TAny*)aRail, (TAny*)aOn); }
	inline TInt IrqEnable(TBool aOn) { return DoControl(EControlIrqEnable, NULL, (TAny*)aOn); }
	inline TInt Pinmux() { return DoControl(EControlPinmux); }
	inline void WaitIrq(TRequestStatus& aStatus) { DoRequest(ERequestIrq, aStatus); }
	inline void WaitIrqCancel() { DoCancel(1u << ERequestIrq); }
#endif
	};

#endif
