# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

"""svpfb_patch.py -- the one delta to the 2010 Syborg framebuffer driver that BOARD=virt compiles (virtfb.cpp).

svpframebuffer.cpp allocates the secure-display buffer with its own Epoc::AllocPhysicalRam call, then ignores
the result and maps `ivRamPhys + iSize` -- it assumes the two allocations are adjacent. They were, by luck,
whenever the ROM ran from flash. With the ROM loaded into RAM (CFG_ROMINRAM, the U-Boot boot shape, P3 rung 3)
the RAM layout differs, the guess maps memory owned by someone else (the page directories), and the first user
thread faults on its own stack (pinephone-port.md). The fix maps what was allocated. Asserted to match once.
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
# ROW PITCH (2026-10-22): the PinePhone display (BOARD=a64) uses the real phones' framebuffer row pitch, 64-byte aligned
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
text = PITCH_DEF + text
os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "svpframebuffer.cpp"), "w", encoding="latin-1", newline="\n") as f:
    f.write(text.replace(OLD, NEW).replace(OLD2, NEW2))
print(f"svpfb_patch: {SRC} -> {OUT}/svpframebuffer.cpp")
