# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-1.0

"""gui_rom_args -- rombuild arguments for every built registry component.

Prints one argument per line (`--xip` / spec / `--data` / spec ...) for
build_ekern_rom.sh's GUI=1 ROM: every emitter/components.py entry whose image
exists in emitter/, plus the files its build staged for the ROM:
  <image>.romfiles   compiled resources (build_mmp.build_resources)
  <stem>.rsc         ECOM registration next to a plug-in DLL -> \\Resource\\Plugins
minigui slot renames: wserv.exe -> EWSRV.EXE, econswserv.dll -> ECONS.DLL.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "emitter"))
import components as C  # noqa: E402
import mmp as MMP  # noqa: E402

SLOT = {"wserv.exe": "EWSRV.EXE", "econswserv.dll": "ECONS.DLL"}
SLOT_TAKEN = {v.lower() for v in SLOT.values()} | {"edisp.dll"}   # the text window server's display driver too


# NGA=1 (GPU plan G1b, the user's "minimal image" bisect 2026-10-09): the NGA window server stack replaces its
# non-NGA twins (as tools/phorepoc-x86/x32_rom_args.py); GUI_ROOTS=<key>,... keeps only those components plus the
# closure of their static imports (read from the images: "name{000a0000}.dll" strings) -- a minimal minigui ROM.
NGA = os.environ.get("NGA") == "1"
NGA_TWINS = {"ws32", "wserv", "wsgraphicdrawer", "wsstdplugin", "remotegc", "sgresource", "surfaceupdateclient"}
ROOTS = [k for k in os.environ.get("GUI_ROOTS", "").split(",") if k]
NGA_KEEP = {k for k in os.environ.get("NGA_KEEP", "").split(",") if k}   # twins kept non-NGA (bisecting)
NGA_TWINS -= NGA_KEEP


# ALF=s1b|s2|s3 (1 = s2, 2 = s3; s1 = s1b = s2: the real clients need the server, the compositor needs the alf stage): how much of the real Alfred/Hitchcock set goes in the ROM
ALF_LV = {"s1b": 2, "s1": 2, "1": 2, "s2": 2, "2": 3, "s3": 3}.get(os.environ.get("ALF", "").lower(), 0)

def _closure(roots):
    import re
    by_rom = {}
    for key, comp in C.COMPONENTS.items():
        if comp.get("alf") and (comp["alf"] == "build" or ALF_LV < int(comp["alf"])):   # ALF=1 only (alf-plan.md)
            continue
        if ALF_LV >= 2 and (key in ("alfclient", "alfdecoderserverclient", "libegl", "sflibopenvgrefimpl") or key == "goommonitor" or key == "goommonitor_alfnull" and ALF_LV >= 3):
            continue
        if comp.get("nga") and not NGA or NGA and key in NGA_TWINS:
            continue
        if NGA and comp.get("nga") and key.endswith("_nga") and key[:-4] in NGA_KEEP:
            continue
        try:
            tt = MMP.parse(os.path.join(MMP.BASE, comp["mmp"])).targettype
        except (RuntimeError, FileNotFoundError):
            tt = None
        rom = C.rom_name(key, tt)
        by_rom[(comp.get("rom_as", rom) if NGA else rom).lower()] = (key, rom)
    keep, todo = set(), list(roots)
    while todo:
        k = todo.pop()
        if k in keep or k not in C.COMPONENTS:
            continue
        keep.add(k)
        rom = None
        for name, (key, r) in by_rom.items():
            if key == k:
                rom = r
        if not rom or not os.path.isfile(os.path.join(REPO, "emitter", rom)):
            continue
        data = open(os.path.join(REPO, "emitter", rom), "rb").read()
        for m in re.findall(rb"([A-Za-z0-9_]+)(?:\{[0-9a-fA-F]{8}\})?(?:\[[0-9a-fA-F]{8}\])?\.(dll|DLL)", data):
            dep = (m[0].decode() + ".dll").lower()
            if dep in by_rom:
                todo.append(by_rom[dep][0])
    return keep


def main() -> None:
    out = []
    seen = set()
    keep = _closure(ROOTS) if ROOTS else None
    if keep is not None:
        print("gui_rom_args: GUI_ROOTS closure = %d components" % len(keep), file=sys.stderr)
    for key, comp in C.COMPONENTS.items():
        if comp.get("alf") and (comp["alf"] == "build" or ALF_LV < int(comp["alf"])):   # ALF=1 only (alf-plan.md)
            continue
        if ALF_LV >= 2 and (key in ("alfclient", "alfdecoderserverclient") or key == "goommonitor" or key == "goommonitor_alfnull" and ALF_LV >= 3):
            continue
        if comp.get("nga") and not NGA:         # the NGA stack only in an NGA=1 ROM
            continue
        if NGA and key in NGA_TWINS:
            continue
        if NGA and comp.get("nga") and key.endswith("_nga") and key[:-4] in NGA_KEEP:
            continue
        if keep is not None and key not in keep:
            continue
        try:
            targettype = MMP.parse(os.path.join(MMP.BASE, comp["mmp"])).targettype
        except (RuntimeError, FileNotFoundError) as e:   # FileNotFoundError: an mmp absent from this tree
            # an mmp the host cpp cannot preprocess any more (syborg keymap.mmp: <variant.mmh> is not in the
            # .epoc32 export tree) must not empty the whole list; the registry's rom= names the file anyway
            if "rom" not in comp:
                print("gui_rom_args: skipped %s (%s)" % (key, str(e).splitlines()[0]), file=sys.stderr)
                continue
            targettype = None
        rom = C.rom_name(key, targettype)
        img = os.path.join(REPO, "emitter", rom)
        # one ROM name once, case-insensitively (two keys can share one); the minigui slots win: the text-mode
        # ewsrv.exe / econs.dll / edisp.dll would otherwise shadow EWSRV.EXE / ECONS.DLL (Z: is case-blind)
        stage = comp.get("rom_as", rom) if NGA else rom     # an NGA twin ships under its non-NGA name
        zname = SLOT.get(stage, stage).lower()
        if stage.lower() in SLOT_TAKEN and stage not in SLOT:
            continue
        if os.path.isfile(img) and not comp.get("rsc_only") and zname not in seen:
            seen.add(zname)
            out += ["--xip", "Sys\\Bin\\" + SLOT.get(stage, stage) + "=emitter/" + rom]
            if targettype == "plugin":
                rsc = os.path.splitext(rom)[0] + ".rsc"
                if os.path.isfile(os.path.join(REPO, "emitter", rsc)):
                    out += ["--data", "Resource\\Plugins\\" + rsc + "=emitter/" + rsc]
        man = img + ".romfiles"
        if os.path.isfile(man):
            for line in open(man).read().split():
                out += ["--data", line]
    # the default skin package (py tools/skin_gen.py)
    if os.path.isfile(os.path.join(REPO, ".skin-gen", "data", "series60skin.skn")):
        out += ["--data", "private\\10207114\\import\\101f84b9\\series60skin.skn"
                          "=.skin-gen/data/series60skin.skn"]
        for ext in (".mif", ".mbm"):   # the skin's own bitmaps (same tool)
            if os.path.isfile(os.path.join(REPO, ".skin-gen", "data", "series60skin" + ext)):
                out += ["--data", "resource\\skins\\101f84b9\\series60skin" + ext
                                  + "=.skin-gen/data/series60skin" + ext]
    # HWRM reservation policies: the ones Symbian's stub HW adaptation exports (with hwrmstubplugins)
    stubs = "baseline/symbian-2010/oss.FCL.sf.adapt.stubs/systemswstubs/hwrmstubplugins/data/"
    for f in ("HWRMLightsPolicy.ini", "HWRMVibraPolicy.ini"):
        if os.path.isfile(os.path.join(REPO, stubs + f)):
            out += ["--data", "private\\101f7a02\\" + f + "=" + stubs + f]
    # the icon files (py tools/mif_gen.py)
    icons = os.path.join(REPO, ".icon-gen")
    if os.path.isdir(icons):
        for f in sorted(os.listdir(icons)):
            if f.lower().endswith((".mif", ".mbm")):
                out += ["--data", "resource\\apps\\" + f + "=.icon-gen/" + f]
    # Z:-drive data the packages' bld.inf files export (py tools/zdata_gen.py): suite XML,
    # layout templates, cenrep, MCS content -- the real launcher's runtime data
    zman = os.path.join(REPO, ".zdata-gen", "manifest.txt")
    if os.path.isfile(zman):
        out += [a for line in open(zman).read().splitlines() if line.strip()
                for a in ("--data", line.strip())]
    print("\n".join(out))


if __name__ == "__main__":
    main()
