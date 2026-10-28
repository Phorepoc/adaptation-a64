/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// a64sensors_api.h -- OURS (phone sensors S2): the
// user/kernel contract of the sensor LDD "Sensors.A64" (a64sensors.cpp). One unit per chip; v1 = unit 0, the MPU-6050
// accelerometer. A client opens a unit, sets the rate, and reads samples: a Read completes as soon as the driver has
// at least one new sample, with up to the descriptor's capacity of them. The SSY (src/ssya64) is the one client.
#ifndef A64SENSORS_API_H
#define A64SENSORS_API_H
#include <e32cmn.h>
#include <e32ver.h>
#ifndef __KERNEL_MODE__
#include <e32std.h>
#endif

_LIT(KA64SensorsLddName, "Sensors.A64");
_LIT(KA64SensorsLddFile, "a64sensors");			// User::LoadLogicalDevice name (the efile loader alias -> Sensors.A64)

enum TA64SensorUnit { EA64SensorAccel = 0, EA64SensorUnits = 1 };

// a sample: the time in microseconds since 0 AD (Kern::SystemTime = TTime's Int64), and the three axes in the DEVICE
// frame (the DTS mount matrix applied: x = right, y = up the screen, z = out of the screen), in LSB of +-2 g (16384 / g)
struct TA64SensorSample
	{
	TInt64 iTime;
	TInt16 iX, iY, iZ, iPad;
	};

struct TA64SensorInfo
	{
	TUint32 iWhoAmI;		// MPU-6050: 0x68
	TInt iLsbPerG;			// 16384 at +-2 g
	TInt iRangeG;			// 2
	TInt iRateHz;			// the current sampling rate
	};

class RA64Sensor : public RBusLogicalChannel
	{
public:
	enum { EMajorVersion = 1 };
	enum TControl { EControlSetRate = 0, EControlGetInfo = 1 };
	enum TRequest { ERequestRead = 0, ERequestCount = 1 };
#ifndef __KERNEL_MODE__
	inline TInt Open(TInt aUnit)
		{ return DoCreate(KA64SensorsLddName, TVersion(EMajorVersion, 0, 0), aUnit, NULL, NULL, EOwnerThread); }
	inline TInt SetRate(TInt aHz)						{ return DoControl(EControlSetRate, (TAny*)aHz); }
	inline TInt GetInfo(TA64SensorInfo& aInfo)			{ TPckg<TA64SensorInfo> p(aInfo); return DoControl(EControlGetInfo, &p); }
	inline void Read(TRequestStatus& aStatus, TDes8& aSamples)	{ DoRequest(ERequestRead, aStatus, &aSamples); }
	inline void ReadCancel()							{ DoCancel(1u << ERequestRead); }
#endif
	};

#endif
