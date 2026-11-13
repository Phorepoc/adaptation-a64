// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// a64uart.cpp -- OURS (2026-10-09, Bluetooth rung R1):
// the phone's UART1 (the RTL8723CS Bluetooth link) as a serial PDD "Comm.A64", unit 1, under the REAL serial LDD
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
// Modem (-DPHE_MODEM = the MODEM=1 kernel build only): unit 3 = UART3 at 0x01C28C00 (GIC SPI 3, CCU bit 19, no
// hardware flow control) to the Quectel EG25-G. Opening unit 3 powers the modem with NuttX's pinephone_modem_init on its
// own DFC thread; STATUS (low = on) reads as KSignalDSR; closing runs the Zephyr power-off sequence (block below).
#include <kernel/kern_priv.h>
#include <drivers/comm.h>
#include <assp.h>
#include "syborg.h"
#ifdef PHE_MODEM
#include "phe_modem_power.h"	// modem-eg25/port: NuttX power-on + Zephyr power-off (C, NuttX environment)
#endif

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
	if (iUnit == KModemUnit)									// no modem-control lines on UART3: DSR = STATUS low
		{
		sig = KSignalCTS;
		if (ModemDsr())											// the modem block below: sequence done + STATUS low
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
// ---- The EG25-G power + UART3 -------------------------------------------------------------------------------------
// Power: modem-eg25/port/phe_modem_power.c = Apache NuttX's pinephone_modem_init() (unmodified) for on, the Zephyr
// modem_cellular power-off sequence (EG2x-G timing) for off; both block, so they run on the modem's own DFC thread.
static TDynamicDfcQue* ModemQ;
static TDfc* ModemUpDfc;
static TDfc* ModemDownDfc;
static volatile TInt ModemState;		// 0 off, 1 powering, 2 on (NuttX init returned OK), 3 powering down

extern "C" void phe_modem_sleep_ms(unsigned aMs)	{ NKern::Sleep(NKern::TimerTicks(aMs)); }	// NuttX up_mdelay (modem only)
static TBool ModemStatusOn()			{ return phe_modem_status_on(); }
static TBool ModemDsr()					{ return ModemState == 2 && ModemStatusOn(); }
static void ModemLog(const char* aStep)
	{
	ekseam_puts("[EKERN][modem] t="); ekseam_puthex(NKern::TickCount());
	ekseam_puts(" "); ekseam_puts(aStep);
	ekseam_puts(" on="); ekseam_puthex(ModemStatusOn()); ekseam_puts("\n");
	}

static void ModemUpFn(TAny*)
	{
	ModemLog("power-up (NuttX pinephone_modem_init)");
	const int r = phe_modem_power_on();
	ModemState = (r == 0) ? 2 : 0;
	ModemLog(r == 0 ? "modem ready (STATUS low)" : "power-up failed");
	}

static void ModemDownFn(TAny*)
	{
	ModemLog("power-down (Zephyr power-off sequence)");
	phe_modem_power_off();
	ModemState = 0;
	ModemLog("PWR_BAT off");
	}

static void A64ModemRelease()
	{
	A64SoundVoiceRoute(EFalse);
	ModemLog("port closed");
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
	phe_modem_uart_pins();
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
	ekseam_puts("[EKERN][uart3] Comm.A64 unit 3 (modem) created\n");
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
