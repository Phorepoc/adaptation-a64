/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

// ssya64.h -- OURS (PinePhone sensors rung S2): the SSY of
// the "Sensors.A64" LDD. One class plays the three SSY roles the sensor server expects (CSsyControl = open/close a
// channel, MSsyChannelDataProvider = fill the server's buffer, MSsyPropertyProvider = the channel's properties) for the
// one channel of v1: the accelerometer XYZ (TSensrvAccelerometerAxisData, device frame, LSB of +-2 g). The samples come
// from RA64Sensor::Read (an active object in the server's SSY thread).
#ifndef SSYA64_H
#define SSYA64_H
#include <e32base.h>
#include <ssycontrol.h>
#include <ssychanneldataprovider.h>
#include <ssypropertyprovider.h>
#include <sensrvtypes.h>
#include <sensrvproperty.h>
#include <sensrvchannelinfo.h>
#include <sensrvaccelerometersensor.h>
#include "a64sensors_api.h"

class MSsyCallback;

class CSsyA64 : public CSsyControl, public MSsyChannelDataProvider, public MSsyPropertyProvider
	{
public:
	static CSsyA64* NewL(MSsyCallback* aCallback);
	~CSsyA64();
	// CSsyControl
	void OpenChannelL(TSensrvChannelId aChannelId);
	void CloseChannelL(TSensrvChannelId aChannelId);
	// MSsyChannelDataProvider
	void StartChannelDataL(const TSensrvChannelId aChannelId, TUint8* aBuffer, TInt aCount);
	void StopChannelDataL(const TSensrvChannelId aChannelId);
	void ForceBufferFilledL(const TSensrvChannelId aChannelId);
	void GetChannelDataProviderInterfaceL(TUid aInterfaceUid, TAny*& aInterface);
	// MSsyPropertyProvider
	void CheckPropertyDependenciesL(const TSensrvChannelId aChannelId, const TSensrvProperty& aProperty, RSensrvChannelList& aAffectedChannels);
	void SetPropertyL(const TSensrvChannelId aChannelId, const TSensrvProperty& aProperty);
	void GetPropertyL(const TSensrvChannelId aChannelId, TSensrvProperty& aProperty);
	void GetAllPropertiesL(const TSensrvChannelId aChannelId, RSensrvPropertyList& aChannelPropertyList);
	void GetPropertyProviderInterfaceL(TUid aInterfaceUid, TAny*& aInterface);
private:
	CSsyA64(MSsyCallback* aCallback);
	void ConstructL();
	void BuildPropertiesL();
	TSensrvProperty* FindProperty(TSensrvPropertyId aId);
	void IssueRead();
	void SamplesArrived(TInt aError);
	void SendBufferFilled();
	static TInt OpenedCb(TAny* aPtr);
	static TInt ClosedCb(TAny* aPtr);
	// the reader: a CActive on the SSY thread's scheduler
	class CReader : public CActive
		{
	public:
		CReader(CSsyA64& aOwner) : CActive(EPriorityStandard), iOwner(aOwner) { CActiveScheduler::Add(this); }
		~CReader() { Cancel(); }
		void Go() { SetActive(); }
		void RunL() { iOwner.SamplesArrived(iStatus.Int()); }
		void DoCancel() { iOwner.iDevice.ReadCancel(); }
		CSsyA64& iOwner;
		};
	friend class CReader;
private:
	MSsyCallback* iCallback;
	TSensrvChannelId iChannelId;
	RSensrvPropertyList iProperties;
	RA64Sensor iDevice;
	TBool iOpen;
	CReader* iReader;
	// ChannelOpened / ChannelClosed must reach the server AFTER OpenChannelL / CloseChannelL return: the SSY mediator
	// marks the transaction "handled at SSY" only then (sensrvssymediator.cpp), a synchronous callback is dropped
	// (boot 210: the client's open ended KErrCancel). The reference SSY answers through an active object; so do we.
	CAsyncCallBack* iOpenedCb;
	CAsyncCallBack* iClosedCb;
	TInt iOpenResult;
	TA64SensorSample iSamples[16];
	TPtr8 iSamplesPtr;
	TUint8* iBuffer;				// the server's buffer (aCount items of TSensrvAccelerometerAxisData)
	TInt iMaxCount;
	TInt iCount;
	TInt iRateHz;
	};

#endif
