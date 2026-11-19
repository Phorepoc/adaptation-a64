// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// phe_usb_ether.cpp -- the modem's USB Ethernet interface (FreeBSD's if_cdce.c: CDC ECM / NCM, on phe_usb_net.c) as an
// Ethernet PDD, "Ethernet.USB", under the real Ethernet LDD (d_ethernet.cpp) that the comms stack's packet driver
// (EtherPkt -> ethint.nif) opens as RBusDevEthernet. Same contract as every other Ethernet PDD (the x86 e1000, the
// virtio-net of Syborg): Send = one raw frame out; per received frame ReceiveIsr(), on which the LDD calls
// ReceiveFrame() to copy it in (no FCS). The frames come from the USB thread (phe_usb_net_rx_cb), are copied into a
// small ring there and handed to the LDD from a DFC. The modem is a router with a DHCP server on that link: DHCP and
// DNS are the unchanged comms stack's; this file moves frames only.
#include <kernel/kern_priv.h>
#include <drivers/ethernet.h>

#define PHE_USB_NET_PORT_ONLY			// the port seam only: the BSD-side shim types are not for this side

extern "C" {
#include "../phe_usb_net.h"
void ekseam_puts(const char*);
}

_LIT(KPddName, "Ethernet.USB");

static const TInt KRingN = 16;
static const TInt KFrameMax = 1536;

class DUsbEtherPdd;
static DUsbEtherPdd* gPdd = NULL;

struct TUsbRing
	{
	TUint8 iBuf[KRingN][KFrameMax];
	TUint16 iLen[KRingN];
	volatile TUint iHead, iTail;			// head: the USB thread; tail: the DFC (single producer, single consumer)
	TInt iCur;								// the slot ReceiveFrame copies from
	};
static TUsbRing gRing;

class DUsbEtherPdd : public DEthernet
	{
public:
	DUsbEtherPdd() : iRxDfc(RxDfc, this, Kern::DfcQue0(), 1), iStarted(EFalse) {}
	~DUsbEtherPdd()
		{
		iRxDfc.Cancel();
		phe_usb_net_down(0);
		if (gPdd == this)
			gPdd = NULL;
		}
	virtual TInt Start()
		{
		gRing.iHead = gRing.iTail = 0;
		iStarted = ETrue;
		return phe_usb_net_up(0) == 0 ? KErrNone : KErrNotReady;	// IFF_UP + cdce_init: the bulk transfers start
		}
	virtual void Stop(TStopMode)
		{
		iStarted = EFalse;
		phe_usb_net_down(0);
		}
	virtual TInt ValidateConfig(const TEthernetConfigV01& aConfig) const
		{ return (aConfig.iEthSpeed == KEthSpeedUnknown || aConfig.iEthDuplex == KEthDuplexUnknown) ? KErrNotSupported : KErrNone; }
	virtual TInt Configure(TEthernetConfigV01&)				{ return KErrNone; }
	virtual void MacConfigure(TEthernetConfigV01&)			{}		// the modem's address is fixed (ECM iMACAddress)
	virtual void GetConfig(TEthernetConfigV01& aConfig) const
		{
		aConfig.iEthSpeed = KEthSpeed100BaseTX;
		aConfig.iEthDuplex = KEthDuplexFull;
		TUint8 mac[PHE_USB_NET_EADDR_LEN];
		if (phe_usb_net_eaddr(0, mac) == 0)
			memcpy(aConfig.iEthAddress, mac, 6);
		}
	virtual void CheckConfig(TEthernetConfigV01&)			{}
	virtual void Caps(TDes8& aCaps) const
		{
		TEthernetCaps caps;
		aCaps.FillZ(aCaps.MaxLength());
		aCaps = caps.Left(Min(caps.Length(), aCaps.MaxLength()));
		}
	virtual TInt Send(TBuf8<KMaxEthernetPacket + 32>& aBuffer)
		{ return phe_usb_net_tx(0, aBuffer.Ptr(), aBuffer.Length()) > 0 ? KErrNone : KErrNotReady; }
	virtual TInt ReceiveFrame(TBuf8<KMaxEthernetPacket + 32>& aBuffer, TBool aOkToUse)
		{
		if (!aOkToUse)
			return KErrGeneral;								// the LDD is out of buffers: the frame is dropped
		aBuffer.Copy(gRing.iBuf[gRing.iCur], Min((TInt)gRing.iLen[gRing.iCur], aBuffer.MaxLength()));
		return KErrNone;
		}
	virtual TInt DisableIrqs()								{ return NKern::DisableAllInterrupts(); }
	virtual void RestoreIrqs(TInt aLevel)					{ NKern::RestoreInterrupts(aLevel); }
	virtual TDfcQue* DfcQ(TInt)								{ return Kern::DfcQue0(); }

	static void RxDfc(TAny* aPtr)
		{
		DUsbEtherPdd& p = *(DUsbEtherPdd*)aPtr;
		while (gRing.iTail != gRing.iHead)
			{
			gRing.iCur = gRing.iTail % KRingN;
			if (p.iLdd && p.iStarted)
				p.ReceiveIsr();								// the LDD pulls the frame with ReceiveFrame()
			++gRing.iTail;
			}
		}
public:
	TDfc iRxDfc;
	TBool iStarted;
	};

// on the USB thread (phe_usb_net.c): one received frame, valid until this returns
extern "C" void phe_usb_net_rx_cb(int aIdx, const void* aFrame, int aLen)
	{
	if (aIdx != 0 || !gPdd || aLen <= 0 || aLen > KFrameMax)
		return;
	const TUint h = gRing.iHead;
	if (h - gRing.iTail >= (TUint)KRingN)
		return;												// the LDD is behind: dropped (TCP recovers)
	memcpy(gRing.iBuf[h % KRingN], aFrame, aLen);
	gRing.iLen[h % KRingN] = (TUint16)aLen;
	__e32_memory_barrier();
	gRing.iHead = h + 1;
	gPdd->iRxDfc.Enque();
	}

class DUsbEtherFactory : public DPhysicalDevice
	{
public:
	DUsbEtherFactory()		{ iUnitsMask = 1; iVersion = TVersion(KEthernetMajorVersionNumber, KEthernetMinorVersionNumber, KEthernetBuildVersionNumber); }
	virtual TInt Install()	{ return SetName(&KPddName); }
	virtual void GetCaps(TDes8& aDes) const
		{
		TEthernetCaps caps;
		aDes.FillZ(aDes.MaxLength());
		aDes = caps.Left(Min(caps.Length(), aDes.MaxLength()));
		}
	virtual TInt Create(DBase*& aChannel, TInt, const TDesC8*, const TVersion&)
		{
		DUsbEtherPdd* p = new DUsbEtherPdd;
		aChannel = p;
		if (!p)
			return KErrNoMemory;
		gPdd = p;
		return KErrNone;
		}
	virtual TInt Validate(TInt aUnit, const TDesC8*, const TVersion& aVer)
		{
		// only once the modem's interface exists (the modem is in ECM/NCM mode and enumerated); one client at a time
		if (!Kern::QueryVersionSupported(iVersion, aVer) || aUnit != 0 || !phe_usb_net_name(0) || (gPdd && gPdd->iLdd))
			return KErrNotSupported;
		return KErrNone;
		}
	};

TInt UsbEtherInstall()
	{
	DPhysicalDevice* pd = new DUsbEtherFactory;
	TInt r = pd ? Kern::InstallPhysicalDevice(pd) : KErrNoMemory;
	ekseam_puts(r == KErrNone ? "[usbh] Ethernet.USB PDD installed (the modem's ECM/NCM interface)\n"
							  : "[usbh] Ethernet.USB PDD install failed\n");
	return r;
	}
