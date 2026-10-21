// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// a64sdio.cpp -- OURS (2026-10-10, PinePhone WiFi rung 0:
// 1.3): the A64's SMHC1 as an SDIO host for the RTL8723CS's WiFi half (SDIO
// function 1, vendor 024c device b703). Nothing of this exists before us: U-Boot never touches mmc1, so power, pins,
// clocks, the controller reset, the sunxi clock-change protocol and the SDIO identification (CMD0/5/3/7, CMD52 to the
// CCCR + CIS) are all here. Polled PIO through the FIFO, no interrupt, no IDMA -- the shape of the SD-card code in
// a64_glue.cpp (SMHC0), and the same register conventions (sunxi-mmc.c is the reference for every bit).
//   A64_SDIO_TEST (A64DEFS): A64SdioInstall() runs the whole bring-up at boot as a self-test and logs the OCR, the RCA,
//   the CCCR, the CIS manufacturer/card ids, then the 4-bit high-speed switch and a CMD53 byte read of the CCCR --
//   rung 0's M0/M1/M2 in one boot. Without it A64SdioInstall() does nothing yet (the host PDD comes with the chip core).
// Hardware facts (verified against the DTS / CCU driver in wifi-linux.md 1.1): SMHC1 0x01C10000, GIC SPI 61 (id 93),
// pins PG0..PG5 fn 2 (CLK CMD D0-D3), CCU bus gate 0x060 b9, reset 0x2C0 b9, module clock SDMMC1_CLK 0x08C
// (gate b31, src b25:24 0 = 24 MHz / 1 = PLL_PERIPH0 x2 = 1.2 GHz, N b17:16 = /2^N, M b3:0 = /(M+1), and the "new
// timing mode" halves it again), IO rail DLDO4 1.8 V (on since the BT PDD), VBAT fixed, the WiFi reset PL2 (active-low,
// board rev 1.2 only; tied high on older boards, so driving it is harmless there), kill switch 2 must be on.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <nkern.h>
#include <syborg.h>

extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(TUint32);			// ends the line
extern "C" TInt A64PmicUpdateReg(TUint8 aReg, TUint8 aClr, TUint8 aSet);

static const TPhysAddr KSmhc1Phys = 0x01C10000u;
// SMHC registers (sunxi-mmc.c)
enum { KGctl = 0x00, KClkcr = 0x04, KTmout = 0x08, KWidth = 0x0C, KBlksz = 0x10, KBcntr = 0x14, KCmdr = 0x18,
	   KCarg = 0x1C, KResp0 = 0x20, KImask = 0x30, KMista = 0x34, KRintr = 0x38, KStas = 0x3C, KFtrgl = 0x40,
	   KFuns = 0x44, KDbgc = 0x50, KNtsr = 0x5C, KHwrst = 0x78, KSampDl = 0x144, KFifo = 0x200 };
static const TUint32 KGctlSoftReset = 1u << 0, KGctlFifoReset = 1u << 1, KGctlDmaReset = 1u << 2,
	KGctlAccessByAhb = 1u << 31;
static const TUint32 KClkOn = 1u << 16, KClkLowPower = 1u << 17, KClkMaskData0 = 1u << 31;
static const TUint32 KCmdStart = 1u << 31, KCmdUpclkOnly = 1u << 21, KCmdSendInit = 1u << 15, KCmdWaitPre = 1u << 13,
	KCmdWrite = 1u << 10, KCmdData = 1u << 9, KCmdCrc = 1u << 8, KCmdLong = 1u << 7, KCmdResp = 1u << 6;
static const TUint32 KRintRespErr = 1u << 1, KRintCmdDone = 1u << 2, KRintDataOver = 1u << 3, KRintRespCrc = 1u << 6,
	KRintDataCrc = 1u << 7, KRintRespTimeout = 1u << 8, KRintDataTimeout = 1u << 9, KRintSdio = 1u << 16,
	KRintErrors = 0xBBC2u;				// RESP_ERR, RESP/DATA CRC, RESP/DATA timeout, FIFO run, HW wrong, start/end bit
static const TUint32 KStaFifoEmpty = 1u << 2, KStaFifoFull = 1u << 3, KStaCardBusy = 1u << 9;
static const TUint32 KNtsr2xTiming = 1u << 31, KSampDlSwEn = 1u << 7;
// CCU
static const TUint KCcuBusGate0 = 0x060, KCcuBusReset0 = 0x2C0, KCcuSdmmc1Clk = 0x08C, KMmc1Bit = 9;
static const TUint KPio = 0x800, KRPio = 0xC00;
// SDIO (the SDIO spec part E1 / Linux sdio_ops.c)
static const TUint KCccrRev = 0x00, KCccrSdSpec = 0x01, KCccrIoEnable = 0x02, KCccrIoReady = 0x03, KCccrIntEnable = 0x04,
	KCccrBusIf = 0x07, KCccrCaps = 0x08, KCccrCisPtr = 0x09, KCccrSpeed = 0x13;
static const TUint8 KCisTplManfid = 0x20, KCisTplFuncId = 0x21, KCisTplEnd = 0xFF;

static volatile TUint32* Smhc1;
static DPlatChunkHw* Smhc1Chunk;
static inline volatile TUint32& R32(TLinAddr aBase, TUint aOff) { return *(volatile TUint32*)(aBase + aOff); }
static inline volatile TUint32& S(TUint aOff) { return Smhc1[aOff >> 2]; }
static const char KNl[] = { 10, 0 };
static void Say(const char* aTag, TUint32 aValue) { ekseam_puts(aTag); ekseam_puthex(aValue); }
static void Wait(TInt aMs) { NKern::Sleep(NKern::TimerTicks(aMs)); }

static TBool WaitClear(TUint aOff, TUint32 aMask, TUint aLoops = 5000000u)
	{
	for (TUint i = 0; i < aLoops; ++i)
		if (!(S(aOff) & aMask))
			return ETrue;
	return EFalse;
	}

// the sunxi clock-change protocol (sunxi-mmc.c sunxi_mmc_clk_set_rate / sunxi_mmc_oclk_onoff): the card clock off,
// UPCLK_ONLY, the CCU rate, the internal divider, the new timing mode, then the clock back on the same way
static TBool UpdateClock()
	{
	S(KCmdr) = KCmdStart | KCmdUpclkOnly | KCmdWaitPre;
	for (TUint i = 0; i < 5000000u; ++i)
		if (!(S(KCmdr) & KCmdStart))
			{
			S(KRintr) = S(KRintr) & ~KRintSdio;		// clear everything but a pending card interrupt
			return ETrue;
			}
	ekseam_puts("[sdio] UPCLK_ONLY stuck\n");
	return EFalse;
	}

// aNewMode: the SMHC's "new timing mode" (NTSR 2X; the module clock is twice the card clock; Linux's choice for the A64,
// with IDMA) or the old mode U-Boot uses for the SD card on SMHC0 (CCU output/sample phase delays in bits 8-10 / 20-22,
// as U-Boot's sunxi_mmc.c mmc_config_clock: <= 400 kHz 0/0, <= 25 MHz 0/5, <= 52 MHz 3/4; the module clock = the card
// clock). Boots 193-197: in new mode every CMD53 was answered and no data byte was ever sampled.
static TBool SetClock(TUint aSrc, TUint aN, TUint aM, TBool aNewMode, TUint aOclkDly, TUint aSclkDly, const char* aWhat)
	{
	S(KClkcr) = (S(KClkcr) & ~(KClkOn | KClkLowPower)) | KClkMaskData0;
	if (!UpdateClock())
		return EFalse;
	if (aNewMode)
		{
		R32(KHwBaseCcuPio, KCcuSdmmc1Clk) = (1u << 31) | (1u << 30) | (aSrc << 24) | (aN << 16) | aM;
		S(KNtsr) |= KNtsr2xTiming;
		}
	else
		{
		R32(KHwBaseCcuPio, KCcuSdmmc1Clk) = (1u << 31) | (aSrc << 24) | (aSclkDly << 20) | (aN << 16) | (aOclkDly << 8) | aM;
		S(KNtsr) &= ~KNtsr2xTiming;
		}
	S(KClkcr) = (S(KClkcr) & ~0xFFu) | 0;					// internal divider 1 (the CCU does the dividing)
	S(KSampDl) = KSampDlSwEn;								// sample delay 0, software calibration, as Allwinner does
	S(KClkcr) |= KClkOn;
	const TBool ok = UpdateClock();
	// MASK_DATA0 is for the clock change only: left on, the controller never sees the card's data on DAT0
	// (boots 193-196: every CMD53 was answered but the data FSM waited for a start bit for ever)
	S(KClkcr) &= ~KClkMaskData0;
	ekseam_puts("[sdio] clock "); ekseam_puts(aWhat); Say(" ccu=", R32(KHwBaseCcuPio, KCcuSdmmc1Clk)); Say("[sdio]   clkcr=", S(KClkcr)); Say("[sdio]   ntsr=", S(KNtsr));
	return ok;
	}

// one command, polled; the response (if any) is left in RESP0..3
static TBool Cmd(TUint32 aFlags, TUint aIndex, TUint32 aArg, const char* aWhat)
	{
	if (!WaitClear(KStas, KStaCardBusy))
		{
		ekseam_puts("[sdio] "); ekseam_puts(aWhat); ekseam_puts(": card busy before the command\n");
		return EFalse;
		}
	S(KRintr) = 0xFFFFFFFFu;
	S(KCarg) = aArg;
	S(KCmdr) = KCmdStart | KCmdWaitPre | aFlags | aIndex;
	TUint32 r = 0;
	for (TUint i = 0; i < 5000000u; ++i)
		{
		r = S(KRintr);
		if (r & (KRintCmdDone | KRintErrors))
			break;
		}
	S(KRintr) = r & ~KRintSdio;
	if (!(r & KRintCmdDone) || (r & KRintErrors))
		{
		ekseam_puts("[sdio] "); ekseam_puts(aWhat); Say(" FAILED rint=", r);
		return EFalse;
		}
	return ETrue;
	}

// CMD52 IO_RW_DIRECT: fn, address, write + data; the R5 data byte comes back (or -1)
static TInt Cmd52(TUint aFn, TUint aAddr, TBool aWrite, TUint8 aData, TBool aQuiet = EFalse)
	{
	const TUint32 arg = (aWrite ? 1u << 31 : 0) | (aFn << 28) | (aWrite ? 1u << 27 : 0) | ((aAddr & 0x1FFFF) << 9) | (aWrite ? aData : 0);
	if (!Cmd(KCmdResp | KCmdCrc, 52, arg, aQuiet ? "CMD52" : "CMD52"))
		return -1;
	const TUint32 r5 = S(KResp0);
	if (r5 & 0xCB00)										// R5 flags: COM_CRC, ILLEGAL_COMMAND, ERROR, FUNCTION_NUMBER, OUT_OF_RANGE
		{
		Say("[sdio] CMD52 R5 error flags r5=", r5);
		return -1;
		}
	return r5 & 0xFF;
	}

// a data-read command (CMD53 byte mode, or CMD17 for the SD-card self-check), PIO through the FIFO into aBuf
static TBool DataRead(TUint aIndex, TUint32 aArg, TUint aBlksz, TUint aCount, TUint32* aBuf, const char* aWhat)
	{
	S(KGctl) |= KGctlAccessByAhb;
	S(KBlksz) = aBlksz;
	S(KBcntr) = aCount;
	S(KRintr) = 0xFFFFFFFFu;
	S(KCarg) = aArg;
	S(KCmdr) = KCmdStart | KCmdWaitPre | KCmdData | KCmdResp | KCmdCrc | aIndex;
	const TUint words = (aCount + 3) / 4;
	for (TUint w = 0; w < words; ++w)
		{
		if (!WaitClear(KStas, KStaFifoEmpty))
			{
			ekseam_puts("[sdio] "); ekseam_puts(aWhat); Say(" FIFO wait rint=", S(KRintr));
			S(KGctl) |= KGctlFifoReset;
			return EFalse;
			}
		aBuf[w] = S(KFifo);
		}
	TUint32 r = 0;
	for (TUint i = 0; i < 5000000u; ++i)
		{
		r = S(KRintr);
		if ((r & KRintErrors) || ((r & (KRintCmdDone | KRintDataOver)) == (KRintCmdDone | KRintDataOver)))
			break;
		}
	S(KRintr) = r & ~KRintSdio;
	if ((r & KRintErrors) || !(r & KRintDataOver))
		{
		ekseam_puts("[sdio] "); ekseam_puts(aWhat); Say(" FAILED rint=", r);
		S(KGctl) |= KGctlFifoReset;
		return EFalse;
		}
	return ETrue;
	}

// CMD53 IO_RW_EXTENDED, byte mode, read into aBuf (aCount 1..512), PIO through the FIFO
static TBool Cmd53Read(TUint aFn, TUint aAddr, TBool aIncr, TUint aCount, TUint32* aBuf)
	{
	const TUint32 arg = (aFn << 28) | (aIncr ? 1u << 26 : 0) | ((aAddr & 0x1FFFF) << 9) | (aCount == 512 ? 0 : aCount);
	return DataRead(53, arg, aCount, aCount, aBuf, "CMD53");
	}

// the self-check of DataRead on the SD-card host (SMHC0, U-Boot's set-up, known to move sectors): CMD17 of the SD log
// sector through exactly the code above, under the SD lock -- if this fails the bug is ours, if it works the bug is in
// SMHC1's set-up or the card
extern "C" TBool A64SdProbeLock();
extern "C" void A64SdProbeUnlock(TBool aLocked);
static void DumpRegs(const char* aWho);
static void SelfCheckOnSmhc0()
	{
	DPlatChunkHw* c0 = 0;
	if (DPlatChunkHw::New(c0, 0x01C0F000u, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking) != KErrNone)
		return;
	volatile TUint32* const smhc1 = Smhc1;
	Smhc1 = (volatile TUint32*)c0->LinearAddress();
	Say("[sdio] SMHC0 (U-Boot's): ccu 0x088=", R32(KHwBaseCcuPio, 0x088)); Say("[sdio]   ntsr=", S(KNtsr));
	Say("[sdio]   clkcr=", S(KClkcr)); Say("[sdio]   gctl=", S(KGctl)); Say("[sdio]   samp_dl=", S(KSampDl));
	Say("[sdio]   ftrgl=", S(KFtrgl)); Say("[sdio]   width=", S(KWidth));
	const TBool locked = A64SdProbeLock();
	TUint32 sector[128];
	WaitClear(KStas, KStaCardBusy);
	const TBool ok = DataRead(17, 4096, 512, 512, sector, "SMHC0 CMD17");
	A64SdProbeUnlock(locked);
	Say("[sdio] SELF-CHECK: DataRead on SMHC0 (CMD17 sector 4096) ok=", ok); Say("[sdio]   w0=", sector[0]); Say("[sdio]   w1=", sector[1]);
	DumpRegs("SMHC0 (U-Boot's, after the read)");
	Smhc1 = smhc1;
	}

// the chip's MAC registers through CMD52 (function 1, device id 8 in address bits 16:13 -- rtw88 sdio.h)
static TInt MacRead8(TUint aReg)					{ return Cmd52(1, (8u << 13) | (aReg & 0xFFFF), EFalse, 0, ETrue); }
static TBool MacWrite8(TUint aReg, TUint8 aVal)	{ return Cmd52(1, (8u << 13) | (aReg & 0xFFFF), ETrue, aVal, ETrue) >= 0; }
static TBool MacUpdate8(TUint aReg, TUint8 aMask, TUint8 aValue)
	{
	const TInt v = MacRead8(aReg);
	if (v < 0)
		return EFalse;
	return MacWrite8(aReg, (TUint8)(((TUint8)v & ~aMask) | (aValue & aMask)));
	}
static TBool MacPoll8(TUint aReg, TUint8 aMask, TUint8 aValue)
	{
	for (TInt n = 0; n < 20000; ++n)				// RTW_PWR_POLLING_CNT x 50 us
		{
		const TInt v = MacRead8(aReg);
		if (v >= 0 && ((TUint8)v & aMask) == (aValue & aMask))
			return ETrue;
		Kern::NanoWait(50000);
		}
	Say("[sdio] power-on poll timed out reg=", aReg);
	return EFalse;
	}

// rtw88 mac.c rtw_mac_pre_system_cfg (an 8051-firmware chip: RSV_CTRL = 0, the LDO/SPS select from SYS_CFG1 bit 24) +
// rtw_mac_power_switch (HIMR masked around it) with the rtw8703b card_enable_flow, SDIO interface entries only
static TBool PowerOnMac()
	{
	MacWrite8(0x001C, 0x00);												// REG_RSV_CTRL
	const TInt cfg1b3 = MacRead8(0x00F3);									// REG_SYS_CFG1 byte 3: BIT_LDO = bit 24
	MacWrite8(0x007C, (cfg1b3 >= 0 && (cfg1b3 & 0x01)) ? 0xC3 : 0x83);	// REG_LDO_SWR_CTRL: LDO_SEL / SPS_SEL
	// HIMR (SDIO-local 0x14..0x17) off during the sequence
	TUint8 himr[4];
	for (TInt i = 0; i < 4; ++i) { const TInt v = Cmd52(1, 0x0014 + i, EFalse, 0, ETrue); himr[i] = (TUint8)(v < 0 ? 0 : v); Cmd52(1, 0x0014 + i, ETrue, 0, ETrue); }
	TBool ok = ETrue;
	// trans_carddis_to_cardemu
	ok = ok && MacUpdate8(0x0005, 0x80, 0x00);
	// trans_cardemu_to_act (SDIO)
	ok = ok && MacUpdate8(0x0020, 0x01, 0x01);
	ok = ok && MacUpdate8(0x0067, 0x10, 0x00);
	Wait(1);
	ok = ok && MacUpdate8(0x0000, 0x20, 0x00);
	ok = ok && MacUpdate8(0x0005, 0x1C, 0x00);
	ok = ok && MacPoll8(0x0006, 0x02, 0x02);								// power ready
	ok = ok && MacUpdate8(0x0006, 0x01, 0x01);
	ok = ok && MacUpdate8(0x0005, 0x80, 0x00);
	ok = ok && MacUpdate8(0x0005, 0x18, 0x00);
	ok = ok && MacUpdate8(0x0005, 0x01, 0x01);
	ok = ok && MacPoll8(0x0005, 0x01, 0x00);
	ok = ok && MacUpdate8(0x0010, 0x40, 0x40);
	ok = ok && MacUpdate8(0x0049, 0x02, 0x02);
	ok = ok && MacUpdate8(0x0063, 0x02, 0x02);
	ok = ok && MacUpdate8(0x0062, 0x02, 0x00);
	ok = ok && MacUpdate8(0x0058, 0x01, 0x01);
	ok = ok && MacUpdate8(0x005A, 0x02, 0x02);
	ok = ok && MacUpdate8(0x0069, 0x40, 0x40);
	for (TInt i = 0; i < 4; ++i) Cmd52(1, 0x0014 + i, ETrue, himr[i], ETrue);
	return ok;
	}

static void DumpRegs(const char* aWho)
	{
	static const TUint KOffs[] = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C,
								   0x50, 0x54, 0x58, 0x5C, 0x78, 0x80, 0x100, 0x104, 0x10C, 0x140, 0x144, 0x148 };
	ekseam_puts("[sdio] registers of "); ekseam_puts(aWho); ekseam_puts(":\n");
	for (TUint i = 0; i < sizeof(KOffs) / sizeof(KOffs[0]); ++i)
		{
		ekseam_puts("[sdio]   +"); ekseam_puthex(KOffs[i]); ekseam_puts("[sdio]      = "); ekseam_puthex(S(KOffs[i]));
		}
	}

static TBool PowerPinsClocks()
	{
	// the IO rail (already on for Bluetooth): DLDO4 1.8 V
	A64PmicUpdateReg(0x18, 0x1F, 0x0B);
	A64PmicUpdateReg(0x12, 0, 1u << 6);
	// PG0..PG5 = mmc1 (function 2), pull-up, drive level 3
	volatile TUint32& cfg0 = R32(KHwBaseCcuPio, KPio + 6 * 0x24 + 0x00);
	cfg0 = (cfg0 & ~0x00FFFFFFu) | 0x00222222u;
	volatile TUint32& drv0 = R32(KHwBaseCcuPio, KPio + 6 * 0x24 + 0x14);
	drv0 = (drv0 & ~0x00000FFFu) | 0x00000FFFu;
	volatile TUint32& pul0 = R32(KHwBaseCcuPio, KPio + 6 * 0x24 + 0x1C);
	pul0 = (pul0 & ~0x00000FFFu) | 0x00000555u;
	// the WiFi reset PL2 (R_PIO port L, CFG0 bits 11:8 = output; DAT bit 2): low 10 ms, then high
	volatile TUint32& lcfg = R32(KHwBaseRPio, KRPio + 0x00);
	lcfg = (lcfg & ~(0xFu << 8)) | (1u << 8);
	volatile TUint32& ldat = R32(KHwBaseRPio, KRPio + 0x10);
	ldat &= ~(1u << 2);
	Wait(10);
	ldat |= 1u << 2;
	Wait(10);
	// CCU: bus gate + reset
	R32(KHwBaseCcuPio, KCcuBusGate0) |= 1u << KMmc1Bit;
	R32(KHwBaseCcuPio, KCcuBusReset0) &= ~(1u << KMmc1Bit);
	Wait(1);
	R32(KHwBaseCcuPio, KCcuBusReset0) |= 1u << KMmc1Bit;
	// the module clock on at 400 kHz for identification: 24 MHz / 30 = 800 kHz, halved by the new timing mode
	R32(KHwBaseCcuPio, KCcuSdmmc1Clk) = (1u << 31) | (1u << 30) | (0u << 24) | (1u << 16) | 14u;
	return ETrue;
	}

static TUint32 Ftrgl = 0x000F0000u;		// U-Boot's SMHC0 value (RX trigger 15; Linux's is 0x20070008)

static TBool ResetController()
	{
	S(KGctl) = KGctlSoftReset | KGctlFifoReset | KGctlDmaReset;
	if (!WaitClear(KGctl, KGctlSoftReset | KGctlFifoReset | KGctlDmaReset))
		{
		ekseam_puts("[sdio] controller reset stuck\n");
		return EFalse;
		}
	// mirror U-Boot's working SMHC0 (boot 204 register dump) rather than Linux's init: Linux's DBGC 0xdeb reads back
	// as 1 (debug enable), its FUNS key write leaves 0x10000, and the IDMAC control is 0 there (ours read 0x200)
	S(KFtrgl) = Ftrgl;
	S(KTmout) = 0xFFFFFF40u;
	S(KImask) = 0;							// polled
	S(KRintr) = 0xFFFFFFFFu;
	S(KDbgc) = 0;
	S(0x80) = 0;							// IDMAC control: off
	S(KGctl) |= KGctlAccessByAhb;
	return ETrue;
	}

// after a data command that never completed: the data FSM stays busy (boot 199: the next clock change hung on
// UPCLK_ONLY) -- a full controller reset, the registers again, the clock re-armed (CLKCR/NTSR/CCU survive the reset)
static void Recover()
	{
	ResetController();
	S(KClkcr) |= KClkOn;
	UpdateClock();
	S(KClkcr) &= ~KClkMaskData0;
	}

static TBool ControllerInit()
	{
	if (!ResetController())
		return EFalse;
	return SetClock(0, 1, 14, ETrue, 0, 0, "400 kHz (new mode)");
	}

static void A64SdioProbe()
	{
	ekseam_puts("[sdio] === SMHC1 / RTL8723CS SDIO probe (rung 0) ===\n");
	if (DPlatChunkHw::New(Smhc1Chunk, KSmhc1Phys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking) != KErrNone)
		{
		ekseam_puts("[sdio] no mapping\n");
		return;
		}
	Smhc1 = (volatile TUint32*)Smhc1Chunk->LinearAddress();
	SelfCheckOnSmhc0();
	PowerPinsClocks();
	if (!ControllerInit())
		return;
	Say("[sdio] gctl=", S(KGctl)); Say("[sdio] clkcr=", S(KClkcr)); Say("[sdio] stas=", S(KStas));
	// the I/O card reset first (Linux sdio_reset at every rescan: CCCR ABORT 0x06 with RES bit 3): an SDIO-only card
	// ignores CMD0, and ours stays powered across warm reboots (VBAT is fixed), so it may still hold the state of the
	// previous probe (function enabled, 4-bit, high speed, a data transfer it thinks is in progress)
		{
		const TInt abort = Cmd52(0, 0x06, EFalse, 0, ETrue);
		Say("[sdio] CCCR ABORT before the reset=", abort);
		Cmd52(0, 0x06, ETrue, (TUint8)((abort < 0 ? 0 : abort) | 0x08), ETrue);
		Wait(20);
		}
	// CMD0 with the 80-clock init sequence (no response)
	if (!Cmd(KCmdSendInit, 0, 0, "CMD0"))
		return;
	Wait(2);
	// CMD5 IO_SEND_OP_COND: first the OCR, then with it until ready (R4, no CRC)
	if (!Cmd(KCmdResp, 5, 0, "CMD5 (OCR)"))
		{
		ekseam_puts("[sdio] no card answers CMD5: kill switch 2 off? PL2 reset? power?\n");
		return;
		}
	TUint32 r4 = S(KResp0);
	Say("[sdio] R4 ocr=", r4);
	const TUint32 ocr = r4 & 0x00FFFFFFu;
	TInt tries = 0;
	for (; tries < 100; ++tries)
		{
		if (!Cmd(KCmdResp, 5, ocr, "CMD5"))
			return;
		r4 = S(KResp0);
		if (r4 & 0x80000000u)
			break;
		Wait(10);
		}
	Say("[sdio] R4 ready=", r4);
	Say("[sdio]   functions=", (r4 >> 28) & 7); Say("[sdio]   memory present=", (r4 >> 27) & 1); Say("[sdio]   tries=", tries);
	if (!(r4 & 0x80000000u))
		return;
	// CMD3 (R6: the new RCA), CMD7 select
	if (!Cmd(KCmdResp | KCmdCrc, 3, 0, "CMD3"))
		return;
	const TUint32 rca = S(KResp0) & 0xFFFF0000u;
	Say("[sdio] RCA=", rca >> 16);
	if (!Cmd(KCmdResp | KCmdCrc, 7, rca, "CMD7"))
		return;
	// the card's state as found (did the PL2 pulse reset the WiFi core, or is this the previous boot's card?)
	Say("[sdio] as found: IOEx=", Cmd52(0, KCccrIoEnable, EFalse, 0)); Say("[sdio]   IORx=", Cmd52(0, KCccrIoReady, EFalse, 0));
	Say("[sdio]   BUS_IF=", Cmd52(0, KCccrBusIf, EFalse, 0)); Say("[sdio]   SPEED=", Cmd52(0, KCccrSpeed, EFalse, 0));
	Say("[sdio]   FBR1 blksz lo=", Cmd52(0, 0x110, EFalse, 0)); Say("[sdio]   ABORT=", Cmd52(0, 0x06, EFalse, 0));
	// the CCCR
	const TInt rev = Cmd52(0, KCccrRev, EFalse, 0), spec = Cmd52(0, KCccrSdSpec, EFalse, 0), caps = Cmd52(0, KCccrCaps, EFalse, 0);
	Say("[sdio] CCCR rev=", rev); Say("[sdio] CCCR sd spec=", spec); Say("[sdio] CCCR caps=", caps);
	const TInt c0 = Cmd52(0, KCccrCisPtr, EFalse, 0), c1 = Cmd52(0, KCccrCisPtr + 1, EFalse, 0), c2 = Cmd52(0, KCccrCisPtr + 2, EFalse, 0);
	if (c0 < 0 || c1 < 0 || c2 < 0)
		return;
	TUint cis = (TUint)c0 | ((TUint)c1 << 8) | ((TUint)c2 << 16);
	Say("[sdio] CIS ptr=", cis);
	// the CIS tuples: code, link, data; MANFID = manufacturer + card ids
	TUint manf = 0, card = 0;
	for (TInt n = 0; n < 32 && cis && cis < 0x17000; ++n)
		{
		const TInt code = Cmd52(0, cis, EFalse, 0);
		if (code < 0 || code == KCisTplEnd)
			break;
		const TInt link = Cmd52(0, cis + 1, EFalse, 0);
		if (link < 0)
			break;
		if (code == KCisTplManfid && link >= 4)
			{
			const TInt a = Cmd52(0, cis + 2, EFalse, 0), b = Cmd52(0, cis + 3, EFalse, 0), c = Cmd52(0, cis + 4, EFalse, 0), d = Cmd52(0, cis + 5, EFalse, 0);
			manf = (TUint)a | ((TUint)b << 8);
			card = (TUint)c | ((TUint)d << 8);
			}
		if (link == 0xFF)
			break;
		cis += 2 + link;
		}
	Say("[sdio] CIS manufacturer=", manf); Say("[sdio] CIS card=", card);
	ekseam_puts(manf == 0x024C && card == 0xB703 ? "[sdio] M1 PASS: Realtek RTL8723CS (024c:b703)\n" : "[sdio] M1: unexpected ids\n");
	// M2, bisected (boots 193/194: CMD53 got no data at 25 MHz 4-bit, on function 0 and 1 alike): function 1 on and
	// its block size first, then CMD53 reads in 1-bit at 400 kHz, 4-bit at 400 kHz, 4-bit at 25 MHz, high speed.
	// Realtek's function-1 address: bits 16:13 = the device (0 = the SDIO-local registers, 8 = the MAC registers),
	// the low bits = the register offset (rtw88 sdio.h REG_SDIO_CMD_ADDR_*). SDIO-local 0x00.. = TX_CTRL/TIMEOUT...,
	// 0x14 = HIMR, 0x18 = HISR; MAC 0x00F0 = REG_SYS_CFG1 (the chip version word), 0x0002 = REG_SYS_FUNC_EN.
	Cmd52(0, KCccrIoEnable, ETrue, 0x02);
	TInt ready = 0;
	for (TInt n = 0; n < 100; ++n)
		{
		ready = Cmd52(0, KCccrIoReady, EFalse, 0);
		if (ready & 0x02)
			break;
		Wait(5);
		}
	Say("[sdio] IORx=", ready);
	Cmd52(0, 0x110, ETrue, 0x00); Cmd52(0, 0x111, ETrue, 0x02);
	Say("[sdio] FBR1 blocksize lo=", Cmd52(0, 0x110, EFalse, 0)); Say("[sdio] FBR1 blocksize hi=", Cmd52(0, 0x111, EFalse, 0));
	// function 1's register space through CMD52 first (boot 195: CMD53 is answered but never followed by data)
	Say("[sdio] CMD52 fn1 SDIO-local 0x14..0x17 (HIMR) b0=", Cmd52(1, 0x0014, EFalse, 0)); Say("[sdio]   b1=", Cmd52(1, 0x0015, EFalse, 0));
	Say("[sdio]   b2=", Cmd52(1, 0x0016, EFalse, 0)); Say("[sdio]   b3=", Cmd52(1, 0x0017, EFalse, 0));
	Say("[sdio] CMD52 fn1 MAC REG_SYS_CFG1 b0=", Cmd52(1, (8u << 13) | 0x00F0, EFalse, 0)); Say("[sdio]   b1=", Cmd52(1, (8u << 13) | 0x00F1, EFalse, 0));
	Say("[sdio] CMD52 fn1 MAC REG_SYS_FUNC_EN b0=", Cmd52(1, (8u << 13) | 0x0002, EFalse, 0));
	TUint32 buf[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	Say("[sdio] before CMD53: stas=", S(KStas));
	// the DAT line levels (boot 201: a write ended in END_BIT_ERR and reads never see a start bit, as if DAT0 were held
	// low): PG2..PG5 briefly as GPIO inputs with their pull-ups, read, then back to the mmc1 function
		{
		volatile TUint32& cfg0 = R32(KHwBaseCcuPio, KPio + 6 * 0x24 + 0x00);
		const TUint32 saved = cfg0;
		cfg0 = (cfg0 & ~0x00FFFF00u);							// PG2..PG5 = input (function 0)
		Kern::NanoWait(200000);
		const TUint32 dat = R32(KHwBaseCcuPio, KPio + 6 * 0x24 + 0x10);
		cfg0 = saved;
		Say("[sdio] PG levels (bit n = PGn; DAT0..3 = PG2..5, CLK PG0, CMD PG1)=", dat & 0x3F);
		Say("[sdio]   PG cfg0=", cfg0); Say("[sdio]   PG pull0=", R32(KHwBaseCcuPio, KPio + 6 * 0x24 + 0x1C)); Say("[sdio]   PG drv0=", R32(KHwBaseCcuPio, KPio + 6 * 0x24 + 0x14));
		}
	TBool newMode = ETrue;
	// (a) 4 bytes as before, (b) with the RX FIFO watermark at 1 word (boots 193-199: a 1-word transfer may never
	// reach Linux's 8-word trigger; our SD code only ever moves 128-word blocks), (c) 64 bytes, (d) 512 bytes with
	// U-Boot's watermark, (e) the old timing mode. Each failure leaves the data FSM hung: Recover() in between.
	// THE MAC POWER-ON (rtw88 sdio.c: every register access is a CMD52 byte access until RTW_FLAG_POWERON, and only
	// then CMD53 -- boots 193-205: CMD53 answered, never a data phase; mac.c rtw_mac_pre_system_cfg +
	// rtw_mac_power_switch, rtw8703b.c card_enable_flow: carddis -> cardemu -> act, the SDIO entries)
	Say("[sdio] REG_CR before power-on (0xea = off)=", MacRead8(0x0100));
	const TBool powered = PowerOnMac();
	Say("[sdio] MAC power-on ok=", powered); Say("[sdio]   REG_CR=", MacRead8(0x0100)); Say("[sdio]   REG_SYS_FUNC_EN=", MacRead8(0x0002));
	Say("[sdio]   REG_APS_FSMCO 0x0005=", MacRead8(0x0005)); Say("[sdio]   0x0006=", MacRead8(0x0006));
	TBool ok = Cmd53Read(1, 0x0014, ETrue, 4, buf);
	Say("[sdio] (a) CMD53 fn1 HIMR 4 B @400 kHz (new mode, rx_tl 7) ok=", ok); Say("[sdio]   himr=", buf[0]); Say("[sdio]   R5 (resp0)=", S(KResp0));
	if (!ok)
		{
		Say("[sdio]   stas=", S(KStas)); Say("[sdio]   rintr=", S(KRintr));
		Ftrgl = 0x20000008u; Recover(); Wait(5);
		ok = Cmd53Read(1, 0x0014, ETrue, 4, buf);
		Say("[sdio] (b) CMD53 fn1 HIMR 4 B (rx_tl 0) ok=", ok); Say("[sdio]   himr=", buf[0]); Say("[sdio]   stas=", S(KStas));
		}
	if (!ok)
		{
		Recover(); Wait(5);
		TUint32 big[128];
		ok = Cmd53Read(1, 0x0000, ETrue, 64, big);
		Say("[sdio] (c) CMD53 fn1 SDIO-local 64 B (rx_tl 0) ok=", ok); Say("[sdio]   w0=", big[0]); Say("[sdio]   w5=", big[5]); Say("[sdio]   stas=", S(KStas));
		if (!ok)
			{
			Ftrgl = 0x000F0000u; Recover(); Wait(5);
			ok = Cmd53Read(1, 0x0000, ETrue, 512, big);
			Say("[sdio] (d) CMD53 fn1 SDIO-local 512 B (U-Boot's FTRGL) ok=", ok); Say("[sdio]   w0=", big[0]); Say("[sdio]   w5=", big[5]); Say("[sdio]   stas=", S(KStas));
			}
		if (ok)
			{
			ok = Cmd53Read(1, 0x0014, ETrue, 4, buf);
			Say("[sdio]   ... and 4 B again with that FTRGL ok=", ok); Say("[sdio]   himr=", buf[0]);
			if (!ok) { Recover(); ok = ETrue; }		// the big read worked: go on with the bus steps anyway
			}
		}
	if (!ok)
		{
		Recover(); Wait(5);
		// (e) the old timing mode, as U-Boot ran SD cards before the new one: 24 MHz / 16 / 8 = 187.5 kHz, no phase delays
		newMode = EFalse;
		if (!SetClock(0, 3, 15, EFalse, 0, 0, "187 kHz (old mode)"))
			return;
		ok = Cmd53Read(1, 0x0014, ETrue, 4, buf);
		Say("[sdio] (e) CMD53 fn1 HIMR 4 B @187 kHz (old mode) ok=", ok); Say("[sdio]   himr=", buf[0]); Say("[sdio]   R5=", S(KResp0));
		if (!ok) { Say("[sdio]   stas=", S(KStas)); Say("[sdio]   rintr=", S(KRintr)); Recover(); }
		}
	Wait(5);
	if (Cmd52(0, KCccrBusIf, ETrue, 0x02) < 0)
		return;
	S(KWidth) = 1;											// 4-bit
	ok = Cmd53Read(1, 0x0014, ETrue, 4, buf);
	Say("[sdio] CMD53 fn1 HIMR 4-bit (slow clock) ok=", ok); Say("[sdio]   himr=", buf[0]);
	if (!ok) { Say("[sdio]   stas=", S(KStas)); Say("[sdio]   rintr=", S(KRintr)); Recover(); }
	Wait(5);
	// 25 MHz: new mode = module 50 MHz (1.2 GHz / 24); old mode = module 25 MHz (1.2 GHz / 48), phases 0 / 5
	if (!(newMode ? SetClock(1, 1, 11, ETrue, 0, 0, "25 MHz (new mode)") : SetClock(1, 2, 11, EFalse, 0, 5, "25 MHz (old mode)")))
		return;
	ok = Cmd53Read(1, 0x0014, ETrue, 4, buf);
	Say("[sdio] CMD53 fn1 HIMR 4-bit @25 MHz ok=", ok); Say("[sdio]   himr=", buf[0]);
	if (!ok) Recover();
	Wait(5);
	if (caps >= 0 && (Cmd52(0, KCccrSpeed, EFalse, 0) & 0x01))	// SHS
		{
		Cmd52(0, KCccrSpeed, ETrue, 0x02);						// EHS
		ekseam_puts("[sdio] high speed enabled\n");
		}
	ok = Cmd53Read(1, 0x0000, ETrue, 16, buf);
	Say("[sdio] CMD53 fn1 SDIO-local[0x00..0x0f] @25 MHz HS ok=", ok); Say("[sdio]   w0=", buf[0]); Say("[sdio]   w1=", buf[1]); Say("[sdio]   w2=", buf[2]); Say("[sdio]   w3=", buf[3]);
	if (ok)
		{
		ok = Cmd53Read(1, 0x0014, ETrue, 8, buf);
		Say("[sdio] CMD53 fn1 HIMR/HISR ok=", ok); Say("[sdio]   himr=", buf[0]); Say("[sdio]   hisr=", buf[1]);
		}
	if (ok)
		{
		const TBool macOk = Cmd53Read(1, (8u << 13) | 0x00F0, ETrue, 4, buf);
		Say("[sdio] CMD53 fn1 MAC REG_SYS_CFG1 ok=", macOk); Say("[sdio]   sys_cfg1=", buf[0]);
		const TBool mac0 = Cmd53Read(1, (8u << 13) | 0x0000, ETrue, 4, buf);
		Say("[sdio] CMD53 fn1 MAC 0x0000 ok=", mac0); Say("[sdio]   iso_ctrl/func_en=", buf[0]);
		}
	if (ok && (newMode ? SetClock(1, 0, 11, ETrue, 0, 0, "50 MHz (new mode)") : SetClock(1, 1, 11, EFalse, 3, 4, "50 MHz (old mode)")))
		{
		ok = Cmd53Read(1, 0x0000, ETrue, 16, buf);
		Say("[sdio] CMD53 fn1 SDIO-local @50 MHz ok=", ok); Say("[sdio]   w0=", buf[0]);
		}
	ekseam_puts(ok && (ready & 0x02) ? "[sdio] M2 PASS: 4-bit, high speed, function 1 ready, CMD53 data\n" : "[sdio] M2: incomplete\n");
	}

extern "C" void A64SdioInstall()
	{
#ifdef A64_SDIO_TEST
	A64SdioProbe();
#endif
	}

// ======================================================================================================================
// The host for the chip core (a64wlan.cpp + the vendored rtw88 over wlan/shim): the rung-0 sequence as a plain init,
// then the Linux SDIO function API the shim maps to (shim_rt.h shim_sdio_*: CMD52 bytes, CMD53 byte/block transfers,
// function enable, block size). Everything is called on the WLAN DFC thread; the "interrupt" is polled there too.
// ======================================================================================================================

// a data command either way: PIO through the FIFO; aBuf may be unaligned, aCount any size (the FIFO moves words: the
// last partial word is zero-padded on a write, its extra bytes dropped on a read)
static TBool DataXfer(TBool aWrite, TUint32 aArg, TUint aBlksz, TUint aCount, TUint8* aBuf, const char* aWhat)
	{
	S(KGctl) |= KGctlAccessByAhb;
	S(KBlksz) = aBlksz;
	S(KBcntr) = aCount;
	S(KRintr) = 0xFFFFFFFFu;
	S(KCarg) = aArg;
	S(KCmdr) = KCmdStart | KCmdWaitPre | KCmdData | KCmdResp | KCmdCrc | (aWrite ? KCmdWrite : 0) | 53;
	const TUint words = (aCount + 3) / 4;
	for (TUint w = 0; w < words; ++w)
		{
		if (!WaitClear(KStas, aWrite ? KStaFifoFull : KStaFifoEmpty))
			{
			ekseam_puts("[sdio] "); ekseam_puts(aWhat); Say(" FIFO wait rint=", S(KRintr));
			S(KGctl) |= KGctlFifoReset;
			return EFalse;
			}
		const TUint off = w * 4;
		const TUint n = aCount - off >= 4 ? 4 : aCount - off;
		if (aWrite)
			{
			TUint32 v = 0;
			memcpy(&v, aBuf + off, n);
			S(KFifo) = v;
			}
		else
			{
			const TUint32 v = S(KFifo);
			memcpy(aBuf + off, &v, n);
			}
		}
	TUint32 r = 0;
	for (TUint i = 0; i < 5000000u; ++i)
		{
		r = S(KRintr);
		if ((r & KRintErrors) || ((r & (KRintCmdDone | KRintDataOver)) == (KRintCmdDone | KRintDataOver)))
			break;
		}
	S(KRintr) = r & ~KRintSdio;
	if ((r & KRintErrors) || !(r & KRintDataOver))
		{
		ekseam_puts("[sdio] "); ekseam_puts(aWhat); Say(" FAILED rint=", r); Say("[sdio]   arg=", aArg); Say("[sdio]   count=", aCount);
		S(KGctl) |= KGctlFifoReset;
		return EFalse;
		}
	if (aWrite && !WaitClear(KStas, KStaCardBusy))
		ekseam_puts("[sdio] card busy after the write\n");
	return ETrue;
	}

static TUint HostBlockSize = 512;
// CMD53 IO_RW_EXTENDED, function 1, incrementing address: whole blocks in block mode, the rest in byte mode
static TInt Cmd53Xfer(TBool aWrite, TUint aAddr, TUint8* aBuf, TUint aCount)
	{
	while (aCount)
		{
		TUint n, blocks = 0;
		if (aCount >= HostBlockSize && aCount >= 512)
			{
			blocks = aCount / HostBlockSize;
			if (blocks > 511) blocks = 511;
			n = blocks * HostBlockSize;
			}
		else
			n = aCount > 512 ? 512 : aCount;
		const TUint32 arg = (aWrite ? 1u << 31 : 0) | (1u << 28) | (blocks ? 1u << 27 : 0) | (1u << 26) | ((aAddr & 0x1FFFF) << 9)
			| (blocks ? blocks : (n == 512 ? 0 : n));
		if (!DataXfer(aWrite, arg, blocks ? HostBlockSize : n, n, aBuf, aWrite ? "CMD53 write" : "CMD53 read"))
			{
			Recover();
			return -5;		// -EIO
			}
		aAddr += n; aBuf += n; aCount -= n;
		}
	return 0;
	}

static TBool HostUp;
extern "C" TBool A64SdioHostInit()
	{
	if (HostUp)
		return ETrue;
	ekseam_puts("[sdio] host init for the chip core\n");
	if (!Smhc1 && DPlatChunkHw::New(Smhc1Chunk, KSmhc1Phys, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking) != KErrNone)
		{
		ekseam_puts("[sdio] no mapping\n");
		return EFalse;
		}
	Smhc1 = (volatile TUint32*)Smhc1Chunk->LinearAddress();
	PowerPinsClocks();
	if (!ControllerInit())
		return EFalse;
	// the I/O card reset (CCCR ABORT RES), CMD0 + init clocks, CMD5 until ready, CMD3, CMD7
		{
		const TInt abort = Cmd52(0, 0x06, EFalse, 0, ETrue);
		Cmd52(0, 0x06, ETrue, (TUint8)((abort < 0 ? 0 : abort) | 0x08), ETrue);
		Wait(20);
		}
	if (!Cmd(KCmdSendInit, 0, 0, "CMD0"))
		return EFalse;
	Wait(2);
	if (!Cmd(KCmdResp, 5, 0, "CMD5 (OCR)"))
		{
		ekseam_puts("[sdio] no card answers CMD5: kill switch 2 off? PL2 reset? power?\n");
		return EFalse;
		}
	TUint32 r4 = S(KResp0);
	const TUint32 ocr = r4 & 0x00FFFFFFu;
	// the inquiry's R4 may already say "ready" (boot 193: OCR 0x90fc0000 at once) but the card leaves the idle state
	// only on a CMD5 that carries the OCR -- always send at least one (boot 221: skipping it = CMD7 RESP_ERR)
	for (TInt tries = 0; tries < 100; ++tries)
		{
		if (!Cmd(KCmdResp, 5, ocr, "CMD5"))
			return EFalse;
		r4 = S(KResp0);
		if (r4 & 0x80000000u)
			break;
		Wait(10);
		}
	Say("[sdio] R4=", r4);
	if (!(r4 & 0x80000000u))
		{
		ekseam_puts("[sdio] card never ready\n");
		return EFalse;
		}
	if (!Cmd(KCmdResp | KCmdCrc, 3, 0, "CMD3"))
		return EFalse;
	const TUint32 rca = S(KResp0) & 0xFFFF0000u;
	if (!Cmd(KCmdResp | KCmdCrc, 7, rca, "CMD7"))
		return EFalse;
	// the ids (the chip we expect: 024c:b703)
	TUint manf = 0, card = 0;
		{
		const TInt c0 = Cmd52(0, KCccrCisPtr, EFalse, 0), c1 = Cmd52(0, KCccrCisPtr + 1, EFalse, 0), c2 = Cmd52(0, KCccrCisPtr + 2, EFalse, 0);
		TUint cis = (c0 < 0 || c1 < 0 || c2 < 0) ? 0 : ((TUint)c0 | ((TUint)c1 << 8) | ((TUint)c2 << 16));
		for (TInt n = 0; n < 32 && cis && cis < 0x17000; ++n)
			{
			const TInt code = Cmd52(0, cis, EFalse, 0);
			if (code < 0 || code == KCisTplEnd) break;
			const TInt link = Cmd52(0, cis + 1, EFalse, 0);
			if (link < 0) break;
			if (code == KCisTplManfid && link >= 4)
				{
				const TInt a = Cmd52(0, cis + 2, EFalse, 0), b = Cmd52(0, cis + 3, EFalse, 0), c = Cmd52(0, cis + 4, EFalse, 0), d = Cmd52(0, cis + 5, EFalse, 0);
				manf = (TUint)a | ((TUint)b << 8); card = (TUint)c | ((TUint)d << 8);
				}
			if (link == 0xFF) break;
			cis += 2 + link;
			}
		}
	Say("[sdio] card ids manufacturer=", manf); Say("[sdio]   card=", card);
	if (manf != 0x024C || card != 0xB703)
		ekseam_puts("[sdio] not the RTL8723CS -- going on anyway\n");
	// 4-bit, 25 MHz, high speed, 50 MHz (all new timing mode; boot 206)
	if (Cmd52(0, KCccrBusIf, ETrue, 0x02) < 0)
		return EFalse;
	S(KWidth) = 1;
	if (!SetClock(1, 1, 11, ETrue, 0, 0, "25 MHz (new mode)"))
		return EFalse;
	if (Cmd52(0, KCccrSpeed, EFalse, 0) & 0x01)
		{
		Cmd52(0, KCccrSpeed, ETrue, 0x02);
		if (!SetClock(1, 0, 11, ETrue, 0, 0, "50 MHz (new mode, high speed)"))
			return EFalse;
		}
	HostUp = ETrue;
	ekseam_puts("[sdio] host up: 4-bit, high speed, the card selected; function 1 is the driver's\n");
	return ETrue;
	}

// ---- the Linux SDIO function API (shim_rt.h), function 1
extern "C" void shim_sdio_claim(void) {}
extern "C" void shim_sdio_release(void) {}
extern "C" int shim_sdio_enable_func(int aOn)
	{
	if (Cmd52(0, KCccrIoEnable, ETrue, aOn ? 0x02 : 0x00) < 0)
		return -5;
	if (!aOn)
		return 0;
	for (TInt n = 0; n < 200; ++n)
		{
		const TInt ready = Cmd52(0, KCccrIoReady, EFalse, 0, ETrue);
		if (ready >= 0 && (ready & 0x02))
			return 0;
		Wait(5);
		}
	ekseam_puts("[sdio] function 1 never ready\n");
	return -110;	// -ETIMEDOUT
	}
extern "C" int shim_sdio_set_block_size(unsigned aBytes)
	{
	if (Cmd52(0, 0x110, ETrue, (TUint8)(aBytes & 0xFF)) < 0 || Cmd52(0, 0x111, ETrue, (TUint8)(aBytes >> 8)) < 0)
		return -5;
	HostBlockSize = aBytes;
	return 0;
	}
extern "C" TUint8 shim_sdio_readb(unsigned aAddr, int* aErr)
	{
	const TInt v = Cmd52(1, aAddr, EFalse, 0, ETrue);
	if (aErr) *aErr = v < 0 ? -5 : 0;
	return v < 0 ? 0 : (TUint8)v;
	}
extern "C" void shim_sdio_writeb(unsigned aAddr, TUint8 aValue, int* aErr)
	{
	const TInt v = Cmd52(1, aAddr, ETrue, aValue, ETrue);
	if (aErr) *aErr = v < 0 ? -5 : 0;
	}
extern "C" int shim_sdio_read_incr(unsigned aAddr, void* aDst, unsigned aCount)		{ return Cmd53Xfer(EFalse, aAddr, (TUint8*)aDst, aCount); }
extern "C" int shim_sdio_write_incr(unsigned aAddr, const void* aSrc, unsigned aCount)	{ return Cmd53Xfer(ETrue, aAddr, (TUint8*)aSrc, aCount); }
extern "C" int shim_sdio_claim_irq(void) { return 0; }		// polled from a64wlan.cpp
extern "C" int shim_sdio_release_irq(void) { return 0; }
