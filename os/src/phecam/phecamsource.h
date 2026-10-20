// phecamsource.h -- OURS (docs/phorepoc/camera-plan.md C1): the pixel source of the phecam ECam plug-in -- the 2010 shared-chunk camera driver
// (DCameraScLdd, device "CameraSc") through RDevCameraSc: opens unit 0, runs the sensor in viewfinder mode (YUV 4:2:2 interleaved, 320x240), and keeps
// the most recent frame converted to an EColor16MU CFbsBitmap that the plug-in draws wherever TestCamera drew its moving square.
#ifndef PHECAMSOURCE_H
#define PHECAMSOURCE_H

#include <e32base.h>
#include <fbs.h>
#include <d32camerasc.h>

class CPheCamSource : public CActive
	{
public:
	static CPheCamSource* NewL(TInt aUnit = 0);	// unit = the camera index: 0 rear, 1 front (PinePhone)
	~CPheCamSource();
	CFbsBitmap* Frame() const { return iFrames > 0 ? iBitmap : NULL; }	// NULL until the first frame arrived
	TInt FrameCount() const { return iFrames; }
private:
	CPheCamSource();
	void ConstructL(TInt aUnit);
	void RunL();
	void DoCancel();
	void Convert(const TUint8* aYuv);

	RDevCameraSc iCam;
	RChunk iChunk;
	CFbsBitmap* iBitmap;
	TInt iW, iH, iFrames, iLoggedErr;
	TUint32 iTConv, iTLast, iTGap; TInt iTN;	// timing (2026-10-18)
	TInt iGainR, iGainB; TUint8 iGamma[256];	// raw mode: grey-world gains (1/256), gamma LUT (2026-10-18)
	TBool iOpen, iStarted;
	};

#endif
