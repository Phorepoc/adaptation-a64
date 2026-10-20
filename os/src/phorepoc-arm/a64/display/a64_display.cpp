// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// OURS (PinePhone track P4): bring up the PinePhone's 720x1440 panel.
// The register-level work is NuttX's A64 display drivers (nuttx/*.c, Apache-2.0, vendored unmodified; written up
// at lupyuen.github.io/articles/dsi2 & co): AXP803 rails over RSB, the R_PWM backlight, TCON0, MIPI DSI + D-PHY,
// the Sitronix ST7703 panel init sequence (XBD599), and the Display Engine 2 mixer + blender + UI channel 1.
// They compile against nuttx/compat (putreg32 & co, logging, delays); the A64_*_ADDR bases there are the linear
// addresses of the DPlatChunkHw windows mapped here. The order of calls = NuttX's up_fbinitialize
// (boards/arm64/a64/pinephone/src/pinephone_display.c).
//
// Runs on its own DFC thread, a few seconds after boot (after the USB link is up), so a display hang never takes
// the kernel or the debug log with it. Step 1 (this file): a test pattern straight into the framebuffer.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <kernel/arm/assp.h>
#include <kernel/cache.h>
#include <stdarg.h>
#include "../power/a64_pwrres.h"

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);
extern "C" void ekseam_board_panic();

extern "C" {
TUint32 ph_a64_de, ph_a64_io, ph_a64_dsi, ph_a64_r;
int pinephone_lcd_backlight_enable(TUint32 percent);
int pinephone_lcd_backlight_ticks(TUint32 ticks);
int pinephone_lcd_backlight_disable(void);
int a64_tcon0_init(TUint16 panel_width, TUint16 panel_height);
int pinephone_lcd_panel_reset(bool val);
int pinephone_pmic_init(void);
int a64_mipi_dsi_enable(void);
int a64_mipi_dphy_enable(void);
int pinephone_lcd_panel_init(void);
int a64_mipi_dsi_start(void);
int a64_de_init(void);
int a64_de_blender_init(void);
int a64_de_ui_channel_init(TUint8 channel, const void* fbmem, TUint fblen, TUint16 xres, TUint16 yres,
						   TUint16 xoffset, TUint16 yoffset);
int a64_de_enable(TUint8 channels);
long a64_mipi_dsi_write(unsigned char channel, int cmd, const unsigned char* txbuf, unsigned txlen);	// (cmd 0x05 = DCS short write)

void ph_mdelay(unsigned ms) { Kern::NanoWait(ms * 1000000u); }
void ph_udelay(unsigned us) { Kern::NanoWait(us * 1000u); }

void ph_log(const char* fmt, ...)
	{
	TBuf8<160> buf;
	buf.Append(_L8("[EKERN][disp] nuttx: "));
	VA_LIST list;
	VA_START(list, fmt);
	Kern::AppendFormat(buf, fmt, list);
	VA_END(list);
	if (buf.Length() < buf.MaxLength() - 1 && (buf.Length() == 0 || buf[buf.Length() - 1] != '\n'))
		buf.Append('\n');
	buf.Append('\0');
	ekseam_puts((const char*)buf.Ptr());
	}
}

const TInt KPanelW = 720, KPanelH = 1440;
const TInt KFbBytes = KPanelW * KPanelH * 4;

static DPlatChunkHw* Chunks[8];
static TPhysAddr FbPhys;
static TUint32* Fb;
static TDfcQue* DispQ;
static TDfc* DispDfc;
static TDfc* HealthDfc;			// the panel health check after the first NGA flip (A64DisplayStart creates it)
static void DispTimerFn(TAny*);
static NTimer DispTimer(DispTimerFn, 0);

static TLinAddr Map(TInt aIdx, TPhysAddr aPhys, TInt aSize, TUint aAttr = EMapAttrSupRw | EMapAttrFullyBlocking)
	{
	TInt r = DPlatChunkHw::New(Chunks[aIdx], aPhys, aSize, aAttr);
	if (r != KErrNone)
		{
		ekseam_puts("[EKERN][disp] DPlatChunkHw::New failed for "); ekseam_puthex(aPhys);
		return 0;
		}
	return Chunks[aIdx]->LinearAddress();
	}

// Pauses only up to the PMIC: from the DSI link on, the sequence is timing-sensitive (pauses there = a white panel,
// 2026-10-06) and runs back-to-back as in NuttX.
static TBool StepPause = ETrue;
static TBool Resuming;		// the bring-up replayed after a screen-off (power rung P4): no pauses, no test pattern, no rail cycle

static TBool Step(const char* aWhat, TInt aR)
	{
	ekseam_puts("[EKERN][disp] "); ekseam_puts(aWhat); ekseam_puts(" r="); ekseam_puthex((unsigned)aR);
	if (StepPause && !Resuming)		// let phlink send this line before the next step (a hang kills USB too)
		NKern::Sleep(NKern::TimerTicks(500));
#ifdef EKSEAM_FLUSH_ON_PROC
	ekseam_board_panic();	// (debug: every step into the SD log -- a power cut leaves no other trace)
#endif
	return aR >= 0;
	}

// Colour bars down the panel, a white frame, and the nHD (360x640 @2x = 720x1280) area outlined in yellow: what
// the first Symbian image will use (display plan stage 1).
static void TestPattern()
	{
	static const TUint32 KBars[8] =
		{ 0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00, 0xFFFF00FF, 0xFFFF0000, 0xFF0000FF, 0xFF000000 };
	for (TInt y = 0; y < KPanelH; ++y)
		{
		TUint32* row = Fb + y * KPanelW;
		for (TInt x = 0; x < KPanelW; ++x)
			{
			TUint32 c = KBars[(y * 8) / KPanelH];
			if (x < 4 || x >= KPanelW - 4 || y < 4 || y >= KPanelH - 4)
				c = 0xFFFFFFFF;
			else if ((y >= 80 && y < 84) || (y >= 1360 && y < 1364))
				c = 0xFFFFD000;
			row[x] = c;
			}
		}
	}

// The RSB bus to the AXP803 PMIC: NuttX's a64_rsb.c assumes the bootloader left it set up; on the phone (Debian's
// u-boot-sunxi) it did not ("RSB Transaction Failed", 2026-10-06). Sequence = Linux drivers/bus/sunxi-rsb.c
// (sunxi_rsb_hw_init + sunxi_rsb_init_device_mode + set runtime address): R_PRCM APB0 gate + reset (bit 3),
// PL0/PL1 = function 2 (S_RSB_SCK/SDA), soft reset, 3 MHz from the 24 MHz APB0, PMIC I2C -> RSB mode
// (reg 0x3e = 0x7c), then SRTA: hardware address 0x3a3 (AXP803) -> runtime address 0x2d (what NuttX uses).
static volatile TUint32& R(TUint32 aOff) { return *(volatile TUint32*)(ph_a64_r + aOff); }
const TUint32 KRPrcm = 0x1400, KRPio = 0x2c00, KRsb = 0x3400;
const TUint32 KRsbCtrl = 0x00, KRsbCcr = 0x04, KRsbStat = 0x0c, KRsbPmcr = 0x28, KRsbCmd = 0x2c, KRsbDar = 0x30;

static TBool RsbWait(volatile TUint32& aReg, TUint32 aMask)
	{
	for (TInt i = 0; i < 10000; ++i)
		{
		if (!(aReg & aMask))
			return ETrue;
		ph_udelay(10);
		}
	return EFalse;
	}

static TInt RsbInit()
	{
	R(KRPrcm + 0x28) |= 1u << 3;							// APB0 clock gate: R_RSB
	R(KRPrcm + 0xb0) |= 1u << 3;							// APB0 reset deassert: R_RSB
	R(KRPio + 0x00) = (R(KRPio + 0x00) & ~0xFFu) | 0x22u;	// PL0, PL1 = function 2
	R(KRsb + KRsbCtrl) = 1;									// soft reset
	if (!RsbWait(R(KRsb + KRsbCtrl), 1))
		return -1;
	R(KRsb + KRsbCcr) = (2u << 8) | 3u;						// SDA out delay 2, clock 24 MHz / 2 / (3 + 1) = 3 MHz
	R(KRsb + KRsbPmcr) = (1u << 31) | (0x7cu << 16) | (0x3eu << 8);	// PMIC: I2C -> RSB mode
	if (!RsbWait(R(KRsb + KRsbPmcr), 1u << 31))
		return -2;
	R(KRsb + KRsbStat) = R(KRsb + KRsbStat);				// clear (write 1 to clear)
	R(KRsb + KRsbCmd) = 0xe8;								// SRTA: set runtime address
	R(KRsb + KRsbDar) = (0x2du << 16) | 0x3a3u;
	R(KRsb + KRsbCtrl) = 1u << 7;							// start
	if (!RsbWait(R(KRsb + KRsbCtrl), 1u << 7))
		return -3;
	const TUint32 stat = R(KRsb + KRsbStat);
	R(KRsb + KRsbStat) = stat;
	return (stat & 1) ? 0 : -(TInt)(0x100 | stat);			// TRANS_OVER = done; else the error bits
	}

// Register dump after bring-up: a block whose clock/reset is off reads back zeros; PLL bit 28 = locked.
static void DumpRange(const char* aName, TUint32 aBase, TUint32 aPhys, TUint32 aFrom, TUint32 aTo)
	{
	ekseam_puts("[EKERN][disp] dump "); ekseam_puts(aName); ekseam_puts(" @"); ekseam_puthex(aPhys + aFrom);
	TBuf8<128> line;
	for (TUint32 off = aFrom; off < aTo; off += 4)
		{
		if (((off - aFrom) & 0x1f) == 0)
			{
			line.Zero();
			line.AppendNumFixedWidth(off, EHex, 3);
			line.Append(':');
			}
		line.Append(' ');
		line.AppendNumFixedWidth(*(volatile TUint32*)(aBase + off), EHex, 8);
		if (((off + 4 - aFrom) & 0x1f) == 0 || off + 4 >= aTo)
			{
			line.Append('\n');
			line.Append('\0');
			ekseam_puts((const char*)line.Ptr());
			}
		}
	}

extern "C" int a64_rsb_read(TUint8 rt_addr, TUint8 reg_addr);
extern "C" int a64_rsb_write(TUint8 rt_addr, TUint8 reg_addr, TUint8 value);

// POWER-OFF (2026-10-06): SSM's shutdown ends in Power::PowerDown (the variant's power controller, a64_glue.cpp).
// A64PmicMap (thread context, at registration) maps the R_ block; A64PmicPowerOff (any context) brings the RSB
// bus up if the display has not, then sets the AXP803's power-off bit: REG 32H bit 7, as Linux's axp20x poweroff.
extern "C" TInt A64PmicMap()
	{
	NKern::ThreadEnterCS();
	if (!ph_a64_r) ph_a64_r = Map(3, 0x01F00000, 0x4000);
	NKern::ThreadLeaveCS();
	return ph_a64_r ? KErrNone : KErrNoMemory;
	}
extern "C" TInt A64PmicPowerOff()
	{
	if (!ph_a64_r)
		return KErrNotReady;
	TInt r = RsbInit();
	if (r != 0)
		return r;
	const int v = a64_rsb_read(0x2d, 0x32);
	if (v < 0)
		return v;
	return a64_rsb_write(0x2d, 0x32, (TUint8)(v | 0x80));		// the PMIC cuts every rail: the phone is off
	}

// BATTERY + CHARGER (2026-10-07: a flat battery boot-looped the phone even under Mobian; it sat at 2.9 V with the
// AXP803 reporting "not charging"). The RSB bus is shared with the display's bring-up (its own thread): RsbOwner is
// a try-lock -- the battery side skips a reading when the display holds the bus, the display side waits for it.
// AXP803 registers (datasheet; Linux axp20x_battery / axp20x_usb_power): 00 power status (bit7 ACIN present,
// bit5 VBUS present, bit2 battery current direction 1 = charging), 01 (bit6 charging, bit5 battery present),
// 30 VBUS path (bits 1:0 input limit 900/1500/2000/2500 mA), 33 charge control 1 (bit7 enable, bits 3:0 current),
// 78/79 battery voltage (12 bit, 1.1 mV/LSB), 7A/7B charge current, 7C/7D discharge current (12 bit, ~1 mA/LSB),
// 82 ADC enable (bit7 battery voltage, bit6 battery current), B9 fuel gauge (bit7 valid, bits 6:0 = %).
static volatile TUint32 RsbOwner;
static TBool RsbTryLock() { TUint32 z = 0; return __e32_atomic_cas_ord32(&RsbOwner, &z, 1); }
static void RsbLock() { while (!RsbTryLock()) ph_mdelay(1); }
static void RsbUnlock() { __e32_atomic_store_rel32(&RsbOwner, 0); }

static TInt Pmic12(TUint8 aHi)
	{
	const int h = a64_rsb_read(0x2d, aHi), l = a64_rsb_read(0x2d, (TUint8)(aHi + 1));
	return (h < 0 || l < 0) ? -1 : ((h << 4) | (l & 0xF));
	}

// Charging on, 2 A input limit, battery ADCs on. Called once, before the system starts (a64_glue.cpp stage 3).
extern "C" TInt A64PmicChargeSetup()
	{
	if (!ph_a64_r)
		return KErrNotReady;
	RsbLock();
	TInt r = RsbInit();
	if (r == 0)
		{
		int v = a64_rsb_read(0x2d, 0x33);
		if (v >= 0 && !(v & 0x80)) a64_rsb_write(0x2d, 0x33, (TUint8)(v | 0x80));
		v = a64_rsb_read(0x2d, 0x30);
		if (v >= 0 && (v & 3) < 2) a64_rsb_write(0x2d, 0x30, (TUint8)((v & ~3) | 2));
		v = a64_rsb_read(0x2d, 0x82);
		if (v >= 0 && (v & 0xC0) != 0xC0) a64_rsb_write(0x2d, 0x82, (TUint8)(v | 0xC0));
		r = v < 0 ? v : 0;
		}
	RsbUnlock();
	return r;
	}

// Plug-in = the AXP803's BC1.2 charger detection, which (as Linux's axp20x_usb_power works around) puts the VBUS
// input limit back to 500 mA unless the port signals a dedicated charger -- a USB-C charger (a MacBook's) does
// not: the phone drew 0.4 A from it (2026-10-07). Re-asserted every 5 s from the monitor DFC; returns
// REG30 | REG2C << 8 | REG2F << 16 as found (before any fix), or < 0.
extern "C" TInt A64PmicChargeAssert()
	{
	if (!ph_a64_r)
		return KErrNotReady;
	if (!RsbTryLock())
		return KErrInUse;
	const int r30 = a64_rsb_read(0x2d, 0x30), r2c = a64_rsb_read(0x2d, 0x2c), r2f = a64_rsb_read(0x2d, 0x2f);
	if (r30 >= 0 && (r30 & 3) != 3)
		a64_rsb_write(0x2d, 0x30, (TUint8)((r30 & ~3) | 3));		// 2.5 A: the charger itself limits below that
	const int r33 = a64_rsb_read(0x2d, 0x33);
	if (r33 >= 0 && !(r33 & 0x80))
		a64_rsb_write(0x2d, 0x33, (TUint8)(r33 | 0x80));
	// TS pin (2026-10-07): at the PMIC's reset default the battery-temperature input gates charging, and the
	// PinePhone's pack does not give it an in-range reading -> NO charging (0.2-0.5 A drawn = the phone alone, after
	// the battery was out and the PMIC reset; it charged right after Mobian, which leaves the TS pin as a GPADC).
	// REG 84H bit 2 = 1: TS = external ADC input, temperature monitoring off; bits 1:0 = 0: no TS current source.
	// THE input limit (2026-10-07): on the AXP803/813 it is REG 35H bits 7:4 (Linux axp20x_usb_power, AXP813:
	// curr_lim_fld = AXP22X_CHRG_CTRL3 [7:4], 100/500/900/1500/2000/2500/... mA) -- not REG 30H. At its 500 mA
	// default the phone took ~0.5 A from any charger: enough to run it, nothing left to charge (charged only when
	// off). 5 = 2.5 A; the charger limits below that.
	const int r35 = a64_rsb_read(0x2d, 0x35);
	if (r35 >= 0 && (r35 >> 4) != 5)
		a64_rsb_write(0x2d, 0x35, (TUint8)((r35 & 0x0F) | 0x50));
	const int r84 = a64_rsb_read(0x2d, 0x84);
	if (r84 >= 0 && (r84 & 7) != 4)
		a64_rsb_write(0x2d, 0x84, (TUint8)((r84 & ~7) | 4));
	RsbUnlock();
	return (r30 < 0 || r35 < 0 || r2f < 0 || r84 < 0) ? KErrGeneral
		: (r30 | (r35 << 8) | (r2f << 16) | (r84 << 24));
	}

// aOut: [0] REG00, [1] REG01, [2] battery mV, [3] fuel % (-1 = not valid), [4] charge mA, [5] discharge mA.
extern "C" TInt A64PmicReadBatt(TInt* aOut)
	{
	if (!ph_a64_r)
		return KErrNotReady;
	if (!RsbTryLock())
		return KErrInUse;
	aOut[0] = a64_rsb_read(0x2d, 0x00);
	aOut[1] = a64_rsb_read(0x2d, 0x01);
	const TInt mv = Pmic12(0x78);
	aOut[2] = mv < 0 ? -1 : (mv * 11) / 10;
	const int f = a64_rsb_read(0x2d, 0xB9);
	aOut[3] = (f >= 0 && (f & 0x80)) ? (f & 0x7F) : -1;
	aOut[4] = Pmic12(0x7A);
	aOut[5] = Pmic12(0x7C);
	RsbUnlock();
	return (aOut[0] < 0 || aOut[1] < 0 || mv < 0) ? KErrGeneral : KErrNone;
	}

// A read-modify-write of one AXP803 register for the other drivers (the Bluetooth/WiFi IO rail DLDO4, ...).
extern "C" TInt A64PmicUpdateReg(TUint8 aReg, TUint8 aClr, TUint8 aSet)
	{
	if (!ph_a64_r)
		return KErrNotReady;
	RsbLock();
	const int v = a64_rsb_read(0x2d, aReg);
	const int r = v < 0 ? v : a64_rsb_write(0x2d, aReg, (TUint8)((v & ~aClr) | aSet));
	RsbUnlock();
	return r < 0 ? KErrGeneral : KErrNone;
	}

// One AXP803 register read for the other drivers (power/: DCDC2 read-back). Returns the byte or < 0.
extern "C" TInt A64PmicReadReg(TUint8 aReg)
	{
	if (!ph_a64_r)
		return KErrNotReady;
	RsbLock();
	const int v = a64_rsb_read(0x2d, aReg);
	RsbUnlock();
	return v;
	}

// THE POWER KEY (2026-10-08): the PinePhone's power button is the AXP803's PEK input. Its edge IRQs latch in IRQ
// status 5 (REG 4CH, write 1 to clear) once enabled in REG 44H -- as Linux axp20x-pek: bit 6 PEK_RIS_EDGE ("DBR") =
// pressed, bit 5 PEK_FAL_EDGE ("DBF") = released, bit 4 POKSIRQ = a short press ended, bit 3 POKLIRQ = held past
// the long-press time (REG 36H). On the phone the release edge never came (2026-10-08: "power down" only -> SysAp
// saw a held key = long press = shutdown), hence the short/long bits too. The NMI line is not wired: a64_glue polls.
// Returns the latched bits of 0x78, or < 0.
extern "C" TInt A64PmicKeyPoll()
	{
	if (!ph_a64_r)
		return KErrNotReady;
	if (!RsbTryLock())
		return KErrInUse;
	static TBool enabled;
	if (!enabled)
		{
		const int e = a64_rsb_read(0x2d, 0x44);
		if (e >= 0)
			{
			a64_rsb_write(0x2d, 0x4c, 0x78);					// drop anything latched before we listened
			if ((e & 0x78) != 0x78) a64_rsb_write(0x2d, 0x44, (TUint8)(e | 0x78));
			enabled = ETrue;
			}
		}
	const int s = a64_rsb_read(0x2d, 0x4c);
	if (s > 0 && (s & 0x78))
		a64_rsb_write(0x2d, 0x4c, (TUint8)(s & 0x78));
	RsbUnlock();
	return s < 0 ? s : (s & 0x78);
	}

extern "C" TUint16 crc16ccittpart(const TUint8* src, TUint len, TUint16 crc16val);

static void DumpRegs()
	{
	// AXP803 (runtime address 0x2d): 0x03 chip id, 0x10/0x12/0x13 output on/off 1-3, 0x15/0x16 DLDO1/2,
	// 0x1b..0x1d ELDO1-3, 0x28/0x29 ALDO..., 0x90/0x91 GPIO0 LDO
	static const TUint8 KPmic[] = { 0x03, 0x10, 0x12, 0x13, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
									0x28, 0x29, 0x2a, 0x90, 0x91 };
	for (TUint i = 0; i < sizeof(KPmic); ++i)
		{
		ekseam_puts("[EKERN][disp] pmic reg "); ekseam_puthex(KPmic[i]);
		ekseam_puts("[EKERN][disp]   = "); ekseam_puthex((unsigned)a64_rsb_read(0x2d, KPmic[i]));
		}
	static const TUint16 KCcu[] = { 0x10, 0x28, 0x40, 0x48, 0x60, 0x64, 0x104, 0x118, 0x168, 0x2c0, 0x2c4 };
	for (TUint i = 0; i < sizeof(KCcu) / sizeof(KCcu[0]); ++i)
		DumpRange("ccu", ph_a64_io + 0x20000, 0x01C20000, KCcu[i], KCcu[i] + 4);
	DumpRange("sysctl", ph_a64_io, 0x01C00000, 0x00, 0x08);
	DumpRange("tcon0", ph_a64_io + 0xc000, 0x01C0C000, 0x00, 0x80);
	DumpRange("tcon0 cpu/safe", ph_a64_io + 0xc000, 0x01C0C000, 0x1f0, 0x200);
	DumpRange("dsi", ph_a64_dsi, 0x01CA0000, 0x00, 0x80);
	DumpRange("dphy", ph_a64_dsi + 0x1000, 0x01CA1000, 0x00, 0x60);
	DumpRange("de top", ph_a64_de, 0x01000000, 0x00, 0x20);
	DumpRange("mixer glb", ph_a64_de + 0x100000, 0x01100000, 0x00, 0x10);
	DumpRange("blender", ph_a64_de + 0x101000, 0x01101000, 0x00, 0x20);
	DumpRange("blender ctl", ph_a64_de + 0x101000, 0x01101000, 0x80, 0xa0);
	DumpRange("ui ch1", ph_a64_de + 0x103000, 0x01103000, 0x00, 0x20);
	DumpRange("ui ch1 size", ph_a64_de + 0x103000, 0x01103000, 0x88, 0x8c);
	}

// Read a DCS register back from the panel (LP read + bus turnaround): proves whether the panel takes our commands.
// Sequence = Linux sun6i_mipi_dsi.c sun6i_dsi_dcs_read: DCS READ (0x06) short packet, jump LP11 -> LPDT -> DLY ->
// TBA -> END, then the response in CMD_RX0 (0x240): 0x21/0x22 = short read response (data in bits 15:8),
// 0x02 = acknowledge-with-error.
extern "C" int mipi_dsi_short_packet(TUint8* pktbuf, TUint pktlen, TUint8 channel, TUint8 cmd,
									 const TUint8* txbuf, TUint txlen);
static volatile TUint32& Dsi(TUint32 aOff) { return *(volatile TUint32*)(ph_a64_dsi + aOff); }

static TInt PanelRead(TUint8 aCmd)
	{
	TUint8 pkt[8] = {0};
	if (mipi_dsi_short_packet(pkt, sizeof(pkt), 0, 0x06, &aCmd, 1) != 4)
		return -1;
	Dsi(0x200) = (1u << 26) | (1u << 25) | (1u << 9);		// CMD_CTL: clear RX_OVERFLOW, RX_FLAG, TX_FLAG
	Dsi(0x300) = pkt[0] | (pkt[1] << 8) | (pkt[2] << 16) | ((TUint32)pkt[3] << 24);
	Dsi(0x200) = 4 - 1;
	Dsi(0x48) = (4u << 0) | (7u << 16) | (1u << 28) | (15u << 4);	// LP11->LPDT, LPDT->DLY, DLY->TBA, TBA->END
	Dsi(0x10) &= ~1u;
	Dsi(0x10) |= 1u;
	TInt i;
	for (i = 0; i < 100 && (Dsi(0x10) & 1u); ++i)
		ph_mdelay(1);
	const TUint32 ctl = Dsi(0x200), rx = Dsi(0x240);
	ekseam_puts("[EKERN][disp] panel read cmd "); ekseam_puthex(aCmd);
	ekseam_puts("[EKERN][disp]   CMD_CTL="); ekseam_puthex(ctl);
	ekseam_puts("[EKERN][disp]   CMD_RX0="); ekseam_puthex(rx);
	if (i == 100)
		ekseam_puts("[EKERN][disp]   (instruction never finished)\n");
	if ((rx & 0xff) == 0x1c || (rx & 0xff) == 0x1a)		// DCS / generic LONG read response: data in CMD_RX1
		return (TInt)(Dsi(0x244) & 0xff);
	return ((rx & 0xff) == 0x21 || (rx & 0xff) == 0x22) ? (TInt)((rx >> 8) & 0xff) : -2;
	}

// PANEL HEALTH: the recurring black-with-backlight boots had every readback right
// BEFORE the HS video stream started and nothing looked afterwards. Read the power mode (0x0A: want bits 4:2 set = sleep
// out, normal mode, display on; 0x9C seen on good boots), display mode (0x0D) and signal mode (0x0E), and log them.
// 1 = healthy, 0 = unhealthy, -1 = unreadable (an LP read while the HS video runs may fail: never act on that alone).
static TInt PanelHealth(const char* aWhen)
	{
	const TInt pm = PanelRead(0x0A), dm = PanelRead(0x0D), sm = PanelRead(0x0E);
	ekseam_puts("[EKERN][disp] panel health ("); ekseam_puts(aWhen); ekseam_puts(") power mode="); ekseam_puthex((unsigned)pm);
	ekseam_puts("[EKERN][disp]   display mode="); ekseam_puthex((unsigned)dm);
	ekseam_puts("[EKERN][disp]   signal mode="); ekseam_puthex((unsigned)sm);
	if (pm < 0)
		return -1;
	return (pm & 0x1C) == 0x1C ? 1 : 0;
	}

static void PanelDcs(TUint8 aCmd)
	{
	const unsigned char c = aCmd;
	a64_mipi_dsi_write(0, 0x05, &c, 1);
	}

// bounded recovery: re-send SLPOUT (0x11) + 120 ms + DISPLAY ON (0x29), re-read; max 2 attempts. ETrue = healthy/unknown
static TBool PanelRecover(const char* aWhen)
	{
	if (PanelHealth(aWhen) != 0)
		return ETrue;
	for (TInt i = 1; i <= 2; ++i)
		{
		ekseam_puts("[EKERN][disp] panel UNHEALTHY: re-sending 0x11 / 0x29, attempt "); ekseam_puthex((unsigned)i);
		PanelDcs(0x11);
		NKern::Sleep(NKern::TimerTicks(120));
		PanelDcs(0x29);
		NKern::Sleep(NKern::TimerTicks(50));
		if (PanelHealth("after re-send") != 0)
			{
			ekseam_puts("[EKERN][disp] panel recovered by re-send\n");
			return ETrue;
			}
		}
	return EFalse;
	}

// p-boot's recorded DSI register writes for this panel (xff.cz/git/p-boot src/display.c dsi_init_seq, known to work
// on the PinePhone): compare with the live registers, print mismatches only. 0x0010/0x0000 change at run time.
struct TRegVal { TUint16 iOff; TUint32 iVal; };
static const TRegVal KPbootDsi[] = {
	{ 0x0000, 0x00000001 },
	{ 0x0010, 0x00030000 },
	{ 0x0060, 0x0000000a },
	{ 0x0078, 0x00000000 },
	{ 0x0020, 0x0000001f },
	{ 0x0024, 0x10000001 },
	{ 0x0028, 0x20000010 },
	{ 0x002c, 0x2000000f },
	{ 0x0030, 0x30100001 },
	{ 0x0034, 0x40000010 },
	{ 0x0038, 0x0000000f },
	{ 0x003c, 0x5000001f },
	{ 0x004c, 0x00560001 },
	{ 0x02f8, 0x000000ff },
	{ 0x0014, 0x00005bc7 },
	{ 0x007c, 0x10000007 },
	{ 0x0040, 0x30000002 },
	{ 0x0044, 0x00310031 },
	{ 0x0054, 0x00310031 },
	{ 0x0090, 0x1308703e },
	{ 0x0098, 0x0000ffff },
	{ 0x009c, 0xffffffff },
	{ 0x0080, 0x00010008 },
	{ 0x000c, 0x00000000 },
	{ 0x00b0, 0x12000021 },
	{ 0x00b4, 0x01000031 },
	{ 0x00b8, 0x07000001 },
	{ 0x00bc, 0x14000011 },
	{ 0x0018, 0x0011000a },
	{ 0x001c, 0x05cd05a0 },
	{ 0x00c0, 0x09004a19 },
	{ 0x00c4, 0x50b40000 },
	{ 0x00c8, 0x35005419 },
	{ 0x00cc, 0x757a0000 },
	{ 0x00d0, 0x09004a19 },
	{ 0x00d4, 0x50b40000 },
	{ 0x00e0, 0x0c091a19 },
	{ 0x00e4, 0x72bd0000 },
	{ 0x00e8, 0x1a000019 },
	{ 0x00ec, 0xffff0000 },
};

static void ComparePboot()
	{
	TInt diffs = 0;
	for (TUint i = 0; i < sizeof(KPbootDsi) / sizeof(KPbootDsi[0]); ++i)
		{
		const TUint32 v = Dsi(KPbootDsi[i].iOff);
		if (v != KPbootDsi[i].iVal)
			{
			++diffs;
			ekseam_puts("[EKERN][disp] DSI differs from p-boot at "); ekseam_puthex(KPbootDsi[i].iOff);
			ekseam_puts("[EKERN][disp]   ours  ="); ekseam_puthex(v);
			ekseam_puts("[EKERN][disp]   pboot ="); ekseam_puthex(KPbootDsi[i].iVal);
			}
		}
	ekseam_puts("[EKERN][disp] DSI vs p-boot: differences="); ekseam_puthex(diffs);
	}

// The picture source. Before the Symbian framebuffer driver speaks: this file's own 720x1440 test pattern. Once the
// driver (a64fb.cpp, Syborg's svpframebuffer on a register model) hands us its video RAM: that buffer, 360x640
// (the nHD screen the ROM is built for) doubled by the DE's UI scaler (GSU1, the sun8i_ui_scaler of Linux/p-boot)
// to 720x1280 and centred on the 720x1440 panel -- no copying, the window server draws straight into the scanned
// buffer. Live changes (the driver's FB_BASE writes: secure display, mode switches) reprogram the layer in place.
static TPhysAddr SrcPhys;
static TInt SrcW, SrcH;
static TBool DisplayReady;
// guards the flip state AND DisplayReady: the screen-off sequence (power rung P4) takes the DE down under it, and every
// path that writes DE registers (flips, source changes) checks DisplayReady inside it, so none touches a DE in reset
static TSpinLock VblankLock(TSpinLock::EOrderGenericIrqLow2);

static inline TUint32 WH(TInt aW, TInt aH) { return ((TUint32)(aH - 1) << 16) | (TUint32)(aW - 1); }

static void ProgramSource()
	{
	TPhysAddr phys = SrcPhys ? SrcPhys : FbPhys;
	const TInt w = SrcPhys ? SrcW : KPanelW, h = SrcPhys ? SrcH : KPanelH;
	const TInt scale = (w * 2 <= KPanelW && h * 2 <= KPanelH) ? 2 : 1;
	const TInt outW = w * scale, outH = h * scale;
	const TInt offY = (KPanelH - outH) / 2, offX = (KPanelW - outW) / 2;
	a64_de_ui_channel_init(1, (const void*)phys, w * h * 4, (TUint16)w, (TUint16)h, 0, 0);
	volatile TUint32* mixer = (volatile TUint32*)ph_a64_de + 0x100000 / 4;
	// a64_de_ui_channel_init (NuttX) sizes the mixer canvas + blender output to the LAYER: the panel needs them at the
	// panel size, or everything past the 360x640 canvas is undriven junk (white scanlines, found 2026-10-06)
	mixer[0xc / 4] = WH(KPanelW, KPanelH);				// GLB_SIZE
	mixer[0x108c / 4] = WH(KPanelW, KPanelH);			// BLD_SIZE (blender output)
	mixer[(0x1008) / 4] = WH(outW, outH);				// blender pipe 0 input size = the scaled picture
	mixer[(0x100c) / 4] = ((TUint32)offY << 16) | (TUint32)offX;	// ... centred on the panel
	volatile TUint32* gsu = mixer + 0x40000 / 4;		// UI scaler of channel 1
	if (scale == 2)
		{
		static const TUint32 KCoeff[16] =				// lan2coefftab16, 1:2 = index 0 (Linux sun8i_ui_scaler.c)
			{ 0x00004000, 0x00033ffe, 0x00063efc, 0x000a3bfb, 0xff0f37fb, 0xfe1433fb, 0xfd192ffb, 0xfd1f29fb,
			  0xfc2424fc, 0xfb291ffd, 0xfb2f19fd, 0xfb3314fe, 0xfb370fff, 0xfb3b0a00, 0xfc3e0600, 0xfe3f0300 };
		gsu[0x40 / 4] = WH(outW, outH);
		gsu[0x80 / 4] = WH(w, h);
		gsu[0x88 / 4] = 0x80000;						// h/v step: 0.5 in 20-bit fixed point
		gsu[0x8c / 4] = 0x80000;
		gsu[0x90 / 4] = 0;
		gsu[0x98 / 4] = 0;
		for (TInt i = 0; i < 16; ++i)
			gsu[0x200 / 4 + i] = KCoeff[i];
		gsu[0] = 0x11;									// enable + coefficients ready
		}
	else
		gsu[0] = 0;
	mixer[0x8 / 4] = 1;									// GLB_DBUFFER: apply
	ekseam_puts("[EKERN][disp] source "); ekseam_puthex(phys);
	ekseam_puts("[EKERN][disp]   size "); ekseam_puthex(((TUint32)w << 16) | (TUint32)h);
	ekseam_puts("[EKERN][disp]   x"); ekseam_puthex((unsigned)scale);
	}

// STATUS STRIP (2026-10-07; the Siemens SX1 idea): the OS picture is 720x1280 centred on the 720x1440 panel, so
// rows 0-79 are a black bar the OS never draws. UI channel 2 scans this file's own panel buffer (Fb, uncached)
// over it through blender pipe 1; a64_bars.cpp draws the top bar there (and the golden menu/keyboard overlay).
const TInt KStripH = 80;
extern "C" void A64BarsStart();
extern "C" TUint32* A64PanelFb() { return Fb; }
// SOFT KEY BAR (2026-10-07): the bottom black bar (rows 1360-1439) carries the S^3 hardware keys the PinePhone lacks --
// green (call) | Menu | red (end); a64touch.cpp turns a touch there into the key. Channel 2 now scans the WHOLE panel
// buffer: the rows between the bars get alpha 0 (ARGB, pixel alpha) so the OS picture on channel 1 shows through.
static void Fill(TInt aX, TInt aY, TInt aW, TInt aH, TUint32 aArgb)
	{
	for (TInt y = aY; y < aY + aH; ++y)
		for (TInt x = aX; x < aX + aW; ++x)
			Fb[y * KPanelW + x] = aArgb;
	}

static void Pill(TInt aCx, TInt aCy, TInt aW, TInt aH, TUint32 aArgb)
	{
	const TInt r = aH / 2;
	for (TInt y = 0; y < aH; ++y)
		for (TInt x = 0; x < aW; ++x)
			{
			const TInt dx = x < r ? r - x : (x >= aW - r ? x - (aW - r - 1) : 0), dy = y - r;
			if (dx * dx + dy * dy <= r * r)
				Fb[(aCy - r + y) * KPanelW + aCx - aW / 2 + x] = aArgb;
			}
	}

static void KeyBarDraw()
	{
	const TInt y0 = KPanelH - KStripH, cy = y0 + KStripH / 2, third = KPanelW / 3;
	Fill(0, y0, KPanelW, KStripH, 0xFF000000);
	Pill(third / 2, cy, 150, 44, 0xFF1FA83A);						// green: call / answer
	for (TInt i = 0; i < 9; ++i)									// Menu: the S^3 key's 3x3 grid
		Fill(KPanelW / 2 - 25 + (i % 3) * 19, cy - 25 + (i / 3) * 19, 12, 12, 0xFFFFFFFF);
	Pill(KPanelW - third / 2, cy, 150, 44, 0xFFD62B2B);				// red: end / reject
	}

static void StripEnable()
	{
	// (the top bar: A64BarsStart below)
	for (TInt i = KPanelW * KStripH; i < KPanelW * (KPanelH - KStripH); ++i)
		Fb[i] &= 0x00FFFFFF;										// see-through between the bars
	KeyBarDraw();
	a64_de_ui_channel_init(2, (const void*)FbPhys, KPanelW * KPanelH * 4, KPanelW, KPanelH, 0, 0);
	volatile TUint32* mixer = (volatile TUint32*)ph_a64_de + 0x100000 / 4;
	mixer[0x1080 / 4] = 0x21;							// BLD_CH_RTCTL: pipe 0 = channel 1, pipe 1 = channel 2
	mixer[0x1000 / 4] = (1u << 8) | (1u << 9) | 1u;		// BLD_FILL_COLOR_CTL: pipes 0 + 1 on, pipe 0 fill
	mixer[0x8 / 4] = 1;									// GLB_DBUFFER: apply
	if (!Resuming)		// (its timer and DFC outlive a screen-off)
		A64BarsStart();
	ekseam_puts("[EKERN][disp] status strip + soft key bar on (UI channel 2, whole panel, see-through middle)\n");
	}


// From a64fb.cpp (the driver's register model), any thread; before bring-up it only records the source.
static void MapSourceForCapture();
// While the NGA display channel is open (a64dispchan.cpp) IT owns what the DE shows. The legacy driver keeps writing
// FB_BASE -- Syborg's DLcdPowerHandler::PowerUpLcd re-programs it on EVERY wake from the display timeout -- and that
// write used to re-point the DE at the legacy video RAM, which nobody draws into under NGA: backlight on, black panel,
// until the next composed frame happened to flip back (user, 2026-10-09: "only the backlight turns on on touch").
static TBool ChannelOwnsDisplay;
extern "C" void A64DisplayChannelOwns(TBool aOwns) { ChannelOwnsDisplay = aOwns; }

extern "C" void A64DisplaySetSource(TUint32 aPhys, TInt aW, TInt aH)
	{
	if (ChannelOwnsDisplay)
		{
		ekseam_puts("[EKERN][disp] legacy FB_BASE while the display channel owns the screen: not shown\n");
		return;
		}
	ekseam_puts("[EKERN][disp] SetSource "); ekseam_puthex(aPhys);
	SrcPhys = aPhys; SrcW = aW; SrcH = aH;
	MapSourceForCapture();					// (thread context: the FB driver's register write)
	TInt irq = __SPIN_LOCK_IRQSAVE(VblankLock);
	if (DisplayReady)
		ProgramSource();
	__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
	}
// For phlink's screenshot ('S', PhoneLab): which 0 = the picture the OS draws (the window server's buffer, or the
// test pattern before it speaks), 1 = this file's own 720x1440 panel layer (status strip + soft key bar). XRGB8888.
// Returns a LINEAR address of a mapping made here in thread context: phlink runs in a DFC, and creating a
// DPlatChunkHw over this RAM from there hung the phone (2026-10-08, the first capture attempt).
static TLinAddr SrcLin;
static TPhysAddr SrcLinPhys;
// the window server's video RAM mapping, made once: the display channel flips away from it and back (a64fb.cpp), and
// a second DPlatChunkHw over the same RAM hangs the phone
static TLinAddr LegacyLin;
static TPhysAddr LegacyLinPhys;
static void MapSourceForCapture()
	{
	if (!SrcPhys || SrcPhys == SrcLinPhys)
		return;
	if (LegacyLin && SrcPhys == LegacyLinPhys)
		{
		SrcLin = LegacyLin;
		SrcLinPhys = SrcPhys;
		return;
		}
	if (SrcPhys == FbPhys)
		{
		// the source IS this file's buffer (the text shell): reuse its mapping -- a second DPlatChunkHw over the
		// same RAM hung the phone (twice, 2026-10-08)
		SrcLin = (TLinAddr)Fb;
		SrcLinPhys = Fb ? SrcPhys : 0;
		return;
		}
	NKern::ThreadEnterCS();
	ekseam_puts("[EKERN][disp] capture map ");
	ekseam_puthex(SrcPhys);
	// the SAME attributes svpframebuffer.cpp maps this video RAM with (EMapAttrBufferedC): a strongly-ordered
	// alias of cached RAM (the first attempt) hung the phone
	const TLinAddr lin = Map(5, SrcPhys & ~0xFFFu, Kern::RoundToPageSize((TInt)(SrcPhys & 0xFFF) + SrcW * SrcH * 4),
		EMapAttrSupRw | EMapAttrBufferedC);
	NKern::ThreadLeaveCS();
	ekseam_puts("[EKERN][disp] capture map lin=");
	ekseam_puthex((TUint32)lin);
	if (lin)
		{
		SrcLin = lin + (SrcPhys & 0xFFF);
		SrcLinPhys = SrcPhys;
		LegacyLin = SrcLin;
		LegacyLinPhys = SrcPhys;
		// The video RAM comes straight from the page allocator and a warm reset (every phlink push) keeps RAM: it still
		// holds the previous boot's last frame (privacy, and "imprints" under the new image). Black it, once, before the
		// DE is pointed at it (ProgramSource follows); clean the cache so the scan-out sees it. (bug-backlog.md "Display")
		const TInt bytes = SrcW * SrcH * 4;
		memclr((TAny*)SrcLin, bytes);
		Cache::SyncMemoryBeforeDmaWrite(SrcLin, bytes);
		ekseam_puts("[EKERN][disp] source video RAM cleared\n");
		}
	}
// ---- capture of what the DE REALLY scans out (2026-10-16). The OS picture is whatever buffer UI channel 1 points at: the
// legacy video RAM, the display channel's composition buffers (mapped by the PDD), or -- with ALF's alfopenwfrs/libWFC --
// USER buffers the kernel never mapped (PostUserBuffer passes no linear address). Read the live channel-1 address and map
// that RAM on demand: a read-only intent mapping (EMapAttrSupRw | EMapAttrBufferedC, the same cache attributes as every other
// mapping of this RAM; there is no Ro constant), made on the display DFC thread (a DPlatChunkHw created from phlink's own
// DFC hung the phone once), cached per physical buffer (a second mapping of the same RAM hung it twice), 4 kept.
static volatile TUint32* Mixer();
static DPlatChunkHw* CapChunk[4];
static TPhysAddr CapPhys[4];
static TLinAddr CapLin[4];
static TInt CapSize[4], CapNext;
static TDfc* CapDfc;							// on DispQ: makes the mapping below
static volatile TPhysAddr CapReqPhys;
static volatile TInt CapReqSize;
static volatile TLinAddr CapReqLin;
static volatile TInt CapReqState;				// 0 idle, 1 requested, 2 done

static TLinAddr CapFind(TPhysAddr aPhys, TInt aBytes)
	{
	for (TInt i = 0; i < 4; ++i)
		if (CapLin[i] && CapPhys[i] == aPhys && CapSize[i] >= aBytes)
			return CapLin[i];
	return 0;
	}

static void CapMapFn(TAny*)
	{
	const TPhysAddr phys = CapReqPhys;
	const TInt bytes = CapReqSize;
	TLinAddr lin = CapFind(phys, bytes);
	if (!lin)
		{
		NKern::ThreadEnterCS();
		TInt slot = -1;
		for (TInt i = 0; i < 4 && slot < 0; ++i)
			if (CapPhys[i] == phys)
				slot = i;						// the same buffer, now larger: replace
		if (slot < 0)
			slot = CapNext++ & 3;
		if (CapChunk[slot])
			{
			CapLin[slot] = 0;
			CapChunk[slot]->Close(NULL);
			CapChunk[slot] = NULL;
			}
		DPlatChunkHw* c = NULL;
		const TInt len = Kern::RoundToPageSize((TInt)(phys & 0xFFF) + bytes);
		TInt r = DPlatChunkHw::New(c, phys & ~0xFFFu, len, EMapAttrSupRw | EMapAttrBufferedC);
		if (r == KErrNone)
			{
			CapChunk[slot] = c;
			CapPhys[slot] = phys;
			CapSize[slot] = bytes;
			CapLin[slot] = c->LinearAddress() + (phys & 0xFFF);
			lin = CapLin[slot];
			}
		NKern::ThreadLeaveCS();
		ekseam_puts("[EKERN][disp] capture: mapped scan-out buffer "); ekseam_puthex(phys);
		ekseam_puts("[EKERN][disp]   lin="); ekseam_puthex(lin);
		}
	CapReqLin = lin;
	CapReqState = 2;
	}

// the linear address of a scan-out buffer (thread context; may wait ~ms for the display thread); 0 = could not
static TLinAddr ScanoutLin(TPhysAddr aPhys, TInt aBytes)
	{
	if (aPhys == SrcLinPhys && SrcLin)
		return SrcLin;
	if (LegacyLin && aPhys == LegacyLinPhys)
		return LegacyLin;
	if (aPhys == FbPhys && Fb)
		return (TLinAddr)Fb;
	TLinAddr lin = CapFind(aPhys, aBytes);
	if (lin || !CapDfc)
		return lin;
	if (CapReqState == 2)
		CapReqState = 0;						// (a request that timed out earlier has finished since)
	TUint32 expect = 0;
	if (!__e32_atomic_cas_ord32(&CapReqState, &expect, 1))
		return 0;								// another capture is mapping
	CapReqPhys = aPhys;
	CapReqSize = aBytes;
	CapDfc->Enque();
	for (TInt i = 0; i < 100 && CapReqState != 2; ++i)
		NKern::Sleep(NKern::TimerTicks(2));
	lin = CapReqState == 2 ? CapReqLin : 0;
	if (CapReqState == 2)
		CapReqState = 0;						// (on a timeout the DFC still finishes and the state stays 1 until then: a later capture finds the mapping)
	return lin;
	}

extern "C" TInt A64DisplayCaptureSource(TInt aWhich, TLinAddr* aLin, TInt* aW, TInt* aH)
	{
	if (!Fb)
		return KErrNotReady;
	TLinAddr lin = 0;
	if (aWhich == 0 && SrcPhys && SrcW > 0 && SrcH > 0)
		{
		// what UI channel 1 scans NOW (the live register: a posted buffer the DE has not latched yet is shown by the
		// next frame); before the display is up, the recorded source
		TPhysAddr phys = SrcPhys;
		if (DisplayReady && Mixer())
			{
			const TUint32 reg = Mixer()[(0x3000 + 0x10) / 4];
			if (reg >= 0x40000000u)
				phys = reg;
			}
		lin = ScanoutLin(phys, SrcW * SrcH * 4);
		if (!lin)
			{
			ekseam_puts("[EKERN][disp] capture: scan-out buffer "); ekseam_puthex(phys);
			ekseam_puts("[EKERN][disp]   has no mapping (busy / failed): the panel layer instead\n");
			}
		}
	const TBool os = lin != 0;
	*aLin = os ? lin : (TLinAddr)Fb;
	*aW = os ? SrcW : KPanelW;
	*aH = os ? SrcH : KPanelH;
	return KErrNone;
	}

// VBLANK + FLIP (NGA rung G1a): the display PDD (a64fb.cpp, DDisplayPddA64) posts
// a buffer by pointing UI channel 1 at it + GLB_DBUFFER, and its post completes on the frame after the DE latched it.
// TCON0 runs the DSI panel in trigger mode, so the per-frame interrupt is TRI_FINISH (Linux sun4i_tcon: GINT0 bit 27
// enable, bit 11 status, write 0 to clear; GIC SPI 86), verified firing on the phone 2026-10-09. The IRQ is only
// enabled while a flip is armed (no 60 Hz wakeups when nothing changes).
static const TInt KIntIdTcon0 = 32 + 86;
static const TUint32 KTriFinishEn = 1u << 27, KTriFinishInt = 1u << 11;
static TDfc* VblankDfc;					// the PDD's completion DFC
static TBool VblankBound, VblankMeasuring;
static volatile TUint32 FlipArmed, FlipAge, VblankCount, FlipForced;
static volatile TUint32 FlipCount, FlipDoneAt[4];	// posts, and how many completed at the 1st/2nd/3rd vblank (or forced)

static volatile TUint32& Gint0() { return *(volatile TUint32*)(ph_a64_io + 0xC004); }
static volatile TUint32* Mixer() { return (volatile TUint32*)ph_a64_de + 0x100000 / 4; }
// the DE2 overlay (VI channel, a64deov.cpp; de2-overlay-plan.md): its own armed flag + completion DFC, same vblank
static TDfc* OvDfc;
static volatile TUint32 OvArmed, OvFlipsDone;

static void Tcon0Isr(TAny*)
	{
	TInt irq = __SPIN_LOCK_IRQSAVE(VblankLock);
	Gint0() &= ~KTriFinishInt;				// (the other status bits are written back as 1 = unchanged)
	++VblankCount;
	if (OvArmed)
		{
		OvArmed = 0;						// (like the UI flip: the DE latches GLB_DBUFFER at the next frame start)
		++OvFlipsDone;
		if (OvDfc)
			OvDfc->Add();
		}
	if (FlipArmed)
		{
		// The frame that was scanning when the flip was armed has just finished: the buffer it showed is free, and
		// the DE loads the new address at the next frame start (GLB_DBUFFER; it reads back set until then -- every
		// one of 216 posts on 2026-10-09 saw it still set here and cleared at the NEXT TRI_FINISH, so waiting for
		// it halved the compositor to 20 posts/s). Complete now, as Linux's sun4i does (its flip event is the
		// next vblank interrupt after the commit). The stats keep the old classification: done@1 = the bit was
		// already clear here, @2 = still set (the usual case), forced = never seen clear (tracked on later frames).
		++FlipDoneAt[(Mixer()[0x8 / 4] & 1) ? 2 : 1];
		FlipArmed = 0;
		if (VblankDfc)
			VblankDfc->Add();
		}
	if (!FlipArmed && !OvArmed && !VblankMeasuring)
		Gint0() &= ~KTriFinishEn;
	__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
	}

// The PDD's DFC, queued at the vblank that completes a flip (ISR context: TDfc::Add).
extern "C" void A64DisplaySetVblankDfc(TDfc* aDfc) { VblankDfc = aDfc; }

// What PhoneLab's screenshot reads: aLin = a kernel mapping of aPhys (the channel's buffers), 0 = the legacy video
// RAM's mapping above if aPhys is that RAM, else none (the capture falls back to the panel layer). Never a stale
// address: a closed channel's chunk is unmapped.
static void SetCaptureLin(TUint32 aPhys, TLinAddr aLin)
	{
	if (!aLin && LegacyLin && aPhys == LegacyLinPhys)
		aLin = LegacyLin;
	SrcLin = aLin;
	SrcLinPhys = aLin ? aPhys : 0;
	}

// Show aPhys (aLin: SetCaptureLin). ETrue = the post completes at a vblank (the
// DFC above); EFalse = it took effect now (display not up yet, or the size changed: full reprogram), complete it now.
extern "C" TBool A64DisplayFlip(TUint32 aPhys, TLinAddr aLin, TInt aW, TInt aH)
	{
	TInt irq = __SPIN_LOCK_IRQSAVE(VblankLock);
	if (!DisplayReady || !VblankBound || aW != SrcW || aH != SrcH)
		{
		// (display not up, or down for a screen-off: only record what to show -- a resume programs it)
		SrcPhys = aPhys; SrcW = aW; SrcH = aH;
		SetCaptureLin(aPhys, aLin);
		if (DisplayReady)
			ProgramSource();
		__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
		return EFalse;
		}
	SrcPhys = aPhys;
	SetCaptureLin(aPhys, aLin);
	Mixer()[(0x3000 + 0x10) / 4] = aPhys;	// OVL_UI_TOP_LADD, UI channel 1 (the scaler/blender setup is unchanged)
	Mixer()[0x8 / 4] = 1;					// GLB_DBUFFER: load at the next frame start
	FlipArmed = 1;
	FlipAge = 0;
	const TBool first = ++FlipCount == 1;
	Gint0() |= KTriFinishEn;
	__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
#ifdef A64_PANEL_HEALTH
	if (first && HealthDfc)
		HealthDfc->Add();
#else
	(void)first;
#endif
	return ETrue;
	}

extern "C" TUint32 A64DisplayVblankCount() { return VblankCount; }
extern "C" TUint32 A64DisplayFlipForced() { return FlipForced; }

// The flip statistics for the periodic monitor line (a64_glue.cpp [mon]): posts since boot and the vblank each
// completed at -- 0 = at once (display not up), 1 = the first TRI_FINISH after arming, 2/3 = later, forced = never
// saw GLB_DBUFFER clear. The compositor's real frame rate = posts per 5 s.
extern "C" void A64DisplayFlipStats()
	{
	ekseam_puts("[EKERN][mon] disp posts="); ekseam_puthex(FlipCount);
	ekseam_puts(" vblanks="); ekseam_puthex(VblankCount);
	ekseam_puts(" done@1="); ekseam_puthex(FlipDoneAt[1]);
	ekseam_puts(" @2="); ekseam_puthex(FlipDoneAt[2]);
	ekseam_puts(" @3="); ekseam_puthex(FlipDoneAt[3]);
	ekseam_puts(" forced="); ekseam_puthex(FlipForced);
	ekseam_puts("\n");
	}

// ==================================================================================================================
// DE2 VIDEO OVERLAY: mixer 0's VI channel (Linux channel 0, regs at mixer + 0x2000),
// with its scaler (VSU, mixer + 0x20000) and CCSC (mixer + 0xAA050), routed to blender pipe 2 (above the UI channel 1
// picture in pipe 0 and the status/key-bar channel 2 in pipe 1). Every register below is the one Linux's sun4i-drm writes:
//   sun8i_mixer.h (offsets), sun8i_vi_layer.c update_coord/update_formats/update_buffer/enable, sun8i_vi_scaler.c
//   setup/set_coeff/enable, sun8i_csc.c (yuv2rgb tables, ccsc_base), A64 config sun50i_a64_mixer0_cfg (vi_num 1, ui_num 3,
//   scaler_mask 0xf, scanline_yuv 4096, mod_rate 297 MHz). The line numbers are cited in the plan. Written by a64deov.cpp's
//   caller in thread context; all DE writes under VblankLock + DisplayReady like the UI flips.
#include "a64_vi_coeff.h"

struct TOvState
	{
	TBool iOn;
	TUint32 iY, iUV;				// bus addresses of the planes
	TInt iPitch, iSrcW, iSrcH, iDstX, iDstY, iDstW, iDstH, iFormat, iColor, iFull, iAlpha;
	TUint32 iHStep, iVStep;			// the scaler steps programmed (the coefficient set is keyed on them)
	TBool iCoeffValid;
	TUint32 iCoeffH, iCoeffV;
	TInt iScaleMode;
	TUint32 iFlips;
	};
static TOvState Ov;

// Linux sun8i_csc.c:23-48 yuv2rgb[range][encoding] (two's complement, 10 fractional bits): rows of 3 factors + constant
static const TUint32 KYuv2Rgb[2][2][12] =
	{
	{ // limited
	{ 0x000004A8, 0x00000000, 0x00000662, 0xFFFC8451, 0x000004A8, 0xFFFFFE6F, 0xFFFFFCC0, 0x00021E4D, 0x000004A8, 0x00000811, 0x00000000, 0xFFFBACA9 },	// BT.601
	{ 0x000004A8, 0x00000000, 0x0000072B, 0xFFFC1F99, 0x000004A8, 0xFFFFFF26, 0xFFFFFDDF, 0x00013383, 0x000004A8, 0x00000873, 0x00000000, 0xFFFB7BEF }	// BT.709
	},
	{ // full
	{ 0x00000400, 0x00000000, 0x0000059B, 0xFFFD322E, 0x00000400, 0xFFFFFEA0, 0xFFFFFD25, 0x00021DD5, 0x00000400, 0x00000716, 0x00000000, 0xFFFC74BD },
	{ 0x00000400, 0x00000000, 0x0000064C, 0xFFFCD9B4, 0x00000400, 0xFFFFFF41, 0xFFFFFE21, 0x00014F96, 0x00000400, 0x0000076C, 0x00000000, 0xFFFC49EF }
	}
	};

const TUint32 KDeModClkHz = 288000000;		// PLL_DE as a64_de_init programs it (24 MHz * 24 / 2); Linux's a64 mod_rate is 297 MHz
const TInt KVi = 0x2000;					// VI channel (Linux sun8i_channel_base(mixer, 0) = DE2_CH_BASE)
const TInt KVsu = 0x20000;					// DE2_VI_SCALER_UNIT_BASE
const TInt KCcsc = 0xAA050;					// CCSC00_OFFSET
const TInt KFccAlpha = 0xAA090;				// SUN8I_MIXER_FCC_GLOBAL_ALPHA_REG
const TInt KOvPipe = 2;						// blender pipe of the overlay (above UI1 = pipe 0 and the bars = pipe 1)

static inline void MW(TInt aOff, TUint32 aV) { Mixer()[aOff / 4] = aV; }
static inline TUint32 MR(TInt aOff) { return Mixer()[aOff / 4]; }

// sun8i_vi_scaler.c:sun8i_vi_scaler_coef_index (steps are 20-bit fixed point)
static TInt CoefIndex(TUint32 aStep)
	{
	const TUint32 scale = aStep >> (20 - 3);
	const TUint32 ip = scale >> 3, fp = scale & 7;
	switch (ip)
		{
		case 0: return 0;
		case 1: return (TInt)fp;
		case 2: return 8 + (TInt)(fp >> 1);
		case 3: return 12;
		case 4: return 13;
		default: return 14;
		}
	}

// sun8i_vi_scaler.c:sun8i_vi_scaler_set_coeff for a 4:2:0 format (chroma: bicubic8 left/right + bicubic4; luma H: lan3
// left/right, luma V: lan2). The vertical index follows the VERTICAL step (Linux 6.6 reuses hstep there -- see the plan).
static void WriteCoeffs(TUint32 aHStep, TUint32 aVStep)
	{
	const TInt ho = CoefIndex(aHStep) * 32, vo = CoefIndex(aVStep) * 32;
	for (TInt i = 0; i < 32; ++i)
		{
		MW(KVsu + 0x200 + 4 * i, KVi_lan3coefftab32_left[ho + i]);		// YHCOEFF0
		MW(KVsu + 0x300 + 4 * i, KVi_lan3coefftab32_right[ho + i]);		// YHCOEFF1
		MW(KVsu + 0x600 + 4 * i, KVi_bicubic8coefftab32_left[ho + i]);	// CHCOEFF0
		MW(KVsu + 0x700 + 4 * i, KVi_bicubic8coefftab32_right[ho + i]);	// CHCOEFF1
		MW(KVsu + 0x400 + 4 * i, KVi_lan2coefftab32[vo + i]);			// YVCOEFF
		MW(KVsu + 0x800 + 4 * i, KVi_bicubic4coefftab32[vo + i]);		// CVCOEFF
		}
	}

// All state in Ov is final; program everything (VblankLock held, DisplayReady). aAll = geometry + format too, else only the addresses.
static void OvProgram(TBool aAll)
	{
	const TInt pitch = Ov.iPitch;
	if (aAll)
		{
		const TInt sw = Ov.iSrcW, sh = Ov.iSrcH, dw = Ov.iDstW, dh = Ov.iDstH;
		// -- vi_layer_update_coord: sizes, then coarse scaling decision
		const TUint32 insize = WH(sw, sh), outsize = WH(dw, dh);
		MW(KVi + 0x4, insize);											// VI_LAYER_SIZE(layer 0)
		MW(KVi + 0xe8, insize);											// VI_OVL_SIZE
		// vertical coarse scaling: the VSU can do `ability` percent of the needed height ratio (BSP: 80 % efficiency)
		TUint32 hn = 0, hm = 0, vn = 0, vm = 0;
		TInt srcw = sw, srch = sh;
		const TUint32 fps = 60, vdisp = (TUint32)KPanelH;
		// (Linux: ability = clk * 80 / (vdisplay * fps * max(src_w, dst_w)); 32-bit here: kHz, and /1000 on the divisor)
		const TUint32 ability = (KDeModClkHz / 1000 * 80) / (vdisp * fps * (TUint32)Max(sw, dw) / 1000);
		const TUint32 required = (TUint32)srch * 100 / (TUint32)dh;
		Ov.iScaleMode = 1;
		if (ability < required)
			{
			vm = (TUint32)srch;
			vn = (TUint32)ability * (TUint32)dh / 100;
			srch = (TInt)vn;
			Ov.iScaleMode = 2;
			}
		if (srcw > 4096)								// scanline_yuv of the A64 mixer 0
			{
			hm = (TUint32)srcw;
			hn = 4096;
			srcw = 4096;
			Ov.iScaleMode = Ov.iScaleMode == 2 ? 4 : 3;
			}
		const TUint32 hscale = ((TUint32)srcw << 16) / (TUint32)dw, vscale = ((TUint32)srch << 16) / (TUint32)dh;
		// -- vi_scaler_setup (phases 0: no source sub-pixel offset)
		const TUint32 hs = hscale << (20 - 16), vs = vscale << (20 - 16);
		const TUint32 chphase = 0 >> 1, cvphase = (0u >> 1) - (1u << (20 - 2));		// 4:2:0 chroma (BSP): -0.25
		MW(KVsu + 0x40, outsize);										// OUTSIZE
		MW(KVsu + 0x80, WH(srcw, srch));								// YINSIZE
		MW(KVsu + 0x88, hs);											// YHSTEP
		MW(KVsu + 0x8c, vs);											// YVSTEP
		MW(KVsu + 0x90, 0);												// YHPHASE
		MW(KVsu + 0x98, 0);												// YVPHASE
		MW(KVsu + 0xc0, WH(srcw / 2, srch / 2));						// CINSIZE
		MW(KVsu + 0xc8, hs / 2);										// CHSTEP
		MW(KVsu + 0xcc, vs / 2);										// CVSTEP
		MW(KVsu + 0xd0, chphase);										// CHPHASE
		MW(KVsu + 0xd8, cvphase);										// CVPHASE
		if (!Ov.iCoeffValid || Ov.iCoeffH != hs || Ov.iCoeffV != vs)
			{
			WriteCoeffs(hs, vs);
			Ov.iCoeffValid = ETrue;
			Ov.iCoeffH = hs;
			Ov.iCoeffV = vs;
			}
		Ov.iHStep = hs;
		Ov.iVStep = vs;
		MW(KVsu + 0x0, 0x11);											// CTRL: EN | COEFF_RDY (always on: subsampled chroma needs it)
		MW(KVi + 0xf0, (hn << 16) | hm);								// HDS_Y
		MW(KVi + 0xf4, (hn << 16) | hm);								// HDS_UV
		MW(KVi + 0xf8, (vn << 16) | vm);								// VDS_Y
		MW(KVi + 0xfc, (vn << 16) | vm);								// VDS_UV
		// -- blender pipe 2: destination rectangle (BLD_CH_ISIZE / OFFSET), blend mode, fill off
		MW(0x1000 + 0x8 + 0x10 * KOvPipe, outsize);
		MW(0x1000 + 0xc + 0x10 * KOvPipe, ((TUint32)Ov.iDstY << 16) | (TUint32)Ov.iDstX);
		MW(0x1090 + 4 * KOvPipe, 0x03010301);							// SUN8I_MIXER_BLEND_MODE_DEF
		// -- vi_layer_update_alpha (vi_num == 1: the FCC global alpha) and update_formats
		MW(KFccAlpha, (MR(KFccAlpha) & 0x00FFFFFF) | ((TUint32)Ov.iAlpha << 24));
		const TInt hw = Ov.iFormat == 1 ? 9 : 8;						// SUN8I_MIXER_FBFMT_NV21 : NV12
		const TUint32 attr = (MR(KVi) & ~((0x1Fu << 8) | (1u << 15))) | ((TUint32)hw << 8);	// RGB_MODE (bit 15) clear = YCbCr
		MW(KVi, attr);
		const TInt enc = (Ov.iColor == 1 || (Ov.iColor == 2 && sw >= 1280)) ? 1 : 0;
		const TUint32* tab = KYuv2Rgb[Ov.iFull ? 1 : 0][enc];
		for (TInt i = 0; i < 12; ++i)
			MW(KCcsc + 0x10 + 4 * i, tab[i]);
		MW(KCcsc, 1);													// CCSC enable
		MW(KVi + 0xc, (TUint32)pitch);									// pitches (plane 0, plane 1)
		MW(KVi + 0x10, (TUint32)pitch);
		}
	MW(KVi + 0x18, Ov.iY);												// TOP_LADDR plane 0 (Y)
	MW(KVi + 0x1c, Ov.iUV);												// plane 1 (UV)
	if (aAll)
		{
		// vi_layer_enable: layer on, blender pipe on, route pipe 2 <- channel 0 (VI)
		MW(KVi, MR(KVi) | 1);
		MW(0x1080, (MR(0x1080) & ~(0xFu << (KOvPipe * 4))) | (0u << (KOvPipe * 4)));
		MW(0x1000, MR(0x1000) | (1u << (8 + KOvPipe)));
		}
	MW(0x8, 1);															// GLB_DBUFFER: latch at the next frame start
	}

static void OvHide()
	{
	MW(KVi, MR(KVi) & ~1u);												// layer off
	MW(0x1000, MR(0x1000) & ~(1u << (8 + KOvPipe)));					// pipe off
	MW(KVsu + 0x0, 0);
	MW(0x8, 1);
	}

// The DE was reset (screen-off resume): StripEnable rewrote the blender routing, put the overlay back if it was on.
static void OvReapply()
	{
	Ov.iCoeffValid = EFalse;
	if (Ov.iOn)
		OvProgram(ETrue);
	}

// ---- the interface a64deov.cpp uses (thread context)
extern "C" void A64DeOvSetDfc(TDfc* aDfc) { OvDfc = aDfc; }
extern "C" void A64DeOvInfo(TInt* aW, TInt* aH, TInt* aOn, TUint32* aVblanks, TUint32* aFlips, TUint32* aDone, TInt* aMode)
	{
	*aW = KPanelW; *aH = KPanelH; *aOn = Ov.iOn; *aVblanks = VblankCount; *aFlips = Ov.iFlips; *aDone = OvFlipsDone;
	*aMode = Ov.iOn ? Ov.iScaleMode : 0;
	}

// aGeom: a full Set (everything); else only a new pair of plane addresses. ETrue = armed for a vblank completion; EFalse =
// recorded only (display off / not up) or taken now: complete at once.
extern "C" TBool A64DeOvApply(TBool aGeom, TUint32 aY, TUint32 aUV, TInt aPitch, TInt aSrcW, TInt aSrcH,
	TInt aDstX, TInt aDstY, TInt aDstW, TInt aDstH, TInt aFormat, TInt aColor, TInt aFull, TInt aAlpha)
	{
	TInt irq = __SPIN_LOCK_IRQSAVE(VblankLock);
	Ov.iY = aY; Ov.iUV = aUV;
	if (aGeom)
		{
		Ov.iPitch = aPitch; Ov.iSrcW = aSrcW; Ov.iSrcH = aSrcH; Ov.iDstX = aDstX; Ov.iDstY = aDstY; Ov.iDstW = aDstW;
		Ov.iDstH = aDstH; Ov.iFormat = aFormat; Ov.iColor = aColor; Ov.iFull = aFull; Ov.iAlpha = aAlpha;
		Ov.iOn = ETrue;
		}
	if (!Ov.iOn)
		{
		__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
		return EFalse;
		}
	if (!DisplayReady)
		{
		__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
		return EFalse;											// a resume re-applies (OvReapply)
		}
	OvProgram(aGeom);
	++Ov.iFlips;
	TBool armed = EFalse;
	if (VblankBound)
		{
		OvArmed = 1;
		Gint0() |= KTriFinishEn;
		armed = ETrue;
		}
	__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
	return armed;
	}

extern "C" void A64DeOvDisable()
	{
	TInt irq = __SPIN_LOCK_IRQSAVE(VblankLock);
	if (Ov.iOn && DisplayReady)
		OvHide();
	Ov.iOn = EFalse;
	__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
	}

// ReadReg for tests: aOff from the mixer-0 base; the allow-list is a64deov.cpp's. -1 = display not up.
extern "C" TInt A64DeOvReadReg(TUint32 aOff, TUint32* aVal)
	{
	TInt irq = __SPIN_LOCK_IRQSAVE(VblankLock);
	TInt r = KErrNotReady;
	if (DisplayReady)
		{
		*aVal = MR((TInt)aOff);
		r = KErrNone;
		}
	__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
	return r;
	}

// End of the bring-up (DFC thread): bind the TCON0 interrupt and measure the frame rate once.
static void VblankInit()
	{
	TInt r = Interrupt::Bind(KIntIdTcon0, Tcon0Isr, 0);
	if (r == KErrNone)
		r = Interrupt::Enable(KIntIdTcon0);
	if (r != KErrNone)
		{
		ekseam_puts("[EKERN][disp] vblank IRQ bind failed r="); ekseam_puthex((unsigned)r);
		return;
		}
	// flips are vblank-paced from here on: during the 1 s rate measurement below the window server's first posts
	// used to take the immediate path (a full UI-channel reprogram mid-scan, three of them right after DISPLAY UP) --
	// the "flicker when the homescreen shows up" (user, 2026-10-09)
	VblankBound = ETrue;
	VblankMeasuring = ETrue;
	const TUint32 c0 = VblankCount;
	Gint0() = (Gint0() & ~KTriFinishInt) | KTriFinishEn;
	NKern::Sleep(NKern::TimerTicks(1000));
	VblankMeasuring = EFalse;
	ekseam_puts("[EKERN][disp] vblank (TCON0 TRI_FINISH) per second: "); ekseam_puthex(VblankCount - c0);
	}

// Mixer 1 (DE + 0x200000) is never used (the PinePhone routes mixer 0 only) but a warm reset keeps whatever the previous
// OS left in it. Its clocks are NOT enabled by a64_de_init (touching an ungated block hangs the bus): open core 1's
// SCLK/HCLK gates + HCLK reset (DE top: +0x00 / +0x04 / +0x08, bit 1), zero its 0x6000-byte window, put the three bits
// back. A64_NO_MIXER1_CLEAR=1 compiles it out. The secure video RAM needs no eager clear: MapSourceForCapture blacks any
// RAM it maps (the secure buffer is mapped, and cleared, the first time it is selected).
static void ClearMixer1()
	{
#ifndef A64_NO_MIXER1_CLEAR
	volatile TUint32* top = (volatile TUint32*)ph_a64_de;
	const TUint32 bit = 1u << 1;
	const TUint32 s0 = top[0] & bit, h0 = top[1] & bit, r0 = top[2] & bit;
	top[0] |= bit;			// SCLK_GATE
	top[2] |= bit;			// AHB_RESET (1 = reset off)
	top[1] |= bit;			// HCLK_GATE
	volatile TUint32* m1 = (volatile TUint32*)ph_a64_de + 0x200000 / 4;
	for (TInt i = 0; i < 0x6000 / 4; ++i)
		m1[i] = 0;
	top[1] = (top[1] & ~bit) | h0;
	top[2] = (top[2] & ~bit) | r0;
	top[0] = (top[0] & ~bit) | s0;
	ekseam_puts("[EKERN][disp] mixer 1 cleared\n");
#endif
	}

static TBool DisplayUpBody()
	{
	DisplayReady = EFalse;
	ekseam_puts("[EKERN][disp] A64 display bring-up (NuttX sequence) starting\n");
	NKern::ThreadEnterCS();
	// (a retry of the whole bring-up, below, must not map / allocate twice)
	if (!ph_a64_de) ph_a64_de  = Map(0, 0x01000000, 0x300000);
	if (!ph_a64_io) ph_a64_io  = Map(1, 0x01C00000, 0x22000);
	if (!ph_a64_dsi) ph_a64_dsi = Map(2, 0x01CA0000, 0x2000);
	if (!ph_a64_r) ph_a64_r = Map(3, 0x01F00000, 0x4000);	// (A64PmicMap may have mapped it already)
	const TInt fbSize = Kern::RoundToPageSize(KFbBytes);
	TInt r = KErrNone;
	if (!Fb)
		{
		r = Epoc::AllocPhysicalRam(fbSize, FbPhys, 12);
		if (r == KErrNone)
			Fb = (TUint32*)Map(4, FbPhys, fbSize);
		}
	NKern::ThreadLeaveCS();
	if (!ph_a64_de || !ph_a64_io || !ph_a64_dsi || !ph_a64_r || !Fb)
		{
		ekseam_puts("[EKERN][disp] mapping failed, giving up\n");
		return EFalse;
		}
	ekseam_puts("[EKERN][disp] framebuffer phys="); ekseam_puthex(FbPhys);
	if (!Resuming)
		{
		TestPattern();
		ekseam_puts("[EKERN][disp] test pattern drawn\n");
		NKern::Sleep(NKern::TimerTicks(500));
		}

	// A WARM reboot (every phlink push = SoC reset with the rails up) leaves TCON0 / DSI+D-PHY / DE running with the
	// previous boot's state. NuttX and p-boot only ever DE-ASSERT the bus resets (a no-op then), so their sequence
	// only works from a cold boot or after Linux (seen 2026-10-06: black after every push, colour bars after a
	// Mobian-triggered reboot). Linux asserts the resets first -- do the same: resets asserted, module clocks and
	// gates off, then the untouched sequence re-enables everything from a known state.
	{
	volatile TUint32* ccu = (volatile TUint32*)(ph_a64_io + 0x20000);
	ccu[0x2c4 / 4] &= ~((1u << 3) | (1u << 12));		// BUS_SOFT_RST1: TCON0, DE into reset
	ccu[0x2c0 / 4] &= ~(1u << 1);						// BUS_SOFT_RST0: MIPI DSI (+ D-PHY) into reset
	ccu[0x064 / 4] &= ~((1u << 3) | (1u << 12));		// bus gates off: TCON0, DE
	ccu[0x060 / 4] &= ~(1u << 1);						// bus gate off: MIPI DSI
	ccu[0x118 / 4] = 0;									// TCON0 module clock off
	ccu[0x104 / 4] &= ~(1u << 31);						// DE module clock off
	ccu[0x168 / 4] &= ~(1u << 15);						// MIPI DSI (D-PHY) module clock off
	ph_mdelay(2);
	ekseam_puts("[EKERN][disp] display blocks held in reset (warm-boot safe)\n");
	}
	if (!Resuming && !Step("backlight", pinephone_lcd_backlight_enable(90))) return EFalse;	// (a resume lights it last)
	if (!Step("tcon0",a64_tcon0_init(KPanelW, KPanelH))) return EFalse;
	if (!Step("panel reset low", pinephone_lcd_panel_reset(false))) return EFalse;
	RsbLock();												// the battery monitor shares the bus (A64PmicReadBatt)
	if (!Step("rsb init (bus to the AXP803)", RsbInit())) { RsbUnlock(); return EFalse; }
	// Warm reboot: the panel's rails stayed up across the SoC reset and a PD23 pulse alone does not recover an ST7703
	// that was mid-video (cold boot = colour bars, warm = black, 2026-10-06). Linux's panel driver power-cycles the
	// rails on every prepare: do the same, reset held low. AXP803: 0x12 bits 3/4 = DLDO1 (VCC-DSI) / DLDO2 (panel
	// IOVCC 1.8 V); 0x90 = 7 = GPIO0-LDO (panel VCC 3.3 V) off. pinephone_pmic_init re-enables all three.
	if (!Resuming)		// (a resume comes from a screen-off that already had DLDO1/2 off and the panel in reset)
	{
	const TInt v = a64_rsb_read(0x2d, 0x12);
	if (v >= 0)
		a64_rsb_write(0x2d, 0x12, (TUint8)(v & ~0x18));
	a64_rsb_write(0x2d, 0x90, 0x07);
	// 500 ms (was 200): black-with-backlight boots after warm resets (2026-10-08, boot 39) had every panel readback
	// right (0xB9 = f1, 0x05 = 0, 0x0A = 9c) and every SoC register identical to a good boot -- the panel lost the
	// HS video stream. Suspect: the rails not fully drained, the ST7703 keeping state. Give them longer.
	ph_mdelay(500);
	ekseam_puts("[EKERN][disp] panel rails power-cycled (warm-boot safe)\n");
	}
	const TBool railsOk = Step("pmic (AXP803 rails)", pinephone_pmic_init());
	RsbUnlock();
	if (!railsOk) return EFalse;
	ph_mdelay(15);
	if (!Step("mipi dsi enable", a64_mipi_dsi_enable())) return EFalse;
	if (!Step("mipi d-phy enable", a64_mipi_dphy_enable())) return EFalse;
	if (!Step("panel reset high", pinephone_lcd_panel_reset(true))) return EFalse;
	ph_mdelay(15);
	if (!Step("st7703 panel init", pinephone_lcd_panel_init())) return EFalse;
	// did the panel take the LONG packets? SETEXTC (0xB9) reads back F1 once unlocked (00 = long writes are lost)
	Step("panel SETEXTC readback (0xB9; want 0xf1)", PanelRead(0xB9));
	Step("panel DSI error count (0x05)", PanelRead(0x05));
	Step("panel power mode (0x0A; want 0x9c)", PanelRead(0x0A));
	if (!Step("mipi dsi start (HSC + HSD)", a64_mipi_dsi_start())) return EFalse;
	if (!Step("display engine init", a64_de_init())) return EFalse;
	ClearMixer1();
	ph_mdelay(160);
	if (!Step("blender", a64_de_blender_init())) return EFalse;
	if (!Step("ui channel 1", a64_de_ui_channel_init(1, (const void*)FbPhys, KFbBytes, KPanelW, KPanelH, 0, 0))) return EFalse;
	if (!Step("ui channel 2 off", a64_de_ui_channel_init(2, 0, 0, 0, 0, 0, 0))) return EFalse;
	if (!Step("ui channel 3 off", a64_de_ui_channel_init(3, 0, 0, 0, 0, 0, 0))) return EFalse;
	ProgramSource();
	if (!Step("de enable", a64_de_enable(1))) return EFalse;
	{
	// under the vblank lock: the flips that arrived while the DE was down only recorded their buffer (A64DisplayFlip),
	// so show the newest one now, then let the flips drive the hardware again
	TInt irq = __SPIN_LOCK_IRQSAVE(VblankLock);
	DisplayReady = ETrue;
	if (Resuming)
		ProgramSource();
	__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
	}
	StripEnable();
	{
	TInt irq = __SPIN_LOCK_IRQSAVE(VblankLock);
	OvReapply();					// the DE2 video overlay (StripEnable rewrote the blender routing)
	__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
	}
	ekseam_puts("[EKERN][disp] DISPLAY UP\n");
	return ETrue;
	}

// the bring-up with the panel health check: after DISPLAY UP + 300 ms read the panel back; on
// a bad reading re-send 0x11/0x29 (max 2), then redo the whole bring-up (max 2 more times; its CCU reset-assert block makes
// it re-entrant), logging every attempt so PhoneLab sees how often it happens. Then the vblank/flip machinery.
static void DisplayUp(TAny*)
	{
	for (TInt attempt = 0; ; ++attempt)
		{
		if (!DisplayUpBody())
			return;				// (a failed step, as before: the log says which)
#ifndef A64_PANEL_HEALTH
		// the post-video health reads are OFF (A64DEFS=-DA64_PANEL_HEALTH = on): an LP DCS read (BTA) while HS video runs
		// stopped TCON0 -- vblanks 60/s -> 0, the panel lit and black, the UI only in the framebuffer (boots 423, 432-433)
		break;
#else
		NKern::Sleep(NKern::TimerTicks(300));
		if (PanelRecover("after DISPLAY UP"))
			break;
#endif
		if (attempt >= 2)
			{
			ekseam_puts("[EKERN][disp] panel still unhealthy after 2 full re-inits: giving up\n");
			break;
			}
		ekseam_puts("[EKERN][disp] panel unhealthy: full display re-init, attempt "); ekseam_puthex((unsigned)(attempt + 1));
		}
	VblankInit();
	}

// Brightness + blanking (display driver step 2, 2026-10-08): the backlight PWM duty (R_PWM ch0, PL10) in percent.
// The display HAL (a64fb.cpp: EDisplayHalSetDisplayBrightness / EDisplayHalSetState -> FB_BLANK) calls these; S60 reaches
// them through the HWRM light plug-in (src/phorepoc-arm/a64/hwrm/LightPlugin.cpp). Blank = backlight off (the panel
// keeps scanning: panel sleep-in needs DCS commands in video mode -- later).
static TInt BlLevel = 90;		// the bring-up's level
static TBool BlBlanked;
// the backlight really OFF (PWM 0): the screensaver's lights-off (HAL EBacklightState = 0, SysAp delta) -- a phone's
// display off. Any lights-on (unblank) clears it (user, 2026-10-15)
static TBool BlOff;
// The LED driver's floor (user, 2026-10-15, boot 445): below ~108 of the PWM's 1199 ticks the backlight is OFF (75 was
// dark, 108 the lowest lit level) -- the whole brightness range starts there, and "blank" (S60's light dimming:
// still faintly lit, user 2026-10-15) is the floor itself, never 0
static const TUint32 KBlFloorTicks = 108;
static const TUint32 KBlDimTicks = KBlFloorTicks;
// level (1..100 %) -> PWM ticks on a perceptual curve over floor..1199: floor + (1199 - floor) * ((level - 1) / 99)^2
// (the eye is most sensitive at the dark end; a linear map put most of the range in the bright half)
static TUint32 BlTicks(TInt aLevel)
	{
	const TUint32 l = aLevel < 1 ? 0 : (TUint32)(aLevel > 100 ? 99 : aLevel - 1);
	return KBlFloorTicks + ((1199u - KBlFloorTicks) * l * l + 4900u) / 9801u;
	}
static void BlApply()
	{
	if (DisplayReady)
		pinephone_lcd_backlight_ticks(BlOff ? 0 : BlBlanked ? KBlDimTicks : BlTicks(BlLevel));
	}
extern "C" void A64DisplaySetBrightness(TInt aPercent)
	{
	BlLevel = aPercent < 1 ? 1 : (aPercent > 100 ? 100 : aPercent);	// 0 = blank's job, not a level
	BlApply();
	}
extern "C" TInt A64DisplayBrightness() { return BlLevel; }

// SCREEN POWER (power plan P4, 2026-10-16). The lights-off state (BlOff: the screensaver's display off, SysAp's delta;
// the display off) now powers the display pipeline down instead of only dropping the backlight, and lights-on
// brings it all back. Off, in Linux's order (st7703 disable/unprepare + sun6i_dsi/sun4i_tcon disable):
//   backlight off (PWM gated, PH10 low)  ->  flips stop touching the DE  ->  DCS display-off (0x28), sleep-in (0x10),
//   120 ms  ->  TCON0 off (GCTL, TCON0_CTL bit 31)  ->  panel reset low (PD23)  ->  TCON0/DE/DSI(+D-PHY) in reset, bus
//   gates + module clocks off, PLL_VIDEO0 / PLL_MIPI / PLL_DE off  ->  rails DLDO2 (MIPI IO 1.8 V) and DLDO1 (DSI link,
//   unless the sensors hold it) released through the resource manager  ->  [A64_SCREENOFF_GATE_ACCEL: GPU + VE clocks].
// On = the proven bring-up replayed (DisplayUpBody with Resuming: its warm-boot reset block already puts every display
// block through reset, rails on first, no test pattern / pauses / rail cycle), the backlight last. The touch panel's
// supply (LDO_IO0) stays on so a tap still wakes the phone. Flips posted while the DE is down only record their buffer
// (A64DisplayFlip), the resume shows the newest. A64DEFS=-DA64_NO_SCREENOFF_POWER = the old backlight-only off.
static TDfc* PowerDfc;
static TBool ScreenIsOn = ETrue;				// the hardware state (only the power DFC changes it)
static volatile TBool ScreenWantOn = ETrue;		// what the lights say
extern "C" void A64TouchSetPollMs(TInt aMs);		// input/a64touch.cpp

#ifndef A64_NO_SCREENOFF_POWER
static void ScreenClocksOff()
	{
	volatile TUint32* ccu = (volatile TUint32*)(ph_a64_io + 0x20000);
	ccu[0x2c4 / 4] &= ~((1u << 3) | (1u << 12));		// BUS_SOFT_RST1: TCON0, DE into reset
	ccu[0x2c0 / 4] &= ~(1u << 1);						// BUS_SOFT_RST0: MIPI DSI (+ D-PHY) into reset
	ccu[0x064 / 4] &= ~((1u << 3) | (1u << 12));		// bus gates off: TCON0, DE
	ccu[0x060 / 4] &= ~(1u << 1);						// bus gate off: MIPI DSI
	ccu[0x118 / 4] = 0;									// TCON0 module clock off
	ccu[0x104 / 4] &= ~(1u << 31);						// DE module clock off
	ccu[0x168 / 4] &= ~(1u << 15);						// MIPI DSI (D-PHY) module clock off
	ccu[0x040 / 4] &= ~((1u << 31) | (1u << 23) | (1u << 22));	// PLL_MIPI (+ its LDOs) off; a64_tcon0_init sets them up again
	ccu[0x010 / 4] &= ~(1u << 31);						// PLL_VIDEO0 off (TCON0's dot clock source)
	ccu[0x048 / 4] &= ~(1u << 31);						// PLL_DE off (a64_de_init re-enables it)
	}

static void ScreenOffSeq()
	{
	ekseam_puts("[EKERN][power] screen OFF: panel sleep-in, display clocks, rails\n");
	pinephone_lcd_backlight_disable();
	TInt irq = __SPIN_LOCK_IRQSAVE(VblankLock);
	DisplayReady = EFalse;						// from here on no flip or source change writes a DE register
	const TBool armed = FlipArmed;
	FlipArmed = 0;
	Gint0() &= ~KTriFinishEn;
	__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
	if (armed && VblankDfc)
		VblankDfc->Add();						// (the post that was waiting for a vblank completes)
	// the panel: display off, then sleep in (the ST7703 wants 120 ms before its supplies may go)
	const unsigned char off = 0x28, slp = 0x10;
	const long r1 = a64_mipi_dsi_write(0, 0x05, &off, 1);
	ph_mdelay(20);
	const long r2 = a64_mipi_dsi_write(0, 0x05, &slp, 1);
	NKern::Sleep(NKern::TimerTicks(120));
	ekseam_puts("[EKERN][power] panel 0x28 r="); ekseam_puthex((unsigned)r1);
	ekseam_puts("[EKERN][power] panel 0x10 r="); ekseam_puthex((unsigned)r2);
	volatile TUint32* tcon = (volatile TUint32*)(ph_a64_io + 0xC000);
	tcon[0x40 / 4] &= ~(1u << 31);				// TCON0_CTL: channel 0 off (sun4i_tcon_channel_disable)
	tcon[0] &= ~(1u << 31);						// TCON_GCTL: TCON off (sun4i_tcon_disable)
	pinephone_lcd_panel_reset(false);
	ph_mdelay(2);
	ScreenClocksOff();
	A64PwrResPut(KA64ResDldo2);
	A64PwrResPut(KA64ResDldo1);
#ifdef A64_SCREENOFF_GATE_ACCEL
	A64PwrResPut(KA64ResGpuClk);
	A64PwrResPut(KA64ResVeClk);
#endif
	// a post that slipped in between the flag and the lock above has armed nothing (it saw !DisplayReady), but be sure
	irq = __SPIN_LOCK_IRQSAVE(VblankLock);
	const TBool armed2 = FlipArmed;
	FlipArmed = 0;
	__SPIN_UNLOCK_IRQRESTORE(VblankLock, irq);
	if (armed2 && VblankDfc)
		VblankDfc->Add();
	A64TouchSetPollMs(60);						// the touch only has to wake the phone: poll it slowly
	A64PwrResDump();
	ekseam_puts("[EKERN][power] screen OFF done\n");
	}

static TBool ScreenOnSeq()
	{
	ekseam_puts("[EKERN][power] screen ON: rails, display bring-up replayed\n");
#ifdef A64_SCREENOFF_GATE_ACCEL
	A64PwrResGet(KA64ResGpuClk);				// the clocks first: a stalled compositor job resumes
	A64PwrResGet(KA64ResVeClk);
#endif
	A64PwrResGet(KA64ResDldo1);
	A64PwrResGet(KA64ResDldo2);
	Resuming = ETrue;
	const TBool ok = DisplayUpBody();
	Resuming = EFalse;
	if (ok)
		{
		ph_mdelay(40);							// a couple of frames of the real picture before the light
		BlApply();
		A64TouchSetPollMs(15);
		}
	A64PwrResDump();
	ekseam_puts(ok ? "[EKERN][power] screen ON done\n" : "[EKERN][power] screen ON FAILED (display bring-up)\n");
	return ok;
	}

static void ScreenPowerFn(TAny*)
	{
	TInt fails = 0;
	while (ScreenWantOn != ScreenIsOn)
		{
		if (!ScreenWantOn)
			{
			if (!DisplayReady)
				break;							// not up (yet / any more): nothing to power down
			ScreenOffSeq();
			ScreenIsOn = EFalse;
			}
		else if (ScreenOnSeq())
			ScreenIsOn = ETrue;
		else if (++fails >= 2)
			{
			ScreenIsOn = ETrue;					// gave up: the dark panel stays, the log says why
			break;
			}
		}
	}
#endif

static void ScreenRequest(TBool aOn)
	{
#ifndef A64_NO_SCREENOFF_POWER
	ScreenWantOn = aOn;
	if (PowerDfc)
		PowerDfc->Add();
#else
	(void)aOn;
#endif
	}

extern "C" void A64DisplayBacklightOff(TBool aOff)
	{
	if (aOff == BlOff)
		return;
	BlOff = aOff;
	ekseam_puts(aOff ? "[EKERN][disp] backlight OFF (screensaver)\n" : "[EKERN][disp] backlight off cleared\n");
	BlApply();
	ScreenRequest(!aOff);
	}
extern "C" TBool A64DisplayBacklightIsOff() { return BlOff; }
extern "C" void A64DisplayBlank(TBool aBlank)
	{
	if (!aBlank && BlOff)
		{
		BlOff = EFalse;			// lights on = out of the screensaver's display-off
		ekseam_puts("[EKERN][disp] backlight off cleared (lights on)\n");
		BlApply();
		ScreenRequest(ETrue);
		}
	if (aBlank == BlBlanked)
		return;
	BlBlanked = aBlank;
	ekseam_puts(aBlank ? "[EKERN][disp] blank (backlight off)\n" : "[EKERN][disp] unblank (backlight on)\n");
	BlApply();
	}

// the panel health check once the NGA composer has flipped for the first time (300 ms later: the picture is on the
// panel by then): re-send only -- a full re-init under a running composer would be worse than the black panel
static void HealthFn(TAny*)
	{
	NKern::Sleep(NKern::TimerTicks(300));
	if (!PanelRecover("after first flip"))
		ekseam_puts("[EKERN][disp] panel still unhealthy after the first flip\n");
	}

static void DispTimerFn(TAny*) { DispDfc->Add(); }		// ISR context

// Called by the seam where the other boards load their framebuffer driver.
extern "C" void A64DisplayStart()
	{
	_LIT8(KName, "A64Disp");
	NKern::ThreadEnterCS();
	TInt r = Kern::DfcQCreate(DispQ, 24, &KName);
	if (r == KErrNone)
		{
		DispDfc = new TDfc(DisplayUp, 0, DispQ, 1);
		HealthDfc = new TDfc(HealthFn, 0, DispQ, 1);
#ifndef A64_NO_SCREENOFF_POWER
		PowerDfc = new TDfc(ScreenPowerFn, 0, DispQ, 1);
#endif
		CapDfc = new TDfc(CapMapFn, 0, DispQ, 1);
		}
	NKern::ThreadLeaveCS();
	if (r != KErrNone || !DispDfc)
		{
		ekseam_puts("[EKERN][disp] no DFC queue r="); ekseam_puthex((unsigned)r);
		return;
		}
	DispTimer.OneShot(NKern::TimerTicks(4000));
	ekseam_puts("[EKERN][disp] display bring-up queued (in 4 s)\n");
	}
