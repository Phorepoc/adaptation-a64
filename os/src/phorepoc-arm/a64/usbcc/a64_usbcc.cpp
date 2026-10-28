// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// a64_usbcc.cpp -- OURS (phone USB step 2): the USB device controller
// PSL for the Allwinner A64's USB OTG port, under the REAL 2010 USB client PIL (kernelhwsrv/kernel/eka/drivers/usbcc:
// chapter9.cpp, descriptors.cpp, misc.cpp, ps_usbc.cpp, queue.cpp -- compiled unmodified next to this file).
//
// The A64's OTG controller is a Mentor Graphics MUSB core -- the same IP as the BeagleBoard's (OMAP3530), whose 2010
// PSL (oss.FCL.sf.adapt.beagleboard/omap3530/omap3530_drivers/usbcc/pa_usbc.cpp, EPL) is this file's template: the
// class shape, the real-endpoint numbering (index 2k = OUT on hardware endpoint k, 2k+1 = IN on k), the PIL upcalls.
// What differs:
//  * Allwinner's MUSB register layout (Linux drivers/usb/musb/sunxi.c): common registers moved (POWER 0x40, INDEX
//    0x42, interrupts 0x44.., FADDR 0x98), the indexed endpoint block at 0x80, FIFOs at 4*ep, and the interrupt status
//    registers are WRITE-1-TO-CLEAR (standard MUSB clears them on read).
//  * The PHY / clocks / VBUS: no OMAP PRCM + TWL PHY; the A64's CCU gates and PHY0 (Linux phy-sun4i-usb.c,
//    sun50i_a64_cfg). VBUS valid and ID are FORCED in the PHY's ISCR (the AXP803's VBUS sense is not wired to us).
//  * Endpoint 0 follows the MUSB peripheral protocol as Linux musb_gadget_ep0.c does (ServicedRxPktRdy withheld until
//    the PIL has decided: + DataEnd for a no-data request, + SendStall to refuse, alone before an IN data stage; the
//    new address written to FADDR only after the status stage). The BeagleBoard PSL acked setups at once and sent
//    the status stage with TxPktRdy -- not what the core expects.
//  * High speed since 2026-10-12 (480 Mbit/s, 512-byte bulk packets; was full speed); bulk OUT completes per packet;
//    PIO only (no DMA).
//  * SMP: the PIL calls in from the USB DFC thread while the ISR runs on CPU 0 -- one spin lock around register
//    access (the ISR path, incl. PIL upcalls that call back into us, runs with it held).

#include <drivers/usbc.h>
#include <kernel/kern_priv.h>
#include "syborg_priv.h"

extern "C" void ekseam_board_log(unsigned c);

// ---- a tiny ISR-safe trace into the board log (the SD log; later the USB link itself) ----
static void UTr(const char* aText)
	{
	while (*aText)
		ekseam_board_log((unsigned char)*aText++);
	}
static void UTrHex(TUint32 aValue)
	{
	for (TInt i = 7; i >= 0; --i)
		ekseam_board_log("0123456789abcdef"[(aValue >> (i * 4)) & 0xF]);
	}
static void UTr2(const char* aText, TUint32 aValue)
	{
	UTr(aText); UTrHex(aValue); UTr("\n");
	}

// ---- hardware ----
const TPhysAddr KUsbOtgPhys = 0x01C19000u;	// MUSB at +0x000, PHY control ("phy_ctrl") at +0x400
const TInt KIntIdUsbOtg = 32 + 71;			// GIC id: SPI 71 (sun50i-a64.dtsi usb_otg)
enum { KCcuBusGate0 = 0x060, KCcuUsbPhyCfg = 0x0CC, KCcuBusRst0 = 0x2C0 };

enum
	{
	KMuPower = 0x40, KMuDevctl = 0x41, KMuIndex = 0x42, KMuVend0 = 0x43,
	KMuIntrTx = 0x44, KMuIntrRx = 0x46, KMuIntrTxE = 0x48, KMuIntrRxE = 0x4A, KMuIntrUsb = 0x4C, KMuIntrUsbE = 0x50,
	KMuFrame = 0x54,
	KMuTxMaxP = 0x80, KMuCsr0 = 0x82, KMuTxCsr = 0x82, KMuRxMaxP = 0x84, KMuRxCsr = 0x86, KMuCount0 = 0x88,
	KMuRxCount = 0x88,
	KMuTxFifoSz = 0x90, KMuTxFifoAdd = 0x92, KMuRxFifoSz = 0x94, KMuRxFifoAdd = 0x96, KMuFAddr = 0x98,
	KPhyIscr = 0x400, KPhyCtl = 0x410, KPhyOtgCtl = 0x420
	};
const TUint8 KPowerSoftConn = 0x40, KPowerHsEnab = 0x20, KPowerSuspendM = 0x01, KPowerResume = 0x04;
const TUint8 KIntrSuspend = 0x01, KIntrResume = 0x02, KIntrReset = 0x04;
const TUint16 KCsr0RxPktRdy = 0x01, KCsr0TxPktRdy = 0x02, KCsr0SentStall = 0x04, KCsr0DataEnd = 0x08,
			  KCsr0SetupEnd = 0x10, KCsr0SendStall = 0x20, KCsr0SvdRxPktRdy = 0x40, KCsr0SvdSetupEnd = 0x80,
			  KCsr0FlushFifo = 0x100;
const TUint16 KTxCsrTxPktRdy = 0x01, KTxCsrFifoNotEmpty = 0x02, KTxCsrUnderrun = 0x04, KTxCsrFlushFifo = 0x08,
			  KTxCsrSendStall = 0x10, KTxCsrSentStall = 0x20, KTxCsrClrDataTog = 0x40, KTxCsrMode = 0x2000;
const TUint16 KRxCsrRxPktRdy = 0x01, KRxCsrOverrun = 0x04, KRxCsrFlushFifo = 0x10, KRxCsrSendStall = 0x20,
			  KRxCsrSentStall = 0x40, KRxCsrClrDataTog = 0x80;

static const TInt KHwEndpoints = 5;							// MUSB endpoints 1..5 (sunxi_musb_mode_cfg_5eps)
static const TInt KUsbTotalEndpoints = 2 + 2 * KHwEndpoints;	// ep0 out/in + an OUT and an IN per hardware endpoint
static const TInt KEp0MaxPktSz = 64;
static const TInt KSetupPacketSize = 8;

static const TUsbcEndpointCaps DeviceEndpoints[KUsbTotalEndpoints] =
	{
	{KUsbEpSize64, (KUsbEpTypeControl | KUsbEpDirOut)},		//  0: ep0 OUT
	{KUsbEpSize64, (KUsbEpTypeControl | KUsbEpDirIn)},		//  1: ep0 IN
#define A64_EP_PAIR \
	{KUsbEpSize8 | KUsbEpSize16 | KUsbEpSize32 | KUsbEpSize64, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)}, \
	{KUsbEpSize8 | KUsbEpSize16 | KUsbEpSize32 | KUsbEpSize64, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)}
	A64_EP_PAIR, A64_EP_PAIR, A64_EP_PAIR, A64_EP_PAIR, A64_EP_PAIR	// 2k / 2k+1: hardware endpoint k = 1..5
#undef A64_EP_PAIR
	};

static const TUint16 KUsbVendorId = KUsbVendorId_Symbian;
static const TUint16 KUsbProductId = 0x0666;
static const TUint16 KUsbDevRelease = 0x0100;
static const TUint16 KUsbLangId = 0x0409;
static const wchar_t KStringManufacturer[] = L"Phorepoc";
static const wchar_t KStringProduct[] = L"Symbian on PinePhone"; // layering: ok
static const wchar_t KStringSerialNo[] = L"0000000001";
static const wchar_t KStringConfig[] = L"Phorepoc link";

enum TEp0State { EEp0Idle, EEp0InData, EEp0OutData, EEp0StatusIn, EEp0StatusOut };

struct TEndpoint
	{
	union { TUint8* iRxBuf; const TUint8* iTxBuf; };
	union { TInt iReceived; TInt iTransmitted; };
	TInt iLength;
	TInt iMaxPacket;
	TBool iZlpReqd;
	TBool iZlpPending;
	TBool iNoBuffer;
	TInt iPackets;
	TUsbcRequestCallback* iRequest;
	TUsbcPacketArray* iPacketIndex;
	TUsbcPacketArray* iPacketSize;
	};

NONSHARABLE_CLASS(DA64Usbcc) : public DUsbClientController
	{
public:
	DA64Usbcc();
	TInt Construct();
	virtual void DumpRegisters();
private:
	virtual TInt SetDeviceAddress(TInt aAddress);
	virtual TInt ConfigureEndpoint(TInt aRealEndpoint, const TUsbcEndpointInfo& aEndpointInfo);
	virtual TInt DeConfigureEndpoint(TInt aRealEndpoint);
	virtual TInt AllocateEndpointResource(TInt aRealEndpoint, TUsbcEndpointResource aResource);
	virtual TInt DeAllocateEndpointResource(TInt aRealEndpoint, TUsbcEndpointResource aResource);
	virtual TBool QueryEndpointResource(TInt aRealEndpoint, TUsbcEndpointResource aResource) const;
	virtual TInt OpenDmaChannel(TInt aRealEndpoint);
	virtual void CloseDmaChannel(TInt aRealEndpoint);
	virtual TInt SetupEndpointRead(TInt aRealEndpoint, TUsbcRequestCallback& aCallback);
	virtual TInt SetupEndpointWrite(TInt aRealEndpoint, TUsbcRequestCallback& aCallback);
	virtual TInt CancelEndpointRead(TInt aRealEndpoint);
	virtual TInt CancelEndpointWrite(TInt aRealEndpoint);
	virtual TInt SetupEndpointZeroRead();
	virtual TInt SetupEndpointZeroWrite(const TUint8* aBuffer, TInt aLength, TBool aZlpReqd = EFalse);
	virtual TInt SendEp0ZeroByteStatusPacket();
	virtual TInt StallEndpoint(TInt aRealEndpoint);
	virtual TInt ClearStallEndpoint(TInt aRealEndpoint);
	virtual TInt EndpointStallStatus(TInt aRealEndpoint) const;
	virtual TInt EndpointErrorStatus(TInt aRealEndpoint) const;
	virtual TInt ResetDataToggle(TInt aRealEndpoint);
	virtual TInt SynchFrameNumber() const;
	virtual void SetSynchFrameNumber(TInt aFrameNumber);
	virtual TInt StartUdc();
	virtual TInt StopUdc();
	virtual TInt UdcConnect();
	virtual TInt UdcDisconnect();
	virtual TBool UsbConnectionStatus() const;
	virtual TBool UsbPowerStatus() const;
	virtual TBool DeviceSelfPowered() const;
	virtual const TUsbcEndpointCaps* DeviceEndpointCaps() const;
	virtual TInt DeviceTotalEndpoints() const;
	virtual TBool SoftConnectCaps() const;
	virtual TBool DeviceStateChangeCaps() const;
	virtual void Suspend();
	virtual void Resume();
	virtual void Reset();
	virtual TInt SignalRemoteWakeup();
	virtual void Ep0ReadSetupPktProceed();
	virtual void Ep0ReceiveProceed();
	virtual TDfcQue* DfcQ(TInt aUnit);
	virtual TBool CurrentlyUsingHighSpeed();
	virtual TBool DeviceHighSpeedCaps() const;
private:
	inline volatile TUint8& R8(TUint aOff) const { return *(volatile TUint8*)(iBase + aOff); }
	inline volatile TUint16& R16(TUint aOff) const { return *(volatile TUint16*)(iBase + aOff); }
	inline volatile TUint32& R32(TUint aOff) const { return *(volatile TUint32*)(iBase + aOff); }
	static inline volatile TUint32& Ccu(TUint aOff) { return *(volatile TUint32*)(KHwBaseCcuPio + aOff); }
	TInt Lock() const;
	void Unlock(TInt aIrq) const;
	void PhyWrite(TUint aAddr, TUint32 aData, TInt aLen);
	void PowerUpHardware();
	void InitialiseUdcRegisters();
	void EnableEpInt(TInt aRealEndpoint, TBool aEnable);
	static void UdcIsr(TAny* aPtr);
	void UdcInterruptService();
	void Ep0IntService();
	void Ep0ReadSetupPkt();
	void Ep0Receive();
	void Ep0Transmit();
	void Ep0Cancel();
	void BulkTxLoad(TInt aRealEndpoint, TUsbcRequestCallback** aDefer = NULL);
	void BulkTxIntService(TInt aRealEndpoint);
	void BulkRxIntService(TInt aRealEndpoint);
	void BulkRxRead(TInt aRealEndpoint, TUint16 aCsr, TUsbcRequestCallback** aDefer = NULL);
private:
	TLinAddr iBase;
	DPlatChunkHw* iChunk;
	mutable TSpinLock iLock;
	TBool iHwPowered;
	TBool iInitialized;
	TBool iIntBound;
	TBool iEp0Configured;
	TEp0State iEp0State;
	TBool iEp0AckPending;		// a setup / data packet read, ServicedRxPktRdy not yet written
	TInt iEp0OutLength;			// wLength of the current control-write
	TInt iEp0OutReceived;
	TInt iPendingAddress;		// -1, or the address to write to FADDR when the status stage completes
	TEndpoint iEndpoints[KUsbTotalEndpoints];
	TDfcQue* iDfcQueue;
	};

_LIT(KDfcName, "USB_DFC");

DA64Usbcc::DA64Usbcc()
	: iBase(0), iChunk(NULL), iLock(TSpinLock::EOrderGenericIrqHigh0), iHwPowered(EFalse), iInitialized(EFalse),
	  iIntBound(EFalse), iEp0Configured(EFalse), iEp0State(EEp0Idle), iEp0AckPending(EFalse), iEp0OutLength(0),
	  iEp0OutReceived(0), iPendingAddress(-1), iDfcQueue(NULL)
	{
	memclr(iEndpoints, sizeof(iEndpoints));
	}

TInt DA64Usbcc::Construct()
	{
	TInt r = Kern::DfcQCreate(iDfcQueue, 45, &KDfcName);
	if (r != KErrNone)
		return r;
#ifdef __SMP__
	// the USB interrupt is routed to CPU 0 (the GIC set-up); keep the DFC thread there too
	NKern::ThreadSetCpuAffinity(iDfcQueue->iThread, 0);
#endif
	r = DPlatChunkHw::New(iChunk, KUsbOtgPhys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return r;
	iBase = iChunk->LinearAddress();

	TUsbcDeviceDescriptor* dev = TUsbcDeviceDescriptor::New(
		0x02, 0x02, 0x00,		// CDC: what Windows' usbser.inf binds to (Microsoft: "class 02, subclass 02")
		KEp0MaxPktSz, KUsbVendorId, KUsbProductId, KUsbDevRelease, 1);
	TUsbcConfigDescriptor* cfg = TUsbcConfigDescriptor::New(1, ETrue, EFalse, 100);
	TUsbcLangIdDescriptor* lang = TUsbcLangIdDescriptor::New(KUsbLangId);
#define A64_STR(s) TUsbcStringDescriptor::New(TPtr8(const_cast<TUint8*>(reinterpret_cast<const TUint8*>(s)), \
		sizeof(s) - 2, sizeof(s) - 2))
	TUsbcStringDescriptor* manu = A64_STR(KStringManufacturer);
	TUsbcStringDescriptor* prod = A64_STR(KStringProduct);
	TUsbcStringDescriptor* ser = A64_STR(KStringSerialNo);
	TUsbcStringDescriptor* conf = A64_STR(KStringConfig);
#undef A64_STR
	if (!dev || !cfg || !lang || !manu || !prod || !ser || !conf)
		return KErrNoMemory;
	if (!InitialiseBaseClass(dev, cfg, lang, manu, prod, ser, conf))
		return KErrGeneral;
	return KErrNone;
	}

void DA64Usbcc::DumpRegisters()
	{
	UTr2("usbc: power=", R8(KMuPower));
	UTr2("usbc: devctl=", R8(KMuDevctl));
	UTr2("usbc: faddr=", R8(KMuFAddr));
	}

TDfcQue* DA64Usbcc::DfcQ(TInt /*aUnit*/)
	{
	return iDfcQueue;
	}

// Thread-context callers take the lock (IRQs off locally); the ISR takes it itself, and the PIL upcalls it makes
// call back into us in interrupt context with it already held.
TInt DA64Usbcc::Lock() const
	{
	if (NKern::CurrentContext() == NKern::EInterrupt)
		return -1;
	return __SPIN_LOCK_IRQSAVE(iLock);
	}

void DA64Usbcc::Unlock(TInt aIrq) const
	{
	if (aIrq != -1)
		__SPIN_UNLOCK_IRQRESTORE(iLock, aIrq);
	}

// ---- PHY / clocks ----

void DA64Usbcc::PhyWrite(TUint aAddr, TUint32 aData, TInt aLen)
	{
	R32(KPhyCtl) = 0;
	for (TInt i = 0; i < aLen; ++i, aData >>= 1)
		{
		R32(KPhyCtl) = (R32(KPhyCtl) & ~(0xFFu << 8)) | ((aAddr + i) << 8);
		TUint8 t = R8(KPhyCtl);
		t = (aData & 1) ? (t | 0x80) : (t & ~0x80);
		R8(KPhyCtl) = t & ~1;
		R8(KPhyCtl) = R8(KPhyCtl) | 1;
		R8(KPhyCtl) = R8(KPhyCtl) & ~1;
		}
	}

void DA64Usbcc::PowerUpHardware()
	{
	if (iHwPowered)
		return;
	Ccu(KCcuBusGate0) |= 1u << 23;					// USB-OTG bus clock
	Ccu(KCcuBusRst0) |= 1u << 23;					// USB-OTG out of reset
	Ccu(KCcuUsbPhyCfg) |= (1u << 0) | (1u << 8);	// PHY0 out of reset, PHY0 clock
	Kern::NanoWait(100000);
	R8(KMuVend0) = 0;								// PIO mode
	PhyWrite(0x0C, 0x01, 1);						// PHY_RES45_CAL_EN
	PhyWrite(0x20, 0x14, 5);						// PHY_TX_AMPLITUDE_TUNE
	PhyWrite(0x2A, 3, 2);							// PHY_DISCON_TH_SEL (A64: 3)
	R32(KPhyOtgCtl) |= 1;							// PHY0 -> the MUSB
	// pull-ups on; ID forced high (peripheral), VBUS forced valid
	R32(KPhyIscr) = (R32(KPhyIscr) & ~((3u << 14) | (3u << 12))) | (1u << 17) | (1u << 16) | (3u << 14) | (3u << 12);
	iHwPowered = ETrue;
	}

// FIFO RAM in 8-byte units: ep0 has the first 64 bytes; then 512 bytes per direction per endpoint (room for high
// speed later), TX for 1..5, then RX for 1..5 -- 5.1 KB of the core's 8 KB.
void DA64Usbcc::InitialiseUdcRegisters()
	{
	for (TInt k = 1; k <= KHwEndpoints; ++k)
		{
		R8(KMuIndex) = (TUint8)k;
		R8(KMuTxFifoSz) = 6;						// 2^(6+3) = 512, single-buffered
		R16(KMuTxFifoAdd) = (TUint16)(8 + (k - 1) * 64);
		R8(KMuRxFifoSz) = 6;
		R16(KMuRxFifoAdd) = (TUint16)(8 + KHwEndpoints * 64 + (k - 1) * 64);
		R16(KMuTxMaxP) = 64;
		R16(KMuRxMaxP) = 64;
		}
	R8(KMuIndex) = 0;
	R16(KMuIntrTxE) = 0x0001;						// ep0 only; the others when configured
	R16(KMuIntrRxE) = 0;
	R8(KMuIntrUsbE) = KIntrSuspend | KIntrResume | KIntrReset;
	R8(KMuIntrUsb) = R8(KMuIntrUsb);				// clear what is pending (write-1-to-clear)
	R16(KMuIntrTx) = R16(KMuIntrTx);
	R16(KMuIntrRx) = R16(KMuIntrRx);
	}

void DA64Usbcc::EnableEpInt(TInt aRealEndpoint, TBool aEnable)
	{
	if (aRealEndpoint < 2)
		{
		R16(KMuIntrTxE) = aEnable ? (R16(KMuIntrTxE) | 1) : (R16(KMuIntrTxE) & ~1);
		return;
		}
	const TUint16 bit = (TUint16)(1u << (aRealEndpoint / 2));
	const TUint off = (aRealEndpoint & 1) ? KMuIntrTxE : KMuIntrRxE;
	R16(off) = aEnable ? (R16(off) | bit) : (R16(off) & ~bit);
	}

// ---- the PSL interface ----

TInt DA64Usbcc::StartUdc()
	{
	if (iInitialized)
		return KErrNone;
	const TInt irq = Lock();
	PowerUpHardware();
	InitialiseUdcRegisters();
	Unlock(irq);
	if (!iIntBound)
		{
		TInt r = Interrupt::Bind(KIntIdUsbOtg, UdcIsr, this);
		if (r != KErrNone)
			{
			UTr2("usbc: Interrupt::Bind failed r=", (TUint32)r);
			return r;
			}
		iIntBound = ETrue;
		}
	Interrupt::Enable(KIntIdUsbOtg);
	iInitialized = ETrue;
	UTr("usbc: StartUdc done\n");
	return KErrNone;
	}

TInt DA64Usbcc::StopUdc()
	{
	if (!iInitialized)
		return KErrNone;
	Interrupt::Disable(KIntIdUsbOtg);
	const TInt irq = Lock();
	R8(KMuPower) &= ~KPowerSoftConn;
	R8(KMuIntrUsbE) = 0;
	R16(KMuIntrTxE) = 0;
	R16(KMuIntrRxE) = 0;
	Unlock(irq);
	iInitialized = EFalse;
	return KErrNone;
	}

TInt DA64Usbcc::UdcConnect()
	{
	const TInt irq = Lock();
	R8(KMuDevctl) |= 0x01;							// SESSION
	R8(KMuPower) = KPowerSoftConn | KPowerHsEnab;	// high speed (2026-10-12): the host chirps at reset
	Unlock(irq);
	UTr("usbc: connected (soft-connect)\n");
	return KErrNone;
	}

TInt DA64Usbcc::UdcDisconnect()
	{
	const TInt irq = Lock();
	R8(KMuPower) &= ~KPowerSoftConn;
	Unlock(irq);
	return KErrNone;
	}

TBool DA64Usbcc::UsbConnectionStatus() const { return ETrue; }		// no cable detect yet: VBUS is forced
TBool DA64Usbcc::UsbPowerStatus() const { return ETrue; }
TBool DA64Usbcc::DeviceSelfPowered() const { return ETrue; }
const TUsbcEndpointCaps* DA64Usbcc::DeviceEndpointCaps() const { return DeviceEndpoints; }
TInt DA64Usbcc::DeviceTotalEndpoints() const { return KUsbTotalEndpoints; }
TBool DA64Usbcc::SoftConnectCaps() const { return ETrue; }
TBool DA64Usbcc::DeviceStateChangeCaps() const { return EFalse; }
// HIGH SPEED (2026-10-12): POWER.HSMODE (bit 4) is set once the reset's chirp handshake ends in high speed; the PIL
// reads it at every bus reset (ProcessResetEvent) and then configures bulk endpoints with their HS size (512)
TBool DA64Usbcc::CurrentlyUsingHighSpeed() { return (R8(KMuPower) & 0x10) != 0; }
TBool DA64Usbcc::DeviceHighSpeedCaps() const { return ETrue; }
TInt DA64Usbcc::SignalRemoteWakeup() { return KErrNotSupported; }
TInt DA64Usbcc::SynchFrameNumber() const { return 0; }
void DA64Usbcc::SetSynchFrameNumber(TInt /*aFrameNumber*/) {}
TInt DA64Usbcc::AllocateEndpointResource(TInt, TUsbcEndpointResource) { return KErrNone; }
TInt DA64Usbcc::DeAllocateEndpointResource(TInt, TUsbcEndpointResource) { return KErrNone; }
TBool DA64Usbcc::QueryEndpointResource(TInt, TUsbcEndpointResource) const { return EFalse; }
TInt DA64Usbcc::OpenDmaChannel(TInt) { return KErrNone; }
void DA64Usbcc::CloseDmaChannel(TInt) {}
TInt DA64Usbcc::EndpointErrorStatus(TInt) const { return KErrNone; }

void DA64Usbcc::Suspend() {}
void DA64Usbcc::Resume() {}

// called by the PIL (in the ISR) after we reported a bus reset: the core has cleared FADDR and the endpoint state
void DA64Usbcc::Reset()
	{
	iEp0State = EEp0Idle;
	iEp0AckPending = EFalse;
	iPendingAddress = -1;
	for (TInt i = 0; i < KUsbTotalEndpoints; ++i)
		{
		iEndpoints[i].iRxBuf = NULL;
		iEndpoints[i].iRequest = NULL;
		iEndpoints[i].iNoBuffer = EFalse;
		iEndpoints[i].iZlpPending = EFalse;
		}
	const TInt irq = Lock();
	InitialiseUdcRegisters();
	Unlock(irq);
	}

TInt DA64Usbcc::SetDeviceAddress(TInt aAddress)
	{
	// MUSB: FADDR takes the new address only after the status stage of SET_ADDRESS (Ep0IntService)
	iPendingAddress = aAddress & 0x7F;
	if (aAddress || GetDeviceStatus() == EUsbcDeviceStateAddress)
		MoveToAddressState();
	return KErrNone;
	}

TInt DA64Usbcc::ConfigureEndpoint(TInt aRealEndpoint, const TUsbcEndpointInfo& aEndpointInfo)
	{
	if (aRealEndpoint < 0 || aRealEndpoint >= KUsbTotalEndpoints)
		return KErrArgument;
	const TInt irq = Lock();
	TEndpoint& ep = iEndpoints[aRealEndpoint];
	ep.iMaxPacket = aEndpointInfo.iSize ? aEndpointInfo.iSize : 64;
	if (aRealEndpoint < 2)
		{
		iEp0Configured = ETrue;
		EnableEpInt(0, ETrue);
		}
	else
		{
		R8(KMuIndex) = (TUint8)(aRealEndpoint / 2);
		if (aRealEndpoint & 1)
			{
			R16(KMuTxMaxP) = (TUint16)ep.iMaxPacket;
			TUint16 csr = KTxCsrMode | KTxCsrClrDataTog;
			if (R16(KMuTxCsr) & KTxCsrFifoNotEmpty)
				csr |= KTxCsrFlushFifo;
			R16(KMuTxCsr) = csr;
			}
		else
			{
			R16(KMuRxMaxP) = (TUint16)ep.iMaxPacket;
			R16(KMuRxCsr) = KRxCsrClrDataTog | KRxCsrFlushFifo;
			}
		EnableEpInt(aRealEndpoint, ETrue);
		}
	Unlock(irq);
	UTr2("usbc: configured ep ", (TUint32)aRealEndpoint | ((TUint32)ep.iMaxPacket << 16));
	return KErrNone;
	}

TInt DA64Usbcc::DeConfigureEndpoint(TInt aRealEndpoint)
	{
	if (aRealEndpoint < 0 || aRealEndpoint >= KUsbTotalEndpoints)
		return KErrArgument;
	const TInt irq = Lock();
	if (aRealEndpoint < 2)
		iEp0Configured = EFalse;
	else
		{
		EnableEpInt(aRealEndpoint, EFalse);
		R8(KMuIndex) = (TUint8)(aRealEndpoint / 2);
		if (aRealEndpoint & 1)
			R16(KMuTxCsr) = KTxCsrMode | KTxCsrFlushFifo;
		else
			R16(KMuRxCsr) = KRxCsrFlushFifo;
		}
	Unlock(irq);
	return KErrNone;
	}

// ---- bulk / interrupt endpoints ----

TInt DA64Usbcc::SetupEndpointRead(TInt aRealEndpoint, TUsbcRequestCallback& aCallback)
	{
	if (aRealEndpoint < 2 || aRealEndpoint >= KUsbTotalEndpoints || (aRealEndpoint & 1))
		return KErrArgument;
	const TInt irq = Lock();
	TEndpoint& ep = iEndpoints[aRealEndpoint];
	if (ep.iRxBuf)
		{
		Unlock(irq);
		return KErrGeneral;
		}
	ep.iRxBuf = aCallback.iBufferStart;
	ep.iReceived = 0;
	ep.iLength = aCallback.iLength;
	ep.iPackets = 0;
	ep.iRequest = &aCallback;
	ep.iPacketIndex = aCallback.iPacketIndex;
	ep.iPacketSize = aCallback.iPacketSize;
	ep.iNoBuffer = EFalse;
	EnableEpInt(aRealEndpoint, ETrue);
	R8(KMuIndex) = (TUint8)(aRealEndpoint / 2);
	const TUint16 csr = R16(KMuRxCsr);
	TUsbcRequestCallback* done = NULL;
	if (csr & KRxCsrRxPktRdy)					// a packet waited for this buffer
		BulkRxRead(aRealEndpoint, csr, &done);
	Unlock(irq);
	if (done)
		EndpointRequestComplete(done);			// outside the lock (see BulkTxLoad)
	return KErrNone;
	}

TInt DA64Usbcc::SetupEndpointWrite(TInt aRealEndpoint, TUsbcRequestCallback& aCallback)
	{
	if (aRealEndpoint < 2 || aRealEndpoint >= KUsbTotalEndpoints || !(aRealEndpoint & 1))
		return KErrArgument;
	const TInt irq = Lock();
	TEndpoint& ep = iEndpoints[aRealEndpoint];
	if (ep.iTxBuf)
		{
		Unlock(irq);
		return KErrGeneral;
		}
	ep.iTxBuf = aCallback.iBufferStart;
	ep.iTransmitted = 0;
	ep.iLength = aCallback.iLength;
	ep.iPackets = 0;
	ep.iZlpReqd = aCallback.iZlpReqd;
	ep.iZlpPending = EFalse;
	ep.iRequest = &aCallback;
	EnableEpInt(aRealEndpoint, ETrue);
	TUsbcRequestCallback* done = NULL;
	BulkTxLoad(aRealEndpoint, &done);
	Unlock(irq);
	if (done)
		EndpointRequestComplete(done);			// outside the lock (see BulkTxLoad)
	return KErrNone;
	}

TInt DA64Usbcc::CancelEndpointRead(TInt aRealEndpoint)
	{
	if (aRealEndpoint < 2 || aRealEndpoint >= KUsbTotalEndpoints)
		return KErrArgument;
	const TInt irq = Lock();
	iEndpoints[aRealEndpoint].iRxBuf = NULL;
	iEndpoints[aRealEndpoint].iRequest = NULL;
	Unlock(irq);
	return KErrNone;
	}

TInt DA64Usbcc::CancelEndpointWrite(TInt aRealEndpoint)
	{
	if (aRealEndpoint < 2 || aRealEndpoint >= KUsbTotalEndpoints)
		return KErrArgument;
	const TInt irq = Lock();
	iEndpoints[aRealEndpoint].iTxBuf = NULL;
	iEndpoints[aRealEndpoint].iRequest = NULL;
	R8(KMuIndex) = (TUint8)(aRealEndpoint / 2);
	R16(KMuTxCsr) = KTxCsrMode | KTxCsrFlushFifo;
	Unlock(irq);
	return KErrNone;
	}

// load the next packet of an IN transfer if the FIFO is free; complete the request once all is sent
// aDefer (thread-context callers, the lock held): hand a finished request back instead of completing it here -- the
// PIL completes from thread context with TDfc::Enque, which must not run under our IRQ-off spinlock (SMP: it can wait
// on another CPU whose ISR spins on this lock: the link wedged with a core at 100 %, 2026-10-20). Linux musb_g_giveback
// drops the lock around the giveback the same way. The ISR paths complete directly (the PIL uses TDfc::Add there).
void DA64Usbcc::BulkTxLoad(TInt aRealEndpoint, TUsbcRequestCallback** aDefer)
	{
	TEndpoint& ep = iEndpoints[aRealEndpoint];
	if (!ep.iTxBuf)
		return;
	const TInt k = aRealEndpoint / 2;
	R8(KMuIndex) = (TUint8)k;
	const TUint16 csr = R16(KMuTxCsr);
	if (csr & KTxCsrTxPktRdy)
		return;									// one in flight: the TX interrupt calls back
	const TInt left = ep.iLength - ep.iTransmitted;
	if (left > 0)
		{
		const TInt n = left < ep.iMaxPacket ? left : ep.iMaxPacket;
		volatile TUint8* fifo = (volatile TUint8*)(iBase + 4 * k);
		const TUint8* p = ep.iTxBuf + ep.iTransmitted;
		for (TInt i = 0; i < n; ++i)
			*fifo = p[i];
		ep.iTransmitted += n;
		++ep.iPackets;
		if (n == ep.iMaxPacket && ep.iTransmitted == ep.iLength && ep.iZlpReqd)
			ep.iZlpPending = ETrue;
		R16(KMuTxCsr) = (TUint16)((csr & ~(KTxCsrUnderrun | KTxCsrSentStall)) | KTxCsrMode | KTxCsrTxPktRdy);
		return;
		}
	if ((ep.iLength == 0 && ep.iPackets == 0) || ep.iZlpPending)
		{
		ep.iZlpPending = EFalse;
		++ep.iPackets;
		R16(KMuTxCsr) = (TUint16)((csr & ~(KTxCsrUnderrun | KTxCsrSentStall)) | KTxCsrMode | KTxCsrTxPktRdy);
		return;
		}
	TUsbcRequestCallback* req = ep.iRequest;
	ep.iTxBuf = NULL;
	ep.iRequest = NULL;
	if (req)
		{
		req->iTxBytes = ep.iTransmitted;
		req->iError = KErrNone;
		if (aDefer)
			*aDefer = req;
		else
			EndpointRequestComplete(req);
		}
	}

void DA64Usbcc::BulkTxIntService(TInt aRealEndpoint)
	{
	R8(KMuIndex) = (TUint8)(aRealEndpoint / 2);
	const TUint16 csr = R16(KMuTxCsr);
	if (csr & (KTxCsrSentStall | KTxCsrUnderrun))
		R16(KMuTxCsr) = (TUint16)(csr & ~(KTxCsrSentStall | KTxCsrUnderrun));
	if (csr & KTxCsrSentStall)
		return;
	BulkTxLoad(aRealEndpoint);
	}

// one packet into the request's buffer; each packet completes the request (the PIL / client reads again)
void DA64Usbcc::BulkRxRead(TInt aRealEndpoint, TUint16 aCsr, TUsbcRequestCallback** aDefer)
	{
	TEndpoint& ep = iEndpoints[aRealEndpoint];
	if (!ep.iRxBuf)
		{
		ep.iNoBuffer = ETrue;					// leave it in the FIFO (the host is NAKed) until a read comes
		EnableEpInt(aRealEndpoint, EFalse);
		return;
		}
	const TInt k = aRealEndpoint / 2;
	const TInt count = R16(KMuRxCount);
	if (count > ep.iLength - ep.iReceived)
		{
		ep.iNoBuffer = ETrue;
		EnableEpInt(aRealEndpoint, EFalse);
		return;
		}
	volatile TUint8* fifo = (volatile TUint8*)(iBase + 4 * k);
	TUint8* p = ep.iRxBuf + ep.iReceived;
	TInt i = 0;
	if (((TLinAddr)p & 3) == 0)		// 32-bit FIFO reads into an aligned buffer: a quarter of the bus reads (2026-10-15)
		{
		volatile TUint32* fifo32 = (volatile TUint32*)fifo;
		for (; i + 4 <= count; i += 4)
			*(TUint32*)(p + i) = *fifo32;
		}
	for (; i < count; ++i)
		p[i] = *fifo;
	ep.iReceived += count;
	R16(KMuRxCsr) = (TUint16)(aCsr & ~(KRxCsrRxPktRdy | KRxCsrOverrun | KRxCsrSentStall));
	TUsbcRequestCallback* req = ep.iRequest;
	if (ep.iPacketIndex)
		ep.iPacketIndex[0] = 0;
	if (ep.iPacketSize)
		ep.iPacketSize[0] = ep.iReceived;
	ep.iRxBuf = NULL;
	ep.iRequest = NULL;
	if (req)
		{
		req->iRxPackets = 1;
		req->iError = KErrNone;
		if (aDefer)
			*aDefer = req;
		else
			EndpointRequestComplete(req);
		}
	}

void DA64Usbcc::BulkRxIntService(TInt aRealEndpoint)
	{
	R8(KMuIndex) = (TUint8)(aRealEndpoint / 2);
	const TUint16 csr = R16(KMuRxCsr);
	if (csr & KRxCsrSentStall)
		{
		R16(KMuRxCsr) = (TUint16)(csr & ~KRxCsrSentStall);
		return;
		}
	if (csr & KRxCsrRxPktRdy)
		BulkRxRead(aRealEndpoint, csr);
	}

// ---- endpoint 0 ----

TInt DA64Usbcc::SetupEndpointZeroRead()
	{
	const TInt irq = Lock();					// the ISR reads this endpoint's state too
	TEndpoint& ep = iEndpoints[KEp0_Out];
	const TInt r = ep.iRxBuf ? KErrGeneral : KErrNone;
	if (r == KErrNone)
		{
		ep.iRxBuf = iEp0_RxBuf;
		ep.iReceived = 0;
		}
	Unlock(irq);
	return r;
	}

TInt DA64Usbcc::SetupEndpointZeroWrite(const TUint8* aBuffer, TInt aLength, TBool aZlpReqd)
	{
	const TInt irq = Lock();
	TEndpoint& ep = iEndpoints[KEp0_In];
	if (ep.iTxBuf)
		{
		Unlock(irq);
		return KErrGeneral;
		}
	ep.iTxBuf = aBuffer;
	ep.iTransmitted = 0;
	ep.iLength = aLength;
	ep.iZlpReqd = aZlpReqd;
	R8(KMuIndex) = 0;
	if (iEp0AckPending)
		{
		R16(KMuCsr0) = KCsr0SvdRxPktRdy;		// release the setup; the data stage follows
		iEp0AckPending = EFalse;
		}
	iEp0State = EEp0InData;
	Ep0Transmit();
	Unlock(irq);
	return KErrNone;
	}

TInt DA64Usbcc::SendEp0ZeroByteStatusPacket()
	{
	const TInt irq = Lock();
	R8(KMuIndex) = 0;
	if (iEp0AckPending && (iEp0State == EEp0Idle || iEp0State == EEp0OutData))
		{
		// no-data request, or the last packet of a control-write: release it and let the core do the status stage
		R16(KMuCsr0) = KCsr0SvdRxPktRdy | KCsr0DataEnd;
		iEp0AckPending = EFalse;
		iEp0State = EEp0StatusIn;
		}
	Unlock(irq);
	return KErrNone;
	}

TInt DA64Usbcc::StallEndpoint(TInt aRealEndpoint)
	{
	if (aRealEndpoint < 0 || aRealEndpoint >= KUsbTotalEndpoints)
		return KErrArgument;
	const TInt irq = Lock();
	if (aRealEndpoint < 2)
		{
		R8(KMuIndex) = 0;
		R16(KMuCsr0) = (TUint16)((iEp0AckPending ? KCsr0SvdRxPktRdy : 0) | KCsr0SendStall);
		iEp0AckPending = EFalse;
		iEp0State = EEp0Idle;
		}
	else
		{
		R8(KMuIndex) = (TUint8)(aRealEndpoint / 2);
		if (aRealEndpoint & 1)
			R16(KMuTxCsr) = (TUint16)(R16(KMuTxCsr) | KTxCsrSendStall);
		else
			R16(KMuRxCsr) = (TUint16)(R16(KMuRxCsr) | KRxCsrSendStall);
		}
	Unlock(irq);
	return KErrNone;
	}

TInt DA64Usbcc::ClearStallEndpoint(TInt aRealEndpoint)
	{
	if (aRealEndpoint < 0 || aRealEndpoint >= KUsbTotalEndpoints)
		return KErrArgument;
	if (aRealEndpoint < 2)
		return KErrNone;						// ep0's stall ends with the next setup
	const TInt irq = Lock();
	R8(KMuIndex) = (TUint8)(aRealEndpoint / 2);
	if (aRealEndpoint & 1)
		R16(KMuTxCsr) = (TUint16)((R16(KMuTxCsr) & ~(KTxCsrSendStall | KTxCsrSentStall)) | KTxCsrClrDataTog);
	else
		R16(KMuRxCsr) = (TUint16)((R16(KMuRxCsr) & ~(KRxCsrSendStall | KRxCsrSentStall)) | KRxCsrClrDataTog);
	Unlock(irq);
	return KErrNone;
	}

TInt DA64Usbcc::EndpointStallStatus(TInt aRealEndpoint) const
	{
	if (aRealEndpoint < 2 || aRealEndpoint >= KUsbTotalEndpoints)
		return EFalse;
	const TInt irq = Lock();
	R8(KMuIndex) = (TUint8)(aRealEndpoint / 2);
	const TBool stalled = (aRealEndpoint & 1) ? (R16(KMuTxCsr) & KTxCsrSendStall) != 0
											  : (R16(KMuRxCsr) & KRxCsrSendStall) != 0;
	Unlock(irq);
	return stalled;
	}

TInt DA64Usbcc::ResetDataToggle(TInt aRealEndpoint)
	{
	if (aRealEndpoint < 2 || aRealEndpoint >= KUsbTotalEndpoints)
		return KErrNone;
	const TInt irq = Lock();
	R8(KMuIndex) = (TUint8)(aRealEndpoint / 2);
	if (aRealEndpoint & 1)
		R16(KMuTxCsr) = (TUint16)(R16(KMuTxCsr) | KTxCsrClrDataTog);
	else
		R16(KMuRxCsr) = (TUint16)(R16(KMuRxCsr) | KRxCsrClrDataTog);
	Unlock(irq);
	return KErrNone;
	}

void DA64Usbcc::Ep0ReadSetupPktProceed()
	{
	const TInt irq = Lock();
	EnableEpInt(0, ETrue);
	Unlock(irq);
	}

void DA64Usbcc::Ep0ReceiveProceed()
	{
	Ep0ReadSetupPktProceed();
	}

// load the next ep0 IN packet; DataEnd on the last one, after which the host's status stage ends the transfer
void DA64Usbcc::Ep0Transmit()
	{
	TEndpoint& ep = iEndpoints[KEp0_In];
	if (iEp0State != EEp0InData || !ep.iTxBuf)
		return;
	R8(KMuIndex) = 0;
	if (R16(KMuCsr0) & KCsr0TxPktRdy)
		return;
	const TInt left = ep.iLength - ep.iTransmitted;
	const TInt n = left < KEp0MaxPktSz ? left : KEp0MaxPktSz;
	volatile TUint8* fifo = (volatile TUint8*)iBase;
	const TUint8* p = ep.iTxBuf + ep.iTransmitted;
	for (TInt i = 0; i < n; ++i)
		*fifo = p[i];
	ep.iTransmitted += n;
	const TBool last = (n < KEp0MaxPktSz) || (ep.iTransmitted == ep.iLength && !ep.iZlpReqd);
	if (n == KEp0MaxPktSz && ep.iTransmitted == ep.iLength && ep.iZlpReqd)
		ep.iZlpReqd = EFalse;					// the next interrupt sends the zero-length end
	R16(KMuCsr0) = (TUint16)(KCsr0TxPktRdy | (last ? KCsr0DataEnd : 0));
	if (last)
		iEp0State = EEp0StatusOut;			// completed to the PIL when the status stage is over (Ep0IntService):
	}										// never from inside the PIL's own SetupEndpointZeroWrite call

void DA64Usbcc::Ep0Cancel()
	{
	TEndpoint& ep = iEndpoints[KEp0_In];
	iEp0State = EEp0Idle;
	iEp0AckPending = EFalse;
	if (ep.iTxBuf)
		{
		ep.iTxBuf = NULL;
		(void)Ep0RequestComplete(KEp0_In, ep.iTransmitted, KErrCancel);
		}
	}

void DA64Usbcc::Ep0ReadSetupPkt()
	{
	TEndpoint& ep = iEndpoints[KEp0_Out];
	R8(KMuIndex) = 0;
	const TInt count = R16(KMuCount0);
	TUint8* buf = ep.iRxBuf;
	if (!buf || count != KSetupPacketSize)
		{
		for (TInt i = 0; i < count; ++i)
			(void)R8(0);
		R16(KMuCsr0) = KCsr0SvdRxPktRdy | KCsr0SendStall;
		UTr2("usbc: bad setup / no ep0 buffer, count=", (TUint32)count);
		return;
		}
	for (TInt i = 0; i < KSetupPacketSize; ++i)
		buf[i] = R8(0);
	iEp0AckPending = ETrue;
	const TUsbcEp0State next = EnquireEp0NextState(buf);
	if (next == EEp0StateDataIn)
		iEp0State = EEp0InData;
	else if (next == EEp0StateDataOut)
		{
		iEp0State = EEp0OutData;
		iEp0OutLength = buf[6] | (buf[7] << 8);
		iEp0OutReceived = 0;
		}
	else
		iEp0State = EEp0Idle;
	ep.iRxBuf = NULL;
	const TInt r = Ep0RequestComplete(KEp0_Out, KSetupPacketSize, KErrNone);
	if (r == KErrNotFound)
		EnableEpInt(0, EFalse);				// the recipient has no read posted yet: Ep0ReadSetupPktProceed resumes
	if (iEp0State == EEp0OutData && iEp0AckPending)
		{
		R8(KMuIndex) = 0;
		R16(KMuCsr0) = KCsr0SvdRxPktRdy;	// let the data stage come in
		iEp0AckPending = EFalse;
		}
	}

void DA64Usbcc::Ep0Receive()
	{
	TEndpoint& ep = iEndpoints[KEp0_Out];
	R8(KMuIndex) = 0;
	if (!ep.iRxBuf)
		{
		EnableEpInt(0, EFalse);
		return;
		}
	const TInt count = R16(KMuCount0);
	for (TInt i = 0; i < count; ++i)
		ep.iRxBuf[i] = R8(0);
	iEp0OutReceived += count;
	iEp0AckPending = ETrue;
	if (iEp0OutReceived < iEp0OutLength && count == KEp0MaxPktSz)
		{
		R16(KMuCsr0) = KCsr0SvdRxPktRdy;	// more to come
		iEp0AckPending = EFalse;
		}
	ep.iRxBuf = NULL;
	const TInt r = Ep0RequestComplete(KEp0_Out, count, KErrNone);
	if (r == KErrNotFound)
		EnableEpInt(0, EFalse);
	}

void DA64Usbcc::Ep0IntService()
	{
	R8(KMuIndex) = 0;
	TUint16 csr = R16(KMuCsr0);
	if (csr & KCsr0SentStall)
		{
		R16(KMuCsr0) = (TUint16)(csr & ~KCsr0SentStall);
		iEp0State = EEp0Idle;
		iEp0AckPending = EFalse;
		csr = R16(KMuCsr0);
		}
	if (csr & KCsr0SetupEnd)
		{
		R16(KMuCsr0) = KCsr0SvdSetupEnd;
		Ep0Cancel();
		csr = R16(KMuCsr0);
		}
	switch (iEp0State)
		{
	case EEp0StatusIn:
	case EEp0StatusOut:
		// the status stage is over
		if (iPendingAddress >= 0)
			{
			R8(KMuFAddr) = (TUint8)iPendingAddress;
			iPendingAddress = -1;
			}
		if (iEp0State == EEp0StatusOut && iEndpoints[KEp0_In].iTxBuf)
			{
			TEndpoint& in = iEndpoints[KEp0_In];
			in.iTxBuf = NULL;
			iEp0State = EEp0Idle;
			(void)Ep0RequestComplete(KEp0_In, in.iTransmitted, KErrNone);	// re-arms the ep0 read
			}
		iEp0State = EEp0Idle;
		if (csr & KCsr0RxPktRdy)
			Ep0ReadSetupPkt();
		break;
	case EEp0InData:
		if (csr & KCsr0RxPktRdy)			// the host gave up on the data stage: a new setup
			{
			Ep0Cancel();
			Ep0ReadSetupPkt();
			}
		else
			Ep0Transmit();
		break;
	case EEp0OutData:
		if ((csr & KCsr0RxPktRdy) && !iEp0AckPending)
			Ep0Receive();
		break;
	case EEp0Idle:
	default:
		if ((csr & KCsr0RxPktRdy) && !iEp0AckPending)
			Ep0ReadSetupPkt();
		break;
		}
	}

// ---- the interrupt ----

void DA64Usbcc::UdcIsr(TAny* aPtr)
	{
	static_cast<DA64Usbcc*>(aPtr)->UdcInterruptService();
	}

void DA64Usbcc::UdcInterruptService()
	{
	__SPIN_LOCK(iLock);
	const TUint8 intr = R8(KMuIntrUsb);
	const TUint16 tx = R16(KMuIntrTx), rx = R16(KMuIntrRx);
	R8(KMuIntrUsb) = intr;						// write-1-to-clear
	R16(KMuIntrTx) = tx;
	R16(KMuIntrRx) = rx;
	if (intr & KIntrReset)
		{
		UTr("usbc: bus reset\n");
		(void)DeviceEventNotification(EUsbEventReset);
		}
	if (intr & KIntrResume)
		(void)DeviceEventNotification(EUsbEventResume);
	if (tx & 1)
		Ep0IntService();
	for (TInt k = 1; k <= KHwEndpoints; ++k)
		{
		if (tx & (1u << k))
			BulkTxIntService(2 * k + 1);
		if (rx & (1u << k))
			BulkRxIntService(2 * k);
		}
	if (intr & KIntrSuspend)
		(void)DeviceEventNotification(EUsbEventSuspend);
	__SPIN_UNLOCK(iLock);
	}

// ---- the extension entry ----

DECLARE_STANDARD_EXTENSION()
	{
	DA64Usbcc* const usbcc = new DA64Usbcc();
	if (!usbcc)
		return KErrNoMemory;
	TInt r = usbcc->Construct();
	if (r != KErrNone)
		{
		UTr2("usbc: Construct failed r=", (TUint32)r);
		return r;
		}
	if (usbcc->RegisterUdc(0) == NULL)
		{
		UTr("usbc: RegisterUdc failed\n");
		return KErrGeneral;
		}
	UTr("usbc: A64 USB device controller registered\n");
	return KErrNone;
	}
