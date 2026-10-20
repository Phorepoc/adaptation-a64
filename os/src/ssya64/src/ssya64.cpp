// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-1.0

// ssya64.cpp -- OURS (2026-10-10, PinePhone sensors rung S2): the SSY of the "Sensors.A64" LDD (see ssya64.h). The
// sensor server loads every KSsyControlInterfaceUid implementation into an SSY thread of its own, calls NewL with its
// callback, and expects RegisterChannelsL from the constructor; then OpenChannelL -> ChannelOpened(...providers),
// StartChannelDataL(buffer, count) -> BufferFilled when the buffer holds count items (the server swaps the buffer).
// Trace: [ssya64] lines (RDebug, A64_RDEBUG_LOG).
#include "ssya64.h"
#include <ecom/implementationproxy.h>
#include <ssycallback.h>
#include <sensrvgeneralproperties.h>
#include <e32debug.h>

_LIT8(KLocation, "PinePhone");
_LIT8(KVendor, "Phorepoc");
static const TInt KDefaultRateHz = 25;
static const TInt KMaxRateHz = 100;

CSsyA64::CSsyA64(MSsyCallback* aCallback)
	: iCallback(aCallback), iChannelId(0), iOpen(EFalse), iReader(NULL), iOpenedCb(NULL), iClosedCb(NULL), iOpenResult(KErrNone),
	  iSamplesPtr((TUint8*)iSamples, 0, sizeof(iSamples)), iBuffer(NULL), iMaxCount(0), iCount(0), iRateHz(KDefaultRateHz)
	{
	}

CSsyA64* CSsyA64::NewL(MSsyCallback* aCallback)
	{
	CSsyA64* self = new (ELeave) CSsyA64(aCallback);
	CleanupStack::PushL(self);
	self->ConstructL();
	CleanupStack::Pop(self);
	return self;
	}

void CSsyA64::ConstructL()
	{
	RDebug::Printf("[ssya64] NewL: registering the accelerometer XYZ channel");
	RSensrvChannelInfoList list(1);
	CleanupClosePushL(list);
	TSensrvChannelInfo info(0, ESensrvContextTypeDevice, ESensrvQuantityAcceleration, KSensrvChannelTypeIdAccelerometerXYZAxisData,
		KLocation, KVendor, sizeof(TSensrvAccelerometerAxisData), TSensrvAccelerometerAxisData::KDataTypeId);
	list.AppendL(info);
	iCallback->RegisterChannelsL(list);
	iChannelId = list[0].iChannelId;
	CleanupStack::PopAndDestroy(&list);
	BuildPropertiesL();
	iReader = new (ELeave) CReader(*this);
	iOpenedCb = new (ELeave) CAsyncCallBack(TCallBack(OpenedCb, this), CActive::EPriorityStandard);
	iClosedCb = new (ELeave) CAsyncCallBack(TCallBack(ClosedCb, this), CActive::EPriorityStandard);
	RDebug::Printf("[ssya64] channel id %d registered", iChannelId);
	}

CSsyA64::~CSsyA64()
	{
	delete iOpenedCb;
	delete iClosedCb;
	delete iReader;
	if (iOpen)
		iDevice.Close();
	iProperties.Reset();
	}

void CSsyA64::BuildPropertiesL()
	{
	// the mandatory channel properties (sensrvgeneralproperties.h); the data is "scaled": the raw +-ScaledRange maps to
	// +-MeasureRange g, 16384 LSB / g at the MPU-6050's +-2 g
	iProperties.AppendL(TSensrvProperty(KSensrvPropIdDataRate, KSensrvItemIndexNone, iRateHz, KMaxRateHz, 1, EFalse, ESensrvIntProperty));
	iProperties.AppendL(TSensrvProperty(KSensrvPropIdAvailability, KSensrvItemIndexNone, (TInt)ESensrvAvailabilityTrue, (TInt)ESensrvAvailabilityTrue, (TInt)ESensrvAvailabilityFalse, ETrue, ESensrvIntProperty));
	iProperties.AppendL(TSensrvProperty(KSensrvPropIdMeasureRange, KSensrvItemIndexNone, TReal(2.0), TReal(2.0), TReal(-2.0), ETrue, ESensrvRealProperty));
	iProperties.AppendL(TSensrvProperty(KSensrvPropIdChannelDataFormat, KSensrvItemIndexNone, (TInt)ESensrvChannelDataFormatScaled, (TInt)ESensrvChannelDataFormatScaled, (TInt)ESensrvChannelDataFormatScaled, ETrue, ESensrvIntProperty));
	iProperties.AppendL(TSensrvProperty(KSensrvPropIdScaledRange, KSensrvItemIndexNone, 32767, 32767, -32768, ETrue, ESensrvIntProperty));
	iProperties.AppendL(TSensrvProperty(KSensrvPropIdChannelAccuracy, KSensrvItemIndexNone, TReal(0.0), TReal(0.0), TReal(0.0), ETrue, ESensrvRealProperty));
	iProperties.AppendL(TSensrvProperty(KSensrvPropIdChannelScale, KSensrvItemIndexNone, 0, 0, 0, ETrue, ESensrvIntProperty));
	iProperties.AppendL(TSensrvProperty(KSensrvPropIdChannelUnit, KSensrvItemIndexNone, (TInt)ESensrvChannelUnitGravityConstant, (TInt)ESensrvChannelUnitGravityConstant, (TInt)ESensrvChannelUnitGravityConstant, ETrue, ESensrvIntProperty));
	}

TSensrvProperty* CSsyA64::FindProperty(TSensrvPropertyId aId)
	{
	for (TInt i = 0; i < iProperties.Count(); ++i)
		if (iProperties[i].GetPropertyId() == aId)
			return &iProperties[i];
	return NULL;
	}

// --- CSsyControl -----------------------------------------------------------------------------------------------

void CSsyA64::OpenChannelL(TSensrvChannelId aChannelId)
	{
	if (aChannelId != iChannelId)
		User::Leave(KErrNotFound);
	TInt r = iOpen ? KErrAlreadyExists : iDevice.Open(EA64SensorAccel);
	RDebug::Printf("[ssya64] OpenChannelL(%d): RA64Sensor::Open = %d", aChannelId, r);
	if (r == KErrNone)
		{
		iOpen = ETrue;
		TA64SensorInfo info;
		if (iDevice.GetInfo(info) == KErrNone)
			RDebug::Printf("[ssya64] chip id 0x%02x, %d LSB/g, +-%d g, %d Hz", info.iWhoAmI, info.iLsbPerG, info.iRangeG, info.iRateHz);
		iDevice.SetRate(iRateHz);
		}
	iOpenResult = r;
	iOpenedCb->CallBack();
	}

TInt CSsyA64::OpenedCb(TAny* aPtr)
	{
	CSsyA64& s = *(CSsyA64*)aPtr;
	const TBool ok = s.iOpenResult == KErrNone;
	RDebug::Printf("[ssya64] ChannelOpened(%d, %d)", s.iChannelId, s.iOpenResult);
	s.iCallback->ChannelOpened(s.iChannelId, s.iOpenResult, ok ? &s : NULL, ok ? &s : NULL);
	return KErrNone;
	}

TInt CSsyA64::ClosedCb(TAny* aPtr)
	{
	CSsyA64& s = *(CSsyA64*)aPtr;
	RDebug::Printf("[ssya64] ChannelClosed(%d)", s.iChannelId);
	s.iCallback->ChannelClosed(s.iChannelId);
	return KErrNone;
	}

void CSsyA64::CloseChannelL(TSensrvChannelId aChannelId)
	{
	if (aChannelId != iChannelId)
		User::Leave(KErrNotFound);
	RDebug::Printf("[ssya64] CloseChannelL(%d)", aChannelId);
	iReader->Cancel();
	iBuffer = NULL;
	if (iOpen)
		iDevice.Close();
	iOpen = EFalse;
	iClosedCb->CallBack();
	}

// --- MSsyChannelDataProvider ----------------------------------------------------------------------------------

void CSsyA64::StartChannelDataL(const TSensrvChannelId aChannelId, TUint8* aBuffer, TInt aCount)
	{
	if (aChannelId != iChannelId || !iOpen)
		User::Leave(KErrNotFound);
	RDebug::Printf("[ssya64] StartChannelDataL: buffer %08x, count %d", (TUint)aBuffer, aCount);
	iBuffer = aBuffer;
	iMaxCount = aCount;
	iCount = 0;
	IssueRead();
	}

void CSsyA64::StopChannelDataL(const TSensrvChannelId aChannelId)
	{
	if (aChannelId != iChannelId)
		User::Leave(KErrNotFound);
	RDebug::Printf("[ssya64] StopChannelDataL");
	iReader->Cancel();
	iBuffer = NULL;
	}

void CSsyA64::ForceBufferFilledL(const TSensrvChannelId aChannelId)
	{
	if (aChannelId != iChannelId)
		User::Leave(KErrNotFound);
	if (iBuffer)
		SendBufferFilled();
	}

void CSsyA64::GetChannelDataProviderInterfaceL(TUid aInterfaceUid, TAny*& aInterface)
	{
	aInterface = aInterfaceUid.iUid == KSsyChannelDataProviderInterface1.iUid ? static_cast<MSsyChannelDataProvider*>(this) : NULL;
	}

void CSsyA64::IssueRead()
	{
	if (!iBuffer || iReader->IsActive())
		return;
	iSamplesPtr.Zero();
	iDevice.Read(iReader->iStatus, iSamplesPtr);
	iReader->Go();
	}

void CSsyA64::SamplesArrived(TInt aError)
	{
	if (aError != KErrNone)
		{
		// loud, not hot: a failing driver read stops the channel (boot 211 spun on KErrArgument at full speed)
		RDebug::Printf("[ssya64] Read completed %d -- channel data stopped", aError);
		iBuffer = NULL;
		return;
		}
	if (!iBuffer)
		return;
	const TInt n = iSamplesPtr.Length() / (TInt)sizeof(TA64SensorSample);
	TSensrvAccelerometerAxisData* out = (TSensrvAccelerometerAxisData*)iBuffer;
	for (TInt i = 0; i < n && iCount < iMaxCount; ++i)
		{
		const TA64SensorSample& s = iSamples[i];
		TSensrvAccelerometerAxisData d;
		d.iTimeStamp = TTime(s.iTime);
		d.iAxisX = s.iX;
		d.iAxisY = s.iY;
		d.iAxisZ = s.iZ;
		Mem::Copy(&out[iCount], &d, sizeof(d));		// the server's buffer is byte-aligned
		++iCount;
		}
	if (iCount >= iMaxCount)
		SendBufferFilled();
	IssueRead();
	}

void CSsyA64::SendBufferFilled()
	{
	TUint8* newBuf = NULL;
	TInt newCount = 0;
	iCallback->BufferFilled(iChannelId, iCount, newBuf, newCount);
	iBuffer = newBuf;
	iMaxCount = newCount;
	iCount = 0;
	}

// --- MSsyPropertyProvider -------------------------------------------------------------------------------------

void CSsyA64::CheckPropertyDependenciesL(const TSensrvChannelId, const TSensrvProperty&, RSensrvChannelList&)
	{
	}

void CSsyA64::SetPropertyL(const TSensrvChannelId aChannelId, const TSensrvProperty& aProperty)
	{
	if (aChannelId != iChannelId)
		User::Leave(KErrArgument);
	TSensrvProperty* p = FindProperty(aProperty.GetPropertyId());
	if (!p)
		User::Leave(KErrNotFound);
	if (p->ReadOnly())
		User::Leave(KErrAccessDenied);
	if (aProperty.GetPropertyId() == KSensrvPropIdDataRate)
		{
		TInt hz = 0;
		aProperty.GetValue(hz);
		if (hz < 1 || hz > KMaxRateHz)
			User::Leave(KErrArgument);
		if (iOpen)
			User::LeaveIfError(iDevice.SetRate(hz));
		iRateHz = hz;
		p->SetValue(hz);
		RDebug::Printf("[ssya64] data rate -> %d Hz", hz);
		}
	}

void CSsyA64::GetPropertyL(const TSensrvChannelId aChannelId, TSensrvProperty& aProperty)
	{
	if (aChannelId != iChannelId && aChannelId != 0)
		User::Leave(KErrArgument);
	TSensrvProperty* p = FindProperty(aProperty.GetPropertyId());
	if (!p)
		User::Leave(KErrNotFound);
	aProperty = *p;
	}

void CSsyA64::GetAllPropertiesL(const TSensrvChannelId aChannelId, RSensrvPropertyList& aChannelPropertyList)
	{
	if (aChannelId != iChannelId)
		User::Leave(KErrArgument);
	aChannelPropertyList.Reset();
	for (TInt i = 0; i < iProperties.Count(); ++i)
		aChannelPropertyList.AppendL(iProperties[i]);
	}

void CSsyA64::GetPropertyProviderInterfaceL(TUid aInterfaceUid, TAny*& aInterface)
	{
	aInterface = aInterfaceUid.iUid == KSsyPropertyProviderInterface1.iUid ? static_cast<MSsyPropertyProvider*>(this) : NULL;
	}

// --- ECOM -----------------------------------------------------------------------------------------------------

static CSsyControl* NewSsyL(TAny* aCallback)
	{
	return CSsyA64::NewL(static_cast<MSsyCallback*>(aCallback));
	}

const TImplementationProxy ImplementationTable[] =
	{
	IMPLEMENTATION_PROXY_ENTRY(0xE7015735, NewSsyL)
	};

EXPORT_C const TImplementationProxy* ImplementationGroupProxy(TInt& aTableCount)
	{
	aTableCount = sizeof(ImplementationTable) / sizeof(TImplementationProxy);
	return ImplementationTable;
	}
