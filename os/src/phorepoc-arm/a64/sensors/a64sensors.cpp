// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// a64sensors.cpp -- OURS (2026-10-10, PinePhone sensors rung S2: "Sensors: the
// decision"): the sensor LDD "Sensors.A64", one unit per chip on the TWI1 bus (a64twi.cpp). v1 = unit 0, the
// InvenSense MPU-6050 accelerometer: on the first open the chip is reset and configured (gyro in standby, DLPF 20 Hz,
// +-2 g, SMPLRT_DIV for the rate), then a timer on the driver's own DFC queue reads ACCEL_XOUT..ZOUT every period,
// applies the board's mount matrix (the DTS: device x = -sensor y, device y = sensor x, device z = sensor z) and
// pushes the sample into a ring; a client Read completes with what the ring holds (at least one sample). The chip
// sleeps again when the channel closes. The user contract is a64sensors_api.h; the one client is the SSY (src/ssya64),
// which feeds the 2010 sensor server; the RCL_3 orientationssy derives the orientation from this channel.
// Shape: the kernel's DLogicalChannelBase request model with TClientRequest completions (as the Mali LDD).
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <nkern.h>
#include "a64twi.h"
#include "a64sensors_api.h"

extern "C" void ekseam_puts(const char*);
extern "C" void ekseam_puthex(TUint32);

// MPU-6050 registers
static const TUint8 KMpuAddr = 0x68, KRegSmplrtDiv = 0x19, KRegConfig = 0x1A, KRegAccelConfig = 0x1C, KRegAccelXoutH = 0x3B,
	KRegPwrMgmt1 = 0x6B, KRegPwrMgmt2 = 0x6C, KRegWhoAmI = 0x75;
static const TInt KRingSize = 32;
static const TInt KDefaultRateHz = 25;
_LIT(KDfcQName, "A64Sens");

class DA64SensorChannel;

// the accelerometer sampler (one per unit; a single open channel at a time in v1)
class TA64Accel
	{
public:
	TA64Accel() : iDfc(DfcFn, this, 1), iTimer(TimerFn, this), iRateHz(KDefaultRateHz), iRunning(EFalse), iHead(0), iCount(0), iChannel(0), iWhoAmI(0) {}
	TInt Start(TInt aRateHz, DA64SensorChannel* aChannel);
	void Stop();
	TInt SetRate(TInt aRateHz);
	TInt Take(TA64SensorSample* aOut, TInt aMax);		// under the sampler lock
	static void TimerFn(TAny* aPtr);
	static void DfcFn(TAny* aPtr);
	void Sample();
	TInt Configure();
public:
	TDfc iDfc;
	NTimer iTimer;
	TInt iRateHz;
	TBool iRunning;
	TA64SensorSample iRing[KRingSize];
	TInt iHead, iCount;
	DA64SensorChannel* iChannel;
	TUint32 iWhoAmI;
	NFastMutex iLock;
	};

class DA64SensorsDevice : public DLogicalDevice
	{
public:
	DA64SensorsDevice();
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const;
	virtual TInt Create(DLogicalChannelBase*& aChannel);
public:
	TDfcQue* iDfcQ;
	TA64Accel iAccel;
	};

class DA64SensorChannel : public DLogicalChannelBase
	{
public:
	DA64SensorChannel(DA64SensorsDevice* aDev) : iDev(aDev), iClient(0), iReadReq(0), iReadDes(0), iUnit(-1) {}
	virtual ~DA64SensorChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aReqNo, TAny* a1, TAny* a2);
	void DataReady();									// DFC context: a new sample is in the ring
	TInt DeliverRead();									// copy the ring into the client's descriptor, complete
public:
	DA64SensorsDevice* iDev;
	DThread* iClient;
	TClientRequest* iReadReq;
	TDes8* iReadDes;
	TInt iUnit;
	};

static DA64SensorsDevice* TheDevice;

// --- the sampler ---------------------------------------------------------------------------------------------------

TInt TA64Accel::Configure()
	{
	TA64Twi* twi = A64Twi1Get();
	if (!twi)
		return KErrHardwareNotAvailable;
	TUint8 who = 0;
	twi->Lock();
	TInt r = twi->Read(KMpuAddr, KRegWhoAmI, &who, 1);
	if (r == KErrNone)
		{
		iWhoAmI = who;
		twi->Write(KMpuAddr, KRegPwrMgmt1, 0x80);			// device reset
		twi->Unlock();
		NKern::Sleep(NKern::TimerTicks(100));
		twi->Lock();
		twi->Write(KMpuAddr, KRegPwrMgmt1, 0x01);			// sleep off, clock = gyro X PLL (stable)
		twi->Write(KMpuAddr, KRegPwrMgmt2, 0x07);			// gyro axes in standby
		twi->Write(KMpuAddr, KRegConfig, 0x04);				// DLPF 20 Hz
		twi->Write(KMpuAddr, KRegAccelConfig, 0x00);			// +-2 g
		TInt div = 1000 / (iRateHz > 0 ? iRateHz : KDefaultRateHz) - 1;	// the 1 kHz base with the DLPF on
		if (div < 0) div = 0; if (div > 255) div = 255;
		twi->Write(KMpuAddr, KRegSmplrtDiv, (TUint8)div);
		}
	twi->Unlock();
	if (r == KErrNone)
		NKern::Sleep(NKern::TimerTicks(30));
	ekseam_puts("[EKERN][sensors] MPU-6050 configure r="); ekseam_puthex(r);
	return r;
	}

TInt TA64Accel::Start(TInt aRateHz, DA64SensorChannel* aChannel)
	{
	if (iChannel)
		return KErrInUse;
	iRateHz = aRateHz;
	iChannel = aChannel;
	A64SensorRailsOn();
	const TInt r = Configure();
	if (r != KErrNone)
		{
		iChannel = 0;
		return r;
		}
	iHead = iCount = 0;
	iRunning = ETrue;
	iTimer.OneShot(NKern::TimerTicks(1000 / iRateHz), ETrue);
	return KErrNone;
	}

void TA64Accel::Stop()
	{
	iRunning = EFalse;
	iTimer.Cancel();
	iDfc.Cancel();
	iChannel = 0;
	TA64Twi* twi = A64Twi1Get();
	if (twi)
		{
		twi->Lock();
		twi->Write(KMpuAddr, KRegPwrMgmt1, 0x40);				// sleep
		twi->Unlock();
		}
	}

TInt TA64Accel::SetRate(TInt aRateHz)
	{
	if (aRateHz < 1 || aRateHz > 200)
		return KErrArgument;
	iRateHz = aRateHz;
	if (iRunning)
		{
		TA64Twi* twi = A64Twi1Get();
		TInt div = 1000 / aRateHz - 1;
		if (div < 0) div = 0; if (div > 255) div = 255;
		twi->Lock();
		twi->Write(KMpuAddr, KRegSmplrtDiv, (TUint8)div);
		twi->Unlock();
		}
	return KErrNone;
	}

void TA64Accel::TimerFn(TAny* aPtr)
	{
	TA64Accel& a = *(TA64Accel*)aPtr;
	if (a.iRunning)
		a.iDfc.Add();
	}

void TA64Accel::DfcFn(TAny* aPtr)
	{
	TA64Accel& a = *(TA64Accel*)aPtr;
	if (!a.iRunning)
		return;
	a.Sample();
	if (a.iRunning)
		a.iTimer.OneShot(NKern::TimerTicks(1000 / (a.iRateHz > 0 ? a.iRateHz : KDefaultRateHz)), ETrue);
	}

void TA64Accel::Sample()
	{
	TA64Twi* twi = A64Twi1Get();
	TUint8 d[6] = { 0, 0, 0, 0, 0, 0 };
	twi->Lock();
	const TInt r = twi->Read(KMpuAddr, KRegAccelXoutH, d, 6);
	twi->Unlock();
	if (r != KErrNone)
		return;
	const TInt16 sx = (TInt16)((d[0] << 8) | d[1]), sy = (TInt16)((d[2] << 8) | d[3]), sz = (TInt16)((d[4] << 8) | d[5]);
	TA64SensorSample s;
	s.iTime = Kern::SystemTime();
	// the DTS mount matrix (0,-1,0) (1,0,0) (0,0,1): device x = -sensor y, device y = sensor x, device z = sensor z
	s.iX = (TInt16)-sy;
	s.iY = sx;
	s.iZ = sz;
#ifdef A64_SENSOR_FAKE_LANDSCAPE
	// A64DEFS=-DA64_SENSOR_FAKE_LANDSCAPE (the rotation chain's bench): report the phone as turned 90 degrees, its right
	// side up (gravity along +x = EOrientationDisplayRightUp), so the UI must go landscape with the phone sitting upright
	{ const TInt16 x = s.iX; s.iX = s.iY; s.iY = (TInt16)-x; }
#endif
	s.iPad = 0;
	NKern::FMWait(&iLock);
	iRing[(iHead + iCount) % KRingSize] = s;
	if (iCount < KRingSize)
		++iCount;
	else
		iHead = (iHead + 1) % KRingSize;		// the oldest is dropped
	NKern::FMSignal(&iLock);
	if (iChannel)
		iChannel->DataReady();
	}

TInt TA64Accel::Take(TA64SensorSample* aOut, TInt aMax)
	{
	NKern::FMWait(&iLock);
	TInt n = 0;
	while (n < aMax && iCount > 0)
		{
		aOut[n++] = iRing[iHead];
		iHead = (iHead + 1) % KRingSize;
		--iCount;
		}
	NKern::FMSignal(&iLock);
	return n;
	}

// --- the device ----------------------------------------------------------------------------------------------------

DA64SensorsDevice::DA64SensorsDevice() : iDfcQ(0)
	{
	iParseMask = KDeviceAllowUnit;
	iUnitsMask = 1u << EA64SensorAccel;
	iVersion = TVersion(RA64Sensor::EMajorVersion, 0, 0);
	}

TInt DA64SensorsDevice::Install()
	{
	TInt r = Kern::DfcQCreate(iDfcQ, 26, &KDfcQName);
	if (r != KErrNone)
		return r;
	iAccel.iDfc.SetDfcQ(iDfcQ);
	return SetName(&KA64SensorsLddName);
	}

void DA64SensorsDevice::GetCaps(TDes8& aDes) const
	{
	TA64SensorInfo info = { iAccel.iWhoAmI, 16384, 2, iAccel.iRateHz };
	TPckgC<TA64SensorInfo> p(info);
	Kern::InfoCopy(aDes, p);
	}

TInt DA64SensorsDevice::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DA64SensorChannel(this);
	return aChannel ? KErrNone : KErrNoMemory;
	}

// --- the channel ---------------------------------------------------------------------------------------------------

TInt DA64SensorChannel::DoCreate(TInt aUnit, const TDesC8*, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(TVersion(RA64Sensor::EMajorVersion, 0, 0), aVer))
		return KErrNotSupported;
	if (aUnit != EA64SensorAccel)
		return KErrNotSupported;
	iClient = &Kern::CurrentThread();
	iClient->Open();
	TInt r = Kern::CreateClientRequest(iReadReq);
	if (r != KErrNone)
		return r;
	iUnit = aUnit;
	return iDev->iAccel.Start(KDefaultRateHz, this);
	}

DA64SensorChannel::~DA64SensorChannel()
	{
	if (iUnit == EA64SensorAccel && iDev->iAccel.iChannel == this)
		iDev->iAccel.Stop();
	if (iReadReq)
		Kern::DestroyClientRequest(iReadReq);
	if (iClient)
		Kern::SafeClose((DObject*&)iClient, NULL);
	}

TInt DA64SensorChannel::DeliverRead()
	{
	if (!iReadReq->IsReady() || !iReadDes)
		return KErrNone;
	TA64SensorSample out[KRingSize];
	const TInt maxLen = Kern::ThreadGetDesMaxLength(iClient, iReadDes);
	const TInt maxN = maxLen / (TInt)sizeof(TA64SensorSample);
	if (maxN <= 0)
		{
		Kern::QueueRequestComplete(iClient, iReadReq, KErrArgument);
		return KErrArgument;
		}
	const TInt n = iDev->iAccel.Take(out, maxN < KRingSize ? maxN : KRingSize);
	if (n == 0)
		return KErrNone;						// nothing yet: the next sample completes it
	TPtrC8 p((const TUint8*)out, n * sizeof(TA64SensorSample));
	TInt r = Kern::ThreadDesWrite(iClient, iReadDes, p, 0, KChunkShiftBy0, iClient);
	Kern::QueueRequestComplete(iClient, iReadReq, r);
	return r;
	}

void DA64SensorChannel::DataReady()
	{
	DeliverRead();
	}

TInt DA64SensorChannel::Request(TInt aReqNo, TAny* a1, TAny* a2)
	{
	if (aReqNo == KMaxTInt)
		{	// cancel
		if (((TUint)a1 & (1u << RA64Sensor::ERequestRead)) && iReadReq->IsReady())
			Kern::QueueRequestComplete(iClient, iReadReq, KErrCancel);
		return KErrNone;
		}
	if (aReqNo < 0)
		{	// an asynchronous request: ~aReqNo, a1 = the TRequestStatus, a2 = the user's {arg1, arg2} (as the Mali LDD;
		// boot 211 took a2 itself for the descriptor -> every read KErrArgument)
		const TInt req = ~aReqNo;
		if (req != RA64Sensor::ERequestRead)
			return KErrNotSupported;
		TAny* args[2];
		kumemget32(args, a2, sizeof(args));
		TInt r = iReadReq->SetStatus((TRequestStatus*)a1);
		if (r != KErrNone)
			return r;
		iReadDes = (TDes8*)args[0];
		DeliverRead();
		return KErrNone;
		}
	switch (aReqNo)
		{
		case RA64Sensor::EControlSetRate:
			return iDev->iAccel.SetRate((TInt)a1);
		case RA64Sensor::EControlGetInfo:
			{
			TA64SensorInfo info = { iDev->iAccel.iWhoAmI, 16384, 2, iDev->iAccel.iRateHz };
			TPckgC<TA64SensorInfo> p(info);
			return Kern::ThreadDesWrite(iClient, a1, p, 0, KChunkShiftBy0, iClient);
			}
		default:
			return KErrNotSupported;
		}
	}

extern "C" void A64SensorsLddInstall()
	{
	TheDevice = new DA64SensorsDevice;
	TInt r = TheDevice ? Kern::InstallLogicalDevice(TheDevice) : KErrNoMemory;
	ekseam_puts("[EKERN][sensors] InstallLogicalDevice(Sensors.A64) r="); ekseam_puthex(r);
	}
