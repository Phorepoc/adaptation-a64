// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// a64sound.cpp -- OURS (2026-10-09): the PinePhone's audio as a SOUND PDD ("SoundSc.A64") under the REAL shared-chunk
// sound LDD (kernel/eka/drivers/soundsc/soundldd.cpp, linked into ekern unmodified with its entry renamed, exactly as
// the x86 AC'97 PDD tools/phorepoc-x86/ekern/xk_ac97.cpp). The same user chain as Path C (mdasoundadapter -> the A3F
// DevSound server -> MediaClientAudio) runs on top unchanged.
//
// The hardware (Allwinner A64; register documentation = the Linux drivers, read as a datasheet: sound/soc/sunxi/
// sun4i-i2s.c "allwinner,sun50i-a64-codec-i2s", sun8i-codec.c "allwinner,sun50i-a64-codec", sun50i-codec-analog.c +
// sun8i-adda-pr-regmap.c, drivers/dma/sun6i-dma.c "allwinner,sun50i-a64-dma"; routing = the PinePhone device tree and
// the distros' ALSA UCM; the research in docs/phorepoc/research/sound-{linux,2010,701}.md):
//   CPU --DMA (ch 7, DRQ 15)--> DAI 0x01C22C00 (an I2S controller in the sun4i register layout, TX FIFO at +0x20;
//   master, I2S, 16-bit, BCLK = 32 fs, MCLK = 128 fs from PLL_AUDIO) --> digital codec 0x01C22E00 (AIF1 slot 0 ->
//   DAC mixer -> DAC) --> analog codec (5-bit registers behind PRCM+0x1C0: DAC -> output mixers -> LINEOUT as a
//   MONO DIFFERENTIAL pair, the speaker is mono) --> speaker amplifier (enable = PC7) -> the loudspeaker.
//   Headphones/earpiece/microphones/jack: v2 (the "Sound: the decision" section of pinephone-port.md).
// Data path: a physically contiguous ring of 16 periods (~10 ms each, sized per rate), played by the DMA in a cyclic
// linked list (one LLI per period, PKG interrupt per period). TransferData copies the LDD's fragment into the ring
// at the write counter (KErrNotReady when it does not fit: the LDD retries from the next PlayCallback); the
// completion DFC advances the played counter over the periods the DMA has passed, completes the fragments that ended
// in them, and zeroes them (silence if the stream underruns). The LDD calls the PDD from this same DFC thread, so the
// counters need no locking. Rates: both PLL_AUDIO families (48k: 8/12/16/24/32/48 kHz; 44.1k: 11025/22050/44100),
// 16-bit stereo only (mdasoundadapter converts mono/other rates).
// Power: the codec, PLL and amplifier are up only while a channel is open (PowerUp/PowerDown ref-counted over both
// units; PowerUp runs in the client thread, so it is marshalled onto the DFC thread as NaviEngine does); the amp goes
// on at StartTransfer and off ~1.5 s after StopTransfer (a key click opens and closes the channel each time).
// Volume = the soundsc contract (255 = max, 0.5 dB per step; mdasoundadapter sends dB codes): coarse 1.5 dB steps on
// LINEOUT, the remainder in 0.75 dB steps on the DAC digital volume, 0 = mute.
// The record unit (4) delivers silence at the real rate from a timer (mdasoundadapter opens it with every playback;
// a synchronous completion from TransferData faults the LDD). The real MIC1 path is v2.
// A64_SOUND_TEST: a boot self-test plays 1 s of 440 Hz through the ring before any LDD.
#include <kernel/kern_priv.h>
#include <drivers/soundsc.h>
#include <assp.h>
#include "syborg.h"

extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(unsigned);

static inline volatile TUint32& R32(TLinAddr aBase, TUint aOff) { return *(volatile TUint32*)(aBase + aOff); }

// --- physical layout -------------------------------------------------------------------------------------------
static const TPhysAddr KPhysDma = 0x01C02000, KPhysAudio = 0x01C22000, KPhysPrcm = 0x01F01000;
static const TUint KDai = 0xC00, KCodec = 0xE00, KAddaPr = 0x5C0;			// offsets in the audio / PRCM pages
static const TInt KIntDma = 32 + 50;											// GIC SPI 50
static const TUint KDmaChan = 7, KDrqCodec = 15, KDrqSdram = 1;
// CCU (KHwBaseCcuPio + 0)
static const TUint KCcuPllAudio = 0x008, KCcuPllAudioPat = 0x284, KCcuPllAudioBias = 0x224, KCcuBusGate0 = 0x060, KCcuBusGate2 = 0x068,
				   KCcuAcDig = 0x140, KCcuBusReset0 = 0x2C0, KCcuBusReset2 = 0x2D0;
// DAI (sun4i layout; sun4i-i2s.c)
static const TUint KI2sCtrl = 0x00, KI2sFmt0 = 0x04, KI2sFifoCtrl = 0x14, KI2sFifoSta = 0x18,
				   KI2sDmaIntCtrl = 0x1C, KI2sClkDiv = 0x24, KI2sTxCnt = 0x28,
				   KI2sTxChanSel = 0x30, KI2sTxChanMap = 0x34, KI2sRxChanSel = 0x38, KI2sRxChanMap = 0x3C,
				   KI2sFifoTx = 0x20;												// the A64 codec DAI's TX FIFO
// digital codec (sun8i-codec.c)
static const TUint KCdSysClk = 0x00C, KCdModClkEna = 0x010, KCdModRst = 0x014, KCdSysSr = 0x018, KCdAif1Clk = 0x040,
				   KCdAif1DacDat = 0x048, KCdAif1Vol3 = 0x058, KCdDacDig = 0x120, KCdDacVol = 0x124, KCdDacMxr = 0x130;
// analog codec (sun50i-codec-analog.c, through the PR register)
static const TUint KAnHpCtrl = 0x00, KAnOlMix = 0x01, KAnOrMix = 0x02, KAnLineoutCtrl0 = 0x05, KAnLineoutCtrl1 = 0x06,
				   KAnMixDac = 0x0A, KAnJackMic = 0x1D;
#ifdef PHE_MODEM
// modem rung M5: the voice call route state (VoiceRoute below); the codec stays powered while it is on
static TDfc* VoiceDfc;
static volatile TBool VoiceWanted, VoiceOn;
static void VoiceDfcFn(TAny*);
#endif
// DMA (sun6i-dma.c, A64 variant: H3 burst bits, A31 DRQ/mode bits)
static const TUint KDmaIrqEn = 0x00, KDmaIrqStat = 0x10, KDmaAutoGate = 0x28, KDmaChanBase = 0x100 + KDmaChan * 0x40,
				   KDmaEnable = 0x00, KDmaPause = 0x04, KDmaLliAddr = 0x08, KDmaCurCfg = 0x0C, KDmaCurCnt = 0x18;
static const TUint32 KDmaIrqPkg = 2u;

static const TUint KPeriods = 16, KMaxPeriodBytes = 2048, KRingMax = KMaxPeriodBytes * KPeriods;
static const TInt KMaxTransfer = 8192;
static const TInt KMaxFrags = 32;
static const TInt KMaxRxFrags = 2;
static const TInt KAmpHoldOffMs = 1500;
// the amp OFF during a stream's digital silence (user, 2026-10-15: hiss with no music -- games keep their stream open and
// write zeros): after the whole ring + this much queued silence the last sound has played; any sound turns it back on
static const TInt KAmpSilenceMs = 300;

_LIT(KA64PddName, "SoundSc.A64");

struct SLli { TUint32 iCfg, iSrc, iDst, iLen, iPara, iNext; TUint32 iPad[2]; };	// 32 B each (the DMA reads 24)
struct SFrag { TUint iId; TInt iBytes; TUint32 iEnd; };								// iEnd = the write counter after it

class DA64SoundPdd;

class TA64Sound
	{
public:
	TInt Init();
	void PowerOn();
#ifdef PHE_MODEM
	void VoiceRoute(TBool aOn);
#endif
	void PowerOff();
	TInt SetRate(TInt aHz);
	void SetVolume(TInt aVol);
	TInt Queue(TLinAddr aLin, TInt aBytes, TUint aId);
	void Start();
	void Stop();
	void Pause(TBool aPause);
	TInt64 BytesPlayed();
	void AmpOn();
	void AmpOff();
	void SelfTest();
	// the record unit: silence at the real rate
	TInt RxQueue(TLinAddr aLin, TInt aBytes, TUint aId);
	void RxStop();
	void RxArm();
	static void Isr(TAny* aPtr);
	static void DoneDfcFn(TAny* aPtr);
	static void AmpTimerFn(TAny* aPtr);
	static void AmpDfcFn(TAny* aPtr);
	static void PowerDfcFn(TAny* aPtr);
	static void RxTimerFn(TAny* aPtr);
	static void RxDfcFn(TAny* aPtr);
private:
	void Clocks(TBool a44k);
	void DaiInit();
	void CodecInit();
	void ApplyVolume();
	void Run();
	void AnalogWrite(TUint aReg, TUint8 aVal);
	TUint8 AnalogRead(TUint aReg);
	TInt DmaPeriod();											// the period the DMA is transferring now
	inline volatile TUint32& Dai(TUint o)	{ return R32(iAudio, KDai + o); }
	inline volatile TUint32& Cd(TUint o)	{ return R32(iAudio, KCodec + o); }
	inline volatile TUint32& Dma(TUint o)	{ return R32(iDma, o); }
	inline volatile TUint32& Ch(TUint o)	{ return R32(iDma, KDmaChanBase + o); }
	inline volatile TUint32& Ccu(TUint o)	{ return R32(KHwBaseCcuPio, o); }
public:
	TLinAddr iDma, iAudio, iPrcm;
	TPhysAddr iRingPhys;
	TUint8* iRing;
	SLli* iLli;
	TUint iPeriodBytes, iRingBytes;
	TUint32 iWritten, iPlayed;									// byte counters (ring offset = counter % iRingBytes)
	TInt iLastPeriod;											// the period the DFC saw the DMA in last
	SFrag iFrags[KMaxFrags];
	TInt iFragHead, iFragCount;
	TInt iRate;
	TBool iIs44k;
	TBool iRunning, iRunPending, iPaused;
	TBool iPowered, iAmp;
	TUint32 iSilentBytes;										// consecutive silence queued (the amp-off rule)
	TInt iUsers;												// open channels (both units)
	TInt iVolume;
	DA64SoundPdd* iTxPdd;
	DA64SoundPdd* iRxPdd;
	TInt iRxRate;
	SFrag iRx[KMaxRxFrags];
	TInt iRxHead, iRxCount;
	TBool iRxRunning;
	TDynamicDfcQue* iDfcQ;
	TDfc iDoneDfc, iAmpDfc, iPowerDfc, iRxDfc;
	NTimer iAmpTimer, iRxTimer;
	NFastSemaphore iPowerSem;
	TA64Sound() : iDoneDfc(DoneDfcFn, this, 1), iAmpDfc(AmpDfcFn, this, 2), iPowerDfc(PowerDfcFn, this, 0),
				  iRxDfc(RxDfcFn, this, 1), iAmpTimer(AmpTimerFn, this), iRxTimer(RxTimerFn, this) {}
	};

static TA64Sound* TheSound;

// --- the analog codec's indirect registers (sun8i-adda-pr-regmap.c) ---------------------------------------------
void TA64Sound::AnalogWrite(TUint aReg, TUint8 aVal)
	{
	volatile TUint32& pr = R32(iPrcm, KAddaPr);
	pr |= 1u << 28;												// de-assert reset
	pr = (pr & ~(0x1Fu << 16)) | ((aReg & 0x1F) << 16);
	pr = (pr & ~(0xFFu << 8)) | ((TUint32)aVal << 8);
	pr |= 1u << 24;												// write pulse
	pr &= ~(1u << 24);
	}

TUint8 TA64Sound::AnalogRead(TUint aReg)
	{
	volatile TUint32& pr = R32(iPrcm, KAddaPr);
	pr |= 1u << 28;
	pr &= ~(1u << 24);
	pr = (pr & ~(0x1Fu << 16)) | ((aReg & 0x1F) << 16);
	return (TUint8)(pr & 0xFF);
	}

// --- clocks: PLL_AUDIO (sigma-delta patterns as ccu-sun50i-a64.c) + the AC_DIG module clock ---------------------
void TA64Sound::Clocks(TBool a44k)
	{
	// PLL_AUDIO, the speaker hiss/whine fix (2026-10-15; Samuel Holland's "clk: sunxi-ng: a64: Increase PLL_AUDIO base
	// frequency", smaeul/linux 7b51d5b09136, the PinePhone kernels; noise found to leak VCC-PLL -> AVCC, megi's log 020):
	// (1) the base at 3x (67.7376 / 73.728 MHz, SDM patterns from that commit) with the 1x post-divider (bits 19:16) at
	// 2 = /3, so the codec still gets 22.5792 / 24.576 MHz but the PLL's harmonic leaves the audible band;
	// (2) PLL_AUDIO_BIAS (CCU + 0x224) = 0x10040000, a lower bias current: "drastically decreases ... the white noise".
	// Was: the base at 1x (patterns 0xC0010D84 n7 m8 / 0xC000AC02 n14 m14, post-divider 1) with the reset bias.
	Ccu(KCcuPllAudioBias) = 0x10040000u;
	const TUint32 pat = a44k ? 0xC000EF35u : 0xC000B852u;
	const TUint n = a44k ? 8 : 15, m = a44k ? 3 : 5;
	Ccu(KCcuPllAudioPat) = pat;
	Ccu(KCcuPllAudio) = (1u << 31) | (1u << 24) | (2u << 16) | ((n - 1) << 8) | (m - 1);
	for (TInt i = 0; i < 100000 && !(Ccu(KCcuPllAudio) & (1u << 28)); ++i) {}
	Ccu(KCcuAcDig) |= 1u << 31;									// AC_DIG module clock = PLL_AUDIO
	iIs44k = a44k;
	}

// --- the DAI (sun4i-i2s.c: runtime_resume + set_fmt(I2S, master) + the rate-independent part of hw_params) --------
void TA64Sound::DaiInit()
	{
	Dai(KI2sCtrl) = 0;
	Dai(KI2sCtrl) |= 1u << 0;									// GL_EN
	Dai(KI2sCtrl) |= 1u << 8;									// SDO0 enable
	// format: I2S (bits 1:0 = 0), 16-bit word select (bits 3:2 = 0), 16-bit samples (bits 5:4 = 0), normal clock
	// polarities (bits 6, 7 = 0); master = CTRL bit 5 = 0
	Dai(KI2sFmt0) = 0;
	Dai(KI2sTxChanMap) = 0x76543210u;
	Dai(KI2sRxChanMap) = 0x00003210u;
	Dai(KI2sTxChanSel) = (Dai(KI2sTxChanSel) & ~7u) | 1u;		// 2 channels
	Dai(KI2sRxChanSel) = (Dai(KI2sRxChanSel) & ~7u) | 1u;
	// FIFOs: 16-bit samples, both directions (TX_MODE bit 2 = 1, RX_MODE bits 1:0 = 1)
	Dai(KI2sFifoCtrl) = (Dai(KI2sFifoCtrl) & ~7u) | (1u << 2) | 1u;
	}

// --- the digital + analog codec: the AIF1-playback supply chain (sun8i-codec.c DAPM) and the speaker route from
// the PinePhone UCM ("Line Out Source = Mono Differential", "DAC Reversed Playback Switch" on) ------------------
void TA64Sound::CodecInit()
	{
	// AIF1CLK and AIF2CLK from PLL_AUDIO, SYSCLK from AIF1CLK; AIF1CLK + SYSCLK enabled
	Cd(KCdSysClk) = (Cd(KCdSysClk) & ~((3u << 8) | (3u << 4) | 1u)) | (2u << 8) | (2u << 4);
	Cd(KCdSysClk) |= (1u << 11) | (1u << 3);
	// module clocks + resets: AIF1 (bit 15), DAC (bit 2)
	Cd(KCdModClkEna) |= (1u << 15) | (1u << 2);
	Cd(KCdModRst) |= (1u << 15) | (1u << 2);
	// AIF1: the codec is the clock slave (MSTR_MOD bit 15 = 1 = "DAI master"), I2S (bits 3:2 = 0), 16-bit words
	// (bits 5:4 = 1), LRCK = 32 BCLK (bits 8:6 = order 5 - 4 = 1), no clock inversion (bits 14:13 = 0);
	// BCLK_DIV (bits 12:9) per rate in SetRate
	Cd(KCdAif1Clk) = (1u << 15) | (1u << 4) | (1u << 6);
	// AIF1 slot 0 DA0 L/R in, stereo (SRC bits 11:10 / 9:8 = 0)
	Cd(KCdAif1DacDat) = (1u << 15) | (1u << 14);
	Cd(KCdAif1Vol3) = 0xA0A0u;									// AIF1 DA0 volume 0 dB
	Cd(KCdDacMxr) = (1u << 15) | (1u << 11);					// DAC mixer <- AIF1 DA0 L/R
	Cd(KCdDacVol) = 0xA0A0u;									// DAC volume 0 dB (ApplyVolume refines it)
	Cd(KCdDacDig) |= 1u << 15;									// DAC on
	// analog: DACs + output mixers on; BOTH mixers take BOTH DACs (the mono mix L+R); LINEOUT L/R = "Mono
	// Differential" (bits 5:4) from the mixers; both enabled (bits 7:6); 0 dB
	AnalogWrite(KAnMixDac, (1u << 7) | (1u << 6) | (1u << 5) | (1u << 4));
	AnalogWrite(KAnOlMix, 0x03);
	AnalogWrite(KAnOrMix, 0x03);
	AnalogWrite(KAnLineoutCtrl0, 0xF0);
	AnalogWrite(KAnLineoutCtrl1, 0x1F);
	}

void TA64Sound::PowerOn()
	{
	if (iPowered)
		return;
	Clocks(iIs44k);
	DaiInit();
	CodecInit();
	iPowered = ETrue;
	SetRate(iRate);
	ApplyVolume();
	}

void TA64Sound::PowerOff()
	{
	if (!iPowered)
		return;
	iAmpTimer.Cancel();
	AmpOff();
#ifdef PHE_MODEM
	if (VoiceOn)
		return;													// a call holds the codec (VoiceRoute(EFalse) powers it off)
#endif
	AnalogWrite(KAnLineoutCtrl0, 0x30);							// LINEOUT off (mute) first
	AnalogWrite(KAnMixDac, 0);									// mixers + DACs off
	Cd(KCdDacDig) &= ~(1u << 15);
	Cd(KCdModClkEna) &= ~((1u << 15) | (1u << 2));
	Cd(KCdModRst) &= ~((1u << 15) | (1u << 2));
	Cd(KCdSysClk) &= ~((1u << 11) | (1u << 3));
	Dai(KI2sCtrl) = 0;
	Ccu(KCcuAcDig) &= ~(1u << 31);
	Ccu(KCcuPllAudio) &= ~(1u << 31);
	iPowered = EFalse;
	}

// the speaker amplifier: PC7 (port C = PIO + 2 * 0x24; CFG0 bits 31:28 = pin 7; DAT bit 7)
void TA64Sound::AmpOn()
	{
	R32(KHwBaseCcuPio, 0x800 + 2 * 0x24 + 0x10) |= 1u << 7;
	iAmp = ETrue;
	}

void TA64Sound::AmpOff()
	{
	R32(KHwBaseCcuPio, 0x800 + 2 * 0x24 + 0x10) &= ~(1u << 7);
	iAmp = EFalse;
	}

void TA64Sound::AmpTimerFn(TAny* aPtr)	{ ((TA64Sound*)aPtr)->iAmpDfc.Add(); }
void TA64Sound::AmpDfcFn(TAny* aPtr)
	{
	TA64Sound& s = *(TA64Sound*)aPtr;
	if (!s.iRunning && !s.iRunPending)
		s.AmpOff();
	}

// PowerUp runs in the client's thread: the hardware work happens on the DFC thread, the client waits
void TA64Sound::PowerDfcFn(TAny* aPtr)
	{
	TA64Sound& s = *(TA64Sound*)aPtr;
	s.PowerOn();
	NKern::FSSignal(&s.iPowerSem);
	}

TInt TA64Sound::SetRate(TInt aHz)
	{
	// the DAI: MCLK = 128 fs from PLL_AUDIO (mclk div code), BCLK = MCLK / 4 = 32 fs (bclk div 4 = code 1);
	// the codec: SYS_SR_CTRL AIF1_FS code, AIF1 BCLK_DIV code for (sysclk / fs) / 32 (sun8i_codec_get_bclk_div)
	struct SRate { TInt iHz; TUint8 iMclkDiv; TUint8 iFs; TUint8 iBclkDiv; TBool i44k; };
	static const SRate KRates[] = {
		{ 8000, 7, 0x0, 11, EFalse }, { 11025, 6, 0x1, 10, ETrue }, { 12000, 6, 0x2, 10, EFalse },
		{ 16000, 5, 0x3, 9, EFalse }, { 22050, 4, 0x4, 8, ETrue }, { 24000, 4, 0x5, 8, EFalse },
		{ 32000, 3, 0x6, 7, EFalse }, { 44100, 2, 0x7, 6, ETrue }, { 48000, 2, 0x8, 6, EFalse } };
	const SRate* r = NULL;
	for (TUint i = 0; i < sizeof(KRates) / sizeof(KRates[0]); ++i)
		if (KRates[i].iHz == aHz)
			r = &KRates[i];
	if (!r)
		return KErrNotSupported;
#ifdef PHE_MODEM
	if (VoiceOn && r->i44k)
		return KErrNotSupported;								// the call's AIF2 clock needs the 24.576 MHz family
#endif
	iRate = aHz;
	// periods of ~10 ms: rate * 4 bytes / 100, rounded up to 64 bytes (48 k = 1920 = 480 frames, 8 k = 320)
	iPeriodBytes = ((TUint)aHz * 4 / 100 + 63) & ~63u;
	if (iPeriodBytes > KMaxPeriodBytes) iPeriodBytes = KMaxPeriodBytes;
	iRingBytes = iPeriodBytes * KPeriods;
	for (TUint p = 0; p < KPeriods; ++p)
		{
		iLli[p].iSrc = iRingPhys + p * iPeriodBytes;
		iLli[p].iLen = iPeriodBytes;
		}
	if (!iPowered)
		return KErrNone;
	if (r->i44k != iIs44k)
		Clocks(r->i44k);
	Dai(KI2sClkDiv) = (1u << 7) | (1u << 4) | r->iMclkDiv;		// MCLK_EN | BCLK div code 1 | MCLK div code
	Cd(KCdSysSr) = (Cd(KCdSysSr) & ~(0xFu << 12)) | ((TUint32)r->iFs << 12);
	Cd(KCdAif1Clk) = (Cd(KCdAif1Clk) & ~(0xFu << 9)) | ((TUint32)r->iBclkDiv << 9);
	return KErrNone;
	}

void TA64Sound::SetVolume(TInt aVol)
	{
	iVolume = aVol;
	ApplyVolume();
	}

void TA64Sound::ApplyVolume()
	{
	if (!iPowered)
		return;
	if (iVolume <= 0)
		{
		AnalogWrite(KAnLineoutCtrl0, 0x30);						// LINEOUT off = mute (sources stay mono differential)
		return;
		}
	// attenuation = (255 - v) * 0.5 dB, in quarter-dB units; LINEOUT takes whole 1.5 dB steps (codes 31..2), the DAC
	// digital volume (0.75 dB per step, 0xA0 = 0 dB) the remainder and anything past the analog range
	const TInt att4 = (255 - iVolume) * 2;
	TInt analog = att4 / 6;
	if (analog > 29) analog = 29;
	TInt dig = (att4 - analog * 6 + 1) / 3;
	TInt code = 0xA0 - dig;
	if (code < 0) code = 0;
	AnalogWrite(KAnLineoutCtrl1, (TUint8)(31 - analog));
	AnalogWrite(KAnLineoutCtrl0, 0xF0);
	Cd(KCdDacVol) = ((TUint32)code << 8) | (TUint32)code;
	}

TInt TA64Sound::Init()
	{
	DPlatChunkHw* ch;
	TInt r;
	ch = NULL; r = DPlatChunkHw::New(ch, KPhysDma, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone) return r;
	iDma = ch->LinearAddress();
	ch = NULL; r = DPlatChunkHw::New(ch, KPhysAudio, 0x2000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone) return r;
	iAudio = ch->LinearAddress();
	ch = NULL; r = DPlatChunkHw::New(ch, KPhysPrcm, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone) return r;
	iPrcm = ch->LinearAddress();
	// the ring + its LLIs: physically contiguous; the ring uncached-buffered (the CPU copies into it, the DMA reads
	// it: no cache maintenance), the LLIs strongly ordered
	const TInt lliBytes = Kern::RoundToPageSize(KPeriods * sizeof(SLli));
	r = Epoc::AllocPhysicalRam(KRingMax + lliBytes, iRingPhys);
	if (r != KErrNone) return r;
	ch = NULL; r = DPlatChunkHw::New(ch, iRingPhys, KRingMax, EMapAttrSupRw | EMapAttrBufferedNC);
	if (r != KErrNone) return r;
	iRing = (TUint8*)ch->LinearAddress();
	ch = NULL; r = DPlatChunkHw::New(ch, iRingPhys + KRingMax, lliBytes, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone) return r;
	iLli = (SLli*)ch->LinearAddress();
	memclr(iRing, KRingMax);
	memclr(iLli, lliBytes);
	const TPhysAddr lliPhys = iRingPhys + KRingMax;
	for (TUint p = 0; p < KPeriods; ++p)
		{
		// memory (DRQ 1, linear, 4-byte, burst 8) -> the DAI TX FIFO (DRQ 15, IO mode, 2-byte, burst 8)
		iLli[p].iCfg = (KDrqSdram << 0) | (0u << 5) | (2u << 6) | (2u << 9) |
					   ((TUint32)KDrqCodec << 16) | (1u << 21) | (2u << 22) | (1u << 25);
		iLli[p].iDst = KPhysAudio + KDai + KI2sFifoTx;
		iLli[p].iPara = 8;										// NORMAL_WAIT
		iLli[p].iNext = lliPhys + ((p + 1) % KPeriods) * sizeof(SLli);	// cyclic
		}
	// bus clocks + resets, once: DMA (gate0 bit 6 / reset0 bit 6), audio codec (gate2 bit 0 / reset2 bit 0)
	Ccu(KCcuBusGate0) |= 1u << 6;
	Ccu(KCcuBusReset0) |= 1u << 6;
	Ccu(KCcuBusGate2) |= 1u << 0;
	Ccu(KCcuBusReset2) |= 1u << 0;
	// the analog standby state (Linux idle): headphone PA clock ungated, jack detector on; everything else off
	AnalogWrite(KAnHpCtrl, (TUint8)(AnalogRead(KAnHpCtrl) & ~(1u << 7)));
	AnalogWrite(KAnJackMic, (TUint8)(AnalogRead(KAnJackMic) | (1u << 7)));
	// the speaker amplifier pin: PC7 output, LOW (port C CFG0 bits 31:28 = 1)
	volatile TUint32& cfg0 = R32(KHwBaseCcuPio, 0x800 + 2 * 0x24 + 0x00);
	cfg0 = (cfg0 & ~(0xFu << 28)) | (1u << 28);
	AmpOff();
	iRate = 48000;
	iRxRate = 48000;
	iVolume = 255;
	SetRate(48000);
	// the DMA controller: auto-gating on (sun6i_enable_clock_autogate_h3), our channel idle, its PKG IRQ enabled
	Dma(KDmaAutoGate) = 0x4;
	Ch(KDmaEnable) = 0;
	Dma(KDmaIrqStat) = 0xFu << (KDmaChan * 4);
	Dma(KDmaIrqEn) = (Dma(KDmaIrqEn) & ~(0x7u << (KDmaChan * 4))) | (KDmaIrqPkg << (KDmaChan * 4));
	_LIT(KSndDfc, "A64Snd DFC");
	r = Kern::DynamicDfcQCreate(iDfcQ, 26, KSndDfc);
	if (r != KErrNone) return r;
	iDoneDfc.SetDfcQ(iDfcQ);
	iAmpDfc.SetDfcQ(iDfcQ);
	iPowerDfc.SetDfcQ(iDfcQ);
	iRxDfc.SetDfcQ(iDfcQ);
#ifdef PHE_MODEM
	VoiceDfc = new TDfc(VoiceDfcFn, NULL, iDfcQ, 1);
#endif
	r = Interrupt::Bind(KIntDma, Isr, this);
	if (r >= 0)
		r = Interrupt::Enable(KIntDma);
	ekseam_puts("[EKERN][snd] A64 codec PDD: analog hp="); ekseam_puthex(AnalogRead(KAnHpCtrl));
	ekseam_puts(" dma irq bind="); ekseam_puthex((unsigned)r);
	return r < 0 ? r : KErrNone;
	}

TInt TA64Sound::DmaPeriod()
	{
	// LLI_ADDR holds the NEXT descriptor (the one the DMA will fetch; sun6i_get_chan_size reads it that way)
	const TUint32 next = Ch(KDmaLliAddr);
	const TUint32 lliPhys = iRingPhys + KRingMax;
	if (next < lliPhys || next >= lliPhys + KPeriods * sizeof(SLli))
		return -1;
	return (TInt)(((next - lliPhys) / sizeof(SLli) + KPeriods - 1) % KPeriods);
	}

TInt TA64Sound::Queue(TLinAddr aLin, TInt aBytes, TUint aId)
	{
	// keep two periods between the write counter and the played counter: the DMA reads the period after the
	// played ones, and the DFC's view lags the DMA by up to one period
	if (iFragCount >= KMaxFrags || aBytes > KMaxTransfer ||
		(iWritten - iPlayed) + (TUint32)aBytes > iRingBytes - 2 * iPeriodBytes)
		return KErrNotReady;
	const TUint off = iWritten % iRingBytes;
	TInt first = (TInt)(iRingBytes - off);
	if (aBytes < first)
		first = aBytes;
	memcpy(iRing + off, (const TAny*)aLin, first);
	if (aBytes > first)
		memcpy(iRing, (const TAny*)(aLin + first), aBytes - first);
	iWritten += aBytes;
	// the silence rule: |sample| <= 1 counts as silence (a dither LSB)
	{
	const TInt16* smp = (const TInt16*)aLin;
	TBool silent = ETrue;
	for (TInt i = 0, n = aBytes / 2; i < n && silent; ++i)
		silent = smp[i] >= -1 && smp[i] <= 1;
	if (!silent)
		{
		iSilentBytes = 0;
		if (!iAmp)
			{
			iAmpTimer.Cancel();
			AmpOn();
			}
		}
	else
		{
		iSilentBytes += (TUint32)aBytes;
		const TUint32 limit = iRingBytes + (TUint32)(iRate > 0 ? iRate : 48000) * 4u * KAmpSilenceMs / 1000u;
		if (iAmp && iSilentBytes >= limit)
			AmpOff();
		}
	}
	SFrag& f = iFrags[(iFragHead + iFragCount) % KMaxFrags];
	f.iId = aId; f.iBytes = aBytes; f.iEnd = iWritten;
	++iFragCount;
	if (iRunPending)
		{
		iRunPending = EFalse;
		Run();
		}
	return KErrNone;
	}

void TA64Sound::Run()
	{
	// the DAI: flush the TX FIFO, clear the counter, TX on, TX DRQ on (sun4i_i2s_start_playback)
	Dai(KI2sFifoCtrl) |= 1u << 25;
	Dai(KI2sTxCnt) = 0;
	Dai(KI2sCtrl) |= 1u << 2;
	Dai(KI2sDmaIntCtrl) |= 1u << 7;
	// the DMA from period 0 (the ring beyond the written data is zero = silence)
	iLastPeriod = 0;
	Dma(KDmaIrqStat) = 0xFu << (KDmaChan * 4);
	Ch(KDmaLliAddr) = iRingPhys + KRingMax;
	Ch(KDmaEnable) = 1;
	iRunning = ETrue;
	}

void TA64Sound::Start()
	{
	// the amplifier on now (the hold-off timer cancelled); the engine runs once the first fragment is in the ring
	// (else the DMA would read the period being written)
	iAmpTimer.Cancel();
	if (!iAmp)
		AmpOn();
	if (!iRunning)
		iRunPending = ETrue;
	}

void TA64Sound::Stop()
	{
	Ch(KDmaEnable) = 0;
	Ch(KDmaPause) = 0;
	Dai(KI2sDmaIntCtrl) &= ~(1u << 7);
	Dai(KI2sCtrl) &= ~(1u << 2);
	iRunning = iRunPending = iPaused = EFalse;
	iSilentBytes = 0;
	memclr(iRing, iRingBytes);
	iWritten = iPlayed = 0;
	iLastPeriod = 0;
	iFragHead = iFragCount = 0;
	// the amplifier stays on for a while: a key click = open, play, close
	iAmpTimer.Cancel();
	iAmpTimer.OneShot(NKern::TimerTicks(KAmpHoldOffMs));
	}

void TA64Sound::Pause(TBool aPause)
	{
	if (aPause)
		{
		Ch(KDmaPause) = 2u;
		Dai(KI2sDmaIntCtrl) &= ~(1u << 7);						// TX DRQ off, TX block off (as sun4i_i2s PAUSE_PUSH)
		Dai(KI2sCtrl) &= ~(1u << 2);
		}
	else
		{
		Dai(KI2sFifoCtrl) |= 1u << 25;							// flush, TX on, DRQ on, DMA resumes from the next sample
		Dai(KI2sCtrl) |= 1u << 2;
		Dai(KI2sDmaIntCtrl) |= 1u << 7;
		Ch(KDmaPause) = 0u;
		}
	iPaused = aPause;
	}

TInt64 TA64Sound::BytesPlayed()
	{
	if (!iRunning)
		return 0;
	// the played periods + the DMA's progress in the current one (CUR_CNT counts the bytes left in the LLI)
	TUint32 left = Ch(KDmaCurCnt);
	if (left > iPeriodBytes) left = iPeriodBytes;
	return (TInt64)iPlayed + (TInt64)(iPeriodBytes - left);
	}

void TA64Sound::Isr(TAny* aPtr)
	{
	TA64Sound& s = *(TA64Sound*)aPtr;
	const TUint32 st = s.Dma(KDmaIrqStat) & (0x7u << (KDmaChan * 4));
	if (st)
		{
		s.Dma(KDmaIrqStat) = st;								// write-1-to-clear
		s.iDoneDfc.Add();
		}
	}

// --- the record unit: a timer completes each fragment, zero-filled, after bytes / (rate * 4) seconds --------------
TInt TA64Sound::RxQueue(TLinAddr aLin, TInt aBytes, TUint aId)
	{
	if (iRxCount >= KMaxRxFrags)
		return KErrNotReady;
	memclr((TAny*)aLin, aBytes);
	SFrag& f = iRx[(iRxHead + iRxCount) % KMaxRxFrags];
	f.iId = aId; f.iBytes = aBytes; f.iEnd = 0;
	if (iRxCount++ == 0 && iRxRunning)
		RxArm();
	return KErrNone;
	}

void TA64Sound::RxArm()
	{
	const SFrag& f = iRx[iRxHead];
	TInt ms = (TInt)((TInt64)f.iBytes * 1000 / (iRxRate * 4));
	if (ms < 1) ms = 1;
	iRxTimer.OneShot(NKern::TimerTicks(ms));
	}

void TA64Sound::RxStop()
	{
	iRxTimer.Cancel();
	iRxHead = iRxCount = 0;
	iRxRunning = EFalse;
	}

void TA64Sound::RxTimerFn(TAny* aPtr)	{ ((TA64Sound*)aPtr)->iRxDfc.Add(); }

// --- the PDD ------------------------------------------------------------------------------------------------------
class DA64SoundPdd : public DSoundScPdd
	{
public:
	DA64SoundPdd(TInt aUnit) : iUnit(aUnit), iTraced(0) {}
	~DA64SoundPdd()
		{
		if (!TheSound) return;
		if (TheSound->iTxPdd == this) { TheSound->Stop(); TheSound->iTxPdd = NULL; }
		if (TheSound->iRxPdd == this) { TheSound->RxStop(); TheSound->iRxPdd = NULL; }
		}
	virtual TDfcQue* DfcQ(TInt)				{ return TheSound->iDfcQ; }
	virtual void GetChunkCreateInfo(TChunkCreateInfo& aInfo)
		{
		aInfo.iType = TChunkCreateInfo::ESharedKernelMultiple;
		aInfo.iMapAttr = EMapAttrCachedMax;						// the PDD copies with the CPU; the LDD syncs the cache
		aInfo.iOwnsMemory = ETrue;
		aInfo.iDestroyedDfc = NULL;
		}
	virtual void Caps(TDes8& aCapsBuf) const
		{
		TSoundFormatsSupportedV02 caps;
		caps.iDirection = iUnit == KSoundScTxUnit0 ? ESoundDirPlayback : ESoundDirRecord;
		caps.iChannels = KSoundStereoChannel;
		caps.iRates = KSoundRate8000Hz | KSoundRate11025Hz | KSoundRate12000Hz | KSoundRate16000Hz | KSoundRate22050Hz |
					  KSoundRate24000Hz | KSoundRate32000Hz | KSoundRate44100Hz | KSoundRate48000Hz;
		caps.iEncodings = KSoundEncoding16BitPCM;
		caps.iDataFormats = KSoundDataFormatInterleaved;
		caps.iRequestMinSize = 4;
		caps.iRequestAlignment = 2;								// log2: 4-byte aligned
		caps.iHwConfigNotificationSupport = EFalse;
		aCapsBuf.FillZ(aCapsBuf.MaxLength());
		TPtrC8 p((const TUint8*)&caps, sizeof(caps));
		aCapsBuf = p.Left(Min(p.Length(), aCapsBuf.MaxLength()));
		}
	virtual TInt MaxTransferLen() const		{ return KMaxTransfer; }
	virtual TInt SetConfig(const TDesC8& aConfigBuf)
		{
		TCurrentSoundFormatV02 c;
		TPtr8 p((TUint8*)&c, sizeof(c));
		Kern::InfoCopy(p, aConfigBuf);
		if (c.iChannels != 2 || c.iEncoding != ESoundEncoding16BitPCM || c.iDataFormat != ESoundDataFormatInterleaved)
			return KErrNotSupported;
		static const TInt KRates[] = { 7350, 8000, 8820, 9600, 11025, 12000, 14700, 16000, 22050, 24000, 29400, 32000,
									   44100, 48000 };
		if ((TUint)c.iRate >= sizeof(KRates) / sizeof(KRates[0]))
			return KErrNotSupported;
		if (iUnit != KSoundScTxUnit0)
			{
			TheSound->iRxRate = KRates[c.iRate];
			return KErrNone;
			}
		const TInt r = TheSound->SetRate(KRates[c.iRate]);
		ekseam_puts("[EKERN][snd] SetConfig rate Hz="); ekseam_puthex((unsigned)KRates[c.iRate]);
		return r;
		}
	virtual TInt SetVolume(TInt aVolume)
		{
		if (iUnit == KSoundScTxUnit0)
			{
			ekseam_puts("[EKERN][snd] SetVolume="); ekseam_puthex((unsigned)aVolume);
			TheSound->SetVolume(aVolume);
			}
		return KErrNone;
		}
	virtual TInt StartTransfer()
		{
		if (iUnit != KSoundScTxUnit0)
			{
			TheSound->iRxPdd = this;
			TheSound->iRxRunning = ETrue;
			if (TheSound->iRxCount)
				TheSound->RxArm();
			return KErrNone;
			}
		ekseam_puts("[EKERN][snd] StartTransfer\n");
		TheSound->iTxPdd = this;
		TheSound->Start();
		return KErrNone;
		}
	virtual TInt TransferData(TUint aId, TLinAddr aLin, TPhysAddr, TInt aBytes)
		{
		if (iUnit != KSoundScTxUnit0)
			return TheSound->RxQueue(aLin, aBytes, aId);
		const TInt r = TheSound->Queue(aLin, aBytes, aId);
		if (iTraced < 2)
			{
			++iTraced;
			ekseam_puts("[EKERN][snd] TransferData bytes="); ekseam_puthex((unsigned)aBytes);
			ekseam_puts(" r="); ekseam_puthex((unsigned)r);
			}
		return r;
		}
	virtual void StopTransfer()
		{
		if (iUnit != KSoundScTxUnit0)
			{
			TheSound->RxStop();
			return;
			}
		ekseam_puts("[EKERN][snd] StopTransfer played="); ekseam_puthex(TheSound->iPlayed);
		TheSound->Stop();
		}
	virtual TInt PauseTransfer()
		{
		if (iUnit != KSoundScTxUnit0)
			{
			// the fragment in flight is reported once, partially filled (with 0 bytes: silence), as the LDD allows
			// while paused (ID 0); the rest is dropped (the LDD cancels its queue itself)
			TheSound->iRxTimer.Cancel();
			const TBool inFlight = TheSound->iRxCount > 0;
			TheSound->iRxHead = TheSound->iRxCount = 0;
			if (inFlight)
				Ldd()->RecordCallback(0, KErrNone, 0);
			return KErrNone;
			}
		TheSound->Pause(ETrue);
		return KErrNone;
		}
	virtual TInt ResumeTransfer()
		{
		if (iUnit != KSoundScTxUnit0)
			return KErrNone;
		TheSound->Pause(EFalse);
		return KErrNone;
		}
	virtual TInt PowerUp()
		{
		// in the client's thread: both units share the codec power; the first open brings it up on the DFC thread
		if (TheSound->iUsers++ == 0)
			{
			TheSound->iPowerSem.iOwningThread = NKern::CurrentThread();
			TheSound->iPowerDfc.Enque();
			NKern::FSWait(&TheSound->iPowerSem);
			}
		return KErrNone;
		}
	virtual void PowerDown()
		{
		if (--TheSound->iUsers <= 0)
			{
			TheSound->iUsers = 0;
			if (iUnit == KSoundScTxUnit0 || TheSound->iTxPdd == NULL)
				TheSound->Stop();
			TheSound->RxStop();
			TheSound->PowerOff();
			}
		}
	virtual TInt CustomConfig(TInt, TAny*)	{ return KErrNotSupported; }
	virtual TInt TimeTransferred(TInt64& aTime, TInt)
		{
		if (iUnit != KSoundScTxUnit0)
			return KErrNotSupported;
		aTime = TheSound->BytesPlayed() * 1000000 / ((TInt64)TheSound->iRate * 4);
		return KErrNone;
		}
	void Done(TUint aId, TInt aBytes)		{ Ldd()->PlayCallback(aId, KErrNone, aBytes); }
	void RxDone(TUint aId, TInt aBytes)		{ Ldd()->RecordCallback(aId, KErrNone, aBytes); }
private:
	TInt iUnit;
	TInt iTraced;
	};

void TA64Sound::DoneDfcFn(TAny* aPtr)
	{
	TA64Sound& s = *(TA64Sound*)aPtr;
	if (!s.iRunning)
		return;
	const TInt now = s.DmaPeriod();
	if (now < 0)
		return;
	// the periods from iLastPeriod up to (not including) the one being transferred have been played: advance the
	// played counter over them, complete the fragments that ended in them, zero them (silence if the writer is late)
	while (s.iLastPeriod != now)
		{
		const TInt p = s.iLastPeriod;
		s.iLastPeriod = (p + 1) % KPeriods;
		s.iPlayed += s.iPeriodBytes;
		if (s.iPlayed - s.iWritten < 0x80000000u && s.iPlayed != s.iWritten)
			s.iWritten = s.iPlayed;									// the writer fell behind: silence from here
		while (s.iFragCount)
			{
			SFrag& f = s.iFrags[s.iFragHead];
			if (s.iPlayed - f.iEnd >= 0x80000000u)				// f.iEnd is beyond iPlayed: not played yet
				break;
			s.iFragHead = (s.iFragHead + 1) % KMaxFrags;
			--s.iFragCount;
			if (s.iTxPdd)
				s.iTxPdd->Done(f.iId, f.iBytes);
			}
		memclr(s.iRing + p * s.iPeriodBytes, s.iPeriodBytes);
		}
	}

void TA64Sound::RxDfcFn(TAny* aPtr)
	{
	TA64Sound& s = *(TA64Sound*)aPtr;
	if (!s.iRxRunning || !s.iRxCount)
		return;
	const SFrag f = s.iRx[s.iRxHead];
	s.iRxHead = (s.iRxHead + 1) % KMaxRxFrags;
	--s.iRxCount;
	if (s.iRxCount)
		s.RxArm();
	if (s.iRxPdd)
		s.iRxPdd->RxDone(f.iId, f.iBytes);
	}

class DA64SoundFactory : public DPhysicalDevice
	{
public:
	DA64SoundFactory()		{ iUnitsMask = (1 << KSoundScTxUnit0) | (1 << KSoundScRxUnit0); iVersion = RSoundSc::VersionRequired(); }
	virtual TInt Install()	{ return SetName(&KA64PddName); }
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DBase*& aChannel, TInt aUnit, const TDesC8*, const TVersion&)
		{
		aChannel = new DA64SoundPdd(aUnit);
		return aChannel ? KErrNone : KErrNoMemory;
		}
	virtual TInt Validate(TInt aUnit, const TDesC8*, const TVersion& aVer)
		{
		if (!Kern::QueryVersionSupported(RSoundSc::VersionRequired(), aVer) || !TheSound)
			return KErrNotSupported;
		return (aUnit == KSoundScTxUnit0 || aUnit == KSoundScRxUnit0) ? KErrNone : KErrNotSupported;
		}
	};

// A64_SOUND_TEST: 1 s of 440 Hz at 48 kHz (a parabolic sine, amplitude 8000), fed through the ring as the LDD would
void TA64Sound::SelfTest()
	{
	const TInt KBytes = 48000 * 4;
	TPhysAddr phys;
	if (Epoc::AllocPhysicalRam(Kern::RoundToPageSize(KBytes), phys) != KErrNone)
		return;
	DPlatChunkHw* ch = NULL;
	if (DPlatChunkHw::New(ch, phys, Kern::RoundToPageSize(KBytes), EMapAttrSupRw | EMapAttrFullyBlocking) != KErrNone)
		return;
	TInt16* s = (TInt16*)ch->LinearAddress();
	for (TInt i = 0; i < KBytes / 4; ++i)
		{
		const TInt ph = (i * 440 * 4096 / 48000) & 4095;
		const TInt x = (ph & 2047) - 1024;
		TInt v = 8000 - (8000 * x / 1024) * x / 1024;
		if (ph >= 2048)
			v = -v;
		s[2 * i] = s[2 * i + 1] = (TInt16)v;
		}
	PowerOn();
	SetRate(48000);
	SetVolume(255);
	TInt off = 0, id = 0, refused = 0;
	Start();
	const TUint32 t0 = NKern::TickCount();
	while (off < KBytes && NKern::TickCount() - t0 < (TUint32)NKern::TimerTicks(3000))
		{
		const TInt n = Min(KMaxTransfer, KBytes - off);
		if (Queue((TLinAddr)s + off, n, id) == KErrNone)
			{ off += n; ++id; }
		else
			{ ++refused; NKern::Sleep(NKern::TimerTicks(5)); }
		}
	NKern::Sleep(NKern::TimerTicks(300));
	ekseam_puts("[EKERN][snd] self-test: queued "); ekseam_puthex((unsigned)off);
	ekseam_puts(" bytes in "); ekseam_puthex((unsigned)id);
	ekseam_puts(" pieces, refused "); ekseam_puthex((unsigned)refused);
	ekseam_puts(" times; played="); ekseam_puthex(iPlayed);
	ekseam_puts(" dai tx cnt="); ekseam_puthex(Dai(KI2sTxCnt));
	ekseam_puts(" lineout0="); ekseam_puthex(AnalogRead(KAnLineoutCtrl0));
	ekseam_puts(" olmix="); ekseam_puthex(AnalogRead(KAnOlMix));
	ekseam_puts(" frags left="); ekseam_puthex((unsigned)iFragCount);
	Stop();
	NKern::Sleep(NKern::TimerTicks(KAmpHoldOffMs + 100));
	PowerOff();
	ekseam_puts("[EKERN][snd] self-test: powered off, amp="); ekseam_puthex((unsigned)iAmp);
	}

#ifdef PHE_MODEM
// ---- Modem rung M5 (telephony-plan.md section 16): the VOICE CALL ROUTE, the EG25-G's PCM on AIF2 -----------------------
// A codec-internal path, no CPU data (research/sound-linux.md 1.3-1.5 + 3.5 = sun8i-codec.c / sun50i-codec-analog.c / the
// PinePhone UCM VoiceCall verb): AIF2 = PB4-7 (function 2), the A64 codec is the PCM clock master (the modem is slave:
// AT+QDAI=1,1,... mode 1), DSP_A, BCLK inverted, 16-bit, 1 x 32-bit slot, 8 kHz (BCLK 256 kHz = the QDAI clock 1).
// Downlink: AIF2 DAC (Mix Mono) -> DAC mixer -> DAC -> left output mixer -> EARPIECE. Uplink: MIC1 (+24 dB boost, MBIAS)
// -> ADC mixer -> ADC -> AIF2 ADC mixer -> AIF2 ADC (Mix Mono) -> the modem. Switched on the sound DFC thread (serialised
// with PowerOn/PowerOff); the codec stays powered while the route is on even with no sound channel open.
static const TUint KCdAif2Clk = 0x080, KCdAif2AdcDat = 0x084, KCdAif2DacDat = 0x088, KCdAif2Mxr = 0x08C,
				   KCdAif2AdcVol = 0x090, KCdAif2DacVol = 0x098, KCdAdcDig = 0x100, KCdAdcVol = 0x104, KCdAif3Path = 0x0CC;
static const TUint KAnEarCtrl0 = 0x03, KAnEarCtrl1 = 0x04, KAnMic1Ctrl = 0x07, KAnAdcMixL = 0x0B, KAnAdcMixR = 0x0C,
				   KAnAdcCtrl = 0x0D, KAnMicBias = 0x0E;

void TA64Sound::VoiceRoute(TBool aOn)
	{
	if (aOn == VoiceOn)
		return;
	if (aOn)
		{
		if (!iPowered)
			{
			iIs44k = EFalse;										// the AIF2 divider below assumes the 24.576 MHz family
			PowerOn();
			}
		if (iIs44k)
			Clocks(EFalse);
		// pins PB4 SYNC, PB5 BCLK, PB6 DOUT, PB7 DIN = AIF2 (port B CFG0 bits 31:16, function 2)
		volatile TUint32& pb = R32(KHwBaseCcuPio, 0x800 + 1 * 0x24 + 0x00);
		pb = (pb & 0x0000FFFFu) | 0x22220000u;
		Cd(KCdSysClk) |= 1u << 7;									// AIF2CLK_ENA
		Cd(KCdModClkEna) |= (1u << 14) | (1u << 3);					// AIF2 + ADC module clocks
		Cd(KCdModRst) |= (1u << 14) | (1u << 3);
		Cd(KCdSysSr) &= ~(0xFu << 8);								// AIF2_FS = 0 = 8 kHz
		// AIF2_CLK_CTRL: MSTR_MOD 0 (codec master), CLK_INV 0b10 (BCLK inverted: DSP_A), BCLK_DIV code 11 (/96: 24.576 MHz
		// / 8000 >> 5), LRCK_DIV 1 (32 BCLK), 16-bit words, DATA_FMT 3 (DSP)
		Cd(KCdAif2Clk) = (1u << 14) | (11u << 9) | (1u << 6) | (1u << 4) | (3u << 2);
		Cd(KCdAif2AdcDat) = (1u << 15) | (1u << 14) | (3u << 10) | (3u << 8);	// AIF2 ADC L/R on, Mix Mono
		Cd(KCdAif2DacDat) = (1u << 15) | (1u << 14) | (3u << 10) | (3u << 8);	// AIF2 DAC L/R on, Mix Mono
		Cd(KCdAif2Mxr) = (1u << 12) | (1u << 8);					// AIF2 ADC mixer <- ADC L/R (the mic)
		Cd(KCdAif2AdcVol) = 0xA0A0u;
		Cd(KCdAif2DacVol) = 0xA0A0u;
		Cd(KCdAif3Path) &= ~(3u << 8);								// AIF2 DAC source = AIF2
		Cd(KCdAdcDig) |= 1u << 15;									// ADC on
		Cd(KCdAdcVol) = 0xA0A0u;
		Cd(KCdDacMxr) |= (1u << 13) | (1u << 9);					// DAC mixer <- AIF2 DAC L/R
		// analog: MIC1 amp on, boost 1 (24 dB); master mic bias; ADC mixers <- MIC1; ADC L/R on; earpiece <- left mixer,
		// PA on, unmuted, 0 dB (31)
		AnalogWrite(KAnMic1Ctrl, (TUint8)((AnalogRead(KAnMic1Ctrl) & ~0x0Fu) | (1u << 3) | 1u));
		AnalogWrite(KAnMicBias, (TUint8)(AnalogRead(KAnMicBias) | (1u << 7)));
		AnalogWrite(KAnAdcMixL, (TUint8)(AnalogRead(KAnAdcMixL) | (1u << 6)));
		AnalogWrite(KAnAdcMixR, (TUint8)(AnalogRead(KAnAdcMixR) | (1u << 6)));
		AnalogWrite(KAnAdcCtrl, (TUint8)(AnalogRead(KAnAdcCtrl) | (1u << 7) | (1u << 6)));
		AnalogWrite(KAnEarCtrl0, (TUint8)((AnalogRead(KAnEarCtrl0) & ~3u) | 3u));
		AnalogWrite(KAnEarCtrl1, (TUint8)((1u << 7) | (1u << 6) | 31u));
		VoiceOn = ETrue;
		}
	else
		{
		AnalogWrite(KAnEarCtrl1, 0);								// earpiece PA off + muted first
		AnalogWrite(KAnAdcCtrl, (TUint8)(AnalogRead(KAnAdcCtrl) & ~((1u << 7) | (1u << 6))));
		AnalogWrite(KAnAdcMixL, (TUint8)(AnalogRead(KAnAdcMixL) & ~(1u << 6)));
		AnalogWrite(KAnAdcMixR, (TUint8)(AnalogRead(KAnAdcMixR) & ~(1u << 6)));
		AnalogWrite(KAnMicBias, (TUint8)(AnalogRead(KAnMicBias) & ~(1u << 7)));
		AnalogWrite(KAnMic1Ctrl, (TUint8)(AnalogRead(KAnMic1Ctrl) & ~(1u << 3)));
		Cd(KCdDacMxr) &= ~((1u << 13) | (1u << 9));
		Cd(KCdAif2Mxr) = 0;
		Cd(KCdAif2AdcDat) = 0;
		Cd(KCdAif2DacDat) = 0;
		Cd(KCdAdcDig) &= ~(1u << 15);
		Cd(KCdModClkEna) &= ~((1u << 14) | (1u << 3));
		Cd(KCdModRst) &= ~((1u << 14) | (1u << 3));
		Cd(KCdSysClk) &= ~(1u << 7);
		VoiceOn = EFalse;
		if (iUsers == 0)
			PowerOff();
		}
	ekseam_puts("[EKERN][snd] voice route "); ekseam_puts(aOn ? "ON" : "OFF");
	ekseam_puts(": aif2clk="); ekseam_puthex(Cd(KCdAif2Clk));
	ekseam_puts(" dacmxr="); ekseam_puthex(Cd(KCdDacMxr));
	ekseam_puts(" aif2mxr="); ekseam_puthex(Cd(KCdAif2Mxr));
	ekseam_puts(" sysclk="); ekseam_puthex(Cd(KCdSysClk));
	ekseam_puts(" ear1="); ekseam_puthex(AnalogRead(KAnEarCtrl1));
	ekseam_puts(" mic1="); ekseam_puthex(AnalogRead(KAnMic1Ctrl));
	ekseam_puts(" adc="); ekseam_puthex(AnalogRead(KAnAdcCtrl)); ekseam_puts("\n");
	}

static void VoiceDfcFn(TAny*)
	{
	if (TheSound)
		TheSound->VoiceRoute(VoiceWanted);
	}

// the switch for the modem side (a64uart.cpp, unit 3 KSignalDTR): any context; the work runs on the sound DFC thread
extern "C" void A64SoundVoiceRoute(TBool aOn)
	{
	if (!VoiceDfc)
		return;
	VoiceWanted = aOn;
	if (NKern::CurrentContext() == NKern::EThread)
		VoiceDfc->Enque();
	else
		VoiceDfc->Add();
	}
#endif

DLogicalDevice* CreateLogicalDevice_soundsc();				// soundldd.cpp (DECLARE_STANDARD_LDD, renamed)

// Board entry (a64_glue.cpp, with the other drivers): the hardware, then the real LDD ("SoundSc") + our PDD factory
// ("SoundSc.A64"). mdasoundadapter loads "ESOUNDSC.LDD"/"SOUNDSC.PDD" by name: no such files in our ROM -- the
// loader delta toolchain/efile_exe/ldr_builtin_devices.cpp answers KErrAlreadyExists for a device already installed.
extern "C" void A64SoundInstall()
	{
	NKern::ThreadEnterCS();
	TheSound = new TA64Sound;
	TInt r = TheSound ? TheSound->Init() : KErrNoMemory;
	if (r != KErrNone)
		{
		ekseam_puts("[EKERN][snd] no audio ("); ekseam_puthex((unsigned)r); ekseam_puts("): no sound\n");
		delete TheSound;
		TheSound = NULL;
		NKern::ThreadLeaveCS();
		return;
		}
#ifdef A64_SOUND_TEST
	TheSound->SelfTest();
#endif
	DLogicalDevice* ld = CreateLogicalDevice_soundsc();
	r = ld ? Kern::InstallLogicalDevice(ld) : KErrNoMemory;
	ekseam_puts("[EKERN][snd] InstallLogicalDevice(SoundSc) r="); ekseam_puthex((unsigned)r);
	DPhysicalDevice* pd = new DA64SoundFactory;
	r = pd ? Kern::InstallPhysicalDevice(pd) : KErrNoMemory;
	ekseam_puts("[EKERN][snd] InstallPhysicalDevice(SoundSc.A64) r="); ekseam_puthex((unsigned)r);
	NKern::ThreadLeaveCS();
	}
