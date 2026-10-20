// a64camera.cpp -- OURS (2026-10-17, docs/phorepoc/camera-plan.md C1-phone + C2): the PinePhone camera board glue.
// C1 (phone): installs the 2010 shared-chunk camera LDD (DCameraScLdd "CameraSc", third_party/symbian2010-camerasc,
//   unmodified, entry renamed) + the 2010 template PDD "CameraSc.TE" (the timer-driven fake sensor), exactly the pair
//   the x86 image runs (tools/phorepoc-x86/ekern/xk_camerasc.cpp). phecam (ECam plug-in over RDevCameraSc) loads them
//   by file name: the loader delta toolchain/efile_exe/ldr_builtin_devices.cpp answers KErrAlreadyExists.
// C2 (phone): the first hardware proof -- AXP803 camera rails (ALDO1 1.8 V, DLDO3 2.8 V, ELDO3 1.8 V), MCLK 24 MHz on
//   PE1, the bit-banged CCI bus (SCL PE12, SDA PE13; Linux's i2c-gpio, pp.dtsi), and the chip IDs:
//   OV5640 (rear, 0x4c) 0x300A/0x300B = 56 40, GC2145 (front, 0x3c) 0xF0/0xF1 = 21 45. Sequences from
//   docs/phorepoc/research/camera-linux.md section 3. Each sensor is powered alone (they share the parallel bus);
//   afterwards both are held in reset, MCLK is gated and the rails/pins go back to what they were.
//   On by default (one-shot, ~150 ms at boot); A64DEFS=-DA64_NO_CAMERA_PROBE turns it off.
#include <kernel/kern_priv.h>
#include <nkern.h>
#include <assp.h>
#include "syborg.h"
#include <kernel/cache.h>
#include "camerasc_plat.h"		// the 2010 template PDD's sensor interface (DSensorIf / DTemplateSensorIf)

extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(TUint32);
extern "C" TInt A64PmicUpdateReg(TUint8 aReg, TUint8 aClr, TUint8 aSet);	// display/a64_display.cpp
extern "C" TInt A64PmicReadReg(TUint8 aReg);

DLogicalDevice* CreateLogicalDevice_camerasc();		// cameraldd.cpp (DECLARE_STANDARD_LDD, renamed)
DPhysicalDevice* CreatePhysicalDevice_camerasc();	// camerasc.cpp, the template PDD (DECLARE_STANDARD_PDD, renamed)

#if 1	// the helpers below serve the C2 probe, the C3 test and the PDD sensor (below)
static inline volatile TUint32& R32(TUint aOff) { return *(volatile TUint32*)(KHwBaseCcuPio + aOff); }

// PIO (CCU page + 0x800): port n at n * 0x24, CFG0..3 +0..+0xC (4 bits/pin), DAT +0x10
static const TUint KPio = 0x800;
enum { KPortC = 2, KPortD = 3, KPortE = 4 };
static const TUint KFnIn = 0, KFnOut = 1, KFnCsi = 2;

static TUint PinFn(TUint aPort, TUint aPin)
	{
	return (R32(KPio + aPort * 0x24 + (aPin / 8) * 4) >> ((aPin % 8) * 4)) & 0xF;
	}
static void SetPinFn(TUint aPort, TUint aPin, TUint aFn)
	{
	volatile TUint32& cfg = R32(KPio + aPort * 0x24 + (aPin / 8) * 4);
	const TUint sh = (aPin % 8) * 4;
	cfg = (cfg & ~(0xFu << sh)) | (aFn << sh);
	}
static void PinLow(TUint aPort, TUint aPin)			// open drain: assert = drive low
	{
	volatile TUint32& dat = R32(KPio + aPort * 0x24 + 0x10);
	dat &= ~(1u << aPin);
	SetPinFn(aPort, aPin, KFnOut);
	}
static void PinFloat(TUint aPort, TUint aPin)			// open drain: release = input, the pull-up wins
	{
	SetPinFn(aPort, aPin, KFnIn);
	}
// boot 975: the OV5640 PWDN line (PC0) has NO pull-up (released it read 0 = the sensor ACTIVE on the shared bus), so the
// sensor-select lines that must be HIGH are driven as push-pull outputs, as Linux does with its powerdown/enable GPIOs
static void PinHigh(TUint aPort, TUint aPin)
	{
	volatile TUint32& dat = R32(KPio + aPort * 0x24 + 0x10);
	dat |= 1u << aPin;
	SetPinFn(aPort, aPin, KFnOut);
	}
static TBool PinIn(TUint aPort, TUint aPin)
	{
	return (R32(KPio + aPort * 0x24 + 0x10) >> aPin) & 1;
	}

// the CCI bus: SCL PE12, SDA PE13, ~100 kHz (i2c-gpio delay-us = 3; we use 5)
static const TUint KScl = 12, KSda = 13;
static inline void Dly() { Kern::NanoWait(5000); }
static void SclHi() { PinFloat(KPortE, KScl); for (TInt i = 0; i < 100 && !PinIn(KPortE, KScl); ++i) Dly(); }
static void SclLo() { PinLow(KPortE, KScl); }
static void SdaHi() { PinFloat(KPortE, KSda); }
static void SdaLo() { PinLow(KPortE, KSda); }
static void Start() { SdaHi(); SclHi(); Dly(); SdaLo(); Dly(); SclLo(); Dly(); }
static void Stop() { SdaLo(); Dly(); SclHi(); Dly(); SdaHi(); Dly(); }
static TBool WriteByte(TUint8 aB)		// ETrue = ACK
	{
	for (TInt i = 7; i >= 0; --i)
		{
		if ((aB >> i) & 1) SdaHi(); else SdaLo();
		Dly(); SclHi(); Dly(); SclLo();
		}
	SdaHi(); Dly(); SclHi(); Dly();
	const TBool ack = !PinIn(KPortE, KSda);
	SclLo(); Dly();
	return ack;
	}
static TUint8 ReadByteNack()
	{
	TUint8 b = 0;
	SdaHi();
	for (TInt i = 0; i < 8; ++i)
		{
		Dly(); SclHi(); Dly();
		b = (TUint8)((b << 1) | (PinIn(KPortE, KSda) ? 1 : 0));
		SclLo();
		}
	SdaHi(); Dly(); SclHi(); Dly(); SclLo(); Dly();	// NACK: the last byte
	return b;
	}
// one register read; aReg16 = OV5640's 16-bit addresses. < 0 = no ACK (-1 address, -2 register, -3 read address)
static TInt CciRead(TUint8 aAddr, TUint aReg, TBool aReg16)
	{
	Start();
	if (!WriteByte((TUint8)(aAddr << 1))) { Stop(); return -1; }
	if (aReg16 && !WriteByte((TUint8)(aReg >> 8))) { Stop(); return -2; }
	if (!WriteByte((TUint8)aReg)) { Stop(); return -2; }
	Start();
	if (!WriteByte((TUint8)((aAddr << 1) | 1))) { Stop(); return -3; }
	const TUint8 v = ReadByteNack();
	Stop();
	return v;
	}

// one 16-bit-address register write (OV5640); < 0 = no ACK
static TInt CciWrite16(TUint8 aAddr, TUint aReg, TUint8 aVal)
	{
	Start();
	const TBool ok = WriteByte((TUint8)(aAddr << 1)) && WriteByte((TUint8)(aReg >> 8)) && WriteByte((TUint8)aReg) && WriteByte(aVal);
	Stop();
	return ok ? KErrNone : -1;
	}

static void Ms(TInt aMs) { NKern::Sleep(NKern::TimerTicks(aMs) + 1); }
static void Log(const char* aS, TUint32 aV) { ekseam_puts(aS); ekseam_puthex(aV); }

#if 1
// ---------------------------------------------------------------------------------------------------------------
// C3 (camera-plan.md): the A64 CSI0 capture engine + a minimal OV5640 VGA YUYV set-up, with a boot test that powers
// the OV5640, programs it, captures frames into physically contiguous RAM and logs the CSI interrupts + the first
// bytes. TA64Csi is the hardware half the "CameraSc.A64" PDD will own (the PDD itself = the next step).
//
// CSI0 (camera-linux.md section 1; register map = sun6i_csi_reg.h offsets, bit fields as described there):
//   EN 0x00 bit0; IF_CFG 0x04 (bit18 VREF_POL, bit17 HREF_POL, bit16 CLK_POL, [10:8] data width 0 = 8 bit,
//   [4:0] CSI_IF 0 = YUV422 interleaved / raw); CAP 0x08 (bit0 single shot, bit1 continuous);
//   CH_CFG 0x44 ([23:20] input fmt 0 = RAW, [19:16] output fmt 0 = pass-through, [11:10] field sel 2 = both);
//   FIFO0 0x50 = MBUS address >> 2 (MBUS address = phys - 0x40000000: a64.dtsi mbus dma-ranges <0 0x40000000 0xc0000000>; boot 912 wrote to phys 0x9xxxxxxx); CH_INT_EN 0x70 / CH_INT_STA 0x74 (bit0 CD, bit1 FD, bits2-4 FIFO overflow, bit7 VS);
//   HSIZE 0x80 / VSIZE 0x84 ([28:16] length, [12:0] start; RAW pass-through counts BYTES horizontally);
//   BUF_LEN 0x88 (line bytes); debug counters FRM_CLK_CNT 0x90, FIFO_STAT 0x98, PCLK_STAT 0x9c.
// Packed YUYV is captured as RAW pass-through: the CSI copies the byte stream, 2 bytes per pixel.
static const TUint32 KPhysCsi0 = 0x01CB0000;
static const TInt KIntCsi = 32 + 84;					// GIC SPI 84 = id 116
static const TInt KCsiW = 640, KCsiH = 480, KCsiBytes = KCsiW * 2 * KCsiH;

class TA64Csi
	{
public:
	TInt Init();
	void Setup(TBool aVsyncActiveLow);
	void Configure(TPhysAddr aPhys, TInt aW, TInt aH, TBool aVsyncActiveLow = ETrue);
	inline void SetAddr(TPhysAddr aPhys) { R(0x50) = (aPhys - 0x40000000u) >> 2; }
	void Start(TBool aContinuous);
	void Stop();
	static void Isr(TAny* aPtr);
	inline volatile TUint32& R(TUint aOff) const { return *(volatile TUint32*)(iBase + aOff); }
public:
	TLinAddr iBase;
	TPhysAddr iBufPhys;
	TLinAddr iBuf;
	volatile TInt iIrqs, iFd, iCd, iVs, iOvf;
	volatile TUint32 iLastSta;
	TDfc* iFrameDfc;								// the PDD sensor: queued on every frame done
	};

TInt TA64Csi::Init()
	{
	DPlatChunkHw* ch = NULL;
	TInt r = DPlatChunkHw::New(ch, KPhysCsi0, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone) return r;
	iBase = ch->LinearAddress();
	const TInt size = Kern::RoundToPageSize(KCsiBytes);
	r = Epoc::AllocPhysicalRam(size, iBufPhys);			// physically contiguous: the CSI DMA has no IOMMU
	if (r != KErrNone) return r;
	ch = NULL;
	r = DPlatChunkHw::New(ch, iBufPhys, size, EMapAttrSupRw | EMapAttrFullyBlocking);	// uncached: the test reads it straight
	if (r != KErrNone) return r;
	iBuf = ch->LinearAddress();
	memset((TAny*)iBuf, 0xA5, 64);						// a marker: unchanged = the DMA never wrote
	r = Interrupt::Bind(KIntCsi, Isr, this);
	if (r == KErrNone) r = Interrupt::Enable(KIntCsi);
	return r;
	}

void TA64Csi::Setup(TBool aVsyncActiveLow)
	{
	Configure(iBufPhys, KCsiW, KCsiH);
	if (!aVsyncActiveLow) R(0x04) &= ~(1u << 18);
	}

// one capture set-up: OV5640 sync polarities, raw pass-through of packed YUYV, the window = aW x aH, FIFO0 = aPhys
void TA64Csi::Configure(TPhysAddr aPhys, TInt aW, TInt aH, TBool aVsyncActiveLow)
	{
	R(0x00) = 0;
	// polarity mapping as Linux's sun6i_csi (a sync line that is ACTIVE LOW sets the "positive" VREF/HREF bit)
	R(0x04) = (aVsyncActiveLow ? (1u << 18) : 0) | (1u << 16);	// VREF_POL_NEGATIVE for an active-low VSYNC, HREF positive, CLK_POL_RISING (bit 16 = 1: sun6i_csi_reg.h), 8-bit, YUV/raw
	R(0x44) = (0u << 20) | (0u << 16) | (2u << 10);		// RAW in, pass-through out, both fields
	R(0x80) = ((TUint32)(aW * 2) << 16) | 0;
	R(0x84) = ((TUint32)aH << 16) | 0;
	R(0x88) = aW * 2;
	SetAddr(aPhys);			// the MBUS view of DRAM starts at 0 (a64.dtsi dma-ranges)
	R(0x74) = 0xFF;
	R(0x70) = 0xFF;									// all interrupts: the test wants to see everything
	R(0x00) = 1u | (1u << 2);					// CSI_EN + CLK_CNT_EN (bit 2: the FRM_CLK/PCLK debug counters only count with it; boot 901 read 0)
	}

void TA64Csi::Start(TBool aContinuous) { R(0x08) = aContinuous ? 2 : 1; }
void TA64Csi::Stop() { R(0x08) = 0; R(0x70) = 0; R(0x74) = 0xFF; R(0x00) = 0; }

void TA64Csi::Isr(TAny* aPtr)
	{
	TA64Csi& c = *(TA64Csi*)aPtr;
	const TUint32 sta = c.R(0x74);
	c.R(0x74) = sta;
	c.iLastSta = sta;
	++c.iIrqs;
	if ((sta & 2) && c.iFrameDfc) c.iFrameDfc->Add();
	if (sta & 1) ++c.iCd;
	if (sta & 2) ++c.iFd;
	if (sta & 0x1C) ++c.iOvf;
	if (sta & 0x80) ++c.iVs;
	}

// OV5640 VGA YUYV, derived from the OV5640 datasheet register descriptions (NOT from Linux's GPL tables).
// Per register group (the derivation is recorded here and in camera-plan.md "C3"):
//  system   0x3008 SYSTEM CTROL0: bit7 software reset, bit6 software power down -> reset, then configure while
//           powered down (0x42), stream with 0x02. 0x3103 SC PLL CTRL: bit1 = system clock from the PLL.
//  pads     0x3017/0x3018 PAD OUTPUT ENABLE 01/02: VSYNC/HREF/PCLK + D[9:0] outputs on (the 8-bit DVP uses D[9:2]).
//           0x300E MIPI CONTROL 00: MIPI off, DVP on.
//  PLL      24 MHz MCLK; 0x3037 pre-divider 3 + root /2, 0x3036 multiplier 0x38 (VCO 448 MHz), 0x3035 system
//           divider 4 (SCLK ~56 MHz: a slow, safe PCLK for the first frames), 0x3034 8-bit mode, 0x3108 PCLK root /1,
//           SCLK root /2.
//  window   0x3800-0x3807 = the full array (x 0..2623, y 4..1947); 0x3814/0x3815 X/Y odd/even increments 3/1 =
//           2x sub-sampling; 0x3810-0x3813 ISP offsets 16/6; 0x3808-0x380B DVP output 640x480 (the ISP scaler,
//           0x5001 bit5, scales the 1296x972 sub-sampled image down); 0x380C-0x380F HTS 1896 / VTS 984 (> the
//           sub-sampled width/height + blanking).
//  format   0x4300 FORMAT CONTROL 00 = 0x30 (YUV422, YUYV order); 0x501F FORMAT MUX = 0 (ISP YUV).
//  ISP      0x5000 = 0xA7 (LENC, gamma, black/white pixel correction), 0x5001 = 0xA3 (SDE, scale, UV average,
//           colour matrix, AWB). AEC/AGC/AWB stay automatic (0x3503 = 0).
//  DVP      0x4740 POLARITY CTRL00 = 0x23: PCLK active high (bit5), HREF active high (bit1), VSYNC active LOW = bit0 SET. Boot 908:
//           the datasheet says bit0=0 is active low, but the hardware is inverted (Linux ov5640.c notes the same mismatch); 0x22 gave an active-high VSYNC against the CSI set for active low = no frames.
// The analogue/sensor-core tuning registers (0x36xx/0x37xx/0x4xxx timing) are not described in the datasheet and are
// left at their reset values: the picture may be off in quality, the timing/format above decides whether frames flow.
static const TUint16 KOv5640Vga[][2] =
	{
	{0x3103, 0x11}, {0x3008, 0x42},
	// boot 899 (no PCLK lines at the CSI): the per-block resets/clock enables explicitly -- SYSTEM RESET00/02 take
	// every block except the JPEG path (0x3002 bits 4:2 = JFIFO/SFIFO/JPG) out of reset, CLOCK ENABLE00/02 clock
	// them (0x3006 = 0xC3: the JPEG clocks off), rather than trusting the reset values
	{0x3000, 0x00}, {0x3002, 0x1C}, {0x3004, 0xFF}, {0x3006, 0xC3},
	{0x3017, 0xFF}, {0x3018, 0xFF}, {0x300E, 0x58},	// (0x18, the working driver's value, gave NO frames here -- boot 1080; its power sequence differs)
	// boot 1004: 6.4 fps at 0x3035 = 0x41 (vs = fd, no drops). 0x21 (boot 1005) HALVED it (3.2 fps, vs = fd): not a plain divider -- open
	{0x3034, 0x18}, {0x3035, 0x41}, {0x3036, 0x38}, {0x3037, 0x13}, {0x3108, 0x01},
	// X start 0 -> 1 (end +1, same width), boot 951 + the datasheet: the array is "BG/GR line-alternating"; the ISP
	// auto-detects red/blue LINES in flip (sec. 4.1), but nothing re-aligns COLUMNS. With the 2x sub-sampling (0x3814 = 0x31:
	// even increment 1, odd increment 3) the readout takes column pairs (s, s+1), (s+4, s+5)...; from an odd start s the
	// sequence is s, s+3, s+4, s+7... = the Bayer pattern one column over. That swaps G with R/B on each line, which matches the
	// observed hue rotation (teal -> magenta, orange -> yellow-green). The old test of this (boot 948, 0x3801 = 1) only
	// looked at the ISP-RAW output, which may be re-aligned; the YUV picture with an odd start was never looked at.
	{0x3800, 0x00}, {0x3801, 0x00}, {0x3802, 0x00}, {0x3803, 0x04}, {0x3804, 0x0A}, {0x3805, 0x3F},	// x 0..2623 again (research: the odd start was the R/G swap)
	{0x3806, 0x07}, {0x3807, 0x9B}, {0x3808, 0x02}, {0x3809, 0x80}, {0x380A, 0x01}, {0x380B, 0xE0},
	{0x380C, 0x07}, {0x380D, 0x68}, {0x380E, 0x03}, {0x380F, 0xD8},
	{0x3810, 0x00}, {0x3811, 0x10}, {0x3812, 0x00}, {0x3813, 0x06}, {0x3814, 0x31}, {0x3815, 0x31},
	// ^ boots 941-946: the ISP window offsets (0x3811/0x3813) do NOT move the raw CFA phase (RAW probe unchanged) and did
	// not change the colour -> back to 16 / 6.
	// ^ USER GROUND TRUTH boot 940: a light-green chair showed MAGENTA = green sites read as red/blue = the CFA phase is one
	// step off. ISP H offset 0x3811 16 -> 17 (one column; variant C, never measured: its log lines were lost in boot 931;
	// the row variant B, 0x3813 = 7, did not change the picture in boot 931)
#ifdef A64_CAMERA_NO_SDE	// test (2026-10-18 daylight Y wrap): ISP CTRL01 without the SDE (bit7)
	{0x4300, 0x30}, {0x501F, 0x00}, {0x5000, 0xA7}, {0x5001, 0x23}, {0x3503, 0x00},
#else
	{0x4300, 0x30}, {0x501F, 0x00}, {0x5000, 0xA7}, {0x5001, 0xA3}, {0x3503, 0x00},
#endif
	{0x4740, 0x23},
	// CMX (boot 946: teal -> magenta, orange -> yellow-green = a hue rotation). 0x5381-0x5389 = CMX1..9, three rows of
	// three over the RGB input: row 1 -> Y, row 2 -> U (Cb), row 3 -> V (Cr); magnitudes in 0x538A bit0 (CMX9 sign) and
	// 0x538B bits 7..0 (signs of CMX8..CMX1, bit n = CMX n+1), 1 = negative. Reading the reset values that way gives a
	// consistent matrix (Y 32/100/8, U -48/-144/+192, V +160/-152/-8: the U and V rows sum to 0), so the encoding holds.
	// Ours = plain BT.601 with no colour correction (identity CCM): Y = 0.299 R + 0.587 G + 0.114 B (x128 = 38 75 15),
	// Cb = -0.169 R - 0.331 G + 0.500 B and Cr = 0.500 R - 0.419 G - 0.081 B (x256 = 43 85 128 / 128 107 21);
	// negative = CMX4, CMX5, CMX8, CMX9 -> 0x538B = 0x98, 0x538A = 0x01
	// boot 956: reading B (0x538B = 0x31) turned everything red -> reading A (bit n = CMX n+1) is right, and the CMX does drive
	// the hue. Observed error = R and G swapped at the CMX INPUT (teal -> magenta, orange -> yellow-green, white stays white).
	// WORKAROUND: BT.601 with the R and G input columns exchanged in every row (root cause of the swapped demosaic output
	// order still unexplained):
	//  Y  = 0.587 [R slot] + 0.299 [G slot] + 0.114 B  (x128: 4B 26 0F)
	//  Cb = -0.331 [R] - 0.169 [G] + 0.500 B          (x256: -55 -2B +80)
	//  Cr = -0.419 [R] + 0.500 [G] - 0.081 B          (x256: -6B +80 -15)
	// negative: CMX4, CMX5, CMX7 (0x538B bits 3, 4, 6 = 0x58) + CMX9 (0x538A bit0)
#ifdef A64_CAMERA_CMX_PLAIN	// plain BT.601 (default since 2026-10-18: binning fixed the CFA phase; the swapped CMX = A64_CAMERA_CMX_SWAP)
	{0x5381, 0x26}, {0x5382, 0x4B}, {0x5383, 0x0F}, {0x5384, 0x2B}, {0x5385, 0x55}, {0x5386, 0x80},
	{0x5387, 0x80}, {0x5388, 0x6B}, {0x5389, 0x15}, {0x538A, 0x01}, {0x538B, 0x98},
#else
	{0x5381, 0x4B}, {0x5382, 0x26}, {0x5383, 0x0F}, {0x5384, 0x55}, {0x5385, 0x2B}, {0x5386, 0x80},
	{0x5387, 0x6B}, {0x5388, 0x80}, {0x5389, 0x15}, {0x538A, 0x01}, {0x538B, 0x58},
#endif
	// boot 918 (live, but grainy with a magenta cast): the AEC/AGC had picked 123 lines of exposure (0x3500-02 = 0x007B0)
	// and gain 24x (0x350A/B = 0x180, 1/16 steps), because its limits and banding steps were at their reset values.
	//  AEC max exposure 60 Hz / 50 Hz (0x3A02/03, 0x3A14/15) = VTS 984 lines: long exposure before gain
	//  band steps from OUR line rate (measured 18 fps * 984 = ~17.7k lines/s): B50 1/100 s = 177 lines (0x3A08/09),
	//  B60 1/120 s = 147 lines (0x3A0A/0B); max bands per frame 984/177 = 5 (0x3A0E), 984/147 = 6 (0x3A0D)
	//  AEC gain ceiling (0x3A18/19) = 0x7C = 7.75x: less noise, darker in dim light
	//  AWB CTRL03 (0x5183) bit7 = simple (grey-world) AWB: the advanced mode needs per-module calibration (0x5184+)
	//  that we do not have
	{0x3A02, 0x03}, {0x3A03, 0xD8}, {0x3A14, 0x03}, {0x3A15, 0xD8},
	{0x3A08, 0x00}, {0x3A09, 0xB1}, {0x3A0A, 0x00}, {0x3A0B, 0x93}, {0x3A0E, 0x05}, {0x3A0D, 0x06},
	{0x3A18, 0x00}, {0x3A19, 0x7C},
	{0x5183, 0x80},
	// boot 940: the auto AWB never moves (status gains 0x519F-A4 stay ~1x) and the picture is magenta (G low against R/B).
	// AWB MANUAL (0x3406 bit0 = 1) at unity gains (0x3400-05: R/G/B, 12 bit, 0x400 = 1x); our grey-world loop in the
	// PDD (AwbStep) sets them from the frame mean
#ifdef A64_CAMERA_GREYWORLD
	{0x3406, 0x01}, {0x3400, 0x04}, {0x3401, 0x00}, {0x3402, 0x04}, {0x3403, 0x00}, {0x3404, 0x04}, {0x3405, 0x00},
#endif
	// boot 925: the colour bars (injected after the pixel array) decode perfectly, yet the live picture is magenta with
	// green highlights, with the AWB gains at ~unity (R 0x400, G/B 0x412). Magenta live + correct bars = the demosaic sees the
	// Bayer pattern one column off: green sites read as red/blue. The array's readout phase depends on the mirror
	// setting. TIMING TC REG21 (0x3821): bit2 ISP mirror, bit1 sensor mirror -- both set, so the ISP's CFA phase follows
	// the sensor's (we had 0x00 = neither). Mainline ov5640.c also streams with both mirror bits set on this
	// sensor. Side effect: the picture is mirrored left-right against the previous image.
	// boot 927: 0x3821 = 0x06 left the cast unchanged (only mirrored the picture) -> back to no mirror; the phase
	// question is now tested by ISP window offsets inside one run (FrameDiag variants A-E)
	// 2026-10-18 research (camera-plan.md): BINNING on with the 2x sub-sampling, as the working driver -- pure skipping from
	// an odd column read the Bayer phase one column over (our R/G swap)
	{0x3820, 0x41}, {0x3821, 0x01},	// (0x07 = the reference baseline: mirrored + still R/G swapped, boot 1085)
	// boot 931: the image is nearly grey (mean |U-128|, |V-128| ~ 0x0b), so the "magenta" is a slight +U/+V bias on an
	// almost colourless picture. Saturation via the special digital effects block (SDE, enabled by 0x5001 bit7):
	// SDE CTRL0 (0x5580) bit1 = saturation enable; SDE CTRL3/CTRL4 (0x5583 / 0x5584) = U / V saturation, 0x40 = 1x
	// (the reset values read 0x40) -> 0x80 = 2x. Auto AWB (0x3406 = 0) and the window offsets stay as they were.
	// boot 933: 2x made the on-screen cast worse while the kernel buffer stays near grey -> back to 1x (SDE saturation off)
	{0x5580, 0x00}, {0x5583, 0x40}, {0x5584, 0x40},
	// boot 958 (hue right, washed out). Datasheet SDE (sec. 7.26): 0x5583/0x5584 are the U/V SATURATION only when 0x5580 bit1
	// = 1 AND 0x5588 bit6 = 1; with bit6 = 0 they are the max/min of a "UV adjust" (that is what the boot-933 "2x" really
	// did). Saturation 1.4x (0x40 = 1x -> 0x5A) with 0x5588 = 0x41 (bit6 + the reset bit0). Contrast: 0x5580 bit2 enables
	// it, Y gain 0x5586 (reset 0x20 = 1x) -> 0x24 = 1.125x, Y bright 0x5587 = 0. Gamma stays the reset curve (0x5000 bit5 on).
	{0x5588, 0x41}, {0x5583, 0x5A}, {0x5584, 0x5A}, {0x5586, 0x24}, {0x5587, 0x00}, {0x5580, 0x06},
	};

static TInt Ov5640Init()
	{
	for (TUint i = 0; i < sizeof(KOv5640Vga) / sizeof(KOv5640Vga[0]); ++i)
		if (CciWrite16(0x4c, KOv5640Vga[i][0], (TUint8)KOv5640Vga[i][1]) != KErrNone)
			return -(TInt)(0x10000 | KOv5640Vga[i][0]);
	return KErrNone;
	}

static void CsiTest()
	{
	// the CSI clocks: SCLK 300 MHz (CCU 0x134 [31] gate, [26:24] 0 = PLL_PERIPH0 600 MHz, [19:16] M = 1 -> /2) next to
	// the MCLK the probe set; DRAM gate CSI (0x100 bit 1); the CSI data pins PE0, PE2-PE11 = function 2
	R32(0x134) = (R32(0x134) & 0xFFFFu) | 0x80000000u | (1u << 16);
	R32(0x100) |= 1u << 1;
	TUint saved[12];
	for (TUint p = 0; p < 12; ++p) { saved[p] = PinFn(KPortE, p); if (p != 1) SetPinFn(KPortE, p, KFnCsi); }
	static TA64Csi csi;
	NKern::ThreadEnterCS();
	TInt r = csi.iBase ? KErrNone : csi.Init();
	NKern::ThreadLeaveCS();
	Log("[EKERN][cam] C3: csi init r=", (TUint32)r); Log(" buf phys=", csi.iBufPhys); ekseam_puts("\n");
	if (r == KErrNone)
		{
		// OV5640 alone again: PWDN off, reset off, move it to 0x4c, program VGA YUYV
		PinLow(KPortC, 0); Ms(2); PinFloat(KPortD, 3); Ms(25);
		CciWrite16(0x3c, 0x3100, 0x4c << 1);
		r = Ov5640Init();
		Log("[EKERN][cam] C3: ov5640 init r=", (TUint32)r); ekseam_puts("\n");
		csi.Setup(ETrue);
		CciWrite16(0x4c, 0x3008, 0x02);					// stream
		Ms(200);										// AEC settles a little; frames already flow
		// boot 899 diagnosis: is the sensor driving the bus at all? Read back its key registers, then sample PCLK
		// (PE0), HSYNC (PE2), VSYNC (PE3), D7 (PE11) as plain GPIO inputs and count edges; and read back the CSI
		// registers (all zero = the block is not clocked / out of reset)
		{
		static const TUint16 KRb[] = {0x3008, 0x3103, 0x300E, 0x3017, 0x3018, 0x3000, 0x3002, 0x3004, 0x3006, 0x4300, 0x4740, 0x3808, 0x3809};
		ekseam_puts("[EKERN][cam] C3: ov5640 rb");
		for (TUint i = 0; i < sizeof(KRb) / sizeof(KRb[0]); ++i)
			{ Log(" ", KRb[i]); Log("=", (TUint32)CciRead(0x4c, KRb[i], ETrue)); }
		ekseam_puts("\n");
		static const TUint KPins[] = {0, 2, 3, 11};
		ekseam_puts("[EKERN][cam] C3: pin edges (PE0 pclk, PE2 hs, PE3 vs, PE11 d7) over 200k reads:");
		for (TUint k = 0; k < 4; ++k)
			{
			SetPinFn(KPortE, KPins[k], KFnIn);
			TUint edges = 0, last = PinIn(KPortE, KPins[k]);
			for (TInt n = 0; n < 200000; ++n) { const TUint v = PinIn(KPortE, KPins[k]); edges += v != last; last = v; }
			SetPinFn(KPortE, KPins[k], KFnCsi);
			Log(" ", edges);
			}
		ekseam_puts("\n");
		// VSYNC over a long window (boot 901: 0 edges in ~200k reads = tens of ms, shorter than one frame at this slow
		// PLL): PE3 as GPIO for ~1.5 s by the tick clock, and the line rate on PE2 alongside
		{
		SetPinFn(KPortE, 3, KFnIn); SetPinFn(KPortE, 2, KFnIn);
		const TUint32 t0 = NKern::TickCount(), dt = (TUint32)NKern::TimerTicks(1500);
		TUint vs = 0, hs = 0, lv = PinIn(KPortE, 3), lh = PinIn(KPortE, 2), n = 0;
		while ((TUint32)(NKern::TickCount() - t0) < dt)
			{
			const TUint v = PinIn(KPortE, 3), h = PinIn(KPortE, 2);
			vs += v != lv; hs += h != lh; lv = v; lh = h; ++n;
			}
		SetPinFn(KPortE, 3, KFnCsi); SetPinFn(KPortE, 2, KFnCsi);
		Log("[EKERN][cam] C3: 1.5 s window vsync edges=", vs); Log(" hsync edges=", hs); Log(" reads=", n); ekseam_puts("\n");
		}
		Log("[EKERN][cam] C3: csi rb en=", csi.R(0x00)); Log(" if=", csi.R(0x04)); Log(" ch=", csi.R(0x44));
		Log(" hs=", csi.R(0x80)); Log(" vs=", csi.R(0x84)); Log(" buflen=", csi.R(0x88)); Log(" fifo0=", csi.R(0x50));
		Log(" inten=", csi.R(0x70)); Log(" ccu134=", R32(0x134)); Log(" gate64=", R32(0x064)); Log(" rst2c4=", R32(0x2C4));
		Log(" dram100=", R32(0x100)); Log(" pe_cfg0=", R32(KPio + KPortE * 0x24)); Log(" pe_cfg1=", R32(KPio + KPortE * 0x24 + 4));
		ekseam_puts("\n");
		}
		csi.Start(EFalse);								// single shot
		for (TInt i = 0; i < 50 && !csi.iFd; ++i) Ms(10);
		Log("[EKERN][cam] C3: single-shot irqs=", csi.iIrqs); Log(" fd=", csi.iFd); Log(" cd=", csi.iCd);
		Log(" vs=", csi.iVs); Log(" ovf=", csi.iOvf); Log(" sta=", csi.iLastSta);
		Log(" frmclk=", csi.R(0x90)); Log(" fifo=", csi.R(0x98)); Log(" pclk=", csi.R(0x9C)); ekseam_puts("\n");
		const TUint32* w = (const TUint32*)csi.iBuf;
		ekseam_puts("[EKERN][cam] C3: frame[0..31]=");
		for (TInt i = 0; i < 8; ++i) { Log(" ", w[i]); }
		ekseam_puts("\n[EKERN][cam] C3: mid-frame=");
		const TUint32* m = (const TUint32*)(csi.iBuf + KCsiBytes / 2 + KCsiW);
		for (TInt i = 0; i < 4; ++i) { Log(" ", m[i]); }
		ekseam_puts("\n");
		// continuous: one second of frame-done interrupts (no buffer flip yet: every frame lands in the same buffer)
		csi.Stop(); csi.Setup(ETrue);
		const TInt fd0 = csi.iFd;
		csi.Start(ETrue);
		// poll the raw status too (an interrupt routing problem would show bits here with irqs = 0)
		TUint32 orSta = 0, orChSta = 0;
		for (TInt i = 0; i < 30; ++i) { orSta |= csi.R(0x74); orChSta |= csi.R(0x6C); Ms(100); }
		Log("[EKERN][cam] C3: continuous polled int_sta|=", orSta); Log(" ch_sta|=", orChSta);
		Log(" frmclk=", csi.R(0x90)); Log(" acc=", csi.R(0x94)); Log(" fifo=", csi.R(0x98)); Log(" pclk=", csi.R(0x9C));
		ekseam_puts("\n");
		csi.Stop();
		Log("[EKERN][cam] C3: continuous 1 s fd=", (TUint32)(csi.iFd - fd0)); Log(" ovf=", csi.iOvf);
		Log(" irqs=", csi.iIrqs); ekseam_puts((csi.iFd - fd0) > 0 ? " PASS\n" : " FAIL\n");
		if (csi.iFd == fd0)
			{
			// still nothing: the CSI VREF polarity flipped once (settles the polarity question in one boot)
			csi.Setup(EFalse);
			const TInt fd1 = csi.iFd;
			csi.Start(ETrue);
			Ms(1500);
			Log("[EKERN][cam] C3: retry VREF positive fd=", (TUint32)(csi.iFd - fd1)); Log(" sta=", csi.iLastSta);
			Log(" frmclk=", csi.R(0x90)); Log(" pclk=", csi.R(0x9C)); ekseam_puts("\n");
			csi.Stop();
			}
		Log("[EKERN][cam] C3: frame[0..15] after=", ((const TUint32*)csi.iBuf)[0]); Log(" ", ((const TUint32*)csi.iBuf)[1]);
		Log(" ", ((const TUint32*)csi.iBuf)[2]); Log(" ", ((const TUint32*)csi.iBuf)[3]); ekseam_puts("\n");
		CciWrite16(0x4c, 0x3008, 0x42);					// sensor power down (software)
		PinFloat(KPortC, 0); PinLow(KPortD, 3);
		}
	for (TUint p = 0; p < 12; ++p) if (p != 1) SetPinFn(KPortE, p, saved[p]);
	R32(0x100) &= ~(1u << 1);
	}
#endif

// AXP803 (camera-linux.md section 3; Linux axp20x-regulator): ALDO1 0x28 (0.7-3.3 V, 100 mV) enable 0x13 bit 5,
// DLDO3 0x17 (0.7-3.3 V, 100 mV) enable 0x12 bit 5, ELDO3 0x1B (0.7-1.9 V, 50 mV) enable 0x12 bit 2
static void CameraProbe()
	{
	const TInt e12 = A64PmicReadReg(0x12), e13 = A64PmicReadReg(0x13);
	if (e12 < 0 || e13 < 0)
		{
		Log("[EKERN][cam] C2: no PMIC access r=", (TUint32)e12);
		ekseam_puts("\n");
		return;
		}
	Log("[EKERN][cam] C2: pmic 12=", (TUint32)e12); Log(" 13=", (TUint32)e13);
	Log(" pio_pow 340=", R32(KPio + 0x340));	// read only: the PIO bank-voltage register, a lead if the bus stays silent
	ekseam_puts("\n");
	A64PmicUpdateReg(0x28, 0x1F, 11);			// ALDO1 1.8 V (bank PE + DOVDD)
	A64PmicUpdateReg(0x17, 0x1F, 21);			// DLDO3 2.8 V (AVDD)
	A64PmicUpdateReg(0x1B, 0x1F, 22);			// ELDO3 1.8 V (OV5640 DVDD, GC2145 IOVDD)
	A64PmicUpdateReg(0x13, 0, 0x20);
	A64PmicUpdateReg(0x12, 0, 0x24);
	Ms(10);
	Log("[EKERN][cam] C2: rails on 12=", (TUint32)A64PmicReadReg(0x12)); Log(" 13=", (TUint32)A64PmicReadReg(0x13));
	Log(" v28=", (TUint32)A64PmicReadReg(0x28)); Log(" v17=", (TUint32)A64PmicReadReg(0x17)); Log(" v1b=", (TUint32)A64PmicReadReg(0x1B));
	ekseam_puts("\n");

	// save the pins we touch
	const TUint fPe1 = PinFn(KPortE, 1), fPe12 = PinFn(KPortE, 12), fPe13 = PinFn(KPortE, 13), fPe16 = PinFn(KPortE, 16),
		fPe17 = PinFn(KPortE, 17), fPd3 = PinFn(KPortD, 3), fPc0 = PinFn(KPortC, 0);
	// both sensors held: reset asserted, powerdown asserted (OV5640 PWDN PC0 active high = float; GC2145 enable PE17
	// active low = float)
	PinLow(KPortD, 3); PinFloat(KPortC, 0);
	PinLow(KPortE, 16); PinFloat(KPortE, 17);
	SdaHi(); SclHi();
	// MCLK: CCU 0x134 [15] gate, [10:8] = 0 osc24M, [4:0] M = 0 (/1); CSI bus gate (0x064 bit 8) + reset (0x2C4 bit 8)
	const TUint32 ccu134 = R32(0x134), ccu64 = R32(0x064), ccu2c4 = R32(0x2C4);
	R32(0x2C4) = ccu2c4 | (1u << 8);
	R32(0x064) = ccu64 | (1u << 8);
	R32(0x134) = (ccu134 & ~0xFFFFu) | 0x8000u;
	SetPinFn(KPortE, 1, KFnCsi);
	Ms(10);
	Log("[EKERN][cam] C2: mclk 134=", R32(0x134)); Log(" bus idle scl/sda=", (PinIn(KPortE, KScl) << 1) | PinIn(KPortE, KSda));
	ekseam_puts("\n");

	// OV5640: reset + PWDN asserted -> >= 5 ms -> PWDN off -> >= 1 ms -> reset off -> >= 20 ms -> SCCB
	Ms(6);
	PinLow(KPortC, 0);
	Ms(2);
	PinFloat(KPortD, 3);
	Ms(25);
	// the OV5640 wakes at its DEFAULT SCCB address 0x3c (the GC2145's too -- held in reset here): Linux's ov5640.c
	// (ov5640_set_slave_id, run at every power-up when the DT address differs) writes SCCB_ID 0x3100 = 0x4c << 1 at
	// 0x3c, and only then is the chip at the DT's 0x4c (megi's pp.dtsi reg = <0x4c>)
	const TInt d0 = CciRead(0x3c, 0x300A, ETrue);
	const TInt sw = CciWrite16(0x3c, 0x3100, 0x4c << 1);
	Log("[EKERN][cam] C2: OV5640 @3c (default) id-hi=", (TUint32)d0); Log(" set SCCB_ID 0x98 r=", (TUint32)sw);
	ekseam_puts("\n");
	const TInt idH = CciRead(0x4c, 0x300A, ETrue), idL = CciRead(0x4c, 0x300B, ETrue);
	Log("[EKERN][cam] C2: OV5640 @4c id=", (TUint32)idH); Log(" ", (TUint32)idL);
	ekseam_puts((idH == 0x56 && idL == 0x40) ? " PASS\n" : " FAIL\n");
	PinFloat(KPortC, 0); PinLow(KPortD, 3);		// back to powerdown + reset
	Ms(2);

	// GC2145: enable (PE17 low) -> 10 ms -> reset off -> 50 ms -> I2C
	PinLow(KPortE, 17);
	Ms(10);
	PinFloat(KPortE, 16);
	Ms(50);
	const TInt gH = CciRead(0x3c, 0xF0, EFalse), gL = CciRead(0x3c, 0xF1, EFalse);
	Log("[EKERN][cam] C2: GC2145 @3c id=", (TUint32)gH); Log(" ", (TUint32)gL);
	ekseam_puts((gH == 0x21 && gL == 0x45) ? " PASS\n" : " FAIL\n");
	PinFloat(KPortE, 17); PinLow(KPortE, 16);
	Ms(2);

#ifdef A64_CAMERA_C3_TEST
	CsiTest();
#endif
	// restore: MCLK + CSI clocks, the pins, the rails we turned on
	SetPinFn(KPortE, 1, fPe1);
	R32(0x134) = ccu134; R32(0x064) = ccu64; R32(0x2C4) = ccu2c4;
	SetPinFn(KPortE, 12, fPe12); SetPinFn(KPortE, 13, fPe13); SetPinFn(KPortE, 16, fPe16);
	SetPinFn(KPortE, 17, fPe17); SetPinFn(KPortD, 3, fPd3); SetPinFn(KPortC, 0, fPc0);
	A64PmicUpdateReg(0x12, 0x24 & ~e12, 0);
	A64PmicUpdateReg(0x13, 0x20 & ~e13, 0);
	Log("[EKERN][cam] C2: done, rails restored 12=", (TUint32)A64PmicReadReg(0x12)); Log(" 13=", (TUint32)A64PmicReadReg(0x13));
	ekseam_puts("\n");
	}
#endif

// ---------------------------------------------------------------------------------------------------------------
// The real sensor under the 2010 template PDD (camera-plan.md C3 "CameraSc.A64" rung): camerasc.cpp (the PDD,
// unmodified) creates a DTemplateSensorIf; on the phone that class is THIS one -- the OV5640 + the CSI -- while the
// fake sensor (camerasc_sensor.cpp, unmodified) is compiled as DTemplateFakeSensorIf (link_ekern.sh -D) and only
// supplies DSensorIf::SetConfig. One instance (one sensor), so the hardware state is file-static.
static TA64Csi TheCsi;
static TBool TheCamPowered;
// front/back (camera-plan.md): unit 0 = the rear OV5640, unit 1 = the front GC2145 (one at a time: they share the
// parallel bus). DA64CameraScPddFactory (below) records the unit being created; the sensor takes it in DoCreate.
static TInt TheCreatingUnit;
static TInt TheSensorUnit;
static TLinAddr TheCurLin;
static TPhysAddr TheCurPhys;
static TInt TheFrames;
static const SDevCamFrameSize KA64FrameSizes[] =
	{
	{ 320, 240, 1, 30 },		// QVGA (phecam's viewfinder size)
	{ 640, 480, 1, 30 },		// VGA
	};
static const SDevCamPixelFormat KA64PixelFormats[] =
	{
	{ EUidPixelFormatYUV_422Interleaved, 2, 2 },	// image
	{ EUidPixelFormatYUV_422Interleaved, 2, 2 },	// video
	{ EUidPixelFormatYUV_422Interleaved, 2, 2 },	// viewfinder
	};

static void FrameDfcFn(TAny* aPtr) { ((DTemplateSensorIf*)aPtr)->BufferDoneCallback(KErrNone); }

DTemplateSensorIf::DTemplateSensorIf(MSensorObserver& aObserver, TDfcQue* aDFCQueue)
	: iDFCQueue(aDFCQueue)
	{
	iObserver = &aObserver;
	}

TInt DTemplateSensorIf::DoCreate()
	{
	TheSensorUnit = TheCreatingUnit;
	iImageTimerDFCs[0] = new TDfc(FrameDfcFn, this, iDFCQueue, 1);
	if (!iImageTimerDFCs[0]) return KErrNoMemory;
	NKern::ThreadEnterCS();
	TInt r = TheCsi.iBase ? KErrNone : TheCsi.Init();
	NKern::ThreadLeaveCS();
	TheCsi.iFrameDfc = iImageTimerDFCs[0];
	Log("[EKERN][cam] PDD: OV5640 sensor + CSI0 r=", (TUint32)r); ekseam_puts("\n");
	return r;
	}

// Boot 920 hung hard (no more kernel log, guard reset) right where camtest closes the camera after frame 400. Close order
// in the LDD: Stop -> RelinquishPower (CSI module clock gated) -> delete sensor, and this destructor then wrote CSI
// registers with SCLK off (an AHB stall), and deleted a TDfc that could still be queued. Now: CSI registers are touched
// only while powered, the interrupt is disabled before the clock goes, and the DFC is cancelled before delete.
DTemplateSensorIf::~DTemplateSensorIf()
	{
	if (TheCamPowered) TheCsi.Stop();
	TheCsi.iFrameDfc = NULL;
	if (iImageTimerDFCs[0]) iImageTimerDFCs[0]->Cancel();
	delete iImageTimerDFCs[0];
	}

TInt DTemplateSensorIf::FrameSizeCaps(TDevCamCaptureMode, TUidPixelFormat, TDes8& aFrameSizeCapsBuf)
	{
	TPtrC8 src((const TUint8*)KA64FrameSizes, sizeof(KA64FrameSizes));
	if (aFrameSizeCapsBuf.MaxSize() < src.Size()) return KErrOverflow;
	aFrameSizeCapsBuf = src;
	return KErrNone;
	}

TInt DTemplateSensorIf::GetCaps(TCameraCapsV02*& aCameraCaps)
	{
	TInt r = sizeof(TCameraCapsV02) + sizeof(KA64PixelFormats);
	TUint8* buf = new TUint8[r];
	if (!buf) return KErrNoMemory;
	memclr(buf, r);
	aCameraCaps = (TCameraCapsV02*)buf;
	aCameraCaps->iFlashModes = ECamFlashNone;
	aCameraCaps->iExposureModes = ECamExposureAuto;
	aCameraCaps->iWhiteBalanceModes = ECamWBAuto;
	aCameraCaps->iOrientation = TheSensorUnit ? ECamOrientationInwards : ECamOrientationOutwards;	// unit 1 = the front camera
	aCameraCaps->iNumImagePixelFormats = 1;
	aCameraCaps->iNumVideoPixelFormats = 1;
	aCameraCaps->iNumViewFinderPixelFormats = 1;
	for (TInt i = 0; i < ECamAttributeMax; i++)
		{ aCameraCaps->iDynamicRange[i].iMin = 0; aCameraCaps->iDynamicRange[i].iMax = 6; aCameraCaps->iDynamicRange[i].iDefault = 3; }
	memcpy(buf + sizeof(TCameraCapsV02), KA64PixelFormats, sizeof(KA64PixelFormats));
	return r;
	}

// ---- the front GC2145 (unit 1), first light. From the GalaxyCore GC2145 CSP datasheet V1.0 (2013-12-01, public PDF;
// no Linux tables): 8-bit register addresses, page select in 0xFE [2:0]; P0:0x84 output format (reset 0x02 = Y Cb Y Cr =
// YUYV, as the OV5640 path); the output window crop P0:0x90-0x98 (0x90 bit0 crop enable, y1/x1/height/width) cuts
// 320x240 out of the 1600x1200 centre (a narrow view until a scaler/sub-sample set-up is derived); the sync outputs
// stay as reset (DT: VSYNC active HIGH -> the CSI's VREF polarity flips). Power-up per camera-linux.md section 3:
// enable (PE17 low) -> 10 ms -> reset released (PE16) -> 50 ms; the OV5640 stays in reset + powerdown.
static TInt CciWrite8(TUint8 aAddr, TUint8 aReg, TUint8 aVal)
	{
	Start();
	const TBool ok = WriteByte((TUint8)(aAddr << 1)) && WriteByte(aReg) && WriteByte(aVal);
	Stop();
	return ok ? KErrNone : -1;
	}
static TInt Put(char* aL, TInt aN, const char* aS);
static TInt PutHex(char* aL, TInt aN, TUint aV, TInt aDigits);
static void Gc2145ClkLine(const char* aStep)
	{
	char line[160];
	TInt n = Put(line, 0, "[EKERN][cam] PDD: gc2145 ");
	n = Put(line, n, aStep);
	n = Put(line, n, " f2/f7..fc");
	line[n++] = ' '; n = PutHex(line, n, (TUint)CciRead(0x3c, 0xF2, EFalse) & 0x1FF, 2);
	for (TUint r = 0xF7; r <= 0xFC; ++r) { line[n++] = ' '; n = PutHex(line, n, (TUint)CciRead(0x3c, r, EFalse) & 0x1FF, 2); }
	line[n++] = 0x0A; line[n] = 0;
	ekseam_puts(line);
	}
static TInt Gc2145Init(TInt aW, TInt aH)
	{
	// boot 962: 0xF8-0xFA read 00 (the datasheet's reset values are 0x81 / 0xFE / 0x11), 0xFC kept 0x06 (our 0xAE did not
	// stick), and no pin moved. So the clock block was never in its documented reset state. Sequence, each step read back:
	//  1. soft reset: 0xFE bit7 (+ bit6 cm_reset, bit4 CISCTL restart) = 0xD0, 10 ms, then page 0
	//  2. the clock block to the DATASHEET RESET VALUES: 0xF7 = 0x05 (pll_en + bit2; the boot-960 DVP-mode guess dropped),
	//     0xF8 = 0x81 (pll_dgdiv_en | 1), 0xF9 = 0xFE, 0xFA = 0x11, 0xFC = 0x06 (analog power as documented), 20 ms for the PLL
	//  3. pads out: 0xF2 = 0x0F (data + sync pads output)
	if (CciWrite8(0x3c, 0xFE, 0xD0) != KErrNone) return -0x100FE;
	Ms(10);
	CciWrite8(0x3c, 0xFE, 0x00);
	Gc2145ClkLine("after soft reset");
	// boot 976 step-back (structure of the Linux drivers read, no tables taken): they program blocks we never touched --
	// the analogue configuration (P0:0x12 SH delay, 0x17-0x34, undocumented values), the window/row/col start explicitly,
	// and the parallel-vs-MIPI selection. From the DATASHEET only: 0xF7 bit7 = "dvp mode" (the parallel output path; the
	// MIPI-only mainline driver leaves it 0) -> 0x85; the MIPI block off (P3:0x10 bit7 lane_enable = 0; P3:0x11 bit4
	// MIPI_enable = 0); P0:0x24 driver mode = maximum pad drive (data 20 mA, sync 16 mA, PCLK 20 mA = 0xFF): with the
	// default (weak) PCLK drive the pin showed ~900 edges/s while the data lines moved at 50-70k/s
	static const TUint8 clk[][2] = {{0xF7, 0x85}, {0xF8, 0x81}, {0xF9, 0xFE}, {0xFA, 0x11}, {0xFC, 0x06}};
	for (TUint i = 0; i < sizeof(clk) / sizeof(clk[0]); ++i)
		if (CciWrite8(0x3c, clk[i][0], clk[i][1]) != KErrNone)
			return -(TInt)(0x10000 | clk[i][0]);
	Ms(20);
	Gc2145ClkLine("after clocks");
	const TInt x1 = (1600 - aW) / 2, y1 = (1200 - aH) / 2;
	const TUint8 regs[][2] =
		{
		{0xFE, 0x03}, {0x10, 0x00}, {0x11, 0x2B},					// page 3: CSI2 lanes off; 0x11 = its reset 0x2B (bit4 MIPI_enable = 0)
		{0xFE, 0x00}, {0x24, 0xFF},								// page 0: maximum pad drive
		{0xF2, 0x0F},												// pads out
		{0xFE, 0x00},												// page 0
		{0x84, 0x02},												// Y Cb Y Cr
		{0x91, (TUint8)(y1 >> 8)}, {0x92, (TUint8)y1}, {0x93, (TUint8)(x1 >> 8)}, {0x94, (TUint8)x1},
		{0x95, (TUint8)(aH >> 8)}, {0x96, (TUint8)aH}, {0x97, (TUint8)(aW >> 8)}, {0x98, (TUint8)aW},
		{0x90, 0x01},												// crop on
		};
	for (TUint i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i)
		if (CciWrite8(0x3c, regs[i][0], regs[i][1]) != KErrNone)
			return -(TInt)(0x10000 | regs[i][0]);
	Gc2145ClkLine("after pads");
	// AEC + AWB on (boot 964: frames, but AEC/AWB off at reset): P0:0xB6 AEC_enable bit0; P0:0x82 block enable bit1 AWB,
	// bit2 ABS (auto black)
	CciWrite8(0x3c, 0xFE, 0x00);
	CciWrite8(0x3c, 0xB6, 0x01);
	// boot 967/968: the CSI measured 10-byte lines. Datasheet P0:0x86 "Sync mode" (reset 0x0F): bit3 opclk GATED in HB,
	// bit2 opclk polarity, bit1 HSYNC high valid, bit0 VSYNC high valid. HSYNC is already a high-valid HREF; with the
	// output clock gated outside the active line, the CSI's line logic sees only a few clocks per line. So bit3 = 0:
	// PCLK runs continuously (0x07)
	CciWrite8(0x3c, 0x86, 0x0F);	// boot 971: back to the reset 0x0F (gated clock): the free-running clock gave 16192-byte lines
	CciWrite8(0x3c, 0x82, 0x06);
	// boot 964: THE fix for "no lines": 0xFE bit4 = CISCTL_restart_n. Our page-0 selects (0xFE = 0x00) leave the readout
	// timing in restart; writing 0x10 releases it (it reads back 0x00 afterwards: a self-clearing pulse). Every register
	// change of the timing therefore ends with this pulse.
	CciWrite8(0x3c, 0xFE, 0x10);
	return KErrNone;
	}
static void Gc2145PowerUp()
	{
	// boot 974: D2-D6 toggle ~10x less than D0/D1/D7 = something clamps the shared bus. The OV5640 was held in RESET; now it is
	// in POWERDOWN only (PWDN released to its pull-up = asserted, active high) with RESET released, so its pads are in
	// the documented standby state rather than whatever reset leaves them in (camera-linux.md "shared bus rule": the
	// idle sensor in powerdown). The rails are shared and stay on for both.
	PinHigh(KPortC, 0);									// OV5640 powerdown first (driven high: no pull-up)
	Ms(2);
	PinFloat(KPortD, 3);									// ... then its reset released
	PinLow(KPortE, 16);										// GC2145 reset
	PinLow(KPortE, 17);										// GC2145 enable (PWDN low)
	Ms(10);
	PinFloat(KPortE, 16);									// reset released
	Ms(50);
	// the released lines as they read (a missing pull-up would leave the OV5640 PWDN low = ACTIVE on the shared bus)
	Log("[EKERN][cam] PDD: front power-up levels ov5640 pwdn(PC0)=", PinIn(KPortC, 0)); Log(" reset(PD3)=", PinIn(KPortD, 3));
	Log(" gc2145 en(PE17)=", PinIn(KPortE, 17)); Log(" rst(PE16)=", PinIn(KPortE, 16)); ekseam_puts("\n");
	}

// power: rails, MCLK + CSI clocks, the CSI pins, the OV5640 out of reset/powerdown at 0x4c with the VGA set-up
// (the GC2145 stays in reset: it shares the bus)
TInt DTemplateSensorIf::RequestPower()
	{
	if (TheCamPowered) return KErrNone;
	A64PmicUpdateReg(0x28, 0x1F, 11); A64PmicUpdateReg(0x17, 0x1F, 21); A64PmicUpdateReg(0x1B, 0x1F, 22);
	A64PmicUpdateReg(0x13, 0, 0x20); A64PmicUpdateReg(0x12, 0, 0x24);
	Ms(10);
	PinLow(KPortE, 16); PinHigh(KPortE, 17);				// GC2145 held: reset + enable driven high = disabled
	PinLow(KPortD, 3); PinHigh(KPortC, 0);					// OV5640 reset + powerdown (driven high)
	SdaHi(); SclHi();
	R32(0x2C4) |= 1u << 8; R32(0x064) |= 1u << 8; R32(0x100) |= 1u << 1;
	R32(0x134) = 0x80000000u | (1u << 16) | 0x8000u;		// SCLK 300 MHz, MCLK 24 MHz
	for (TUint p = 0; p < 12; ++p) SetPinFn(KPortE, p, KFnCsi);
	Ms(10);
	TInt r;
	if (TheSensorUnit == 1)
		{
		Gc2145PowerUp();
		const TInt id = (CciRead(0x3c, 0xF0, EFalse) << 8) | CciRead(0x3c, 0xF1, EFalse);
		r = id == 0x2145 ? KErrNone : KErrNotFound;
		Log("[EKERN][cam] PDD: power up GC2145 (front) id=", (TUint32)id); ekseam_puts("\n");
		}
	else
		{
		Ms(6); PinLow(KPortC, 0); Ms(2); PinFloat(KPortD, 3); Ms(25);
		CciWrite16(0x3c, 0x3100, 0x4c << 1);
		r = Ov5640Init();
		Log("[EKERN][cam] PDD: power up, ov5640 init r=", (TUint32)r); ekseam_puts("\n");
		}
	if (r != KErrNone) return KErrNotReady;
	Interrupt::Enable(KIntCsi);
	TheCamPowered = ETrue;
	return KErrNone;
	}

TInt DTemplateSensorIf::RelinquishPower()
	{
	if (!TheCamPowered) return KErrNone;
	TheCsi.Stop();
	Interrupt::Disable(KIntCsi);
	if (iImageTimerDFCs[0]) iImageTimerDFCs[0]->Cancel();
	if (TheSensorUnit == 1) { PinHigh(KPortE, 17); PinLow(KPortE, 16); }	// GC2145 disabled (driven high) + reset
	else CciWrite16(0x4c, 0x3008, 0x42);
	PinHigh(KPortC, 0); PinLow(KPortD, 3);		// OV5640 powerdown (driven high) + reset
	R32(0x134) = 0; R32(0x100) &= ~(1u << 1);
	A64PmicUpdateReg(0x12, 0x04, 0);						// ELDO3 off (DLDO3/ALDO1 were on before us: left)
	TheCamPowered = EFalse;
	Log("[EKERN][cam] PDD: power down", 0); ekseam_puts("\n");
	return KErrNone;
	}

// a new buffer for the CSI: the cache lines over it are dropped before the DMA writes (the LDD syncs after)
static void ArmBuffer(TLinAddr aLin, TPhysAddr aPhys, TInt aBytes)
	{
	Cache::SyncMemoryBeforeDmaRead(aLin, aBytes);
	TheCsi.SetAddr(aPhys);
	TheCurLin = aLin; TheCurPhys = aPhys;
	}

// boot 916 (uniform grey viewfinder): the frame the CSI just finished, as the CPU sees it, + the sensor's exposure
// state, at frames 1, 20 and 60
// boot 922 (magenta cast): the definitive byte-order / colour check = the OV5640 colour bars. Start() turns on the
// pre-ISP test pattern (0x503D PRE ISP TEST SETTING1: bit7 enable, [1:0] = 0 standard 8 colour bars); at frame 20 each
// bar's YUV (centre of the bar, middle line) and its BT.601 RGB are logged; at frame 25 the pattern goes off (live again).
// Expected bars (OmniVision standard order): white, yellow, cyan, green, magenta, red, blue, black.
static void BarDiag(TInt aW, TInt aH)
	{
	const TUint8* line = (const TUint8*)TheCurLin + (aH / 2) * aW * 2;
	ekseam_puts("[EKERN][cam] PDD: bars (bytes b0 b1 b2 b3 -> R G B with b1=U b3=V)\n");
	for (TInt i = 0; i < 8; ++i)
		{
		const TUint8* p = line + (((i * 2 + 1) * aW / 16) & ~1) * 2;
		const TInt y = p[0], u = p[1] - 128, v = p[3] - 128, c = 298 * (y - 16);
		TInt r = (c + 409 * v + 128) >> 8, g = (c - 100 * u - 208 * v + 128) >> 8, b = (c + 516 * u + 128) >> 8;
		r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
		Log("[EKERN][cam] PDD: bar ", (TUint32)i); Log(" bytes=", p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
		Log(" rgb=", (TUint32)((r << 16) | (g << 8) | b)); ekseam_puts("\n");
		}
	}

// one grep-able log line per call ("PDD: <tag> 5180=xx ..."; ekseam_puthex output would land on continuation lines)
static TInt Put(char* aL, TInt aN, const char* aS) { while (*aS) aL[aN++] = *aS++; return aN; }
static TInt PutHex(char* aL, TInt aN, TUint aV, TInt aDigits)
	{
	for (TInt s = (aDigits - 1) * 4; s >= 0; s -= 4) aL[aN++] = "0123456789abcdef"[(aV >> s) & 0xF];
	return aN;
	}
static void CregLine(const char* aTag, const TUint16* aRegs, TInt aCount)
	{
	char line[512];
	TInt n = Put(line, 0, "[EKERN][cam] PDD: ");
	n = Put(line, n, aTag);
	for (TInt i = 0; i < aCount && n < (TInt)sizeof(line) - 12; ++i)
		{
		const TInt v = CciRead(0x4c, aRegs[i], ETrue);
		line[n++] = ' ';
		n = PutHex(line, n, aRegs[i], 4);
		line[n++] = '=';
		n = v < 0 ? Put(line, n, "--") : PutHex(line, n, v, 2);
		}
	line[n++] = '\n'; line[n] = 0;
	ekseam_puts(line);
	}
static void CregRange(const char* aTag, TUint aFirst, TUint aLast)
	{
	TUint16 regs[48];
	TInt c = 0;
	for (TUint a = aFirst; a <= aLast && c < 48; ++a) regs[c++] = (TUint16)a;
	CregLine(aTag, regs, c);
	}
// the frame's mean Y / U / V over a sparse grid (packed YUYV)
// boot 929: the means are near neutral while the screen looks strongly magenta -> also the per-pixel spread:
//  satU/satV = mean |U-128| / |V-128| (how coloured the pixels are; a grey scene with per-pixel colour errors is high)
//  zipH = mean |U(x) - U(x+1 macropixel)| + |V..| (horizontal chroma alternation), zipV = the same one line down
//  (a demosaic-phase error shows as alternating chroma from pixel to pixel / line to line)
static TUint Abs(TInt a) { return a < 0 ? -a : a; }
static void ChromaLine(TInt aFrame, TInt aW, TInt aH, const char* aVariant)
	{
	Cache::SyncMemoryAfterDmaRead(TheCurLin, aW * 2 * aH);
	TUint sy = 0, su = 0, sv = 0, k = 0, au = 0, av = 0, zh = 0, zv = 0;
	const TInt stride = aW * 2;
	for (TInt y = 8; y + 1 < aH; y += 8)
		for (TInt x = 8; x + 3 < aW; x += 8)
			{
			const TUint8* p = (const TUint8*)TheCurLin + y * stride + (x & ~1) * 2;
			sy += p[0]; su += p[1]; sv += p[3]; ++k;
			au += Abs(p[1] - 128); av += Abs(p[3] - 128);
			zh += Abs(p[1] - p[5]) + Abs(p[3] - p[7]);
			zv += Abs(p[1] - p[stride + 1]) + Abs(p[3] - p[stride + 3]);
			}
	if (!k) k = 1;
	char line[220];
	TInt n = Put(line, 0, "[EKERN][cam] PDD: chroma ");
	n = Put(line, n, aVariant);
	n = Put(line, n, " frame=");
	n = PutHex(line, n, aFrame, 3);
	n = Put(line, n, " meanY="); n = PutHex(line, n, k ? sy / k : 0, 2);
	n = Put(line, n, " meanU="); n = PutHex(line, n, k ? su / k : 0, 2);
	n = Put(line, n, " meanV="); n = PutHex(line, n, k ? sv / k : 0, 2);
	n = Put(line, n, " satU="); n = PutHex(line, n, au / k, 2);
	n = Put(line, n, " satV="); n = PutHex(line, n, av / k, 2);
	n = Put(line, n, " zipH="); n = PutHex(line, n, zh / k, 2);
	n = Put(line, n, " zipV="); n = PutHex(line, n, zv / k, 2);
	line[n++] = '\n'; line[n] = 0;
	ekseam_puts(line);
	}

// OURS: a grey-world white balance (the sensor's auto AWB did not move, boot 940). Every 8th frame: the frame's mean
// Y/U/V over a grid (packed YUYV) -> mean R/G/B (BT.601, x1024 fixed point) -> the R and B gains move half way
// towards G/R and G/B (G stays 1x = 0x400), clamped to 0.25x..3.99x, written as the manual AWB gains (0x3400-05).
// Gains act before the ISP's colour pipeline, so a grey scene converges to U = V = 0x80.
static TInt TheGainR = 0x400, TheGainB = 0x400;
#ifdef A64_CAMERA_GREYWORLD
static void AwbStep(TInt aFrame, TInt aW, TInt aH)
	{
	if (aFrame < 30 || (aFrame & 7)) return;				// after the colour bars; every 8th frame
	Cache::SyncMemoryAfterDmaRead(TheCurLin, aW * 2 * aH);
	TInt sy = 0, su = 0, sv = 0, k = 0;
	for (TInt y = 4; y < aH; y += 8)
		for (TInt x = 4; x + 1 < aW; x += 8)
			{
			const TUint8* p = (const TUint8*)TheCurLin + y * aW * 2 + (x & ~1) * 2;
			if (p[0] < 16 || p[0] > 235) continue;			// clipped or black pixels say nothing about the light
			sy += p[0]; su += p[1] - 128; sv += p[3] - 128; ++k;
			}
	if (k < 64) return;
	const TInt Y = sy / k, U = su / k, V = sv / k;
	const TInt R = Y * 1024 + 1436 * V, G = Y * 1024 - 352 * U - 731 * V, B = Y * 1024 + 1815 * U;
	if (R <= 0 || G <= 0 || B <= 0) return;
	// new = old * (G / C), half way: old + (old * G / C - old) / 2
	TInt nr = (TInt)(((TInt64)TheGainR * G) / R), nb = (TInt)(((TInt64)TheGainB * G) / B);
	nr = TheGainR + (nr - TheGainR) / 2; nb = TheGainB + (nb - TheGainB) / 2;
	nr = nr < 0x100 ? 0x100 : nr > 0xFFF ? 0xFFF : nr;
	nb = nb < 0x100 ? 0x100 : nb > 0xFFF ? 0xFFF : nb;
	TheGainR = nr; TheGainB = nb;
	CciWrite16(0x4c, 0x3400, (TUint8)(nr >> 8)); CciWrite16(0x4c, 0x3401, (TUint8)nr);
	CciWrite16(0x4c, 0x3404, (TUint8)(nb >> 8)); CciWrite16(0x4c, 0x3405, (TUint8)nb);
	if (aFrame == 32 || aFrame == 64 || aFrame == 128 || aFrame == 256)
		{
		char line[160];
		TInt n = Put(line, 0, "[EKERN][cam] PDD: awb frame=");
		n = PutHex(line, n, aFrame, 3);
		n = Put(line, n, " meanY="); n = PutHex(line, n, Y, 2);
		n = Put(line, n, " meanU="); n = PutHex(line, n, U + 128, 2);
		n = Put(line, n, " meanV="); n = PutHex(line, n, V + 128, 2);
		n = Put(line, n, " gainR="); n = PutHex(line, n, nr, 3);
		n = Put(line, n, " gainB="); n = PutHex(line, n, nb, 3);
		line[n++] = '\n'; line[n] = 0;
		ekseam_puts(line);
		}
	}
#endif

// boot 941: the decisive CFA measurement. Frames 70..73: the sensor sends RAW Bayer (FORMAT CONTROL 00 0x4300 [7:4] = 0
// RAW, [3:0] = 0 the array order as is; FORMAT MUX 0x501F [2:0] = 3 = ISP RAW: after defect correction, before
// demosaic), 8 bit = 1 byte per pixel, so the CSI line becomes aW bytes (HSIZE / BUF_LEN). At frame 73 (two frames
// after the switch) the mean of each of the 4 CFA sites p00 p01 p10 p11 (row parity, column parity) is logged for the
// whole frame and for a centre box (half the width/height: the teal chair). Gr ~ Gb identifies the green pair; the
// chair (teal = G and B high, R low) names R and B. Frame 74 switches back to YUV422 (the viewfinder shows raw noise
// for these few frames).
#ifdef A64_CAMERA_RAWPROBE
static void RawProbePass(TInt aFrame, TInt aW, TInt aH, TBool aSecond);
static void RawProbe(TInt aFrame, TInt aW, TInt aH)
	{
	// second pass (boot 946: the ISP offsets did not move the raw phase): the ARRAY X start one column over (0x3801 0 -> 1)
	// for frames 80..85, raw again at 80..84 and logged at 83 ("PDD: raw x+1 ..."), so the next boot shows whether the
	// array start parity is the CFA lever
	if (aFrame == 80) CciWrite16(0x4c, 0x3801, 0x01);
	if (aFrame == 85) CciWrite16(0x4c, 0x3801, 0x00);
	if (aFrame >= 80 && aFrame <= 84) { RawProbePass(aFrame - 10, aW, aH, ETrue); return; }
	RawProbePass(aFrame, aW, aH, EFalse);
	}

static void RawProbePass(TInt aFrame, TInt aW, TInt aH, TBool aSecond)
	{
	if (aFrame == 70)
		{
		CciWrite16(0x4c, 0x4300, 0x00); CciWrite16(0x4c, 0x501F, 0x03);
		TheCsi.R(0x80) = ((TUint32)aW << 16); TheCsi.R(0x88) = aW;
		}
	if (aFrame == 73)
		{
		Cache::SyncMemoryAfterDmaRead(TheCurLin, aW * 2 * aH);
		const TUint8* f = (const TUint8*)TheCurLin;
		TUint s[2][4] = {{0}}, c[2][4] = {{0}};
		for (TInt y = 0; y < aH; ++y)
			for (TInt x = 0; x < aW; ++x)
				{
				const TInt site = ((y & 1) << 1) | (x & 1);
				const TUint v = f[y * aW + x];
				s[0][site] += v; ++c[0][site];
				if (x >= aW / 4 && x < 3 * aW / 4 && y >= aH / 4 && y < 3 * aH / 4) { s[1][site] += v; ++c[1][site]; }
				}
		for (TInt a = 0; a < 2; ++a)
			{
			char line[160];
			TInt n = Put(line, 0, aSecond ? (a ? "[EKERN][cam] PDD: raw x+1 centre" : "[EKERN][cam] PDD: raw x+1 frame ") : (a ? "[EKERN][cam] PDD: raw centre" : "[EKERN][cam] PDD: raw frame "));
			static const char* const KSite[4] = {" p00=", " p01=", " p10=", " p11="};
			for (TInt i = 0; i < 4; ++i) { n = Put(line, n, KSite[i]); n = PutHex(line, n, c[a][i] ? s[a][i] / c[a][i] : 0, 2); }
			line[n++] = '\n'; line[n] = 0;
			ekseam_puts(line);
			}
		}
	if (aFrame == 74)
		{
		CciWrite16(0x4c, 0x4300, 0x30); CciWrite16(0x4c, 0x501F, 0x00);
		TheCsi.R(0x80) = ((TUint32)(aW * 2) << 16); TheCsi.R(0x88) = aW * 2;
		}
	}
#endif

// boot 949: no window/offset/start change ever moved the raw site pattern, and grey-world WB neutralises the mean but the
// hue stays rotated -> is it the 2x SUB-SAMPLING (0x3814/0x3815 = 0x31: odd/even increments 3/1)? Frames 90..149: a
// centre crop read WITHOUT sub-sampling (increments 1/1 = 0x11, binning bits 0x3820/21 bit0 already off), sized so
// the ISP input stays 640x480 (window 672x492 = 640x480 + 2 x the ISP offsets 16/6) and the scaler still gives the
// output size. Chroma logged at frame 85 ("S") = the normal mode and at 140 ("N") = no sub-sampling; the screen
// shows the crop (a 2x zoom) for ~3 s. Frame 150 restores the full-array 2x sub-sampled window.
static void SetWindow(TUint aX0, TUint aY0, TUint aX1, TUint aY1, TUint8 aInc)
	{
	CciWrite16(0x4c, 0x3212, 0x00);								// group hold (group 0): the window lands at one frame start
	CciWrite16(0x4c, 0x3800, (TUint8)(aX0 >> 8)); CciWrite16(0x4c, 0x3801, (TUint8)aX0);
	CciWrite16(0x4c, 0x3802, (TUint8)(aY0 >> 8)); CciWrite16(0x4c, 0x3803, (TUint8)aY0);
	CciWrite16(0x4c, 0x3804, (TUint8)(aX1 >> 8)); CciWrite16(0x4c, 0x3805, (TUint8)aX1);
	CciWrite16(0x4c, 0x3806, (TUint8)(aY1 >> 8)); CciWrite16(0x4c, 0x3807, (TUint8)aY1);
	CciWrite16(0x4c, 0x3814, aInc); CciWrite16(0x4c, 0x3815, aInc);
	CciWrite16(0x4c, 0x3212, 0x10);								// end group 0
	CciWrite16(0x4c, 0x3212, 0xA0);								// launch group 0
	}
static void NoSubsampleTest(TInt aFrame, TInt aW, TInt aH) __attribute__((unused));
static void NoSubsampleTest(TInt aFrame, TInt aW, TInt aH)
	{
	if (aFrame == 85) ChromaLine(aFrame, aW, aH, "S");
	if (aFrame == 90) SetWindow(976, 728, 976 + 672 - 1, 728 + 492 - 1, 0x11);
	if (aFrame == 140) ChromaLine(aFrame, aW, aH, "N");
	if (aFrame == 150) SetWindow(0, 4, 0x0A3F, 0x079B, 0x31);
	}

// boot 953 (no register change ever moves the hue): is OUR write path writing what we think? At frame 2 every register
// of KOv5640Vga is read back and compared; mismatches are logged as "addr:w<table>/r<read>" (r-- = read NACK), several
// lines of up to 12. Expected differences: 0x3008 (stream 0x02 written later), 0x3808-0x380B (Start writes the
// configured size), 0x503D (bars). Anything else = a lost or misdirected write.
static void ReadbackDiff()
	{
	const TInt count = sizeof(KOv5640Vga) / sizeof(KOv5640Vga[0]);
	TInt mism = 0;
	char line[400];
	TInt n = Put(line, 0, "[EKERN][cam] PDD: rbdiff");
	TInt inLine = 0;
	for (TInt i = 0; i < count; ++i)
		{
		const TUint a = KOv5640Vga[i][0], w = KOv5640Vga[i][1];
		TBool later = EFalse;							// a later table entry for the same register wins
		for (TInt j = i + 1; j < count; ++j) if (KOv5640Vga[j][0] == a) later = ETrue;
		if (later) continue;
		const TInt r = CciRead(0x4c, a, ETrue);
		if (r == (TInt)w) continue;
		++mism;
		line[n++] = ' ';
		n = PutHex(line, n, a, 4); n = Put(line, n, ":w"); n = PutHex(line, n, w, 2);
		if (r < 0) n = Put(line, n, "/r--");
		else { n = Put(line, n, "/r"); n = PutHex(line, n, r, 2); }
		if (++inLine == 12)
			{
			line[n++] = '\n'; line[n] = 0; ekseam_puts(line);
			n = Put(line, 0, "[EKERN][cam] PDD: rbdiff"); inLine = 0;
			}
		}
	n = Put(line, n, " | total="); n = PutHex(line, n, count, 2); n = Put(line, n, " mismatches="); n = PutHex(line, n, mism, 2);
	line[n++] = '\n'; line[n] = 0;
	ekseam_puts(line);
	}

// the two remaining readout-phase candidates, one segment each (judge the screen; each switch is logged when made):
//  frames 1..119   as set up (X start 1)
//  frames 120..239 Y start 4 -> 5 (0x3803; end 0x079B -> 0x079C): the line phase of the 2x sub-sampled rows
//  frames 240..359 Y back, X increments odd/even swapped 0x31 -> 0x13 (0x3814)
static void PhaseVariants(TInt aFrame) __attribute__((unused));
static void PhaseVariants(TInt aFrame)
	{
	if (aFrame == 120) { CciWrite16(0x4c, 0x3803, 0x05); CciWrite16(0x4c, 0x3807, 0x9C); ekseam_puts("[EKERN][cam] PDD: variant Y5 (0x3803=5) from frame 120\n"); }
	if (aFrame == 240)
		{
		CciWrite16(0x4c, 0x3803, 0x04); CciWrite16(0x4c, 0x3807, 0x9B); CciWrite16(0x4c, 0x3814, 0x13);
		ekseam_puts("[EKERN][cam] PDD: variant XINC13 (0x3814=0x13) from frame 240\n");
		}
	if (aFrame == 360) { CciWrite16(0x4c, 0x3814, 0x31); ekseam_puts("[EKERN][cam] PDD: variants off from frame 360\n"); }
	}

// the matrix the sensor is running, read back and decoded under BOTH sign-table readings:
//  A = bit n -> CMX n+1 (bits 0..7 = CMX1..8), B = bit n -> CMXn with bit 0 -> CMX8; CMX9 sign = 0x538A bit0 in both.
// Signed decimal-ish hex: "-2b" = negative 0x2B. Rows Y | U | V.
static void CmxLine()
	{
	TInt v[9];
	for (TInt i = 0; i < 9; ++i) v[i] = CciRead(0x4c, 0x5381 + i, ETrue);
	const TInt s8a = CciRead(0x4c, 0x538A, ETrue), s8b = CciRead(0x4c, 0x538B, ETrue);
	for (TInt reading = 0; reading < 2; ++reading)
		{
		char line[200];
		TInt n = Put(line, 0, reading ? "[EKERN][cam] PDD: cmx B" : "[EKERN][cam] PDD: cmx A");
		for (TInt i = 0; i < 9; ++i)
			{
			TBool neg;
			if (i == 8) neg = s8a & 1;
			else if (reading == 0) neg = (s8b >> i) & 1;							// CMX(i+1) at bit i
			else neg = (i == 7) ? (s8b & 1) : ((s8b >> (i + 1)) & 1);			// CMX(i+1) at bit i+1, CMX8 at bit 0
			if (i == 3 || i == 6) n = Put(line, n, " |");
			n = Put(line, n, neg ? " -" : " +");
			n = PutHex(line, n, v[i] < 0 ? 0 : v[i], 2);
			}
		n = Put(line, n, " (538b="); n = PutHex(line, n, s8b, 2); n = Put(line, n, ")");
		line[n++] = '\n'; line[n] = 0;
		ekseam_puts(line);
		}
	}

// boot 958 row stripes: fixed-pattern or temporal? At frames 40 and 41 the per-row mean Y (every 4th pixel) is compared
// with the mean of its two neighbour rows; the 8 rows with the largest |deviation| are logged as row:dev (signed). The
// same rows in both frames = fixed pattern (sensor/BLC/readout); different rows = temporal (exposure flicker, a tear
// in the buffer hand-off, CSI timing).
static void StripeLine(TInt aFrame, TInt aW, TInt aH)
	{
	Cache::SyncMemoryAfterDmaRead(TheCurLin, aW * 2 * aH);
	static TInt rowMean[480];
	const TInt rows = aH < 480 ? aH : 480;
	for (TInt y = 0; y < rows; ++y)
		{
		const TUint8* p = (const TUint8*)TheCurLin + y * aW * 2;
		TInt s = 0, k = 0;
		for (TInt x = 0; x < aW; x += 4) { s += p[x * 2]; ++k; }
		rowMean[y] = k ? (s * 16) / k : 0;					// x16 fixed point
		}
	TInt bestRow[8], bestDev[8];
	for (TInt i = 0; i < 8; ++i) { bestRow[i] = -1; bestDev[i] = 0; }
	for (TInt y = 1; y + 1 < rows; ++y)
		{
		const TInt dev = rowMean[y] - (rowMean[y - 1] + rowMean[y + 1]) / 2;
		const TInt a = dev < 0 ? -dev : dev;
		TInt slot = -1;
		for (TInt i = 0; i < 8; ++i) if (bestRow[i] < 0 || a > (bestDev[i] < 0 ? -bestDev[i] : bestDev[i])) { slot = i; break; }
		if (slot < 0) continue;
		for (TInt i = 7; i > slot; --i) { bestRow[i] = bestRow[i - 1]; bestDev[i] = bestDev[i - 1]; }
		bestRow[slot] = y; bestDev[slot] = dev;
		}
	char line[200];
	TInt n = Put(line, 0, "[EKERN][cam] PDD: stripes frame=");
	n = PutHex(line, n, aFrame, 3);
	n = Put(line, n, " (row:dev x16)");
	for (TInt i = 0; i < 8 && bestRow[i] >= 0; ++i)
		{
		line[n++] = ' ';
		n = PutHex(line, n, bestRow[i], 3); line[n++] = ':';
		if (bestDev[i] < 0) { line[n++] = '-'; n = PutHex(line, n, -bestDev[i], 3); } else n = PutHex(line, n, bestDev[i], 3);
		}
	line[n++] = '\n'; line[n] = 0;
	ekseam_puts(line);
	}

static void FrameDiag(TInt aFrame, TInt aW, TInt aH)
	{
	if (TheSensorUnit == 1)
		{
		// boot 965: the front view is solid (0,128,0) = all-zero YUYV bytes. At frames 5 and 20: the buffer as the CPU
		// sees it (first/mid/last words, luma min/max over a grid, OR/AND of all sampled bytes = stuck data lines) and
		// the GC2145's exposure state (P0:0x03/0x04 exposure, 0xB0 global gain, 0xB6 AEC enable, 0x84 format, 0x82 blocks)
#ifdef A64_CAMERA_DIAG
		if (aFrame == 5 || aFrame == 20)
			{
			Cache::SyncMemoryAfterDmaRead(TheCurLin, aW * 2 * aH);
			const TUint8* b = (const TUint8*)TheCurLin;
			const TInt bytes = aW * 2 * aH;
			TUint mn = 255, mx = 0, orb = 0, andb = 0xFF;
			for (TInt i = 0; i < bytes; i += 37) { const TUint v = b[i]; orb |= v; andb &= v; if (!(i & 1)) { if (v < mn) mn = v; if (v > mx) mx = v; } }
			const TUint32* w = (const TUint32*)b;
			char line[260];
			TInt n = Put(line, 0, "[EKERN][cam] PDD: front buf frame=");
			n = PutHex(line, n, aFrame, 3);
			n = Put(line, n, " phys="); n = PutHex(line, n, TheCurPhys, 8);
			n = Put(line, n, " fifo0="); n = PutHex(line, n, TheCsi.R(0x50), 8);
			n = Put(line, n, " first="); n = PutHex(line, n, w[0], 8); line[n++] = ' '; n = PutHex(line, n, w[1], 8);
			n = Put(line, n, " mid="); n = PutHex(line, n, w[bytes / 8], 8);
			n = Put(line, n, " last="); n = PutHex(line, n, w[bytes / 4 - 1], 8);
			n = Put(line, n, " Ymin="); n = PutHex(line, n, mn, 2); n = Put(line, n, " Ymax="); n = PutHex(line, n, mx, 2);
			n = Put(line, n, " or="); n = PutHex(line, n, orb, 2); n = Put(line, n, " and="); n = PutHex(line, n, andb, 2);
			n = Put(line, n, " if="); n = PutHex(line, n, TheCsi.R(0x04), 8);
			n = Put(line, n, " linestat="); n = PutHex(line, n, TheCsi.R(0x9C), 8);	// PCLK_STAT: max/min line length (bytes)
			n = Put(line, n, " fifostat="); n = PutHex(line, n, TheCsi.R(0x98), 8);
			line[n++] = 0x0A; line[n] = 0;
			ekseam_puts(line);
			static const TUint8 KRegs[] = {0x03, 0x04, 0xB0, 0xB6, 0x84, 0x82, 0x90, 0x99, 0xF2};
			n = Put(line, 0, "[EKERN][cam] PDD: front regs");
			for (TUint i = 0; i < sizeof(KRegs); ++i)
				{ line[n++] = ' '; n = PutHex(line, n, KRegs[i], 2); line[n++] = '='; n = PutHex(line, n, (TUint)CciRead(0x3c, KRegs[i], EFalse) & 0x1FF, 2); }
			line[n++] = 0x0A; line[n] = 0;
			ekseam_puts(line);
			}
#endif
		return;
		}
	if (aFrame == 40 || aFrame == 41) StripeLine(aFrame, aW, aH);
#ifdef A64_CAMERA_BARS_HOLD
	if (aFrame == 300) CciWrite16(0x4c, 0x503D, 0x00);	// PH_CAM_BARS=1 build: bars ~16 s, for an on-screen check of the display path
#else
	if (aFrame == 25) CciWrite16(0x4c, 0x503D, 0x00);		// colour bars off: the live picture again
#endif
#ifndef A64_CAMERA_DIAG
	// the default build: no probes (boot 958 clean-up); A64DEFS=-DA64_CAMERA_DIAG brings back the bars, read-back diff,
	// CMX/creg/awb/aec dumps, chroma and buffer words
	(void)&BarDiag; (void)&CmxLine; (void)&ReadbackDiff; (void)&ChromaLine; (void)&CregRange;
	return;
#endif
	if (aFrame == 2) ReadbackDiff();
	if (aFrame == 3) CmxLine();
	if (aFrame == 20) { Cache::SyncMemoryAfterDmaRead(TheCurLin, aW * 2 * aH); BarDiag(aW, aH); }
	if (aFrame == 60)
		{
		static const TUint16 KRb2[] = {0x3820, 0x3821, 0x3406, 0x3400, 0x3401, 0x3402, 0x3403, 0x3404, 0x3405, 0x5183,
			0x5381, 0x5382, 0x5383, 0x5384, 0x5385, 0x5386, 0x5387, 0x5388, 0x5389, 0x538A, 0x538B, 0x5580, 0x5583, 0x5584,
			0x5000, 0x5001, 0x3811, 0x3813, 0x4300, 0x501F, 0x3803, 0x3814, 0x3815, 0x3820, 0x3821};
		CregLine("creg", KRb2, sizeof(KRb2) / sizeof(KRb2[0]));
		CregRange("awb", 0x5180, 0x51A7);					// AWB control block + its status read-outs
		CregRange("aec", 0x3A0F, 0x3A1F);					// AEC targets / limits
		}
	// boots 927-931 (Bayer-phase variants A-E): zip low and equal in every variant = no CFA phase error; the picture was
	// just desaturated (satU/satV ~0x0b of 128). The variant cycling is gone; one chroma line at frame 60 stays.
	if (aFrame == 60) ChromaLine(aFrame, aW, aH, "L");
#ifdef A64_CAMERA_RAWPROBE
	RawProbe(aFrame, aW, aH);					// boots 943-948 (garbles a few frames: off by default)
#endif
	// NoSubsampleTest: boot 951 produced black frames (the crop window was not valid as written): not called
	if (aFrame != 1 && aFrame != 20 && aFrame != 60) return;
	const TInt bytes = aW * 2 * aH;
	Cache::SyncMemoryAfterDmaRead(TheCurLin, bytes);
	const TUint32* w = (const TUint32*)TheCurLin;
	Log("[EKERN][cam] PDD: frame ", (TUint32)aFrame); Log(" phys=", TheCurPhys); Log(" fifo0=", TheCsi.R(0x50));
	Log(" first", w[0]); Log(" ", w[1]); Log(" ", w[2]); Log(" ", w[3]);
	const TUint32* m = (const TUint32*)(TheCurLin + (aH / 2) * aW * 2 + aW);
	Log(" mid", m[0]); Log(" ", m[1]); Log(" ", m[2]); Log(" ", m[3]);
	const TUint32* l = (const TUint32*)(TheCurLin + bytes - 16);
	Log(" last", l[0]); Log(" ", l[3]);
	TUint32 mn = 0xFFFFFFFFu, mx = 0;
	for (TInt i = 0; i < bytes / 4; i += 97) { const TUint32 v = w[i] & 0xFF; if (v < mn) mn = v; if (v > mx) mx = v; }
	Log(" Y min=", mn); Log(" max=", mx); ekseam_puts("\n");
	if (aFrame == 20)
		{
		static const TUint16 KRb[] = {0x503D, 0x3503, 0x3500, 0x3501, 0x3502, 0x350A, 0x350B, 0x56A1, 0x5000, 0x5001, 0x4300, 0x3008};
		ekseam_puts("[EKERN][cam] PDD: ov5640");
		for (TUint i = 0; i < sizeof(KRb) / sizeof(KRb[0]); ++i) { Log(" ", KRb[i]); Log("=", (TUint32)CciRead(0x4c, KRb[i], ETrue)); }
		ekseam_puts("\n");
		}
	}

TInt DTemplateSensorIf::Start(TDevCamCaptureMode, TLinAddr aLinAddr, TPhysAddr aPhysAddr)
	{
	if (!TheCamPowered && RequestPower() != KErrNone) return KErrNotReady;
	if (TheSensorUnit == 1)
		{
		const TInt r = Gc2145Init(iWidth, iHeight);
		Log("[EKERN][cam] PDD: GC2145 init r=", (TUint32)r); ekseam_puts("\n");
#ifdef A64_CAMERA_DIAG		// front pad/pin diagnostics (boots 969-977)
		// boot 969: lines of 0x3f40 bytes, only zeros captured. The datasheet's pad registers: 0xF2 [4] auto VB pad hi-Z,
		// [3] data pads output, [2:0] sync pads output (0x0F = all out, auto hi-Z off); 0xF3 [0] I2C open; 0xF6 [5:4]
		// pull up/down. Read back, then EVERY CSI pin as GPIO for 300 ms: edges per pin PE0 (PCLK), PE2 (HS), PE3 (VS),
		// PE4..PE11 (D0..D7) and its level at the end, to see which lines this sensor really drives
		{
		char line[300];
		TInt n = Put(line, 0, "[EKERN][cam] PDD: gc2145 pads f0..f3,f6");
		for (TUint a = 0xF0; a <= 0xF3; ++a) { line[n++] = ' '; n = PutHex(line, n, (TUint)CciRead(0x3c, a, EFalse) & 0x1FF, 2); }
		line[n++] = ' '; n = PutHex(line, n, (TUint)CciRead(0x3c, 0xF6, EFalse) & 0x1FF, 2);
		line[n++] = 0x0A; line[n] = 0;
		ekseam_puts(line);
		TUint edges[12], last[12];
		for (TInt p = 0; p < 12; ++p) { edges[p] = 0; if (p != 1) { SetPinFn(KPortE, p, KFnIn); } last[p] = PinIn(KPortE, p); }
		const TUint32 t0 = NKern::TickCount(), dt = (TUint32)NKern::TimerTicks(300);
		while ((TUint32)(NKern::TickCount() - t0) < dt)
			for (TInt p = 0; p < 12; ++p) { if (p == 1) continue; const TUint v = PinIn(KPortE, p); edges[p] += v != last[p]; last[p] = v; }
		for (TInt p = 0; p < 12; ++p) if (p != 1) SetPinFn(KPortE, p, KFnCsi);
		n = Put(line, 0, "[EKERN][cam] PDD: gc2145 pins (edges/level)");
		static const char* const KName[12] = {" pclk=", "", " hs=", " vs=", " d0=", " d1=", " d2=", " d3=", " d4=", " d5=", " d6=", " d7="};
		for (TInt p = 0; p < 12; ++p)
			{
			if (p == 1) continue;
			n = Put(line, n, KName[p]); n = PutHex(line, n, edges[p], 5); line[n++] = '/'; line[n++] = last[p] ? '1' : '0';
			}
		line[n++] = 0x0A; line[n] = 0;
		ekseam_puts(line);
		}
#endif
		TheFrames = 0;
		TheCsi.Configure(aPhysAddr, iWidth, iHeight, EFalse);		// GC2145: VSYNC active high
		// boot 966: every buffer byte 0x03 (D0/D1 high, the rest low) although D7 toggles on the pin: sampled on the wrong
		// PCLK edge -> CLK_POL falling for the front (IF_CFG bit16 = 0; the OV5640 path keeps rising)
		TheCsi.R(0x04) &= ~(1u << 16);
		// (boot 968: HREF inverted on the CSI = no frames at all -> reverted; the 10-byte lines are fixed on the sensor side,
		// P0:0x86 in Gc2145Init)
		ArmBuffer(aLinAddr, aPhysAddr, iWidth * 2 * iHeight);
		iPendingRequests = 1;
		iNextRequest = 0;
		iEnabled = ETrue;
		TheCsi.Start(ETrue);
		Log("[EKERN][cam] PDD: start front ", (TUint32)((iWidth << 16) | iHeight)); ekseam_puts("\n");
#ifdef A64_CAMERA_DIAG		// the front-camera CSI framing matrix (boots 973-977), diagnostics only
		// boot 971: every CSI pin toggles (wiring fine) -> the CSI's framing. Matrix in ONE start, 1 s each (this DFC
		// blocks, so the CSI keeps rewriting the armed buffer): PCLK rising/falling x HREF normal/inverted; per combo the
		// frame-done count, PCLK_STAT (line length) and the OR/AND/min/max of the armed buffer's bytes. The combo whose
		// line length is nearest 2 x width stays. The IF_CFG low bits (8-bit, YUV/raw interface, progressive) are the rear's.
		{
		// boot 973: HREF normal right, both PCLK edges give frames, but every line is 10 bytes. The window registers as
		// they read (P0:0x05-0x10 blanking/row/col start/window height/width, 0x90-0x9A crop/sub-sample), then a second
		// matrix: the GC2145 output clock gated (0x86 = 0x0F) or free (0x07) x CSI HREF normal/inverted, PCLK falling
		{
		char wl[300];
		TInt wn = Put(wl, 0, "[EKERN][cam] PDD: gc2145 win 05..10");
		for (TUint a = 0x05; a <= 0x10; ++a) { wl[wn++] = ' '; wn = PutHex(wl, wn, (TUint)CciRead(0x3c, a, EFalse) & 0x1FF, 2); }
		wn = Put(wl, wn, " | 90..9a");
		for (TUint a = 0x90; a <= 0x9A; ++a) { wl[wn++] = ' '; wn = PutHex(wl, wn, (TUint)CciRead(0x3c, a, EFalse) & 0x1FF, 2); }
		wl[wn++] = 0x0A; wl[wn] = 0;
		ekseam_puts(wl);
		}
		static const TUint32 KCombo[4] = {0, 1u << 17, 0, 1u << 17};
		static const TUint8 KSync[4] = {0x0F, 0x0F, 0x07, 0x07};
		static const char* const KName[4] = {"gated/norm", "gated/inv ", "free/norm ", "free/inv  "};
		const TUint32 base = TheCsi.R(0x04) & ~((1u << 16) | (1u << 17));
		TInt best = 0, bestErr = 0x7FFFFFFF;
		for (TInt c = 0; c < 4; ++c)
			{
			TheCsi.R(0x04) = base | KCombo[c];
			CciWrite8(0x3c, 0x86, KSync[c]);
			const TInt fd0 = TheCsi.iFd;
			Ms(1000);
			const TUint32 ls = TheCsi.R(0x9C);
			Cache::SyncMemoryAfterDmaRead(aLinAddr, iWidth * 2 * iHeight);
			const TUint8* b = (const TUint8*)aLinAddr;
			TUint orb = 0, andb = 0xFF, mn = 255, mx = 0;
			for (TInt i = 0; i < iWidth * 2 * iHeight; i += 41) { const TUint v = b[i]; orb |= v; andb &= v; if (v < mn) mn = v; if (v > mx) mx = v; }
			char line[200];
			TInt n = Put(line, 0, "[EKERN][cam] PDD: front csi ");
			n = Put(line, n, KName[c]);
			n = Put(line, n, " if="); n = PutHex(line, n, TheCsi.R(0x04), 8);
			n = Put(line, n, " fd="); n = PutHex(line, n, TheCsi.iFd - fd0, 3);
			n = Put(line, n, " linestat="); n = PutHex(line, n, ls, 8);
			n = Put(line, n, " or="); n = PutHex(line, n, orb, 2); n = Put(line, n, " and="); n = PutHex(line, n, andb, 2);
			n = Put(line, n, " min="); n = PutHex(line, n, mn, 2); n = Put(line, n, " max="); n = PutHex(line, n, mx, 2);
			line[n++] = 0x0A; line[n] = 0;
			ekseam_puts(line);
			const TInt len = (TInt)(ls & 0xFFFF), err = len > 2 * iWidth ? len - 2 * iWidth : 2 * iWidth - len;
			if (TheCsi.iFd != fd0 && err < bestErr) { bestErr = err; best = c; }
			}
		TheCsi.R(0x04) = base | KCombo[best];
		CciWrite8(0x3c, 0x86, KSync[best]);
		Log("[EKERN][cam] PDD: front csi keeps combo ", (TUint32)best); ekseam_puts("\n");
		}
#endif
		// the full field of view (camera-plan.md): the crop off and the datasheet's sub-sampler on (P0:0x99 [7:4] row
		// ratio, [3:0] column ratio; 1600x1200 / 5 = 320x240). The ratio semantics are not spelled out in the datasheet,
		// so it is CHECKED: no frame done within 1 s -> back to the centre crop (which boot 964 proved).
		if (iWidth == 320 && iHeight == 240 && 0)	// boot 971: the sub-sampler is parked while the CSI framing is settled on the centre crop (the boot-964 state)
			{
			const TInt fd0 = TheCsi.iFd;
			CciWrite8(0x3c, 0x90, 0x00);
			CciWrite8(0x3c, 0x99, 0x55);
			CciWrite8(0x3c, 0xFE, 0x10);					// CISCTL restart pulse (self-clearing)
			Ms(1000);
			const TInt got = TheCsi.iFd - fd0;
			Log("[EKERN][cam] PDD: front subsample 1/5 frames in 1 s=", (TUint32)got); ekseam_puts("\n");
			if (got <= 1)
				{
				CciWrite8(0x3c, 0xFE, 0x00);
				CciWrite8(0x3c, 0x99, 0x11);
				CciWrite8(0x3c, 0x90, 0x01);
				CciWrite8(0x3c, 0xFE, 0x10);
				ekseam_puts("[EKERN][cam] PDD: front back to the centre crop\n");
				}
			}
		return KErrNone;
		}
	// the sensor's DVP output size = the configured frame (the ISP scaler: 0x3808..0x380B)
	CciWrite16(0x4c, 0x3008, 0x42);
#ifdef A64_CAMERA_RAW
	// 2026-10-18 (camera-plan.md "the reference uses RAW"): Bayer raw8 as Megapixels runs this sensor -- FORMAT CONTROL raw,
	// ISP format mux = ISP RAW, scaler off; the client asks W x H (640x480) and gets W x H raw8 = W bytes per line (half its buffer)
	{
	CciWrite16(0x4c, 0x4300, 0x00);
	CciWrite16(0x4c, 0x501F, 0x03);
	CciWrite16(0x4c, 0x5001, 0x83);
	CciWrite16(0x4c, 0x5000, 0x06);	// ISP top: BPC + WPC only -- LENC (bit 7) off: the highlight wrap survives the raw tap, so it is before it
	const TInt rawW = iWidth;
	CciWrite16(0x4c, 0x3808, (TUint8)(rawW >> 8)); CciWrite16(0x4c, 0x3809, (TUint8)rawW);
	CciWrite16(0x4c, 0x380A, (TUint8)(iHeight >> 8)); CciWrite16(0x4c, 0x380B, (TUint8)iHeight);
	}
#else
	CciWrite16(0x4c, 0x3808, (TUint8)(iWidth >> 8)); CciWrite16(0x4c, 0x3809, (TUint8)iWidth);
	CciWrite16(0x4c, 0x380A, (TUint8)(iHeight >> 8)); CciWrite16(0x4c, 0x380B, (TUint8)iHeight);
#endif
#if defined(A64_CAMERA_DIAG) || defined(A64_CAMERA_BARS_HOLD)
	CciWrite16(0x4c, 0x503D, 0x80);							// colour bars for frames 1..24 (BarDiag)
#endif
	TheFrames = 0;
	CciWrite16(0x4c, 0x3008, 0x02);
	// 2026-10-18 research (camera-plan.md, the working driver's sequence -- formulas, not its tables): AE target thresholds
	// from a target luma of 52 (stable range 52*23/25 .. 52*27/25 = 47 .. 56, fast range high*2 / low/2 = 112 / 23), written
	// AFTER the mode is loaded and streaming, then AEC + AGC switched to auto again (0x3503 bits 0/1 = 0). Ours wrote 0x3503
	// once while powered down and never set the targets: exposure/gain read back identical in a dim room and in daylight.
	CciWrite16(0x4c, 0x3A0F, 56); CciWrite16(0x4c, 0x3A10, 47); CciWrite16(0x4c, 0x3A1B, 56);
	CciWrite16(0x4c, 0x3A1E, 47); CciWrite16(0x4c, 0x3A11, 112); CciWrite16(0x4c, 0x3A1F, 23);
	CciWrite16(0x4c, 0x3503, 0x00);
	CciWrite16(0x4c, 0x3406, 0x00);	// AWB auto (as the working driver; our manual-at-unity, boot 940, was compensating for the stalled loops)
#ifdef A64_CAMERA_RAW
	TheCsi.Configure(aPhysAddr, iWidth / 2, iHeight);	// W raw bytes per line = a W/2-pixel YUYV line to the CSI
#else
	TheCsi.Configure(aPhysAddr, iWidth, iHeight);
#endif
	ArmBuffer(aLinAddr, aPhysAddr, iWidth * 2 * iHeight);
	iPendingRequests = 1;
	iNextRequest = 0;
	iEnabled = ETrue;
	TheCsi.Start(ETrue);
	Log("[EKERN][cam] PDD: start ", (TUint32)((iWidth << 16) | iHeight)); ekseam_puts("\n");
	return KErrNone;
	}

// frame done (DFC): hand the frame to the LDD; it returns the next buffer, or an error = none free -> pause the CSI
// until CaptureNextImage brings one (else the CSI would overwrite a frame the client holds)
TInt DTemplateSensorIf::BufferDoneCallback(TInt aResult)
	{
	if (!iEnabled) return KErrNone;
	FrameDiag(++TheFrames, iWidth, iHeight);
	// rate (2026-10-18, boot 1003: the client sees 6.3 fps, its convert takes 4 ms): every 50 frames the sensor's VSYNCs
	// vs the CSI's frame-dones and the time -- VS > FD = the CSI drops frames, both ~6/s = the sensor runs slow
	{
	static TUint32 t0, vs0, fd0;
	if (TheFrames == 1 || TheFrames % 50 == 0)
		{
		const TUint32 now = NKern::TickCount();
		if (TheFrames > 1)
			{
			const TUint32 ms = (now - t0) * (TUint32)NKern::TickPeriod() / 1000;
			Log("[EKERN][cam] PDD: rate ms=", ms);
			Log(" vs=", (TUint32)(TheCsi.iVs - vs0));
			Log(" fd=", (TUint32)(TheCsi.iFd - fd0));
			Log(" ovf=", (TUint32)TheCsi.iOvf); ekseam_puts("\n");
			}
		t0 = now; vs0 = TheCsi.iVs; fd0 = TheCsi.iFd;
		}
#ifdef A64_CAMERA_RATE_REGS
	// what sets the frame time while streaming: PLL, HTS/VTS, VTS adjust (AEC night mode), exposure/gain, AEC ctrl
	if (TheSensorUnit == 0 && TheFrames == 60)
		{
		static const TUint16 KRegs[] = {0x3035, 0x3036, 0x3037, 0x3108, 0x380C, 0x380D, 0x380E, 0x380F, 0x350C, 0x350D,
			0x3A00, 0x3A02, 0x3A03, 0x3500, 0x3501, 0x3502, 0x350A, 0x350B, 0x3503, 0x3824, 0x460C, 0x56A1, 0x3A0F, 0x3A10};
		for (TUint i = 0; i < sizeof(KRegs) / sizeof(KRegs[0]); ++i)
			{
			Log("[EKERN][cam] PDD: rr ", KRegs[i]);
			Log("=", (TUint32)CciRead(0x4c, KRegs[i], ETrue) & 0xFF); ekseam_puts("\n");
			}
		}
#endif
	}
#ifdef A64_CAMERA_GREYWORLD
	AwbStep(TheFrames, iWidth, iHeight);		// off by default: the CFA phase fix first (boot 940 ground truth)
#endif
	NKern::LockedDec(iPendingRequests);
	TLinAddr lin; TPhysAddr phys;
	const TInt r = iObserver->NotifyImageCaptureEvent(aResult, lin, phys);
	if (r == KErrNone)
		{
		NKern::LockedInc(iPendingRequests);
		ArmBuffer(lin, phys, iWidth * 2 * iHeight);
		}
	else
		TheCsi.R(0x08) = 0;
	return r;
	}

TInt DTemplateSensorIf::CaptureNextImage(TLinAddr aLinAddr, TPhysAddr aPhysAddr)
	{
	if (iPendingRequests > 0) return KErrNone;			// the CSI already has a buffer
	NKern::LockedInc(iPendingRequests);
	ArmBuffer(aLinAddr, aPhysAddr, iWidth * 2 * iHeight);
	TheCsi.R(0x08) = 2;										// resume continuous capture
	return KErrNone;
	}

TInt DTemplateSensorIf::Stop()
	{
	iEnabled = EFalse;
	TheCsi.R(0x08) = 0;
	iPendingRequests = 0;
	return KErrNone;
	}

void DTemplateSensorIf::FillBuffer(TLinAddr) {}
TInt DTemplateSensorIf::HostPowerCallback(TAny*, TAny*) { return KErrNone; }
TInt DTemplateSensorIf::SensorClkReqCallback(TAny*) { return KErrNone; }

// the PDD factory: the 2010 template factory (unmodified camerasc.cpp) with two units, 0 = rear, 1 = front. Only one
// unit may be open at a time (the sensors share the parallel bus and the CSI).
class DA64CameraScPddFactory : public DTemplateCameraScPddFactory
	{
public:
	DA64CameraScPddFactory() { iUnitsMask = 0x03; }
	virtual TInt Validate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer)
		{
#ifdef A64_CAMERA_FRONT
		if (aUnit < 0 || aUnit > 1) return KErrNotSupported;
#else
		// the front GC2145 is PARKED (user decision 2026-10-17, camera-plan.md "Front GC2145"): unit 1 is refused;
		// A64DEFS=-DA64_CAMERA_FRONT brings it back for work on it
		if (aUnit != 0) return KErrNotSupported;
#endif
		return DTemplateCameraScPddFactory::Validate(0, aInfo, aVer);
		}
	virtual TInt Create(DBase*& aChannel, TInt aUnit, const TDesC8* aInfo, const TVersion& aVer)
		{
		if (IsUnitOpen(aUnit ^ 1)) return KErrInUse;
		TheCreatingUnit = aUnit;
		return DTemplateCameraScPddFactory::Create(aChannel, aUnit, aInfo, aVer);
		}
	};

// Board entry (a64_glue.cpp, with the other drivers)
extern "C" void A64CameraInstall()
	{
#ifndef A64_NO_CAMERA_PROBE
	CameraProbe();
#endif
	NKern::ThreadEnterCS();
	DLogicalDevice* ld = CreateLogicalDevice_camerasc();
	TInt r = ld ? Kern::InstallLogicalDevice(ld) : KErrNoMemory;
	ekseam_puts("[EKERN][cam] InstallLogicalDevice(CameraSc) r="); ekseam_puthex((TUint32)r);
	DPhysicalDevice* pd = new DA64CameraScPddFactory;		// was CreatePhysicalDevice_camerasc() (one unit)
	r = pd ? Kern::InstallPhysicalDevice(pd) : KErrNoMemory;
	ekseam_puts(" InstallPhysicalDevice(CameraSc.TE) r="); ekseam_puthex((TUint32)r); ekseam_puts("\n");
	NKern::ThreadLeaveCS();
	}
