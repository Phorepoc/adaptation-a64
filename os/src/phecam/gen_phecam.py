#!/usr/bin/env python3
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

"""gen_phecam.py -- builds src/phecam/ (the phecam ECam plug-in) from the 2010 TestCamera plug-in (EPL, baseline/symbian-2010/oss.FCL.sf.os.mm/
mmplugins/cameraplugins/source/testcamera). OURS = a few deliberate edits on top of the release source:
  * new UIDs (TestCameraUids.hrh: DLL 0xE70157E9, plug-in 0xE70157EA, info plug-in 0xE70157EB; ECOM version 4 so it wins over the stock test plug-in when both are in a ROM);
  * GetFrame(): draws the latest frame of the camera driver (CPheCamSource, phecamsource.cpp: RDevCameraSc) instead of the moving square; falls back to the
    square while no frame has arrived or the driver is missing;
  * CPheCamSource is created lazily by the first GetFrame and deleted with the plug-in.
Run from the repo root:  py src/phecam/gen_phecam.py     (writes next to itself; the generated files are committed).
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "baseline", "symbian-2010", "oss.FCL.sf.os.mm", "mmplugins", "cameraplugins", "source", "testcamera")
OUT = os.path.dirname(os.path.abspath(__file__))


def rd(n):
    return open(os.path.join(SRC, n), encoding="latin-1", newline="").read()


def wr(n, s):
    open(os.path.join(OUT, n), "w", encoding="latin-1", newline="\n").write(s.replace("\r\n", "\n"))


def sub1(s, a, b):
    assert s.count(a) == 1, (a[:60], s.count(a))
    return s.replace(a, b)


for f in ("test_advanced_settings.cpp", "test_advanced_settings.h", "test_snapshot.cpp", "test_snapshot.h", "test_image_processing.cpp", "test_image_processing.h"):
    wr(f, rd(f))

h = rd("TestCamera.h").replace("\r\n", "\n")
h = sub1(h, "class CDirectScreenAccess;\n", "class CDirectScreenAccess;\nclass CPheCamSource;\t// OURS: the RDevCameraSc pixel source (phecamsource.h)\n")
h = sub1(h, "\tCPeriodic* iTimer;\n", "\tCPeriodic* iTimer;\n\tCPheCamSource* iPheSource;\t// OURS\n")
h = sub1(h, "const TDisplayMode KViewFinderDisplayMode = EColor4K;", "const TDisplayMode KViewFinderDisplayMode = EColor16MU;\t// OURS (was EColor4K: 4 bits/channel posterised the near-grey live picture into magenta/green patches, boot 933)")
wr("TestCamera.h", h)

c = rd("TestCamera.cpp").replace("\r\n", "\n")
c = sub1(c, '#include "TestCamera.h"\n', '#include "TestCamera.h"\n#include "phecamsource.h"\n')
c = sub1(c, "CTestCamera::~CTestCamera()\n\t{\n\tdelete iTimer;\n", "CTestCamera::~CTestCamera()\n\t{\n\tdelete iTimer;\n\tdelete iPheSource;\n")
old = "void CTestCamera::GetFrame(CFbsBitGc& aGc, TPoint& aPos) const\n\t{\n"
new = """void CTestCamera::GetFrame(CFbsBitGc& aGc, TPoint& aPos) const
\t{
\t// OURS (phecam): the latest frame of the camera driver, scaled to the target; the moving square below is the fallback
\tCTestCamera* self = const_cast<CTestCamera*>(this);
\tif (!iPheSource)
\t\t{
\t\tTRAPD(err, self->iPheSource = CPheCamSource::NewL(iCameraIndex));	// the camera index = the driver unit (0 rear, 1 front)
\t\tif (err != KErrNone)
\t\t\tRDebug::Printf("PH: phecam camera driver unavailable (%d): test pattern", err);
\t\t}
\tCFbsBitmap* frame = iPheSource ? iPheSource->Frame() : NULL;
\tif (frame)
\t\t{
\t\t// the stock target GCs draw FADED (SetFaded(ETrue) + the contrast/brightness fading map below): a real frame must
\t\t// not be faded (boot 938: washed-out, low-saturation live view)
\t\taGc.SetFaded(EFalse);
\t\taGc.DrawBitmap(TRect(TPoint(0, 0), aGc.Device()->SizeInPixels()), frame);
\t\taGc.SetFaded(ETrue);
\t\treturn;
\t\t}
"""
c = sub1(c, old, new)
# the still capture draws through GetImgProcessedFrame (the moving-square generator with image processing), not
# GetFrame: boot 940 saved a uniform 0xbfbfbf still. With a driver frame available, it is the frame (unfaded, scaled)
c = sub1(c, "void CTestCamera::GetImgProcessedFrame(CFbsBitGc& aGc, TPoint& aPos) const\n\t{\n",
         "void CTestCamera::GetImgProcessedFrame(CFbsBitGc& aGc, TPoint& aPos) const\n\t{\n"
         "\tif (iPheSource && iPheSource->Frame())\t// OURS (phecam): a still = the latest driver frame\n"
         "\t\t{\n\t\tGetFrame(aGc, aPos);\n\t\treturn;\n\t\t}\n")
# two cameras on the phone (rear OV5640 = index 0, front GC2145 = index 1: the camera driver's units)
c = sub1(c, "TInt CTestCameraInfo::CamerasAvailable()\n\t{\n\treturn 1;\n",
         "TInt CTestCameraInfo::CamerasAvailable()\n\t{\n#if defined(__MARM__) && defined(PHECAM_FRONT)\n\treturn 2;\t// OURS (phecam): rear + front (the front GC2145 is PARKED: build with PHECAM_FRONT to offer it)\n#endif\n\treturn 1;\n")
# the still sizes: phecam's frame is portrait on the phone (rotated in phecamsource.cpp), so the first still size is too
c = sub1(c, "\tUser::LeaveIfError(iImageSizes.Append( TSize(640, 480) )); // VGA\n",
         "#ifdef __MARM__\n\tUser::LeaveIfError(iImageSizes.Append( TSize(480, 640) )); // OURS (phecam): portrait VGA, the phone frame is portrait\n#else\n\tUser::LeaveIfError(iImageSizes.Append( TSize(640, 480) )); // VGA\n#endif\n")
if "#include <e32debug.h>" not in c:
    c = sub1(c, '#include "phecamsource.h"\n', '#include "phecamsource.h"\n#include <e32debug.h>\n')
old = "void FileDependencyUtil::CheckFileDependencyL()\n\t{\t\n"
assert c.count(old) == 1
c = c.replace(old, old + "\treturn;\t// OURS (phecam): the stock plug-in only loads when C:\\102070cc.txt exists (a guard against it running on a phone); phecam is ROM-only by design\n")
wr("TestCamera.cpp", c)

wr("TestCameraUids.hrh", """// OURS (phecam): the ECOM UIDs of the phecam plug-in (the release's TestCamera ones are 0x102070CC-CE)
#ifndef TESTCAMERAUIDS_HRH
#define TESTCAMERAUIDS_HRH

#define KUidOnboardCameraTestDll\t0xE70157E9
#define KUidOnboardCameraTestPlugin\t0xE70157EA
#define KUidOnboardCameraTestInfo\t0xE70157EB

#endif
""")

r = rd("102070CC.RSS").replace("\r\n", "\n")
r = r.replace('version_no = 3;\n\t\t\t\t\tdisplay_name ="ECam Test Plugin";', 'version_no = 4;\n\t\t\t\t\tdisplay_name ="Phorepoc ECam plugin";')
r = r.replace('version_no = 3;\n\t\t\t\t\tdisplay_name = "ECam Test InfoPlugin";', 'version_no = 4;\n\t\t\t\t\tdisplay_name = "Phorepoc ECam InfoPlugin";')
assert "version_no = 4" in r
wr("E70157E9.rss", r)
print("phecam sources written to", OUT)
