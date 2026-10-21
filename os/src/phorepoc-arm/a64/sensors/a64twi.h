/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

// a64twi.h -- OURS (PinePhone sensors): the A64 TWI (mv64xxx I2C) bus helper of a64twi.cpp, shared by the sensor LDD
// (a64sensors.cpp) and the boot probe. One instance per bus, a fast mutex per bus, polled transfers at 100 kHz.
#ifndef A64TWI_H
#define A64TWI_H
#include <kernel/kernel.h>
#include <nkern.h>

class DPlatChunkHw;

class TA64Twi
	{
public:
	TInt Init(TInt aBus);
	TInt Read(TUint8 aAddr, TUint8 aReg, TUint8* aBuf, TInt aLen);
	TInt Write(TUint8 aAddr, TUint8 aReg, TUint8 aVal);
	void Lock()		{ NKern::FMWait(&iMutex); }
	void Unlock()	{ NKern::FMSignal(&iMutex); }
private:
	TBool WaitFlag();
	TBool Start(TUint32 aExpect1, TUint32 aExpect2);
	TBool Send(TUint8 aByte, TUint32 aExpect);
	void Stop();
	void Reset();
	inline volatile TUint32& R(TUint aOff) { return iTwi[aOff >> 2]; }
public:
	volatile TUint32* iTwi;
	DPlatChunkHw* iChunk;
	NFastMutex iMutex;
	TInt iBus;
	TInt iErrors;
	};

// TWI1 = the sensor bus (MPU-6050 0x68, STK3311 0x48, AF8133J 0x1C / LIS3MDL 0x1E), created on first use, the sensor
// rails (AXP803 DLDO1 + LDO_IO0) switched on with it
extern "C" TA64Twi* A64Twi1Get();
extern "C" void A64SensorRailsOn();

#endif
