// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// a64uart.cpp -- OURS (2026-10-09, Bluetooth rung R1):
// the PinePhone's UART1 (the RTL8723CS Bluetooth link) as a serial PDD "Comm.A64", unit 1, under the REAL serial LDD
// (kernel/eka/drivers/ecomm/d_comm.cpp, ECOMM.LDD "Comm", linked into ekern with its entry renamed). The 2010 BT HCI
// transport plug-ins open this port directly through RBusDevComm (research/bluetooth-2010.md a.1).
//
// Hardware: a Synopsys DW-APB UART at 0x01C28400 (16550-compatible, 32-bit register stride, in UART0's page which
// the bootstrap maps as KHwBaseUart0), GIC SPI 1 (id 33), clock APB2 = 24 MHz (115200 = divisor 13, 1.5 Mbaud =
// divisor 1 exactly), CCU gate 0x06C bit 17 / reset 0x2D8 bit 17, pins PG6 TX / PG7 RX / PG8 RTS / PG9 CTS function 2.
// Model: the NaviEngine 16550 PDD (oss.FCL.sf.adapt.NECNaviengine/.../uart/uart16550_ne.cpp) with the DW-APB
// specifics from Linux 8250_dw.c: the BUSY-detect interrupt (IIR 0x7, cleared by reading USR 0x7C) and the baud
// change while busy (loopback + FIFO reset + USR.BUSY poll, the sunxi HALT register as the fallback), 32-bit access,
// bounded RX batches, rates computed from the clock (TBps or EBpsSpecial + iSpecialRate), auto RTS/CTS (MCR.AFCE)
// when the client asks for KConfigObeyCTS.
// The Bluetooth power pins ride on the port (OURS convention): KSignalDTR = the chip's enable PL4 (R_PIO) with the
// device-wake PH6 asserted (low) together with it; the IO rail (AXP803 DLDO4, 1.8 V) is switched on at create. The
// timing of the power sequence (enable low 100 ms, high 500 ms) is the client's: SetSignals runs in ISR paths too.
// Modem rung M1 (2026-10-17, -DPHE_MODEM = the MODEM=1 kernel build only): unit 3 =
// UART3 at 0x01C28C00 (GIC SPI 3, CCU bit 19, PD0 TX / PD1 RX function 3, no hardware flow control -- the DTS says
// RTS/CTS are not usable) = the Quectel EG25-G's main UART. Opening unit 3 powers the modem (A64ModemPowerUp, a
// bounded sequence on its own DFC thread, PWRKEY always released); the modem's STATUS (PH9, low = on) reads as
// KSignalDSR. Closing the port cuts VBAT only once STATUS says the modem is off (the client sends AT+QPOWD first).
#include <kernel/kern_priv.h>
#include <drivers/comm.h>
#include <assp.h>
#include "syborg.h"

extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(unsigned);
extern "C" TInt A64PmicUpdateReg(TUint8 aReg, TUint8 aClr, TUint8 aSet);	// a64_display.cpp (RSB to the AXP803)

_LIT(KPddName, "Comm.A64");
_LIT(KUartDfc, "A64Uart DFC");
_LIT(KModemDfc, "A64Modem");
static const TInt KUnit = 1;
static const TLinAddr KUart1 = KHwBaseUart0 + 0x400;
static const TLinAddr KUart3 = KHwBaseUart0 + 0xC00;
static const TInt KIntUart3 = 32 + 3;
static const TInt KModemUnit = 3;
static const TInt KIntUart1 = 32 + 1;
static const TUint KApb2Hz = 24000000;
// registers (32-bit stride)
static const TUint KRbr = 0x00, KThr = 0x00, KDll = 0x00, KIer = 0x04, KDlh = 0x04, KIir = 0x08, KFcr = 0x08,
				   KLcr = 0x0C, KMcr = 0x10, KLsr = 0x14, KMsr = 0x18, KUsr = 0x7C, KHalt = 0xA4;
static const TUint KIerRdai = 1, KIerThrei = 2, KIerRlsi = 4, KIerMsi = 8;
static const TUint KIirMask = 0xF, KIirMsi = 0x0, KIirNone = 0x1, KIirThre = 0x2, KIirRda = 0x4, KIirRls = 0x6,
				   KIirBusy = 0x7, KIirTimeout = 0xC;
static const TUint KFcrEnable = 0x01, KFcrRxReset = 0x02, KFcrTxReset = 0x04, KFcrRxTrigHalf = 0x80;
static const TUint KLcrDlab = 0x80, KLcrBreak = 0x40, KLcrParity = 0x08, KLcrEven = 0x10, KLcrStick = 0x20,
				   KLcrStop2 = 0x04;
static const TUint KMcrDtr = 0x01, KMcrRts = 0x02, KMcrLoop = 0x10, KMcrAfce = 0x20;
static const TUint KLsrDr = 0x01, KLsrOe = 0x02, KLsrPe = 0x04, KLsrFe = 0x08, KLsrBi = 0x10, KLsrThre = 0x20,
				   KLsrTemt = 0x40;
static const TUint KUsrBusy = 0x01;
static const TUint KHaltTx = 0x01, KHaltChCfgAtBusy = 0x02, KHaltChangeUpdate = 0x04;
// CCU
static const TUint KCcuBusGate3 = 0x06C, KCcuBusReset4 = 0x2D8, KUart1Bit = 17, KUart3Bit = 19;
// PIO: port G = 6, port H = 7 (PIO = KHwBaseCcuPio + 0x800, port n at n * 0x24: CFG0 +0, CFG1 +4, DAT +0x10)
static const TUint KPio = 0x800;
// R_PIO port L (KHwBaseRPio + 0xC00)
static const TUint KRPio = 0xC00;

static inline volatile TUint32& R32(TLinAddr aBase, TUint aOff) { return *(volatile TUint32*)(aBase + aOff); }

class DA64UartFactory : public DPhysicalDevice
	{
public:
	DA64UartFactory();
	~DA64UartFactory();
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const;
	virtual TInt Create(DBase*& aChannel, TInt aUnit, const TDesC8* anInfo, const TVersion& aVer);
	virtual TInt Validate(TInt aUnit, const TDesC8* anInfo, const TVersion& aVer);
public:
	TDynamicDfcQue* iDfcQ;
	};

class DA64Uart : public DComm
	{
public:
	DA64Uart();
	~DA64Uart();
	TInt DoCreate(TInt aUnit, TDynamicDfcQue* aDfcQ);
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
	static void Isr(TAny* aPtr);
	static TBool TxIdle(TAny* aPtr);
	static TUint RateOf(const TCommConfigV01& aConfig);
	void BtEnable(TBool aOn);
	inline volatile TUint32& R(TUint aOff) const { return R32(iBase, aOff); }
public:
	TInt iIrq;
	TInt iUnit;
	TLinAddr iBase;
	TUint iIer, iMcr, iFcr;										// followers
	TDynamicDfcQue* iDfcQ;
	};

static void PinFunction(TUint aPort, TUint aPin, TUint aFn)
	{
	volatile TUint32& cfg = R32(KHwBaseCcuPio, KPio + aPort * 0x24 + (aPin / 8) * 4);
	const TUint sh = (aPin % 8) * 4;
	cfg = (cfg & ~(0xFu << sh)) | (aFn << sh);
	}

static void PinWrite(TLinAddr aPioBase, TUint aPort, TUint aPin, TBool aHigh)
	{
	volatile TUint32& dat = R32(aPioBase, aPort * 0x24 + 0x10);
	if (aHigh) dat |= 1u << aPin; else dat &= ~(1u << aPin);
	}

DA64UartFactory::DA64UartFactory()
	{
	iUnitsMask = 1u << KUnit;
#ifdef PHE_MODEM
	iUnitsMask |= 1u << KModemUnit;
#endif
	iVersion = TVersion(KCommsMajorVersionNumber, KCommsMinorVersionNumber, KCommsBuildVersionNumber);
	}

DA64UartFactory::~DA64UartFactory()
	{
	if (iDfcQ)
		iDfcQ->Destroy();
	}

TInt DA64UartFactory::Install()
	{
	TInt r = Kern::DynamicDfcQCreate(iDfcQ, 27, KUartDfc);
	if (r == KErrNone)
		r = SetName(&KPddName);
	return r;
	}

static void A64UartCaps(TDes8& aCaps)
	{
	TCommCaps3 capsBuf;
	TCommCapsV03& c = capsBuf();
	// the rates the 24 MHz APB2 clock reaches within 0.5 %: 115200 and below, plus EBpsSpecial (e.g. 1500000)
	c.iRate = KCapsBps1200 | KCapsBps2400 | KCapsBps4800 | KCapsBps9600 | KCapsBps19200 | KCapsBps38400 |
			  KCapsBps57600 | KCapsBps115200 | KCapsBpsSpecial;
	c.iDataBits = KCapsData5 | KCapsData6 | KCapsData7 | KCapsData8;
	c.iStopBits = KCapsStop1 | KCapsStop2;
	c.iParity = KCapsParityNone | KCapsParityEven | KCapsParityOdd;
	c.iSIR = 0;
	c.iHandshake = KCapsObeyCTSSupported | KCapsFailCTSSupported | KCapsFreeRTSSupported | KCapsFreeDTRSupported;
	c.iSignals = KCapsSignalCTSSupported | KCapsSignalRTSSupported | KCapsSignalDTRSupported;
	c.iFifo = KCapsHasFifo;
	c.iNotificationCaps = KNotifyDataAvailableSupported | KNotifySignalsChangeSupported;
	c.iRoleCaps = 0;
	c.iFlowControlCaps = 0;
	c.iBreakSupported = ETrue;
	aCaps.FillZ(aCaps.MaxLength());
	aCaps = capsBuf.Left(Min(capsBuf.Length(), aCaps.MaxLength()));
	}

void DA64UartFactory::GetCaps(TDes8& aDes) const	{ A64UartCaps(aDes); }

TInt DA64UartFactory::Create(DBase*& aChannel, TInt aUnit, const TDesC8*, const TVersion&)
	{
	DA64Uart* pD = new DA64Uart;
	aChannel = pD;
	return pD ? pD->DoCreate(aUnit, iDfcQ) : KErrNoMemory;
	}

TInt DA64UartFactory::Validate(TInt aUnit, const TDesC8*, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(iVersion, aVer))
		return KErrNotSupported;
#ifdef PHE_MODEM
	if (aUnit == KModemUnit)
		return KErrNone;
#endif
	return aUnit == KUnit ? KErrNone : KErrNotSupported;
	}

DA64Uart::DA64Uart() : iIrq(-1), iUnit(KUnit), iBase(KUart1) {}

#ifdef PHE_MODEM
static void A64ModemRelease();
static TBool ModemDsr();
extern "C" void A64SoundVoiceRoute(TBool aOn);			// a64sound.cpp (MODEM=1): the EG25-G PCM <-> codec route
static TInt A64ModemCreate(DA64Uart& aPort);
#endif

DA64Uart::~DA64Uart()
	{
	if (iIrq >= 0)
		Interrupt::Unbind(iIrq);
#ifdef PHE_MODEM
	if (iUnit == KModemUnit)
		A64ModemRelease();
#endif
	}

TInt DA64Uart::DoCreate(TInt aUnit, TDynamicDfcQue* aDfcQ)
	{
	iDfcQ = aDfcQ;
#ifdef PHE_MODEM
	if (aUnit == KModemUnit)
		return A64ModemCreate(*this);
#endif
	if (aUnit != KUnit)
		return KErrNotSupported;
	// the chip's IO rail: AXP803 DLDO4 = 1.8 V (reg 0x18 = (1800 - 700) / 100 = 11), on (reg 0x12 bit 6)
	A64PmicUpdateReg(0x18, 0x1F, 0x0B);
	A64PmicUpdateReg(0x12, 0, 1u << 6);
	// pins: PG6..PG9 = uart1 (function 2); PH6 = output, high (device-wake is active-low: not asserted);
	// PL4 (R_PIO) = output, low (the chip off)
	PinFunction(6, 6, 2); PinFunction(6, 7, 2); PinFunction(6, 8, 2); PinFunction(6, 9, 2);
	PinFunction(7, 6, 1);
	PinWrite(KHwBaseCcuPio + KPio, 7, 6, ETrue);
	volatile TUint32& lcfg = R32(KHwBaseRPio, KRPio + 0x00);
	lcfg = (lcfg & ~(0xFu << 16)) | (1u << 16);
	PinWrite(KHwBaseRPio + KRPio, 0, 4, EFalse);
	// the UART: bus clock + reset, then a quiet state
	R32(KHwBaseCcuPio, KCcuBusGate3) |= 1u << KUart1Bit;
	R32(KHwBaseCcuPio, KCcuBusReset4) |= 1u << KUart1Bit;
	R(KIer) = 0; iIer = 0;
	R(KLcr) = 0x03;
	R(KFcr) = KFcrRxReset | KFcrTxReset; iFcr = 0;
	R(KMcr) = 0; iMcr = 0;
	(void)R(KUsr); (void)R(KLsr); (void)R(KMsr);
	TInt r = Interrupt::Bind(KIntUart1, Isr, this);
	if (r < 0)
		return r;
	iIrq = KIntUart1;
	ekseam_puts("[EKERN][uart1] Comm.A64 unit 1 created, iir="); ekseam_puthex(R(KIir));
	return KErrNone;
	}

TDfcQue* DA64Uart::DfcQ(TInt)	{ return iDfcQ; }

TInt DA64Uart::Start()
	{
	iTransmitting = EFalse;
	R(KIer) = iIer = KIerRdai | KIerRlsi | KIerMsi;
	iLdd->UpdateSignals(Signals());
	Interrupt::Enable(iIrq);
	return KErrNone;
	}

TBool DA64Uart::TxIdle(TAny* aPtr)
	{
	return ((DA64Uart*)aPtr)->R(KLsr) & KLsrTemt;
	}

void DA64Uart::Stop(TStopMode aMode)
	{
	R(KIer) = iIer = 0;
	Interrupt::Disable(iIrq);
	iTransmitting = EFalse;
	if (aMode != EStopEmergency)
		Kern::PollingWait(TxIdle, this, 3, 100);
	}

void DA64Uart::Break(TBool aState)
	{
	if (aState) R(KLcr) |= KLcrBreak; else R(KLcr) &= ~KLcrBreak;
	}

void DA64Uart::EnableTransmit()
	{
	if (__e32_atomic_swp_ord32(&iTransmitting, 1))
		return;
	iIer |= KIerThrei;
	R(KIer) = iIer;
	}

TUint DA64Uart::Signals() const
	{
	const TUint msr = (R(KMsr) >> 4) & 0xF;						// CTS DSR RI DCD
	TUint sig = msr & 3;										// KSignalCTS = 1, KSignalDSR = 2
#ifdef PHE_MODEM
	if (iUnit == KModemUnit)									// no modem-control lines on UART3: DSR = STATUS (PH9) low
		{
		sig = KSignalCTS;
		if (ModemDsr())											// the modem block below: sequence done + PH9 low
			sig |= KSignalDSR;
		return sig;
		}
#endif
	if (msr & 4) sig |= KSignalRNG;
	if (msr & 8) sig |= KSignalDCD;
	return sig;
	}

// DTR = the Bluetooth enable (PL4) + device-wake (PH6, active-low)
void DA64Uart::BtEnable(TBool aOn)
	{
	PinWrite(KHwBaseRPio + KRPio, 0, 4, aOn);
	PinWrite(KHwBaseCcuPio + KPio, 7, 6, !aOn);
	}

void DA64Uart::SetSignals(TUint aSetMask, TUint aClearMask)
	{
	if (aSetMask & KSignalRTS) iMcr |= KMcrRts;
	if (aClearMask & KSignalRTS) iMcr &= ~KMcrRts;
	R(KMcr) = iMcr;
#ifdef PHE_MODEM
	if (iUnit == KModemUnit)									// M5: unit 3 DTR = the voice call route (a64sound.cpp)
		{
		if (aSetMask & KSignalDTR) A64SoundVoiceRoute(ETrue);
		else if (aClearMask & KSignalDTR) A64SoundVoiceRoute(EFalse);
		return;
		}
#endif
	if (iUnit != KUnit)
		return;
	if (aSetMask & KSignalDTR) BtEnable(ETrue);
	else if (aClearMask & KSignalDTR) BtEnable(EFalse);
	}

TUint DA64Uart::RateOf(const TCommConfigV01& aConfig)
	{
	static const TUint KBps[] = { 50, 75, 110, 134, 150, 300, 600, 1200, 1800, 2000, 2400, 3600, 4800, 7200, 9600,
								  19200, 38400, 57600, 115200, 230400, 460800, 576000, 1152000, 4000000, 921600 };
	if (aConfig.iRate == EBpsSpecial)
		return (TUint)aConfig.iSpecialRate;
	if ((TUint)aConfig.iRate < sizeof(KBps) / sizeof(KBps[0]))
		return KBps[aConfig.iRate];
	return 0;
	}

TInt DA64Uart::ValidateConfig(const TCommConfigV01& aConfig) const
	{
	if (aConfig.iSIREnable == ESIREnable)
		return KErrNotSupported;
	if (aConfig.iParity != EParityNone && aConfig.iParity != EParityEven && aConfig.iParity != EParityOdd)
		return KErrNotSupported;
	const TUint bps = RateOf(aConfig);
	if (bps < 1200 || bps > KApb2Hz / 16)
		return KErrNotSupported;
	// the divisor must land within 2 %
	const TUint div = (KApb2Hz + 8 * bps) / (16 * bps);
	const TUint got = KApb2Hz / (16 * div);
	const TUint err = got > bps ? got - bps : bps - got;
	return err * 50 <= bps ? KErrNone : KErrNotSupported;
	}

void DA64Uart::CheckConfig(TCommConfigV01&) {}

void DA64Uart::Configure(TCommConfigV01& aConfig)
	{
	TUint lcr = 0;
	switch (aConfig.iDataBits)
		{
		case EData5: lcr = 0; break;
		case EData6: lcr = 1; break;
		case EData7: lcr = 2; break;
		default: lcr = 3; break;
		}
	if (aConfig.iStopBits == EStop2) lcr |= KLcrStop2;
	if (aConfig.iParity == EParityEven) lcr |= KLcrParity | KLcrEven;
	else if (aConfig.iParity == EParityOdd) lcr |= KLcrParity;
	const TUint bps = RateOf(aConfig);
	TUint div = bps ? (KApb2Hz + 8 * bps) / (16 * bps) : 13;
	if (div < 1) div = 1;
	// the DW-APB rule: LCR/DLL/DLH are ignored while USR.BUSY -- quiesce first (8250_dw.c): no interrupts, TX
	// drained, loopback (no RX), FIFOs reset, stray RBR read, then poll BUSY; the sunxi HALT register as the fallback
	const TUint ier = iIer;
	R(KIer) = 0;
	Kern::PollingWait(TxIdle, this, 3, 100);
	R(KMcr) = iMcr | KMcrLoop;
	R(KFcr) = KFcrRxReset | KFcrTxReset;
	for (TInt n = 0; n < 1000 && (R(KLsr) & KLsrDr); ++n)
		(void)R(KRbr);
	TInt busy = 0;
	for (busy = 0; busy < 200 && (R(KUsr) & KUsrBusy); ++busy)
		Kern::NanoWait(10000);
	if (R(KUsr) & KUsrBusy)
		{
		R(KHalt) = KHaltTx | KHaltChCfgAtBusy;
		R(KLcr) = lcr | KLcrDlab;
		R(KDll) = div & 0xFF;
		R(KDlh) = (div >> 8) & 0xFF;
		R(KLcr) = lcr;
		R(KHalt) = KHaltTx | KHaltChCfgAtBusy | KHaltChangeUpdate;
		for (TInt n = 0; n < 1000 && (R(KHalt) & KHaltChangeUpdate); ++n)
			Kern::NanoWait(1000);
		R(KHalt) = 0;
		}
	else
		{
		R(KLcr) = lcr | KLcrDlab;
		R(KDll) = div & 0xFF;
		R(KDlh) = (div >> 8) & 0xFF;
		R(KLcr) = lcr;
		}
	for (TInt n = 0; n < 10 && (R(KLcr) & 0xFF) != lcr; ++n)	// 8250_dw.c: check the write took
		{
		(void)R(KUsr);
		R(KLcr) = lcr;
		}
	// flow control: auto RTS/CTS in hardware when the client obeys CTS
	if (aConfig.iHandshake & KConfigObeyCTS) iMcr |= KMcrAfce | KMcrRts; else iMcr &= ~KMcrAfce;
	R(KMcr) = iMcr;												// loopback off
	iFcr = KFcrEnable | KFcrRxTrigHalf;
	R(KFcr) = iFcr | KFcrRxReset | KFcrTxReset;
	for (TInt n = 0; n < 64 && (R(KLsr) & KLsrDr); ++n)
		(void)R(KRbr);
	R(KIer) = iIer = ier;
	ekseam_puts(iUnit == KUnit ? "[EKERN][uart1] configured bps=" : "[EKERN][uart3] configured bps="); ekseam_puthex(bps);
	ekseam_puts(" div="); ekseam_puthex(div);
	ekseam_puts(" lcr="); ekseam_puthex(R(KLcr));
	ekseam_puts(" mcr="); ekseam_puthex(iMcr);
	ekseam_puts(" busy loops="); ekseam_puthex((unsigned)busy);
	}

void DA64Uart::Caps(TDes8& aCaps) const	{ A64UartCaps(aCaps); }

TInt DA64Uart::DisableIrqs()			{ return NKern::DisableInterrupts(1); }
void DA64Uart::RestoreIrqs(TInt aLevel)	{ NKern::RestoreInterrupts(aLevel); }

void DA64Uart::Isr(TAny* aPtr)
	{
	DA64Uart& d = *(DA64Uart*)aPtr;
	for (TInt guard = 0; guard < 16; ++guard)
		{
		const TUint iir = d.R(KIir) & KIirMask;
		if (iir == KIirNone)
			return;
		if (iir == KIirBusy)
			{
			(void)d.R(KUsr);									// the busy-detect: clear it, retry the LCR later
			continue;
			}
		if (iir == KIirRda || iir == KIirTimeout || iir == KIirRls)
			{
			const TUint xon = d.iLdd->iRxXonChar, xoff = d.iLdd->iRxXoffChar;
			TUint rx[32];
			TInt n = 0, x = 0;
			TUint lsr;
			while (((lsr = d.R(KLsr)) & (KLsrDr | KLsrOe | KLsrPe | KLsrFe | KLsrBi)) && Kern::PowerGood())
				{
				if (!(lsr & KLsrDr))
					{
					(void)d.R(KRbr);							// an error with no data: consume
					continue;
					}
				TUint ch = d.R(KRbr) & 0xFF;
				if (lsr & KLsrPe) ch |= KReceiveIsrParityError;
				if (lsr & KLsrFe) ch |= KReceiveIsrFrameError;
				if (lsr & KLsrOe) ch |= KReceiveIsrOverrunError;
				if (lsr & KLsrBi) ch |= KReceiveIsrBreakError;
				if (ch == xon) x = 1;
				else if (ch == xoff) x = -1;
				else rx[n++] = ch;
				if (n == 32)
					{
					d.ReceiveIsr(rx, n, x);
					n = 0; x = 0;
					}
				}
			if (n || x)
				d.ReceiveIsr(rx, n, x);
			continue;
			}
		if (iir == KIirThre)
			{
			for (TInt i = 0; i < 32; ++i)						// the DW FIFO is 64 deep; half of it per THRE
				{
				const TInt r = d.TransmitIsr();
				if (r < 0)
					{
					d.iIer &= ~KIerThrei;
					d.R(KIer) = d.iIer;
					d.iTransmitting = EFalse;
					break;
					}
				d.R(KThr) = (TUint)r & 0xFF;
				}
			d.CheckTxBuffer();
			continue;
			}
		if (iir == KIirMsi)
			{
			d.StateIsr(d.Signals());
			continue;
			}
		return;
		}
	}

#ifdef PHE_MODEM
// ---- Modem rung M1: the EG25-G power pins + UART3 -----------------------------------------------------------------
// Pins (verified 2026-10-17 against eg25-manager data/pine64,pinephone-1.2.toml -- dtr 34, pwrkey 35, reset 68,
// apready 231, disable 232, status 233 on 1c20800.pinctrl -- and megi's modem-power DT node: power-supply reg_vbat_bb
// = PL7, enable PH8 GPIO_ACTIVE_LOW, reset PC4, pwrkey PB3, status PH9, host-ready PH7, dtr PB2, RI PL6; all through
// transistors, so the levels below are the GPIO levels Linux writes). Sequence = eg25-manager gpio.c
// gpio_sequence_poweron: RESET + PWRKEY high, 200 ms, RESET low, 1 s, PWRKEY low. Every path releases PWRKEY.
static const TUint KPortB = 1, KPortC = 2, KPortD = 3, KPortH = 7;
static TDynamicDfcQue* ModemQ;
static TDfc* ModemUpDfc;
static TDfc* ModemDownDfc;
static volatile TInt ModemState;		// 0 off, 1 powering, 2 sequence done (STATUS says the rest), 3 powering down

static TBool ModemStatusOn()			{ return !(R32(KHwBaseCcuPio, KPio + KPortH * 0x24 + 0x10) & (1u << 9)); }
static void ModemPin(TUint aPort, TUint aPin, TBool aHigh)	{ PinWrite(KHwBaseCcuPio + KPio, aPort, aPin, aHigh); }
static void ModemVbat(TBool aOn)		{ PinWrite(KHwBaseRPio + KRPio, 0, 7, aOn); }
static TBool ModemDsr()					{ return ModemState == 2 && ModemStatusOn(); }
static void ModemSleepMs(TInt aMs)		{ NKern::Sleep(NKern::TimerTicks(aMs)); }

// raw levels for the log: PH9 (STATUS), PL7 (VBAT), with the nanokernel tick (1 ms) as the timestamp
static TUint ModemPh9()					{ return (R32(KHwBaseCcuPio, KPio + KPortH * 0x24 + 0x10) >> 9) & 1; }
static TUint ModemPl7()					{ return (R32(KHwBaseRPio, KRPio + 0x10) >> 7) & 1; }
static void ModemLog(const char* aStep)
	{
	ekseam_puts("[EKERN][modem] t="); ekseam_puthex(NKern::TickCount());
	ekseam_puts(" "); ekseam_puts(aStep);
	ekseam_puts(" PH9(status)="); ekseam_puthex(ModemPh9());
	ekseam_puts(" PL7(vbat)="); ekseam_puthex(ModemPl7()); ekseam_puts("\n");
	}

static void ModemUpFn(TAny*)
	{
	// Boot 895 lesson: with VBAT off, PH9 reads LOW (nothing drives the STATUS transistor), which looks like "on" --
	// so STATUS is only trusted once VBAT has been on. The pulse is skipped only if VBAT was ALREADY on before we
	// came and STATUS reads low (on); a modem whose VBAT we switch on is off by definition and always gets the pulse.
	ModemLog("power-up start");
	if (ModemPl7() && ModemStatusOn())
		{
		ModemLog("VBAT already on and STATUS low (on): no PWRKEY pulse");
		ModemState = 2;
		return;
		}
	ModemVbat(ETrue);
	ModemLog("VBAT on");
	ModemSleepMs(100);										// VBAT_BB settle (eg25-manager uses >= 60 ms)
	ModemLog("VBAT settled");
	ModemPin(KPortC, 4, ETrue);								// RESET asserted
	ModemPin(KPortB, 3, ETrue);								// PWRKEY asserted
	ModemLog("RESET+PWRKEY high");
	ModemSleepMs(200);
	ModemPin(KPortC, 4, EFalse);
	ModemLog("RESET low");
	ModemSleepMs(1000);
	ModemPin(KPortB, 3, EFalse);							// PWRKEY released: never held longer than 1.2 s
	ModemLog("PWRKEY low (sequence done)");
	ModemState = 2;
	// watch STATUS for 15 s (bounded; only the changes logged) so the PH9 polarity is recorded from the hardware
	TUint last = ModemPh9();
	for (TInt i = 0; i < 150; ++i)
		{
		ModemSleepMs(100);
		const TUint now = ModemPh9();
		if (now != last)
			{
			ModemLog("STATUS changed");
			last = now;
			}
		}
	ModemLog("STATUS watch end");
	}

// Power-down on close (ModemQ thread, after any running power-up): a modem is never left on. The client should have
// sent AT+QPOWD=1 already (STATUS high); if STATUS still says on, the Quectel PWRKEY power-off pulse (eg25-manager
// gpio_sequence_shutdown: PWRKEY 1 s; EG25-G spec >= 650 ms), then up to 30 s for STATUS high, then VBAT off --
// cut even when STATUS never drops (logged "FORCED": the last resort).
static void ModemDownFn(TAny*)
	{
	ModemLog("power-down start");
	if (ModemStatusOn())
		{
		ModemPin(KPortB, 3, ETrue);
		ModemLog("PWRKEY high (off-pulse)");
		ModemSleepMs(1000);
		ModemPin(KPortB, 3, EFalse);						// released: bounded 1 s
		ModemLog("PWRKEY low");
		for (TInt i = 0; i < 300 && ModemStatusOn(); ++i)	// 30 s
			ModemSleepMs(100);
		}
	const TBool forced = ModemStatusOn();
	if (!forced)
		ModemSleepMs(500);									// STATUS high, then a short margin before VBAT goes
	ModemVbat(EFalse);
	ModemPin(KPortC, 4, EFalse);
	ModemState = 0;
	ModemLog(forced ? "VBAT off FORCED (STATUS never dropped)" : "VBAT off (STATUS off)");
	}

static void A64ModemRelease()
	{
	A64SoundVoiceRoute(EFalse);
	ModemLog("port closed");
	ekseam_puts("[EKERN][modem] close: state="); ekseam_puthex(ModemState); ekseam_puts(" -> power-down queued\n");
	ModemState = 3;
	ModemDownDfc->Enque();
	}

static TInt A64ModemCreate(DA64Uart& aPort)
	{
	aPort.iUnit = KModemUnit;
	aPort.iBase = KUart3;
	if (!ModemQ)
		{
		TInt r = Kern::DynamicDfcQCreate(ModemQ, 24, KModemDfc);
		if (r != KErrNone)
			return r;
		ModemUpDfc = new TDfc(ModemUpFn, NULL, ModemQ, 1);
		ModemDownDfc = new TDfc(ModemDownFn, NULL, ModemQ, 1);
		if (!ModemUpDfc || !ModemDownDfc)
			return KErrNoMemory;
		}
	// control pins: outputs at their idle levels before anything else (DTR low = awake, AP_READY low = host awake,
	// W_DISABLE# low = RF enabled, RESET/PWRKEY released); STATUS PH9 input; VBAT PL7 output (level kept)
	ModemPin(KPortB, 2, EFalse); PinFunction(KPortB, 2, 1);
	ModemPin(KPortB, 3, EFalse); PinFunction(KPortB, 3, 1);
	ModemPin(KPortC, 4, EFalse); PinFunction(KPortC, 4, 1);
	ModemPin(KPortH, 7, EFalse); PinFunction(KPortH, 7, 1);
	ModemPin(KPortH, 8, EFalse); PinFunction(KPortH, 8, 1);
	PinFunction(KPortH, 9, 0);
	volatile TUint32& lcfg = R32(KHwBaseRPio, KRPio + 0x00);
	lcfg = (lcfg & ~(0xFu << 28)) | (1u << 28);
	// UART3 pins PD0/PD1 = function 3, bus clock + reset, a quiet state
	PinFunction(KPortD, 0, 3); PinFunction(KPortD, 1, 3);
	R32(KHwBaseCcuPio, KCcuBusGate3) |= 1u << KUart3Bit;
	R32(KHwBaseCcuPio, KCcuBusReset4) |= 1u << KUart3Bit;
	aPort.R(KIer) = 0; aPort.iIer = 0;
	aPort.R(KLcr) = 0x03;
	aPort.R(KFcr) = KFcrRxReset | KFcrTxReset; aPort.iFcr = 0;
	aPort.R(KMcr) = 0; aPort.iMcr = 0;
	(void)aPort.R(KUsr); (void)aPort.R(KLsr); (void)aPort.R(KMsr);
	TInt r = Interrupt::Bind(KIntUart3, DA64Uart::Isr, &aPort);
	if (r < 0)
		return r;
	aPort.iIrq = KIntUart3;
	ekseam_puts("[EKERN][uart3] Comm.A64 unit 3 (modem) created, iir="); ekseam_puthex(aPort.R(KIir));
	ekseam_puts(" lsr="); ekseam_puthex(aPort.R(KLsr)); ekseam_puts("\n");
	ModemLog("pins idle");
	if (ModemState == 0 || ModemState == 3)				// after a queued power-down: runs after it on ModemQ
		{
		ModemState = 1;
		ModemUpDfc->Enque();
		}
	return KErrNone;
	}
#endif

DLogicalDevice* CreateLogicalDevice_ecomm();					// d_comm.cpp (DECLARE_STANDARD_LDD, renamed)

// Board entry (a64_glue.cpp): the real serial LDD "Comm" + this PDD. User::LoadLogicalDevice("ECOMM") is answered
// KErrAlreadyExists by the loader delta (ldr_builtin_devices.cpp alias ecomm -> comm).
extern "C" void A64UartInstall()
	{
	NKern::ThreadEnterCS();
	DLogicalDevice* ld = CreateLogicalDevice_ecomm();
	TInt r = ld ? Kern::InstallLogicalDevice(ld) : KErrNoMemory;
	ekseam_puts("[EKERN][uart1] InstallLogicalDevice(Comm) r="); ekseam_puthex((unsigned)r);
	DPhysicalDevice* pd = new DA64UartFactory;
	r = pd ? Kern::InstallPhysicalDevice(pd) : KErrNoMemory;
	ekseam_puts("[EKERN][uart1] InstallPhysicalDevice(Comm.A64) r="); ekseam_puthex((unsigned)r);
	NKern::ThreadLeaveCS();
	}
