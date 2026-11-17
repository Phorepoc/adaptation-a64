// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// phe_usb_comm.cpp -- the USB serial ports (FreeBSD's ucom: uftdi, u3g) as a Comm PDD, "Comm.USB", under the real
// serial LDD ("Comm", d_comm.cpp) that ECUART.CSY (RComm "COMM::<unit>") opens. Unit KFirstUnit + n = tty n of
// phe_usb_tty.c (for the EG25-G: u3g's ports in interface order, the AT port among them). The bytes move through the
// tty rings; the USB thread calls phe_usb_tty_notify when data came in or room was made, and the transmit side runs
// in the LDD's DFC thread. Modem lines: DTR/RTS are raised when the port opens (the tty layer); the PDD reports
// CTS/DSR/DCD as on while the device is attached (ucom keeps the real line state to itself).
#include <kernel/kern_priv.h>
#include <drivers/comm.h>

extern "C" {
#include "../phe_usb_port.h"
void ekseam_puts(const char*);
}

_LIT(KPddName, "Comm.USB");
_LIT(KDfcName, "UsbComm");
static const TInt KFirstUnit = 8;		// COMM::8.. (units 1 and 3 are the A64 UARTs)
static const TInt KMaxPorts = 8;

class DUsbCommFactory : public DPhysicalDevice
	{
public:
	DUsbCommFactory();
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const;
	virtual TInt Create(DBase*& aChannel, TInt aUnit, const TDesC8* anInfo, const TVersion& aVer);
	virtual TInt Validate(TInt aUnit, const TDesC8* anInfo, const TVersion& aVer);
public:
	TDynamicDfcQue* iDfcQ;
	};

class DUsbComm : public DComm
	{
public:
	DUsbComm(TInt aPort, TDynamicDfcQue* aQ);
	~DUsbComm();
	virtual TInt Start();
	virtual void Stop(TStopMode aMode);
	virtual void Break(TBool aState);
	virtual void EnableTransmit();
	virtual TUint Signals() const;
	virtual void SetSignals(TUint aSetMask, TUint aClearMask);
	virtual TInt ValidateConfig(const TCommConfigV01& aConfig) const;
	virtual void Configure(TCommConfigV01& aConfig);
	virtual void Caps(TDes8& aCaps) const;
	virtual TInt DisableIrqs();
	virtual void RestoreIrqs(TInt aIrq);
	virtual TDfcQue* DfcQ(TInt aUnit);
	virtual void CheckConfig(TCommConfigV01& aConfig);
	void Pump();					// tx: LDD buffer -> tty ring
	void Drain();					// rx: tty ring -> LDD
	static void EventDfcFn(TAny* aPtr);
public:
	TInt iPort;
	TBool iRunning;
	TDynamicDfcQue* iDfcQ;
	TDfc iEventDfc;
	TInt iPending;					// a TransmitIsr byte that did not fit, or -1
	};

static DUsbComm* volatile gPorts[KMaxPorts];

DUsbCommFactory::DUsbCommFactory()
	{
	iUnitsMask = 0xFFu << KFirstUnit;
	iVersion = TVersion(KCommsMajorVersionNumber, KCommsMinorVersionNumber, KCommsBuildVersionNumber);
	}

TInt DUsbCommFactory::Install()
	{
	TInt r = Kern::DynamicDfcQCreate(iDfcQ, 26, KDfcName);
	return r == KErrNone ? SetName(&KPddName) : r;
	}

static void UsbCommCaps(TDes8& aCaps)
	{
	TCommCaps3 capsBuf;
	TCommCapsV03& c = capsBuf();
	c.iRate = KCapsBps9600 | KCapsBps19200 | KCapsBps38400 | KCapsBps57600 | KCapsBps115200 | KCapsBps230400 |
			  KCapsBps460800 | KCapsBps921600;
	c.iDataBits = KCapsData8;
	c.iStopBits = KCapsStop1;
	c.iParity = KCapsParityNone;
	c.iSIR = 0;
	c.iHandshake = KCapsFreeRTSSupported | KCapsFreeDTRSupported;
	c.iSignals = KCapsSignalCTSSupported | KCapsSignalDSRSupported | KCapsSignalDCDSupported |
				 KCapsSignalRTSSupported | KCapsSignalDTRSupported;
	c.iFifo = 0;
	c.iNotificationCaps = KNotifyDataAvailableSupported | KNotifySignalsChangeSupported;
	c.iRoleCaps = 0;
	c.iFlowControlCaps = 0;
	c.iBreakSupported = EFalse;
	aCaps.FillZ(aCaps.MaxLength());
	aCaps = capsBuf.Left(Min(capsBuf.Length(), aCaps.MaxLength()));
	}

void DUsbCommFactory::GetCaps(TDes8& aDes) const	{ UsbCommCaps(aDes); }

TInt DUsbCommFactory::Validate(TInt aUnit, const TDesC8*, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(iVersion, aVer))
		return KErrNotSupported;
	if (aUnit < KFirstUnit || aUnit >= KFirstUnit + KMaxPorts)
		return KErrNotSupported;
	return phe_usb_tty_name(aUnit - KFirstUnit) ? KErrNone : KErrNotReady;	// not (yet) attached
	}

TInt DUsbCommFactory::Create(DBase*& aChannel, TInt aUnit, const TDesC8*, const TVersion&)
	{
	const TInt port = aUnit - KFirstUnit;
	if (gPorts[port])
		return KErrInUse;
	DUsbComm* p = new DUsbComm(port, iDfcQ);
	aChannel = p;
	if (!p)
		return KErrNoMemory;
	gPorts[port] = p;
	return phe_usb_tty_open(port) == 0 ? KErrNone : KErrNotReady;
	}

DUsbComm::DUsbComm(TInt aPort, TDynamicDfcQue* aQ)
	: iPort(aPort), iRunning(EFalse), iDfcQ(aQ), iEventDfc(EventDfcFn, this, aQ, 1), iPending(-1)
	{
	}

DUsbComm::~DUsbComm()
	{
	iEventDfc.Cancel();
	phe_usb_tty_close(iPort);
	gPorts[iPort] = NULL;
	}

TDfcQue* DUsbComm::DfcQ(TInt)	{ return iDfcQ; }

TInt DUsbComm::Start()
	{
	iRunning = ETrue;
	iTransmitting = EFalse;
	iLdd->UpdateSignals(Signals());
	iEventDfc.Enque();			// bytes that arrived before the open
	return KErrNone;
	}

void DUsbComm::Stop(TStopMode)
	{
	iRunning = EFalse;
	iTransmitting = EFalse;
	}

void DUsbComm::Break(TBool)	{ }

// tx, in the LDD's DFC thread (EnableTransmit) and on "room to write" (EventDfcFn): move bytes until the ring is full
void DUsbComm::Pump()
	{
	for (;;)
		{
		TInt c = iPending;
		iPending = -1;
		if (c < 0)
			{
			NKern::Lock();
			c = TransmitIsr();
			NKern::Unlock();
			}
		if (c < 0)
			{
			iTransmitting = EFalse;
			return;
			}
		const TUint8 b = (TUint8)c;
		if (phe_usb_tty_write(iPort, &b, 1) != 1)
			{
			iPending = c;			// the ring is full: the next "room" event goes on from here
			return;
			}
		}
	}

void DUsbComm::EnableTransmit()
	{
	if (__e32_atomic_swp_ord32(&iTransmitting, 1))
		return;
	Pump();
	}

// rx: everything the ring holds to the LDD
void DUsbComm::Drain()
	{
	TUint8 buf[64];
	TUint ch[64];
	TInt n;
	while ((n = phe_usb_tty_read(iPort, buf, sizeof(buf))) > 0)
		{
		for (TInt i = 0; i < n; i++)
			ch[i] = buf[i];
		NKern::Lock();
		ReceiveIsr(ch, n, 0);
		NKern::Unlock();
		}
	}

void DUsbComm::EventDfcFn(TAny* aPtr)
	{
	DUsbComm& d = *(DUsbComm*)aPtr;
	if (!d.iRunning)
		return;
	d.Drain();
	if (d.iTransmitting)
		d.Pump();
	}

TUint DUsbComm::Signals() const
	{
	return phe_usb_tty_name(iPort) ? (KSignalCTS | KSignalDSR | KSignalDCD) : 0;
	}

void DUsbComm::SetSignals(TUint, TUint)	{ }

TInt DUsbComm::ValidateConfig(const TCommConfigV01& aConfig) const
	{
	if (aConfig.iDataBits != EData8 || aConfig.iStopBits != EStop1 || aConfig.iParity != EParityNone)
		return KErrNotSupported;
	return KErrNone;
	}

void DUsbComm::Configure(TCommConfigV01&)	{ }	// 115200 8N1 from the tty layer; a USB AT port ignores the rate
void DUsbComm::CheckConfig(TCommConfigV01&)	{ }
void DUsbComm::Caps(TDes8& aCaps) const	{ UsbCommCaps(aCaps); }
TInt DUsbComm::DisableIrqs()	{ return NKern::DisableAllInterrupts(); }
void DUsbComm::RestoreIrqs(TInt aIrq)	{ NKern::RestoreInterrupts(aIrq); }

// on the USB thread (phe_usb_tty.c): data came in / room was made / the port went away
extern "C" void phe_usb_tty_notify(int aIdx, int aEvents)
	{
	if (aIdx < 0 || aIdx >= KMaxPorts)
		return;
	DUsbComm* p = gPorts[aIdx];
	if (p && (aEvents & (PHE_USB_TTY_RX | PHE_USB_TTY_TX | PHE_USB_TTY_GONE)))
		p->iEventDfc.Enque();
	}

TInt UsbCommInstall()
	{
	DPhysicalDevice* pd = new DUsbCommFactory;
	TInt r = pd ? Kern::InstallPhysicalDevice(pd) : KErrNoMemory;
	ekseam_puts(r == KErrNone ? "[usbh] Comm.USB PDD installed (COMM::8..15 = the USB serial ports)\n"
							  : "[usbh] Comm.USB PDD install failed\n");
	return r;
	}
