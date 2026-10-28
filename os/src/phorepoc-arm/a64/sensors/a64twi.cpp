// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// a64twi.cpp -- OURS (2026-10-10, phone sensors rung S1:
// 1-2): the A64's TWI1 (a Marvell mv64xxx I2C core, 0x01C2B000) as the shared
// bus of the sensors -- the InvenSense MPU-6050 accelerometer+gyro at 0x68, the Sensortek STK3311 proximity/ambient
// light at 0x48, and the magnetometer: an ST LIS3MDL at 0x1E or, on newer 1.2 boards, a Voltafield AF8133J at 0x1C
// (PB1 is the one's data-ready output and the other's active-low reset: probe 0x1E first with PB1 left an input).
// The bus code is the touch driver's (input/a64touch.cpp, TWI0) with the research's corrections: 100 kHz for real
// (CCR 0x59: 24 MHz / (2^1 * (11+1) * 10)), the CCR reprogrammed after every soft reset, bounded waits, 8-bit register
// addresses. One locked helper per bus (TA64Twi), the base of the sensor LDD to come.
//   A64_SENSOR_TEST (A64DEFS): A64SensorsInstall() powers the rails (AXP803 DLDO1 3.3 V for the IMU + magnetometer,
//   LDO_IO0 3.3 V for the STK), brings TWI1 up and logs the chips' identity registers -- rung S1's pass is the three ids.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <nkern.h>
#include <syborg.h>
#include "a64twi.h"
#include "../power/a64_pwrres.h"

extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(TUint32);			// ends the line
extern "C" TInt A64PmicUpdateReg(TUint8 aReg, TUint8 aClr, TUint8 aSet);

static const TPhysAddr KTwiPhys[3] = { 0x01C2AC00u, 0x01C2B000u, 0x01C2B400u };
// TWI registers + CNTR bits (writing 1 to INT_FLAG clears it)
static const TUint KTwiAddr = 0x00, KTwiData = 0x08, KTwiCntr = 0x0C, KTwiStat = 0x10, KTwiCcr = 0x14, KTwiSrst = 0x18,
	KTwiEfr = 0x1C, KTwiLcr = 0x20;
static const TUint32 KBusEn = 0x40, KMSta = 0x20, KMStp = 0x10, KIntFlag = 0x08, KAAck = 0x04;
static const TUint32 KCcr100kHz = (11u << 3) | 1u;		// M = 11, N = 1: 24 MHz / (2 * 12 * 10) = 100 kHz
// CCU: TWI n = bus gate 0x06C bit n, reset 0x2D8 bit n; pins PH0/1 (TWI0), PH2/3 (TWI1), PE14/15 (TWI2) function 2
static const TUint KCcuBusGate3 = 0x06C, KCcuBusReset4 = 0x2D8, KPio = 0x800;
static const char KNl[] = { 10, 0 };
static inline volatile TUint32& R32(TLinAddr aBase, TUint aOff) { return *(volatile TUint32*)(aBase + aOff); }
static void Say(const char* aTag, TUint32 aValue) { ekseam_puts(aTag); ekseam_puthex(aValue); }
static void Wait(TInt aMs) { NKern::Sleep(NKern::TimerTicks(aMs)); }

// the class is declared in a64twi.h (shared with the sensor LDD, a64sensors.cpp)

TBool TA64Twi::WaitFlag()
	{
	for (TInt i = 0; i < 200000; ++i)				// ~ 2 ms at 100 kHz per byte, bounded
		if (R(KTwiCntr) & KIntFlag)
			return ETrue;
	return EFalse;
	}

TBool TA64Twi::Start(TUint32 aExpect1, TUint32 aExpect2)
	{
	R(KTwiCntr) = KBusEn | KMSta | (R(KTwiCntr) & KIntFlag);	// (repeated) start; clears a pending flag
	if (!WaitFlag())
		return EFalse;
	const TUint32 s = R(KTwiStat);
	return s == aExpect1 || s == aExpect2;
	}

TBool TA64Twi::Send(TUint8 aByte, TUint32 aExpect)
	{
	R(KTwiData) = aByte;
	R(KTwiCntr) = KBusEn | KIntFlag;
	return WaitFlag() && R(KTwiStat) == aExpect;
	}

void TA64Twi::Stop()
	{
	R(KTwiCntr) = KBusEn | KMStp | KIntFlag;
	for (TInt i = 0; i < 200000 && (R(KTwiCntr) & KMStp); ++i) {}
	for (TInt i = 0; i < 200000 && R(KTwiStat) != 0xF8; ++i) {}		// idle
	}

// the recovery: soft reset (which also clears the clock register: reprogram it), 9 clocks through LCR to free a slave
// holding SDA, then the bus enable again
void TA64Twi::Reset()
	{
	++iErrors;
	R(KTwiSrst) = 1;
	for (TInt i = 0; i < 100000 && (R(KTwiSrst) & 1); ++i) {}
	R(KTwiCcr) = KCcr100kHz;
	R(KTwiEfr) = 0;
	R(KTwiLcr) = 0x3A;								// SCL/SDA under LCR control, both released
	for (TInt n = 0; n < 9; ++n)
		{
		R(KTwiLcr) = 0x3A & ~0x08u;					// SCL low
		Kern::NanoWait(5000);
		R(KTwiLcr) = 0x3A;							// SCL high
		Kern::NanoWait(5000);
		}
	R(KTwiLcr) = 0;
	R(KTwiCntr) = KBusEn;
	}

TInt TA64Twi::Init(TInt aBus)
	{
	iBus = aBus;
	TInt r = DPlatChunkHw::New(iChunk, KTwiPhys[aBus] & ~0xFFFu, 0x1000, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r != KErrNone)
		return r;
	iTwi = (volatile TUint32*)(iChunk->LinearAddress() + (KTwiPhys[aBus] & 0xFFF));
	if (aBus == 0 || aBus == 1)
		{	// PH0/PH1 or PH2/PH3 = TWIn SCK/SDA (function 2); port H config register 0, 4 bits per pin
		volatile TUint32& cfg = R32(KHwBaseCcuPio, KPio + 7 * 0x24);
		const TUint sh = aBus * 8;
		cfg = (cfg & ~(0xFFu << sh)) | (0x22u << sh);
		}
	else
		{	// PE14/PE15, port E config register 1
		volatile TUint32& cfg = R32(KHwBaseCcuPio, KPio + 4 * 0x24 + 4);
		cfg = (cfg & ~0xFF000000u) | 0x22000000u;
		}
	R32(KHwBaseCcuPio, KCcuBusGate3) |= 1u << aBus;
	R32(KHwBaseCcuPio, KCcuBusReset4) |= 1u << aBus;
	R(KTwiSrst) = 1;
	for (TInt i = 0; i < 100000 && (R(KTwiSrst) & 1); ++i) {}
	R(KTwiCcr) = KCcr100kHz;
	R(KTwiEfr) = 0;
	R(KTwiCntr) = KBusEn;
	return KErrNone;
	}

// START, addr+W, reg, RESTART, addr+R, aLen bytes (ACK all but the last), STOP. Status codes: 0x08 start, 0x10 repeated
// start, 0x18 addr+W acked, 0x28 data acked, 0x40 addr+R acked, 0x50 byte received + acked, 0x58 byte received, nacked
TInt TA64Twi::Read(TUint8 aAddr, TUint8 aReg, TUint8* aBuf, TInt aLen)
	{
	TInt r = KErrGeneral;
	if (Start(0x08, 0x08) && Send(aAddr << 1, 0x18) && Send(aReg, 0x28) && Start(0x10, 0x08) && Send((aAddr << 1) | 1, 0x40))
		{
		r = KErrNone;
		for (TInt i = 0; i < aLen; ++i)
			{
			const TBool last = i == aLen - 1;
			R(KTwiCntr) = KBusEn | KIntFlag | (last ? 0 : KAAck);
			if (!WaitFlag() || R(KTwiStat) != (last ? 0x58u : 0x50u))
				{ r = KErrGeneral; break; }
			aBuf[i] = (TUint8)R(KTwiData);
			}
		}
	Stop();
	if (r != KErrNone)
		Reset();
	return r;
	}

TInt TA64Twi::Write(TUint8 aAddr, TUint8 aReg, TUint8 aVal)
	{
	TInt r = KErrGeneral;
	if (Start(0x08, 0x08) && Send(aAddr << 1, 0x18) && Send(aReg, 0x28) && Send(aVal, 0x28))
		r = KErrNone;
	Stop();
	if (r != KErrNone)
		Reset();
	return r;
	}

static TA64Twi* Twi1;
static TBool RailsOn;

extern "C" void A64SensorRailsOn()
	{
	if (RailsOn)
		return;
	RailsOn = ETrue;
	// power rung P4: the sensors hold DLDO1 for good, so the screen going off (a64_display.cpp) never takes the IMU's supply
	A64PwrResGet(KA64ResDldo1);
	// AXP803: DLDO1 = 3.3 V ("vcc-dsi-sensor": the IMU + the magnetometer) -- reg 0x15 = (3300 - 700) / 100 = 26, on = reg 0x12 bit 3
	A64PmicUpdateReg(0x15, 0x1F, 26);
	A64PmicUpdateReg(0x12, 0, 1u << 3);
	// LDO_IO0 = 3.3 V ("vcc-lcd-ctp-stk": the STK3311) -- AXP GPIO0 in LDO mode: reg 0x91 voltage, reg 0x90 function 3 = on
	A64PmicUpdateReg(0x91, 0x1F, 26);
	A64PmicUpdateReg(0x90, 0x07, 0x03);
	// the AF8133J's reset (PB1, active low): output, high -- harmless on a LIS3MDL board (there PB1 is the chip's DRDY output)
	volatile TUint32& bcfg = R32(KHwBaseCcuPio, KPio + 1 * 0x24);
	bcfg = (bcfg & ~(0xFu << 4)) | (1u << 4);
	R32(KHwBaseCcuPio, KPio + 1 * 0x24 + 0x10) |= 1u << 1;
	Wait(120);												// MPU-6050: 100 ms after power-up
	}

// the TWI1 bus, created on first use (the LDD's first open or the boot probe); NULL if the controller did not come up
extern "C" TA64Twi* A64Twi1Get()
	{
	if (Twi1)
		return Twi1;
	TA64Twi* t = new TA64Twi;
	if (!t)
		return NULL;
	if (t->Init(1) != KErrNone)
		{
		delete t;
		return NULL;
		}
	Twi1 = t;
	return Twi1;
	}

static void Probe(const char* aName, TUint8 aAddr, TUint8 aReg, TUint8 aExpect)
	{
	TUint8 v = 0;
	const TInt r = Twi1->Read(aAddr, aReg, &v, 1);
	ekseam_puts("[twi1] "); ekseam_puts(aName); Say(" addr=", aAddr); Say("[twi1]   reg=", aReg); Say("[twi1]   r=", r);
	Say("[twi1]   value=", v); Say("[twi1]   expected=", aExpect);
	ekseam_puts(r == KErrNone && v == aExpect ? "[twi1]   PRESENT\n" : (r == KErrNone ? "[twi1]   answers, other id\n" : "[twi1]   absent / no ack\n"));
	}

static void A64SensorsProbe()
	{
	ekseam_puts("[twi1] === TWI1 sensor bus probe (rung S1) ===\n");
	A64SensorRailsOn();
	if (!A64Twi1Get())
		{
		ekseam_puts("[twi1] bus init failed\n");
		return;
		}
	Say("[twi1] cntr=", Twi1->iTwi[KTwiCntr >> 2]); Say("[twi1] stat=", Twi1->iTwi[KTwiStat >> 2]); Say("[twi1] ccr=", Twi1->iTwi[KTwiCcr >> 2]);
	Probe("MPU-6050 WHO_AM_I", 0x68, 0x75, 0x68);
	Probe("STK3311 PDT_ID", 0x48, 0x3E, 0x12);			// 0x12 = STK3311-X; 0x1D / 0x1E / 0x15 / 0x51 are the siblings
	TUint8 v = 0;
	const TInt lis = Twi1->Read(0x1E, 0x0F, &v, 1);
	if (lis == KErrNone)
		{
		Say("[twi1] LIS3MDL WHO_AM_I value=", v); Say("[twi1]   expected=", 0x3D);
		ekseam_puts(v == 0x3D ? "[twi1]   PRESENT (board rev <= 1.2 early)\n" : "[twi1]   answers, other id\n");
		}
	else
		{
		// no LIS3MDL: the AF8133J variant -- PB1 is its reset (released by A64SensorRailsOn), PCODE at 0x1C
		ekseam_puts("[twi1] LIS3MDL absent at 0x1E: trying the AF8133J\n");
		Probe("AF8133J PCODE", 0x1C, 0x00, 0x5E);
		}
	// a first accelerometer sample: wake the MPU-6050 (PWR_MGMT_1 = 0: clock = internal, sleep off), then ACCEL_XOUT_H..
	if (Twi1->Write(0x68, 0x6B, 0x00) == KErrNone)
		{
		Wait(50);
		TUint8 a[6] = { 0, 0, 0, 0, 0, 0 };
		if (Twi1->Read(0x68, 0x3B, a, 6) == KErrNone)
			{
			const TInt16 x = (TInt16)((a[0] << 8) | a[1]), y = (TInt16)((a[2] << 8) | a[3]), z = (TInt16)((a[4] << 8) | a[5]);
			ekseam_puts("[twi1] MPU-6050 accel raw (+-2 g = 16384/g): x="); ekseam_puthex((TUint32)(TInt32)x);
			Say("[twi1]   y=", (TUint32)(TInt32)y); Say("[twi1]   z=", (TUint32)(TInt32)z);
			}
		Twi1->Write(0x68, 0x6B, 0x40);						// back to sleep
		}
	Say("[twi1] bus errors=", Twi1->iErrors);
	ekseam_puts("[twi1] === end ===\n");
	}

extern "C" void A64SensorsLddInstall();		// a64sensors.cpp: the "Sensors.A64" LDD

extern "C" void A64SensorsInstall()
	{
#ifdef A64_SENSOR_TEST
	A64SensorsProbe();
#endif
	A64SensorsLddInstall();
	}
