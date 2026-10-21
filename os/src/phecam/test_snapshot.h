// Copyright (c) 2007-2009 Nokia Corporation and/or its subsidiary(-ies).
// All rights reserved.
// This component and the accompanying materials are made available
// under the terms of the Eclipse Public License v2.0, available at
// https://www.eclipse.org/legal/epl-2.0/ (relicensed from v1.0 under
// section 7 of the Eclipse Public License v1.0).
// This Source Code may also be made available under the following Secondary
// Licenses when the conditions for such availability set forth in the Eclipse
// Public License, v. 2.0 are satisfied: GNU General Public License, version 2
// or later.
//
// Initial Contributors:
// Nokia Corporation - initial contribution.
//
// Contributors:
// The Phorepoc Authors
//
// Description:
// testsnapshot.h
// 
//

#ifndef TESTSNAPSHOT_H
#define TESTSNAPSHOT_H

#include "TestCamera.h"
#include <ecam/mcamerasnapshot.h>

const TInt KNumOfSnapshotExtensions = 1;

// Implementations of the MCameraSnapshot class
class CTestCamSnapshot : 	public CBase, 
							public MCameraSnapshot
	{
	friend class CTestCamera;
public:
	static CTestCamSnapshot* NewL(CTestCamera& aOwner);
	~CTestCamSnapshot();
	
private:
	void Release();
	
	//from MCameraSnapshot
	TUint32 SupportedFormats();
	
    void PrepareSnapshotL(CCamera::TFormat aFormat, const TPoint& aPosition, const TSize& aSize, const TRgb& aBgColor, TBool aMaintainAspectRatio);
    
	void PrepareSnapshotL(CCamera::TFormat aFormat, const TSize& aSize, TBool aMaintainAspectRatio);
	
	void SetBgColorL(const TRgb& aBgColor); 
	
	void SetPositionL(const TPoint& aPosition);
	
    TBool IsSnapshotActive() const;

    void StartSnapshot();

	void StopSnapshot();
        	
	MCameraBuffer& SnapshotDataL(RArray<TInt>& aFrameIndexOrder);
	
	// New functions
	void ServiceSnapshot(const CFbsBitGc& aSourceBitmapGc);
	void ServiceBurstImageSnapshot(const CFbsBitGc& aSourceBitmapGc);

private:
	CTestCamSnapshot(CTestCamera& aOwner);
	void Init();
	void ConstructL();
	void DoStartSnapshotL();
	
private:
	CTestCamera& iOwner;
	
	TInt iRefCount;
			
	CFbsBitmap* iSnapshot;
	CFbsBitmapDevice* iSnapshotDev;
	CFbsBitGc* iSnapshotGc;
	
	CFbsBitmap* iSnapshotImage;
	CFbsBitmapDevice* iSnapshotImageDev;
	CFbsBitGc* iSnapshotImageGc;
	
	TRect iSnapshotImageRect;
	CCamera::TFormat iSnapshotFormat;
	TRgb iBgColor;
	
	TUint32 iImageFormatsSupported;
	
	RArray<TRect> iSupportedSnapshotRect;
	
	RTestCameraFrameBuffer iSnapshotBuffer;
	
	volatile TBool iSnapshotPrepared;
	volatile TBool iSnapshotActive;
		
	};
	
#endif // TESTSNAPSHOT_H
