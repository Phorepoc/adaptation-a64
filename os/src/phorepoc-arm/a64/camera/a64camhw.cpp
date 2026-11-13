// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
//
// a64camhw.cpp -- the "CamHw.A64" LDD (contract: a64camhw_api.h): the thin hardware channel of the user-side camera
// server. Everything camera-specific above register level (sensor drivers, CSI programming, the buffer queue) lives in
// that server (the camera-a64 Catalogue package); this driver only hands out the camera block's own resources, each
// from a fixed list: the CSI register window and contiguous DMA memory as shared chunks, the camera clocks/reset in the
// CCU, the CSI pin function, six sensor GPIOs, three AXP803 rails (power/a64_rail.cpp) and the CSI interrupt.
// Interrupt model: the ISR masks the line and queues a DFC; the DFC completes the client's pending ERequestIrq; the
// client runs its handler and unmasks with EControlIrqEnable. An interrupt with no request pending is latched and
// completes the next request at once.
// Register facts: A64 user manual (CCU, PIO) -- the same offsets Linux's sunxi-ng and sunxi pinctrl drivers use.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <kernel/arm/assp.h>
#include "a64camhw_api.h"
#include "../power/a64_rail.h"

extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(TUint32);

static const TPhysAddr KPhysCsi = 0x01CB0000, KPhysCcu = 0x01C20000, KPhysPio = 0x01C20800;
static const TInt KIntCsi = 116;	// GIC SPI 84 + 32
_LIT(KDfcQName, "A64CamHw");

// the sensor GPIOs the channel may drive (bank, pin): PE12/PE13 = camera I2C (bit-banged), PE16/PE17 = GC2145
// reset/enable, PD3 = OV5640 reset, PC0 = OV5640 powerdown
static const TUint8 KGpios[][2] = { { 4, 12 }, { 4, 13 }, { 4, 16 }, { 4, 17 }, { 3, 3 }, { 2, 0 } };

class DA64CamHwDevice : public DLogicalDevice
	{
public:
	DA64CamHwDevice() { iVersion = TVersion(RA64CamHw::EMajorVersion, 0, 0); iParseMask = 0; }
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const { (void)aDes; }
	virtual TInt Create(DLogicalChannelBase*& aChannel);
	TDfcQue* iDfcQ;
	TBool iOpen;
	};

class DA64CamHwChannel : public DLogicalChannelBase
	{
public:
	DA64CamHwChannel(DA64CamHwDevice* aDev);
	virtual ~DA64CamHwChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aReqNo, TAny* a1, TAny* a2);
	static void Isr(TAny* aPtr);
	static void IrqDfc(TAny* aPtr);
private:
	TInt MapCsi();
	TInt AllocDma(TAny* aPckg);
	TInt Clock(TInt aId, TBool aOn);
	TInt ClockRate(TInt aId, TUint32 aHz);
	TUint32 ClockGetRate(TInt aId);
	TInt Gpio(TAny* aPckg);
	void Pinmux();
	volatile TUint32* Ccu(TUint aOff) { return (volatile TUint32*)(iCcu->LinearAddress() + aOff); }
	volatile TUint32* Pio(TUint aBank, TUint aOff) { return (volatile TUint32*)(iPio->LinearAddress() + aBank * 0x24 + aOff); }
	void PioFunc(TUint aBank, TUint aPin, TUint aFunc);
private:
	DA64CamHwDevice* iDev;
	DThread* iClient;
	DPlatChunkHw* iCcu;
	DPlatChunkHw* iPio;
	TClientRequest* iIrqReq;
	TDfc iDfc;
	TBool iBound;
	TBool iLatched;
	TInt iRails[EA64RailCount];
	TUint8 iOpenDrain[5][32];
	};

static DA64CamHwDevice* TheDevice;

TInt DA64CamHwDevice::Install()
	{
	TInt r = Kern::DfcQCreate(iDfcQ, 27, &KDfcQName);
	return r == KErrNone ? SetName(&KA64CamHwLddName) : r;
	}

TInt DA64CamHwDevice::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DA64CamHwChannel(this);
	return aChannel ? KErrNone : KErrNoMemory;
	}

DA64CamHwChannel::DA64CamHwChannel(DA64CamHwDevice* aDev)
	: iDev(aDev), iClient(NULL), iCcu(NULL), iPio(NULL), iIrqReq(NULL), iDfc(IrqDfc, this, aDev->iDfcQ, 1),
	  iBound(EFalse), iLatched(EFalse)
	{
	memclr(iRails, sizeof(iRails));
	memclr(iOpenDrain, sizeof(iOpenDrain));
	}

TInt DA64CamHwChannel::DoCreate(TInt, const TDesC8*, const TVersion& aVer)
	{
	if (!Kern::CurrentThreadHasCapability(ECapabilityCommDD, __PLATSEC_DIAGNOSTIC_STRING("CamHw.A64 needs CommDD")))
		return KErrPermissionDenied;
	if (!Kern::QueryVersionSupported(TVersion(RA64CamHw::EMajorVersion, 0, 0), aVer))
		return KErrNotSupported;
	if (iDev->iOpen)
		return KErrInUse;
	iClient = &Kern::CurrentThread();
	iClient->Open();
	TInt r = Kern::CreateClientRequest(iIrqReq);
	if (r == KErrNone)
		r = DPlatChunkHw::New(iCcu, KPhysCcu, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r == KErrNone)
		r = DPlatChunkHw::New(iPio, KPhysPio, 0x400, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r == KErrNone && (r = Interrupt::Bind(KIntCsi, Isr, this)) >= 0)
		{
		iBound = ETrue;
		r = KErrNone;
		}
	if (r == KErrNone)
		iDev->iOpen = ETrue;
	return r;
	}

DA64CamHwChannel::~DA64CamHwChannel()
	{
	if (iBound)
		{
		Interrupt::Disable(KIntCsi);
		Interrupt::Unbind(KIntCsi);
		}
	iDfc.Cancel();
	for (TInt i = 0; i < EA64RailCount; ++i)
		while (iRails[i]-- > 0)
			A64RailSet(i, EFalse);
	if (iCcu)
		{
		*Ccu(0x2C4) &= ~(1u << 8);			// CSI back into reset, clocks off
		*Ccu(0x134) &= ~((1u << 31) | (1u << 15));
		*Ccu(0x064) &= ~(1u << 8);
		iCcu->Close(NULL);
		}
	if (iPio)
		iPio->Close(NULL);
	if (iIrqReq)
		{
		if (iIrqReq->IsReady())
			Kern::QueueRequestComplete(iClient, iIrqReq, KErrCancel);
		Kern::DestroyClientRequest(iIrqReq);
		}
	if (iClient)
		Kern::SafeClose((DObject*&)iClient, NULL);
	iDev->iOpen = EFalse;
	}

void DA64CamHwChannel::Isr(TAny* aPtr)
	{
	DA64CamHwChannel* c = (DA64CamHwChannel*)aPtr;
	Interrupt::Disable(KIntCsi);
	c->iDfc.Add();
	}

void DA64CamHwChannel::IrqDfc(TAny* aPtr)
	{
	DA64CamHwChannel* c = (DA64CamHwChannel*)aPtr;
	if (c->iIrqReq->IsReady())
		Kern::QueueRequestComplete(c->iClient, c->iIrqReq, KErrNone);
	else
		c->iLatched = ETrue;
	}

// the CSI register window, as a shared chunk over its physical page (memory-mapped I/O: uncached, strongly ordered)
TInt DA64CamHwChannel::MapCsi()
	{
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = 0x1000;
	info.iMapAttr = EMapAttrUserRw | EMapAttrFullyBlocking;
	info.iOwnsMemory = EFalse;
	info.iDestroyedDfc = NULL;
	DChunk* chunk;
	TLinAddr kaddr;
	TUint32 attr;
	NKern::ThreadEnterCS();
	TInt r = Kern::ChunkCreate(info, chunk, kaddr, attr);
	if (r == KErrNone)
		{
		r = Kern::ChunkCommitPhysical(chunk, 0, 0x1000, KPhysCsi);
		if (r == KErrNone)
			r = Kern::MakeHandleAndOpen(NULL, chunk);
		Kern::ChunkClose(chunk);
		}
	NKern::ThreadLeaveCS();
	return r;
	}

// physically contiguous memory as a shared chunk (the chunk owns it: closing the last handle frees it)
TInt DA64CamHwChannel::AllocDma(TAny* aPckg)
	{
	TA64CamHwDma d;
	TPckg<TA64CamHwDma> p(d);
	TInt r = Kern::ThreadDesRead(iClient, aPckg, p, 0, KChunkShiftBy0);
	if (r != KErrNone)
		return r;
	if (d.iSize == 0 || d.iSize > 16 * 1024 * 1024)
		return KErrArgument;
	TUint size = Kern::RoundToPageSize(d.iSize);
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = size;
	info.iMapAttr = EMapAttrUserRw | EMapAttrFullyBlocking;	// uncached: the CSI writes behind the CPU's back
	info.iOwnsMemory = ETrue;
	info.iDestroyedDfc = NULL;
	DChunk* chunk;
	TLinAddr kaddr;
	TUint32 attr;
	TPhysAddr phys = 0;
	NKern::ThreadEnterCS();
	r = Kern::ChunkCreate(info, chunk, kaddr, attr);
	if (r == KErrNone)
		{
		r = Kern::ChunkCommitContiguous(chunk, 0, size, phys);
		if (r == KErrNone)
			r = Kern::MakeHandleAndOpen(NULL, chunk);
		Kern::ChunkClose(chunk);
		}
	NKern::ThreadLeaveCS();
	if (r < 0)
		return r;
	d.iPhys = phys;
	TInt w = Kern::ThreadDesWrite(iClient, aPckg, p, 0, KChunkShiftBy0, iClient);
	return w == KErrNone ? r : w;
	}

TInt DA64CamHwChannel::Clock(TInt aId, TBool aOn)
	{
	TUint off, bit;
	switch (aId)
		{
		case EA64ClkBusCsi: off = 0x064; bit = 8; break;	// BUS_CLK_GATING_REG1
		case EA64ClkDramCsi: off = 0x100; bit = 1; break;	// DRAM_CLK_GATING_REG
		case EA64ClkCsiMisc: off = 0x130; bit = 31; break;	// CSI_MISC_CLK_REG
		case EA64ClkCsiSclk: off = 0x134; bit = 31; break;	// CSI_CLK_REG: SCLK gate
		case EA64ClkCsiMclk: off = 0x134; bit = 15; break;	// CSI_CLK_REG: MCLK gate
		default: return KErrArgument;
		}
	volatile TUint32* r = Ccu(off);
	*r = aOn ? (*r | (1u << bit)) : (*r & ~(1u << bit));
	return KErrNone;
	}

TInt DA64CamHwChannel::ClockRate(TInt aId, TUint32 aHz)
	{
	if (aHz == 0)
		return KErrArgument;
	volatile TUint32* r = Ccu(0x134);
	TUint32 m;
	switch (aId)
		{
		case EA64ClkCsiSclk:		// parent pll-periph0 (600 MHz, mux [26:24] = 0), M [19:16]
			m = (600000000u + aHz - 1) / aHz - 1;
			if (m > 15) m = 15;
			*r = (*r & ~((7u << 24) | (0xFu << 16))) | (m << 16);
			return KErrNone;
		case EA64ClkCsiMclk:		// parent osc24M (mux [10:8] = 0), M [4:0]
			m = (24000000u + aHz - 1) / aHz - 1;
			if (m > 31) m = 31;
			*r = (*r & ~((7u << 8) | 0x1Fu)) | m;
			return KErrNone;
		case EA64ClkBusCsi: case EA64ClkDramCsi: case EA64ClkCsiMisc:
			return KErrNone;	// fixed parents
		default:
			return KErrArgument;
		}
	}

TUint32 DA64CamHwChannel::ClockGetRate(TInt aId)
	{
	TUint32 v = *Ccu(0x134);
	switch (aId)
		{
		case EA64ClkCsiSclk: return 600000000u / (((v >> 16) & 0xF) + 1);
		case EA64ClkCsiMclk: return 24000000u / ((v & 0x1F) + 1);
		default: return 0;
		}
	}

void DA64CamHwChannel::PioFunc(TUint aBank, TUint aPin, TUint aFunc)
	{
	volatile TUint32* cfg = Pio(aBank, (aPin / 8) * 4);
	TUint sh = (aPin % 8) * 4;
	*cfg = (*cfg & ~(7u << sh)) | (aFunc << sh);
	}

void DA64CamHwChannel::Pinmux()
	{
	for (TUint pin = 0; pin <= 11; ++pin)		// PE0-PE11: PCLK, MCLK, HSYNC, VSYNC, D0-D7 (function 2 = CSI)
		PioFunc(4, pin, 2);
	}

// open drain as Linux's gpiolib emulates it: drive low = output 0, high = input (the line has a pull-up)
TInt DA64CamHwChannel::Gpio(TAny* aPckg)
	{
	TA64CamHwGpio g;
	TPckg<TA64CamHwGpio> p(g);
	TInt r = Kern::ThreadDesRead(iClient, aPckg, p, 0, KChunkShiftBy0);
	if (r != KErrNone)
		return r;
	TBool ok = EFalse;
	for (TUint i = 0; i < sizeof(KGpios) / sizeof(KGpios[0]); ++i)
		ok |= (KGpios[i][0] == g.iBank && KGpios[i][1] == g.iPin);
	if (!ok)
		return KErrAccessDenied;
	volatile TUint32* dat = Pio(g.iBank, 0x10);
	switch (g.iOp & 1)
		{
		case EA64GpioInput:
			if (!iOpenDrain[g.iBank][g.iPin] || g.iOp == EA64GpioInput)
				PioFunc(g.iBank, g.iPin, 0);
			return (*dat >> g.iPin) & 1;
		default:
			iOpenDrain[g.iBank][g.iPin] = (g.iOp & EA64GpioOpenDrain) ? 1 : 0;
			if (iOpenDrain[g.iBank][g.iPin] && g.iValue)
				{
				PioFunc(g.iBank, g.iPin, 0);	// released: the pull-up takes the line high
				return KErrNone;
				}
			*dat = g.iValue ? (*dat | (1u << g.iPin)) : (*dat & ~(1u << g.iPin));
			PioFunc(g.iBank, g.iPin, 1);
			return KErrNone;
		}
	}

TInt DA64CamHwChannel::Request(TInt aReqNo, TAny* a1, TAny* a2)
	{
	if (&Kern::CurrentThread() != iClient)
		return KErrAccessDenied;
	if (aReqNo < 0 && aReqNo != ~RA64CamHw::ERequestIrq && aReqNo != KMaxTInt)
		return KErrNotSupported;
	if (aReqNo == ~RA64CamHw::ERequestIrq)		// DoRequest(ERequestIrq): a1 = the TRequestStatus
		{
		TInt r = iIrqReq->SetStatus((TRequestStatus*)a1);
		if (r != KErrNone)
			return r;
		if (iLatched)
			{
			iLatched = EFalse;
			Kern::QueueRequestComplete(iClient, iIrqReq, KErrNone);
			}
		return KErrNone;
		}
	if (aReqNo == KMaxTInt)				// DoCancel
		{
		if (((TUint)a1 & (1u << RA64CamHw::ERequestIrq)) && iIrqReq->IsReady())
			Kern::QueueRequestComplete(iClient, iIrqReq, KErrCancel);
		return KErrNone;
		}
	switch (aReqNo)
		{
		case RA64CamHw::EControlMapCsi: return MapCsi();
		case RA64CamHw::EControlAllocDma: return AllocDma(a1);
		case RA64CamHw::EControlClock: return Clock((TInt)a1, (TBool)a2);
		case RA64CamHw::EControlClockRate: return ClockRate((TInt)a1, (TUint32)a2);
		case RA64CamHw::EControlClockGetRate:
			{
			TUint32 hz = ClockGetRate((TInt)a1);
			return Kern::ThreadRawWrite(iClient, a2, &hz, sizeof(hz));
			}
		case RA64CamHw::EControlReset:			// BUS_SOFT_RST_REG1 bit 8 (1 = released)
			if ((TInt)a1 != EA64RstBusCsi)
				return KErrArgument;
			*Ccu(0x2C4) = a2 ? (*Ccu(0x2C4) & ~(1u << 8)) : (*Ccu(0x2C4) | (1u << 8));
			return KErrNone;
		case RA64CamHw::EControlGpio: return Gpio(a1);
		case RA64CamHw::EControlRail:
			{
			TInt rail = (TInt)a1;
			if (rail < 0 || rail >= EA64RailCount)
				return KErrArgument;
			if (!a2 && iRails[rail] == 0)
				return KErrNone;
			TInt r = A64RailSet(rail, (TBool)a2);
			if (r == KErrNone)
				iRails[rail] += a2 ? 1 : -1;
			return r;
			}
		case RA64CamHw::EControlIrqEnable:
			if (a2)
				Interrupt::Enable(KIntCsi);
			else
				Interrupt::Disable(KIntCsi);
			return KErrNone;
		case RA64CamHw::EControlPinmux: Pinmux(); return KErrNone;
		default: return KErrNotSupported;
		}
	}

extern "C" void A64CamHwInstall()
	{
	TheDevice = new DA64CamHwDevice;
	TInt r = TheDevice ? Kern::InstallLogicalDevice(TheDevice) : KErrNoMemory;
	ekseam_puts("[EKERN][camera] InstallLogicalDevice(CamHw.A64) r="); ekseam_puthex(r);
	}
