# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

"""svpfb_patch.py -- the one delta to the 2010 Syborg framebuffer driver that BOARD=virt compiles (virtfb.cpp).

svpframebuffer.cpp allocates the secure-display buffer with its own Epoc::AllocPhysicalRam call, then ignores
the result and maps `ivRamPhys + iSize` -- it assumes the two allocations are adjacent. They were, by luck,
whenever the ROM ran from flash. With the ROM loaded into RAM (CFG_ROMINRAM, the U-Boot boot shape, P3 rung 3)
the RAM layout differs, the guess maps memory owned by someone else (the page directories), and the first user
thread faults on its own stack (the port notes). The fix maps what was allocated. Asserted to match once.
Usage: py tools/kernel/svpfb_patch.py <svpframebuffer.cpp> <out dir>  -> <out>/svpframebuffer.cpp
"""
import os
import sys

SRC, OUT = sys.argv[1], sys.argv[2]
OLD = "\tiSecurevRamPhys = ivRamPhys + iSize;\n"
NEW = "\tiSecurevRamPhys = Syborg::VideoRamPhysSecure;\t// VIRT: map the secure buffer actually allocated\n"
with open(SRC, encoding="latin-1", newline="") as f:
    text = f.read().replace("\r\n", "\n")
n = text.count(OLD)
if n != 1:
    sys.exit(f"svpfb_patch: expected 1 match, found {n}")
# four composition buffers, not two (2026-10-09: the OURS drivers/display.h in src/phorepoc-arm/a64/display/lddinc
# says KDisplayCBMax = 4 so the compositor never waits for a post; the extra buffer is harmless on BOARD=virt)
OLD2 = "\tTUint round = 2*Kern::RoundToPageSize(iSize);\n"
NEW2 = "\tTUint round = 4*Kern::RoundToPageSize(iSize);\t// OURS: 4 composition buffers (lddinc/drivers/display.h)\n"
if text.count(OLD2) != 1:
    sys.exit(f"svpfb_patch: expected 1 match of the composition RAM line, found {text.count(OLD2)}")
# ROW PITCH (2026-10-22): the phone display (BOARD=a64) uses the real phones' framebuffer row pitch, 64-byte aligned
# (360 px -> 368 px = 1472 bytes): the games written for S60 5.2+ hard-code it and write the screen memory directly
# (SkyForce Reloaded: UserSvr::ScreenInfo). iSize, iOffsetBetweenLines and the specified-mode info all use it. The QEMU virt
# board (ramfb, stride width*4) keeps width*4.
PITCH_DEF = ("#ifdef __A64__\n#define PHFB_PITCH(w) ((((w)*4)+63)&~63)\n#else\n#define PHFB_PITCH(w) ((w)*4)\n#endif\n")
for old, new in (("iVideoInfo.iSizeInPixels.iWidth*4; //Offset depends on width of framebuffer",
                  "PHFB_PITCH(iVideoInfo.iSizeInPixels.iWidth); //Offset depends on width of framebuffer"),
                 ("iSize = 4*width*height; //*4 as 32bits per pixel", "iSize = PHFB_PITCH(width)*height;"),
                 ("iVideoInfo.iOffsetBetweenLines = width*4;", "iVideoInfo.iOffsetBetweenLines = PHFB_PITCH(width);")):
    if text.count(old) != 1:
        sys.exit(f"svpfb_patch: pitch line not found once: {old!r}")
    text = text.replace(old, new)
# FLIPPED SURFACE (2026-10-23, r24): the generic screen driver (screendriver/sgeneric/scnew.cpp DeviceOrientationsAvailable, SetDeviceOrientation)
# asks HAL::Get(EDisplayOffsetBetweenLines) with the in-value  EHalField | EHalFlippedFlag (0x80000000)  for the stride of the
# 90/270-degree surface. The Syborg handler treats the in-value as a mode number, so the flag made it KErrArgument -> the screen
# device claimed only Normal|180 -> WSERV's DSA device was rejected for a rotated scene ("Current Rotation Mode not supported by the
# DSA device") -> RDirectScreenAccess::Construct KErrNotSupported (Dungeon Hunter 2: the intro video player, "Feature is not supported").
# Strip the flag, answer with the flipped geometry (sizes swapped, stride = height*4, as a64dispchan.cpp's iFlipped).
OLD3 = "TInt DLcdPowerHandler::GetSpecifiedDisplayModeInfo(TInt aMode, TVideoInfoV01& aInfo)\n{\n  if(aMode < 0 || aMode >= KConfigLcdNumberOfDisplayModes)"
NEW3 = ("TInt DLcdPowerHandler::GetSpecifiedDisplayModeInfo(TInt aMode, TVideoInfoV01& aInfo)\n{\n"
        "  const TBool phFlipped = (TUint32)aMode & 0x80000000u;\t// TScreenSurfaceUsage::EHalFlippedFlag\n"
        "  aMode = (TInt)((TUint32)aMode & 0x7fffffffu);\n"
        "  if(aMode < 0 || aMode >= KConfigLcdNumberOfDisplayModes)")
OLD4 = "\t  aInfo.iBitsPerPixel       = KConfigBitsPerPixel;\n\t}\n  return KErrNone;\n}"
NEW4 = ("\t  aInfo.iBitsPerPixel       = KConfigBitsPerPixel;\n\t}\n"
        "  if(phFlipped)\n\t{\n"
        "\t  TInt pw = aInfo.iSizeInPixels.iWidth, ph = aInfo.iSizeInPixels.iHeight;\n"
        "\t  aInfo.iSizeInPixels.iWidth = ph; aInfo.iSizeInPixels.iHeight = pw;\n"
        "\t  TInt tw = aInfo.iSizeInTwips.iWidth, th = aInfo.iSizeInTwips.iHeight;\n"
        "\t  aInfo.iSizeInTwips.iWidth = th; aInfo.iSizeInTwips.iHeight = tw;\n"
        "\t  aInfo.iOffsetBetweenLines = ph*4;\n\t}\n"
        "  return KErrNone;\n}")
for o in (OLD3, OLD4):
    if text.count(o) != 1:
        sys.exit(f"svpfb_patch: flipped-mode anchor not found once: {o!r}")
text = text.replace(OLD3, NEW3).replace(OLD4, NEW4)
text = PITCH_DEF + text
os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "svpframebuffer.cpp"), "w", encoding="latin-1", newline="\n") as f:
    out = text.replace(OLD, NEW).replace(OLD2, NEW2)
    if os.environ.get("PH_FB_CACHED") == "1":
        # r34 experiment: the legacy video RAM (the DSA surface) CACHED: the compositor reads it every update and an
        # uncached read costs ~15 ms per 640x360 frame
        out = out.replace("DPlatChunkHw::New(iChunk,ivRamPhys,iSize,EMapAttrUserRw|EMapAttrBufferedC)", "DPlatChunkHw::New(iChunk,ivRamPhys,iSize,EMapAttrUserRw|EMapAttrCachedMax)")
    f.write(out)
print(f"svpfb_patch: {SRC} -> {OUT}/svpframebuffer.cpp")
