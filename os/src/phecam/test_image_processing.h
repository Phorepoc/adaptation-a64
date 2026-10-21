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
// testimageprocessing.h
// 
//

#ifndef TESTIMAGEPROCESSING_H
#define TESTIMAGEPROCESSING_H

#include "TestCamera.h"
#include <ecam/ecamadvsettingsintf.h>

const TInt KNumOfImgProcExtensions = 2;
const TInt KMinBrightness = -100;
const TInt KMaxBrightness = 100;
const TInt KMinContrast = -100;
const TInt KMaxContrast = 100;

// Implementations of the MCameraImageProcessing class
class CTestCamImgProc : 	public CBase, 
							public MCameraImageProcessing,
							public MCameraImageProcessing2
	{
	friend class CTestCamera;
public:
	static CTestCamImgProc* NewL(CTestCamera& aOwner);
	~CTestCamImgProc();
	
protected:
	//from MCameraImageProcessing
	void Release();
	void GetSupportedTransformationsL(RArray<TUid>& aTransformations) const;
	void GetActiveTransformationsL(RArray<TUid>& aTransformations) const;
	void GetTransformationSupportedValuesL(TUid aTransformation, RArray<TInt>& aValues, TValueInfo& aInfo) const;
	TInt TransformationValue(TUid aTransformation) const;
	TInt GetTransformationValue(TUid aTransformation, TInt& aTransformationValue) const;
	void SetTransformationValue(TUid aTransformation, TInt aValue);
	void GetActiveTransformSequenceL(RArray<TUid>& aTransformSequence) const;
	void SetActiveTransformSequenceL(RArray<TUid>& aTransformSequence);
	void SetSourceRect(const TRect& aRect);
	void GetSourceRect(TRect& aRect) const;
	
	//from MCameraImageProcessing2
	void GetConcurrentColorSwappingsSupportedL(TInt& aConcurrentColorSwappingSupported) const;
	void GetColorSwapCapabilitiesL(TInt aIndex, CCamera::CCameraImageProcessing::TColorOperationCapabilities& aColorSwapCapabilities) const;
	void SetColorSwapEntry(TInt aIndex, const CCamera::CCameraImageProcessing::TColorOperationEntry& aColorSwapParameters); 
	void RemoveColorSwapEntry(TInt aIndex);
	void GetColorSwapEntryL(TInt aIndex, CCamera::CCameraImageProcessing::TColorOperationEntry& aColorSwapParameters) const; 
	void StartColorSwapping();
	void CancelColorSwappingL();
	void GetConcurrentColorAccentSupportedL(TInt& aConcurrentColorAccentSupported) const;
	void GetColorAccentCapabilitiesL(TInt aIndex, CCamera::CCameraImageProcessing::TColorOperationCapabilities& aColorAccentCapabilities) const;
	void SetColorAccentEntry(TInt aIndex, const CCamera::CCameraImageProcessing::TColorOperationEntry& aColorAccentParameters); 
	void RemoveColorAccentEntry(TInt aIndex);
	void GetColorAccentEntryL(TInt aIndex, CCamera::CCameraImageProcessing::TColorOperationEntry& aColorAccentParameters) const; 
	void StartColorAccent();
	void CancelColorAccentL();

private:
	CTestCamImgProc(CTestCamera& aOwner);
	void ConstructL();
	
	TBool IsTransform(TUid aTransformation);
	
private:
	CTestCamera& iOwner;
	
	TInt iRefCount;
	
	RArray<TUid> iSupportedTranformations;
	RArray<TUid> iActiveTransformations;
	RArray<TUid> iActiveTransformSequence;
	};
#endif // TESTIMAGEPROCESSING_H
