// OURS (PinePhone, docs/phorepoc/pinephone-port.md "Bars", 2026-10-08): what the kernel draws in the panel's own
// layer (a64_display.cpp: UI channel 2 over the OS picture, see-through in the middle) --
//  * the TOP BAR (rows 0-79, every image): CPU % (per-core bars + a sparkline), free RAM (+ sparkline), battery,
//    and a second line that is a progress bar while something long runs (an image upload over phlink) or the
//    latest notification (kernel faults, thread panics in red; a fault counter stays);
//  * the GOLDEN overlay (A64_GOLDEN = the text-shell image only): the Menu soft key opens a kernel-drawn menu --
//    on-screen keyboard (for ESHELL), boot image B, restart, format C: (factory reset), power off -- with a
//    confirmation for each action. Kernel-drawn on purpose: it works with no user side, or a broken one.
// One drawing context: a 1/4 s (1/25 s while the overlay is up) timer DFC on DFC queue 0 does all the drawing;
// everything else (the battery monitor, phlink, the fault seam, the touch DFC) only posts state.
// A stepping stone: the bars get an API for apps (or go away) in "productionised" images.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <e32keys.h>

extern "C" void ekseam_puts(const char* s);
extern "C" void ekseam_puthex(unsigned v);
extern "C" TUint32* A64PanelFb();
extern "C" TInt A64CpuBusy(TUint64* aLastNow, TUint64* aLastIdle, TUint* aBusy, TInt aMax);
#ifdef A64_GOLDEN
extern "C" TBool A64SdTransfer(TUint aSector, TUint aCount, TUint32* aBuf, TBool aWrite);
extern "C" TInt A64SlotRearm();
extern "C" void A64WatchdogReboot();
extern "C" TInt A64PmicPowerOff();
#endif

const TInt KW = 720, KH = 1440, KBarH = 80;
static TUint32* Fb;

// ---- the font: 5x7, ASCII 0x20-0x7E, column bytes (bit 0 = top row)
static const TUint8 KFont[95][5] = {
	{0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
	{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
	{0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
	{0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
	{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
	{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
	{0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
	{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
	{0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
	{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},{0x3E,0x41,0x41,0x51,0x32},
	{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
	{0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
	{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
	{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},
	{0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
	{0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
	{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
	{0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
	{0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x00,0x7F,0x10,0x28,0x44},
	{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
	{0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
	{0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
	{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
	{0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x02,0x01,0x02,0x04,0x02} };

// ---- drawing (the panel layer is ARGB: alpha 0 = the OS picture shows through)
const TUint32 KBg = 0xFF101418, KFg = 0xFFFFFFFF, KDim = 0xFF8A949E, KRed = 0xFFFF4D4D, KGreen = 0xFF3DDC84,
	KAmber = 0xFFFFC107, KBlue = 0xFF4FA3FF, KPanel = 0xFF20262E, KKey = 0xFF333B45, KKeyDown = 0xFF4FA3FF;

static void Rect(TInt aX, TInt aY, TInt aW, TInt aH, TUint32 aC)
	{
	if (aX < 0) { aW += aX; aX = 0; }
	if (aY < 0) { aH += aY; aY = 0; }
	if (aX + aW > KW) aW = KW - aX;
	if (aY + aH > KH) aH = KH - aY;
	for (TInt y = aY; y < aY + aH; ++y)
		{
		TUint32* p = Fb + y * KW + aX;
		for (TInt x = 0; x < aW; ++x)
			p[x] = aC;
		}
	}

static TInt TextW(const char* aS, TInt aScale)
	{
	TInt n = 0;
	while (aS[n]) ++n;
	return n ? n * 6 * aScale - aScale : 0;
	}

// Draws aS (clipped at aMaxW pixels, 0 = no limit); returns the width drawn.
static TInt Text(TInt aX, TInt aY, const char* aS, TInt aScale, TUint32 aC, TInt aMaxW = 0)
	{
	TInt x = aX;
	for (; *aS; ++aS)
		{
		if (aMaxW && x + 5 * aScale > aX + aMaxW)
			break;
		TUint ch = (TUint8)*aS;
		if (ch < 0x20 || ch > 0x7E) ch = '?';
		const TUint8* g = KFont[ch - 0x20];
		for (TInt c = 0; c < 5; ++c)
			for (TInt r = 0; r < 7; ++r)
				if (g[c] & (1 << r))
					Rect(x + c * aScale, aY + r * aScale, aScale, aScale, aC);
		x += 6 * aScale;
		}
	return x - aX;
	}

// tiny printf helpers (no Kern::AppendFormat: these run from the fault seam too)
static char* PutStr(char* aP, const char* aS) { while (*aS) *aP++ = *aS++; *aP = 0; return aP; }
static char* PutDec(char* aP, TUint aV)
	{
	char t[12]; TInt n = 0;
	do { t[n++] = (char)('0' + aV % 10); aV /= 10; } while (aV);
	while (n) *aP++ = t[--n];
	*aP = 0;
	return aP;
	}
// bytes as "123B", "45.6K", "12.3M", "1.20G"
static char* PutSize(char* aP, TUint64 aB)
	{
	static const char KU[] = "BKMG";
	TInt u = 0;
	TUint64 scale = 1;
	while (u < 3 && aB >= scale * 1024) { scale *= 1024; ++u; }
	if (!u) { aP = PutDec(aP, (TUint)aB); return PutStr(aP, "B"); }
	const TUint64 tenths = (aB * 10 + scale / 2) / scale;
	aP = PutDec(aP, (TUint)(tenths / 10));
	*aP++ = '.';
	*aP++ = (char)('0' + tenths % 10);
	*aP++ = KU[u];
	*aP = 0;
	return aP;
	}

// ---- state posted by the rest of the kernel
const TInt KSpark = 50;				// sparkline samples (1 per second) = 2 px each in a 100 px box
static TUint8 CpuHist[KSpark], RamHist[KSpark];
static TInt HistN;
static TUint CpuNow[4], CpuAvg;
static TInt NCpus = 1;
static TUint RamFreeKB, RamTotalKB;
static TInt BattPct = -1, BattMv;
static TBool BattChg;

static volatile TBool ProgOn;
static char ProgLabel[16];
static TUint64 ProgDone, ProgTotal;

const TInt KNoteLen = 60;
static char Note[KNoteLen + 1];
static TInt NoteLevel;				// 0 info, 1 ok, 2 warning, 3 error
static TUint NoteTick;				// NKern::TickCount when posted
static volatile TUint Faults;
static volatile TBool TopDirty = ETrue;

extern "C" void A64StatusBattery(TInt aPct, TBool aCharging, TInt aMv)
	{
	BattPct = aPct; BattChg = aCharging; BattMv = aMv;
	TopDirty = ETrue;
	}

extern "C" void A64BarsProgress(const char* aLabel, TUint64 aDone, TUint64 aTotal)
	{
	TInt i = 0;
	for (; aLabel[i] && i < (TInt)sizeof(ProgLabel) - 1; ++i)
		ProgLabel[i] = aLabel[i];
	ProgLabel[i] = 0;
	ProgDone = aDone; ProgTotal = aTotal;
	ProgOn = ETrue;
	TopDirty = ETrue;
	}

extern "C" void A64BarsProgressEnd()
	{
	ProgOn = EFalse;
	TopDirty = ETrue;
	}

// level: 0 info, 1 ok, 2 warning, 3 error (counted as a fault)
extern "C" void A64BarsNotify(TInt aLevel, const char* aText)
	{
	TInt i = 0;
	for (; aText[i] && i < KNoteLen; ++i)
		Note[i] = aText[i];
	Note[i] = 0;
	NoteLevel = aLevel;
	NoteTick = NKern::TickCount();
	if (aLevel >= 3)
		++Faults;
	TopDirty = ETrue;
	}

// the fault seam (tools/kernel/ekern_seam.cpp: weak hook): a thread panic or a kernel exception
extern "C" void ekseam_board_notify(int aLevel, const char* aText)
	{
	A64BarsNotify(aLevel, aText);
	}

// ---- the top bar
static void Spark(TInt aX, TInt aY, TInt aW, TInt aH, const TUint8* aHist, TUint32 aC)
	{
	Rect(aX, aY, aW, aH, 0xFF1C232B);
	const TInt n = HistN < KSpark ? HistN : KSpark;
	for (TInt i = 0; i < n; ++i)
		{
		const TInt v = aHist[(HistN - n + i) % KSpark];		// 0..100, oldest first
		const TInt h = (v * aH + 99) / 100;
		Rect(aX + (KSpark - n + i) * (aW / KSpark), aY + aH - h, aW / KSpark, h, aC);
		}
	}

static void TopDraw()
	{
	char b[80];
	Rect(0, 0, KW, KBarH, KBg);
	// line 1 (y 8..29, scale 3): CPU % + per-core bars + sparkline | RAM free + sparkline | battery
	char* p = PutStr(b, "CPU ");
	p = PutDec(p, CpuAvg);
	PutStr(p, "%");
	TInt x = 8 + Text(8, 8, b, 3, CpuAvg >= 90 ? KRed : KFg);
	x += 8;
	for (TInt c = 0; c < NCpus && c < 4; ++c)
		{
		const TInt h = (TInt)(CpuNow[c] * 21 / 100);
		Rect(x + c * 8, 8, 6, 21, 0xFF1C232B);
		Rect(x + c * 8, 29 - h, 6, h, CpuNow[c] >= 90 ? KRed : KGreen);
		}
	x += 4 * 8 + 4;
	Spark(x, 8, 70, 21, CpuHist, KGreen);
	// "RAM 212M/1G" = used / total (the sparkline is used % too); short forms so "RAM 1023M/2G" still clears the
	// battery (user, 2026-10-08)
	x = 270;
	p = PutStr(b, "RAM ");
	{
	const TUint usedKB = RamTotalKB > RamFreeKB ? RamTotalKB - RamFreeKB : 0;
	if (usedKB < 1024 * 1024) { p = PutDec(p, (usedKB + 512) / 1024); p = PutStr(p, "M"); }
	else { const TUint t = (usedKB * 10 + 512 * 1024) / (1024 * 1024); p = PutDec(p, t / 10); *p++ = '.'; *p++ = (char)('0' + t % 10); p = PutStr(p, "G"); }
	p = PutStr(p, "/");
	p = PutDec(p, (RamTotalKB + 512 * 1024) / (1024 * 1024));
	p = PutStr(p, "G");
	}
	x += Text(x, 8, b, 3, RamTotalKB && RamFreeKB < RamTotalKB / 10 ? KAmber : KFg) + 8;
	Spark(x, 8, 70, 21, RamHist, KBlue);
	// battery: an icon + % (or volts while the gauge is not valid), "+" = charging
	// right-aligned: the text ends at the panel's edge, the icon 12 px before it (user, 2026-10-08)
	p = b;
	if (BattPct >= 0) { p = PutDec(p, (TUint)BattPct); p = PutStr(p, "%"); }
	else if (BattMv) { p = PutDec(p, (TUint)BattMv / 1000); *p++ = '.'; p = PutDec(p, (TUint)(BattMv % 1000) / 100); p = PutStr(p, "V"); }
	if (BattChg) p = PutStr(p, "+");
	const TInt tx = KW - 8 - TextW(b, 2), bx = tx - 12 - 33;
	const TUint32 bc = BattPct >= 0 && BattPct <= 15 ? KRed : KFg;
	Rect(bx, 9, 30, 19, bc); Rect(bx + 2, 11, 26, 15, KBg); Rect(bx + 30, 14, 3, 9, bc);
	if (BattPct >= 0)
		Rect(bx + 4, 13, 22 * BattPct / 100, 11, BattChg ? KGreen : bc);
	Text(tx, 12, b, 2, KFg);

	// line 2 (y 44..72): progress | the latest notification (20 s) | uptime + fault count
	const TUint now = NKern::TickCount();
	const TUint tickMs = (TUint)NKern::TickPeriod() / 1000;
	if (ProgOn)
		{
		const TInt bw = 380;
		Rect(8, 46, bw, 24, 0xFF1C232B);
		if (ProgTotal)
			{
			const TInt w = (TInt)(ProgDone * (TUint64)(bw - 4) / ProgTotal);
			Rect(10, 48, w, 20, KBlue);
			}
		else	// unknown size: a moving block
			Rect(10 + (TInt)((now / 4) % (bw - 44)), 48, 40, 20, KBlue);
		p = PutStr(b, ProgLabel);
		*p++ = ' ';
		p = PutSize(p, ProgDone);
		if (ProgTotal) { p = PutStr(p, "/"); p = PutSize(p, ProgTotal); }
		Text(8 + bw + 12, 51, b, 2, KFg, KW - bw - 28);
		}
	else if (Note[0] && (now - NoteTick) * tickMs < 20000)
		{
		static const TUint32 KLevel[4] = { KFg, KGreen, KAmber, KRed };
		Text(8, 51, Note, 2, KLevel[NoteLevel & 3], KW - 100);
		}
	else
		{
		const TUint s = now * tickMs / 1000;
		p = PutStr(b, "up ");
		if (s >= 3600) { p = PutDec(p, s / 3600); p = PutStr(p, "h"); }
		p = PutDec(p, (s / 60) % 60); p = PutStr(p, "m ");
		p = PutDec(p, s % 60); p = PutStr(p, "s");
		if (Note[0]) { p = PutStr(p, "   last: "); p = PutStr(p, Note); }
		Text(8, 51, b, 2, KDim, KW - 100);
		}
	if (Faults)
		{
		p = PutStr(b, "!");
		PutDec(p, Faults);
		Text(KW - 8 - TextW(b, 3), 48, b, 3, KRed);
		}
	}

static void Sample()
	{
	static TUint64 lastNow, lastIdle[4];
	TUint busy[4] = { 0, 0, 0, 0 };
	NCpus = A64CpuBusy(&lastNow, lastIdle, busy, 4);
	TUint sum = 0;
	for (TInt c = 0; c < NCpus; ++c)
		{
		CpuNow[c] = busy[c] > 100 ? 100 : busy[c];
		sum += CpuNow[c];
		}
	CpuAvg = NCpus ? sum / NCpus : 0;
	RamFreeKB = (TUint)(Kern::FreeRamInBytes() >> 10);
	CpuHist[HistN % KSpark] = (TUint8)CpuAvg;
	RamHist[HistN % KSpark] = (TUint8)(RamTotalKB ? 100 - (TUint64)RamFreeKB * 100 / RamTotalKB : 0);	// used %
	++HistN;
	TopDirty = ETrue;
	}

// ---- the golden overlay: menu, confirmation, on-screen keyboard
#ifdef A64_GOLDEN
enum TOverlay { ENone, EMenu, EConfirm, EKeyboard };
enum TAction { EActKeyboard, EActBootB, EActRestart, EActFormatC, EActPowerOff, EActCancel, EActCount };
static const char* const KMenu[EActCount] = { "Keyboard", "Boot image B", "Restart", "Format C: (factory reset)", "Power off", "Cancel" };
static const char* const KAsk[EActCount] = { "", "Boot image B now?", "Restart now?", "Format C: ?", "Power off now?", "" };
static const char* const KAskMore[EActCount] = { "", "One trial boot of slot B", "Back to this golden image", "S60 rebuilds C: on its next boot", "Press the power key to start again", "" };
static volatile TInt Ovl = ENone, OvlShown = ENone, Pending = -1, Pressed = -1, Run = -1;
static volatile TBool OvlDirty;
const TInt KMenuW = 600, KItemH = 96, KMenuTitleH = 70;
const TInt KMenuX = (KW - KMenuW) / 2, KMenuH = KMenuTitleH + EActCount * KItemH, KMenuY = (KH - KMenuH) / 2;
const TInt KAskW = 600, KAskH = 330, KAskX = (KW - KAskW) / 2, KAskY = (KH - KAskH) / 2;

// the keyboard: 5 rows of 10 units, 72 x 84 px, just above the soft key bar
const TInt KKeyW = 72, KKeyH = 84, KKbdRows = 5, KKbdY = KH - KBarH - KKbdRows * KKeyH;
struct SKey { const char* iLabel; TUint8 iScan; TUint8 iShift; TUint8 iUnits2; };	// iUnits2 = width in half units
static const SKey KKbd[] = {
	{"1",'1',0,2},{"2",'2',0,2},{"3",'3',0,2},{"4",'4',0,2},{"5",'5',0,2},{"6",'6',0,2},{"7",'7',0,2},{"8",'8',0,2},{"9",'9',0,2},{"0",'0',0,2},
	{"q",'Q',0,2},{"w",'W',0,2},{"e",'E',0,2},{"r",'R',0,2},{"t",'T',0,2},{"y",'Y',0,2},{"u",'U',0,2},{"i",'I',0,2},{"o",'O',0,2},{"p",'P',0,2},
	{"a",'A',0,2},{"s",'S',0,2},{"d",'D',0,2},{"f",'F',0,2},{"g",'G',0,2},{"h",'H',0,2},{"j",'J',0,2},{"k",'K',0,2},{"l",'L',0,2},{":",EStdKeySemiColon,1,2},
	{"z",'Z',0,2},{"x",'X',0,2},{"c",'C',0,2},{"v",'V',0,2},{"b",'B',0,2},{"n",'N',0,2},{"m",'M',0,2},{".",EStdKeyFullStop,0,2},{"\\",EStdKeyBackSlash,0,2},{"/",EStdKeyForwardSlash,0,2},
	{"hide",0,0,4},{"-",EStdKeyMinus,0,2},{"space",EStdKeySpace,0,8},{"del",EStdKeyBackspace,0,3},{"enter",EStdKeyEnter,0,3} };
const TInt KKbdKeys = sizeof(KKbd) / sizeof(KKbd[0]);

static void KeyRect(TInt aI, TInt& aX, TInt& aY, TInt& aW)
	{
	TInt row = 0, u = 0;
	for (TInt i = 0; i < aI; ++i)
		{
		u += KKbd[i].iUnits2;
		if (u >= 20) { u = 0; ++row; }
		}
	aX = u * KKeyW / 2;
	aY = KKbdY + row * KKeyH;
	aW = KKbd[aI].iUnits2 * KKeyW / 2;
	}

static void ClearMiddle(TInt aY, TInt aH)
	{
	Rect(0, aY, KW, aH, 0x00000000);		// alpha 0: see-through again
	}

static void OvlDraw()
	{
	if (OvlShown != Ovl)
		{
		ClearMiddle(KBarH, KH - 2 * KBarH);
		OvlShown = Ovl;
		}
	if (Ovl == EMenu)
		{
		Rect(KMenuX - 4, KMenuY - 4, KMenuW + 8, KMenuH + 8, KBlue);
		Rect(KMenuX, KMenuY, KMenuW, KMenuH, KPanel);
		Text(KMenuX + 24, KMenuY + 22, "Phorepoc golden", 3, KDim);
		for (TInt i = 0; i < EActCount; ++i)
			{
			const TInt y = KMenuY + KMenuTitleH + i * KItemH;
			Rect(KMenuX + 8, y + 4, KMenuW - 16, KItemH - 8, Pressed == i ? KKeyDown : KKey);
			Text(KMenuX + 28, y + (KItemH - 21) / 2, KMenu[i], 3, i == EActFormatC ? KAmber : KFg);
			}
		}
	else if (Ovl == EConfirm && Pending >= 0)
		{
		Rect(KAskX - 4, KAskY - 4, KAskW + 8, KAskH + 8, Pending == EActFormatC ? KAmber : KBlue);
		Rect(KAskX, KAskY, KAskW, KAskH, KPanel);
		Text(KAskX + 28, KAskY + 36, KAsk[Pending], 4, KFg);
		Text(KAskX + 28, KAskY + 100, KAskMore[Pending], 2, KDim);
		Rect(KAskX + 24, KAskY + 200, 260, 100, Pressed == 0 ? KKeyDown : KKey);
		Rect(KAskX + KAskW - 284, KAskY + 200, 260, 100, Pressed == 1 ? KKeyDown : KKey);
		Text(KAskX + 24 + (260 - TextW("Yes", 4)) / 2, KAskY + 236, "Yes", 4, KGreen);
		Text(KAskX + KAskW - 284 + (260 - TextW("No", 4)) / 2, KAskY + 236, "No", 4, KRed);
		}
	else if (Ovl == EKeyboard)
		{
		for (TInt i = 0; i < KKbdKeys; ++i)
			{
			TInt x, y, w;
			KeyRect(i, x, y, w);
			Rect(x, y, w, KKeyH, 0xFF0B0E12);
			Rect(x + 3, y + 3, w - 6, KKeyH - 6, Pressed == i ? KKeyDown : KKey);
			const TInt sc = KKbd[i].iLabel[1] ? 3 : 5;
			Text(x + (w - TextW(KKbd[i].iLabel, sc)) / 2, y + (KKeyH - 7 * sc) / 2, KKbd[i].iLabel, sc,
				KKbd[i].iScan ? KFg : KDim);
			}
		}
	}

static void Key(TUint aScan, TBool aDown)
	{
	TRawEvent ev;
	ev.Set(aDown ? TRawEvent::EKeyDown : TRawEvent::EKeyUp, (TInt)aScan);
	Kern::AddEvent(ev);
	}

static void FormatC()
	{
	// C: = MBR entry 2 (P3, sd_layout.py): wipe its boot sector; ESTART finds it corrupt on the next S60 boot and
	// formats it (FS_FORMAT_CORRUPT, the quick path) -- the same as the A64_FACTORY_RESET test build
	TUint32 s[128];
	if (!A64SdTransfer(0, 1, s, EFalse))
		{ A64BarsNotify(3, "Format C: failed: cannot read the MBR"); return; }
	const TUint8* e = (const TUint8*)s + 446 + 16 * 2;
	const TUint start = e[8] | e[9] << 8 | e[10] << 16 | (TUint)e[11] << 24;
	if (e[4] == 0 || start == 0)
		{ A64BarsNotify(3, "Format C: failed: no partition 3 on the card"); return; }
	memclr(s, sizeof(s));
	const TBool ok = A64SdTransfer(start, 1, s, ETrue);
	ekseam_puts("[EKERN][bars] format C: boot sector "); ekseam_puthex(start); ekseam_puts(ok ? " wiped\n" : " FAILED\n");
	A64BarsNotify(ok ? 1 : 3, ok ? "C: erased: S60 formats it on its next boot" : "Format C: failed: SD write error");
	}

static void RunAction(TInt aAct)
	{
	ekseam_puts("[EKERN][bars] menu action "); ekseam_puthex((unsigned)aAct);
	switch (aAct)
		{
	case EActBootB:
		{
		const TInt r = A64SlotRearm();
		if (r != KErrNone)
			{ A64BarsNotify(3, "No valid image in slot B"); return; }
		A64BarsNotify(0, "Booting image B...");
		TopDraw();
		A64WatchdogReboot();
		break;
		}
	case EActRestart:
		A64BarsNotify(0, "Restarting...");
		TopDraw();
		A64WatchdogReboot();
		break;
	case EActFormatC:
		FormatC();
		break;
	case EActPowerOff:
		A64BarsNotify(0, "Powering off...");
		TopDraw();
		A64PmicPowerOff();
		break;
		}
	}

// The Menu soft key (a64touch.cpp, golden only): open / close the menu.
extern "C" void A64BarsMenuKey()
	{
	Ovl = Ovl == ENone || Ovl == EKeyboard ? EMenu : ENone;
	Pressed = -1;
	OvlDirty = ETrue;
	}

static TBool In(TInt aX, TInt aY, TInt aRx, TInt aRy, TInt aRw, TInt aRh)
	{
	return aX >= aRx && aX < aRx + aRw && aY >= aRy && aY < aRy + aRh;
	}

// A touch in panel pixels (a64touch.cpp, the touch DFC): aAction 0 = down, 1 = move, 2 = up. Returns ETrue if the
// overlay takes it (the OS never sees that touch). Decided at the down; the rest of the gesture follows it.
static TBool Owned;
static TInt KbdScan, KbdShift;
extern "C" TBool A64BarsTouch(TInt aX, TInt aY, TInt aAction)
	{
	if (aAction == 0)
		{
		Owned = EFalse;
		if (Ovl == ENone)
			return EFalse;
		if (Ovl == EKeyboard && aY < KKbdY)
			return EFalse;							// above the keyboard: the OS screen stays usable
		Owned = ETrue;
		Pressed = -1;
		if (Ovl == EMenu)
			{
			for (TInt i = 0; i < EActCount; ++i)
				if (In(aX, aY, KMenuX, KMenuY + KMenuTitleH + i * KItemH, KMenuW, KItemH))
					Pressed = i;
			}
		else if (Ovl == EConfirm)
			{
			if (In(aX, aY, KAskX + 24, KAskY + 200, 260, 100)) Pressed = 0;
			if (In(aX, aY, KAskX + KAskW - 284, KAskY + 200, 260, 100)) Pressed = 1;
			}
		else
			{
			for (TInt i = 0; i < KKbdKeys; ++i)
				{
				TInt x, y, w;
				KeyRect(i, x, y, w);
				if (In(aX, aY, x, y, w, KKeyH))
					Pressed = i;
				}
			if (Pressed >= 0 && KKbd[Pressed].iScan)
				{
				KbdScan = KKbd[Pressed].iScan;
				KbdShift = KKbd[Pressed].iShift;
				if (KbdShift) Key(EStdKeyLeftShift, ETrue);
				Key(KbdScan, ETrue);					// WSERV repeats a held key
				}
			}
		OvlDirty = ETrue;
		return ETrue;
		}
	if (!Owned)
		return EFalse;
	if (aAction == 1)
		return ETrue;
	// up
	Owned = EFalse;
	const TInt hit = Pressed;
	Pressed = -1;
	OvlDirty = ETrue;
	if (Ovl == EMenu)
		{
		if (hit == EActKeyboard) Ovl = EKeyboard;
		else if (hit == EActCancel || hit < 0) Ovl = ENone;	// a tap outside the menu closes it
		else { Pending = hit; Ovl = EConfirm; }
		}
	else if (Ovl == EConfirm)
		{
		if (hit == 0) Run = Pending;					// done by the bars DFC (SD writes, reboots)
		if (hit >= 0) Ovl = ENone;
		}
	else if (Ovl == EKeyboard)
		{
		if (KbdScan) { Key(KbdScan, EFalse); if (KbdShift) Key(EStdKeyLeftShift, EFalse); }
		KbdScan = KbdShift = 0;
		if (hit >= 0 && !KKbd[hit].iScan) Ovl = ENone;	// "hide"
		}
	return ETrue;
	}
#else	// not golden: no overlay (the hooks exist so phlink can call them on any image)
extern "C" void A64BarsMenuKey() {}
extern "C" TBool A64BarsTouch(TInt, TInt, TInt) { return EFalse; }
#endif	// A64_GOLDEN

// phlink 'T' in panel pixels (PhoneLab "panel" touches): the overlay and the soft key bar as a finger would hit
// them, else an OS pointer event. aAction 0 down / 1 move / 2 up.
extern "C" TBool A64BarsPanelTouch(TInt aX, TInt aY, TInt aAction)
	{
	static TBool owned;
	if (aAction == 0)
		return owned = A64BarsTouch(aX, aY, 0);	// (returning ETrue here swallowed every down on S60, 2026-10-08)
	if (owned)
		{
		A64BarsTouch(aX, aY, aAction);
		if (aAction == 2)
			owned = EFalse;
		}
	else
		return EFalse;
	return ETrue;
	}

// ---- the DFC that draws everything
static void BarsTimerFn(TAny*);
static NTimer BarsTimer(BarsTimerFn, 0);
static TDfc* BarsDfc;

static void BarsDfcFn(TAny*)
	{
	static TUint lastSample;
	const TUint now = NKern::TickCount();
	const TUint tickMs = (TUint)NKern::TickPeriod() / 1000;
	if ((now - lastSample) * tickMs >= 1000)
		{
		lastSample = now;
		Sample();
		}
	TInt next = 250;
#ifdef A64_GOLDEN
	if (Run >= 0)
		{
		const TInt a = Run;
		Run = -1;
		RunAction(a);
		}
	if (OvlDirty || OvlShown != Ovl)
		{
		OvlDirty = EFalse;
		OvlDraw();
		}
	if (Ovl != ENone)
		next = 40;
#endif
	if (TopDirty || ProgOn)
		{
		TopDirty = EFalse;
		TopDraw();
		}
	BarsTimer.OneShot(NKern::TimerTicks(next));
	}

static void BarsTimerFn(TAny*)
	{
	BarsDfc->Add();
	}

// From a64_display.cpp once the panel layer scans (StripEnable).
extern "C" void A64BarsStart()
	{
	Fb = A64PanelFb();
	if (!Fb)
		return;
	NKern::ThreadEnterCS();
	BarsDfc = new TDfc(BarsDfcFn, NULL, Kern::DfcQue0(), 1);
	TMemoryInfoV1Buf mi;
	if (Kern::HalFunction(EHalGroupKernel, EKernelHalMemoryInfo, &mi, NULL) == KErrNone)
		RamTotalKB = (TUint)(mi().iTotalRamInBytes >> 10);
	NKern::ThreadLeaveCS();
	if (!BarsDfc)
		return;
	TopDraw();
	BarsTimer.OneShot(NKern::TimerTicks(250));
	ekseam_puts("[EKERN][bars] top bar on"
#ifdef A64_GOLDEN
		" + golden menu (Menu soft key)"
#endif
		"\n");
	}
