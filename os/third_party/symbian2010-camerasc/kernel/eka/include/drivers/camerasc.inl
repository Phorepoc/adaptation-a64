// Copyright (c) 2004-2009 Nokia Corporation and/or its subsidiary(-ies).
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
// e32\include\drivers\camerasc.inl
// 
//

inline DCameraScPdd* DCameraScLdd::Pdd()
	{return((DCameraScPdd*)iPdd);}

inline DThread* DCameraScLdd::OwningThread()
	{return(iOwningThread);}

inline TInt DCameraScLdd::CurrentFrameHeight()
	{return(iCaptureModeConfig[iCaptureMode].iFrameHeight);}

inline TInt DCameraScLdd::CurrentFrameWidth()
	{return(iCaptureModeConfig[iCaptureMode].iFrameWidth);}

inline TInt DCameraScPdd::SpecifyCustomConfig(TDes8& /*aConfig*/)
	{return(KErrArgument);}

inline TBool TCameraScRequestQueue::IsEmpty()
	{return(iPendRequestQ.IsEmpty());}
