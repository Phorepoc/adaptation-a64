# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

"""components_a64 -- the adaptation-a64 components (merged into core's registry when this adaptation is built:
PHOREPOC_ADAPTATIONS, see the pdk's tools/emitter/paths.py). Paths are relative to the merged OS tree."""
import os

from paths import OS_ROOT  # noqa: F401

COMPONENTS = {
    "ssya64": dict(mmp=os.path.join(OS_ROOT, "src/ssya64/group/ssya64.mmp")),
    # OURS (NGA G1a): RDisplayChannel flip test -- red/blue composition buffers, vblank-paced post rate
    "tdispflip": dict(mmp=os.path.join(OS_ROOT, "src/tdispflip/tdispflip.mmp"), test=True),
    # OURS (GPU G3-K2): the first Mali-400 frame -- a GP + PP clear job built by hand, pixel-checked, shown
    "tmaliclear": dict(mmp=os.path.join(OS_ROOT, "src/tmaliclear/tmaliclear.mmp"), test=True),
    # OURS (NGA G1b): the LoadLogicalDevice freeze probe
    "tldd": dict(mmp=os.path.join(OS_ROOT, "src/tldd/tldd.mmp"), test=True),
    # OURS (VDU V4): the DevVideo decoder hw device (ECOM, interface 0x101FB4BE) on the A64 Video Engine: H.264 + MPEG-2;
    # src/phevdec/phevdec.h. Needs devvideo + puresolverutils + codecapiresolver (+ ecom) in the ROM.
    "phevdec": dict(mmp=os.path.join(OS_ROOT, "src/phevdec/phevdec.mmp")),
    # OURS (VDU V4): drives the phevdec plug-in through CMMFDevVideoPlay over the phevdetest clips ("PH: dvp");
    # SSM_TEST=phevdectest ROM_TAIL="devvideo,puresolverutils,codecapiresolver,phevdec,phevdectest"
    "phevdectest": dict(mmp=os.path.join(OS_ROOT, "src/phevdectest/phevdectest.mmp"), test=True, stack=0x20000,   # the plug-in decodes on this thread's stack
                        rom_data=["resource\\phevde\\ipb.m2v=src/phevdetest/data/ipb.m2v",
                                  "resource\\phevde\\ipb.nv12=src/phevdetest/data/ipb.nv12"] +
                                 [f"resource\\phevde\\{s}.{x}=src/phevdetest/data/{s}.{x}"
                                  for s in ("hbase", "hmain", "hh8x8", "hcqm", "hhigh", "hmbaffm", "hmbaffh", "hpaff1", "hpaff2", "hpaff3", "hpaff4", "hpaffl") for x in ("h264", "fnv")]),
    "phevdec_sw": dict(mmp=os.path.join(OS_ROOT, "src/phevdec/phevdec_sw.mmp"), rom="phevdec.dll", allow_wsd=True),   # (stage-timing statics)
    # decoded back with the phevdec cores; "PH: venc" lines. SSM_TEST=phevenctest (no ROM data, no extra ROM_TAIL)
    "phevenctest": dict(mmp=os.path.join(OS_ROOT, "src/phevenctest/phevenctest.mmp"), test=True),
    # OURS (VE encoder rung 2, ve-encoder-plan.md section 11): 640x480 + 1280x720, IDR + 15 P, fixed QP 28 and rate control, decoded back
    # with the phevdec cores; "PH: venc2" lines. SSM_TEST=phevenctest2 (no ROM data, no extra ROM_TAIL)
    "phevenctest2": dict(mmp=os.path.join(OS_ROOT, "src/phevenctest2/phevenctest2.mmp"), test=True),
    "phevdetest": dict(mmp=os.path.join(OS_ROOT, "src/phevdetest/phevdetest.mmp"), test=True,
                       rom_data=["resource\\phevde\\t320.m2v=src/phevdetest/data/t320.m2v",
                                 "resource\\phevde\\t320.nv12=src/phevdetest/data/t320.nv12",
                                 "resource\\phevde\\ipb.m2v=src/phevdetest/data/ipb.m2v",
                                 "resource\\phevde\\ipb.nv12=src/phevdetest/data/ipb.nv12"] +
                                [f"resource\\phevde\\{s}.{x}=src/phevdetest/data/{s}.{x}"
                                 for s in ("hbase", "hmain", "hh8x8", "hcqm", "hhigh", "hmbaffm", "hmbaffh", "hpaff1", "hpaff2", "hpaff3", "hpaff4", "hpaffl") for x in ("h264", "fnv")]),   # VE driver test (VDU V0-V3)
    # hardware-scaled into the panel, DE register readback + 240 vblank-paced flips; "PH: de2ov" lines, ends PH: de2ov PASS/FAIL.
    #   ARM_VFP=1 CPUS=4 JOBS=4 ./ph arm phedetest ; SSM_TEST=phedetest (no ROM data, no extra ROM_TAIL; needs the kernel with a64deov.cpp)
    "phedetest": dict(mmp=os.path.join(OS_ROOT, "src/phedetest/phedetest.mmp"), test=True),
    # GPU plan G6b: RSgImage <-> GLES2 zero copy (eglCreatePixmapSurface / eglCreateImageKHR on an RSgImage*); "PH: sg" lines.
    #   ARM_VFP=1 ./ph arm phgl libegl phesgtest   (+ the Mali LDD for EMaliCtrlImportAddress); SSM_TEST=phesgtest
    "phesgtest": dict(mmp=os.path.join(OS_ROOT, "src/phesgtest/phesgtest.mmp"), test=True),
    #   NEON: the colour-conversion intrinsics need -march=armv7-a -mfpu=neon-vfpv4 (softfp: links with the rest of the ROM; D16-D31 are saved)
    "pheencbench": dict(mmp=os.path.join(OS_ROOT, "src/pheencbench/pheencbench.mmp"), test=True, device=True, cdriver=True, allow_wsd=True,
                        static_ctors=True, rtti=False, defs=["__SOFTFP="], stack=0x40000,
                        flags=["-march=armv7-a", "-mfpu=neon-vfpv4", "-mfloat-abi=softfp", "-mword-relocations"],
                        arm_archives=[".codecpack/openh264-arm/lib/libopenh264enc.a"]),
    # through the "PhFs.A64" LDD); started by the SSM's non-critical list (r_cmd_phfs)
    "phfs": dict(mmp=os.path.join(OS_ROOT, "src/phfs/phfs.mmp")),
    # phecam (camera-plan.md C1): the TestCamera plug-in with its pixel source replaced by RDevCameraSc (the 2010 shared-chunk camera driver, built into ekern on x86). Generated
    # CAMERA_RAW=1: Bayer raw8 + our debayer (camera-plan.md "the reference uses RAW"; kernel side A64DEFS=-DA64_CAMERA_RAW)
    "phecam": dict(mmp=os.path.join(OS_ROOT, "src/phecam/phecam.mmp"), defs=(["PHE_CAM_RAW"] if os.environ.get("CAMERA_RAW") == "1" else [])),
    # a minimal CCamera client with a full-screen viewfinder (SSM_TEST=phecamtest): the C0/C1 proof when the S60 test app is not usable
    "phecamtest": dict(mmp=os.path.join(OS_ROOT, "src/phecamtest/phecamtest.mmp"), test=True),
    "testcameraapp": dict(mmp=os.path.join(OS_ROOT, "src/phecam/testcameraapp.mmp"), rsc_incs=["toolchain/shims/techview_rh"]),
}

# core components that get this SoC's hardware (core registry: EXTEND)
EXTEND = {
    # phgl's GPU channel: the Mali-400 (src/symgl/symmali.cpp over RMaliChannel) instead of core's null one
    "phgl": dict(drop=["symgpu_null.cpp"], extra=["src/symgl/symmali.cpp"]),
    # the software H.264 decoder (pheffmpeg) shares the ROM name phevdec.dll with the Video Engine one: on this board the VE decoder is the one,
    # and a full component build must not overwrite phevdec.dll with the software variant
    # (test=True: not in the ROM -- ECOM matches a plug-in DLL by UID3, so a second phevdec DLL in the ROM can be loaded instead of the VE one)
    "phevdec_sw": dict(rom="phevdec_sw.dll", test=True),
}
