#!/usr/bin/env python3
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# h2inc.py -- Symbian h2inc equivalent for the bootstrap chain.
#
# The RVCT bootstrap `INCLUDE`s several `.inc` files that Symbian's own build GENERATES
# from the matching C++ `.h` by an "h2inc" step (they are not in the source tree). This tool
# regenerates them so armasm2gas + arm-none-eabi-as can assemble the bootstrap.
#
# Why compile-based, not a C parser:  the struct-offset symbols (SSuperPageBase_iPageDir,
# ...) MUST match EXACTLY the byte layout the C++ compiler gives those structs -- the
# bootstrap writes fields the compiled kernel reads.  So we let the REAL toolchain
# (arm-none-eabi-g++ 14.2, the same one symcc drives) compute every value:  a probe TU
# emits one `.equ NAME, <value>` per symbol via inline asm with an "i" immediate operand
# (offsetof / sizeof / enum / const), compiled with `-S`; we harvest those `.equ` lines
# and rewrite them as armasm `NAME EQU value` for the translator to fold.
#
# CPU/MM config is pinned to the syborg target (Cortex-A8 / ARMv7 / flexible MM) so the
# generated constants (e.g. KArmV6PermRWNO under __CPU_MEMORY_TYPE_REMAPPING) match the
# kernel build, NOT symcc's ARMv5 default.
#
# Auto-harvested constants use a drop-on-undefined retry loop, so names that only exist in
# an inactive #if branch (ARMv5-only, SMP-only, direct-MM-only) are silently skipped
# rather than breaking the whole probe.
#
# Source headers are the EPL-licensed 2010 open-source release.
#
# Usage:   py tools/armasm2gas/h2inc.py [--out-dir DIR] [--only name1,name2] [-v]
# Output:  <out-dir>/<name>.inc   (default out-dir: .bootstrap-gen/bootstrap-inc)

import argparse
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EKA = os.path.join(REPO, "baseline", "symbian-2010",
                   "oss.FCL.sf.os.kernelhwsrv", "kernel", "eka")

def _find_gxx():
    import shutil
    cands = []
    env = os.environ.get("SYMBIAN_GXX")
    if env:
        cands.append(env)
    cands += [
        r"C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-g++.exe",
        "/c/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin/arm-none-eabi-g++",
    ]
    for c in cands:
        # normalise a leading msys /c/ to a native Windows path
        n = re.sub(r"^/([a-zA-Z])/", lambda m: m.group(1).upper() + ":/", c)
        for p in (c, n, n + ".exe"):
            if os.path.exists(p):
                return p
    onpath = shutil.which("arm-none-eabi-g++")
    return onpath or cands[-1]


GXX = _find_gxx()

# --- compile flags: match the syborg (Cortex-A8 / ARMv7 / flexible MM) kernel build ------
DEFS = [
    "-D__SYMBIAN32__", "-D__GCCE__", "-D__EABI__", "-D__MARM__", "-D__EPOC32__",
    "-D_UNICODE", "-D__KERNEL_MODE__",
    "-D__CPU_CORTEX_A8N__",       # -> __CPU_CORTEX_A8__ -> __CPU_ARMV7 -> memory-type remapping
    "-D__MEMMODEL_FLEXIBLE__",
]
FLAGS = ["-c", "-S", "-march=armv7-a", "-mfpu=vfpv3", "-fshort-wchar",
         "-std=gnu++03", "-fno-exceptions", "-fno-rtti", "-w"]

# Memory model: "flexible" or "multiple" (2010 syborg reference / this QEMU).
# Selects which mmboot.h feeds mmboot.inc and adds the matching __MEMMODEL_*__ define.
MM_MODEL = os.environ.get("MM_MODEL", "flexible")
if MM_MODEL == "multiple":
    DEFS = [d for d in DEFS if d != "-D__MEMMODEL_FLEXIBLE__"] + ["-D__MEMMODEL_MULTIPLE__"]
if os.environ.get("SMP") == "1":        # mmboot.h's KAPBootPage* are #ifdef __SMP__; cpudefs.h only accepts
    DEFS = [d for d in DEFS if d != "-D__CPU_CORTEX_A8N__"] + ["-D__SMP__", "-D__CPU_CORTEX_A9__"]  # A9 (as the kernel)
INCS = [
    "-include", os.path.join(REPO, "toolchain", "symbian_gcc_prefix.h"),
    "-I" + os.path.join(REPO, "toolchain", "generated"),
    "-I" + os.path.join(EKA, "include"),
    "-I" + os.path.join(EKA, "common"),
    "-I" + os.path.join(EKA, "include", "kernel"),
    "-I" + os.path.join(EKA, "include", "memmodel"),
    "-I" + os.path.join(EKA, "include", "nkern"),
    "-I" + os.path.join(EKA, "include", "nkern", "arm"),
    "-I" + os.path.join(EKA, "include", "kernel", "arm"),
    "-I" + os.path.join(EKA, "include", "memmodel", "epoc"),
    "-I" + os.path.join(EKA, "include", "memmodel", "epoc", MM_MODEL),  # memmodel.h
    "-I" + os.path.join(REPO, "toolchain", "kinc"),
]

# --- what each generated .inc must contain -----------------------------------------------
# Per include:
#   headers : #include lines (in order) for the probe TU
#   const_src : header file(s) to text-scan for const/enum/#define NAMES to auto-harvest
#   structs : list of (cpp_type, armasm_prefix, [field...]) -> offsetof each + a <prefix>_sz
# armasm_prefix "" means use cpp_type as prefix. cpp_type may be nested (A::B).

SP_FIELDS = ["iTotalRomSize", "iTotalRamSize", "iPageDir", "iRamBootData", "iRomBootData",
             "iBootTable", "iCodeBase", "iBootFlags", "iBootAlloc", "iMachineData",
             "iActiveVariant", "iPrimaryEntry", "iInitialHeapSize", "iDebugPort", "iCpuId",
             "iUncachedAddress", "iRootDirList", "iHwStartupReason", "iKernelLimit",
             "iRamBase", "iStackInfo", "iArmL2CacheBase", "iRomHeaderPhys",
             "iAPBootPagePhys", "iAPBootPageLin", "iAPBootPageDirPhys", "iSmrData",
             "iPlatformSpecificMappings"]

ROMHDR_FIELDS = ["iRomBase", "iRomSize", "iRomRootDirectoryList", "iKernDataAddress",
                 "iKernelLimit", "iPrimaryFile", "iTime", "iCheckSum", "iVersion",
                 "iUserDataAddress", "iTotalSvDataSize", "iDebugPort",
                 "iUncompressedSize", "iPageableRomStart"]

EXTHDR_FIELDS = ["iRomBase", "iRomRootDirectoryList", "iTime", "iKernelVersion",
                 "iKernelCheckSum", "iKernelTime", "iUncompressedSize"]

IMGHDR_FIELDS = ["iCodeAddress", "iDataAddress", "iDataSize", "iEntryPoint",
                 "iHardwareVariant", "iHeapSizeMin", "iHeapSizeMax", "iStackSize",
                 "iNextExtension"]

STACK_FIELDS = ["iIrqStackBase", "iIrqStackSize", "iFiqStackBase", "iFiqStackSize",
                "iUndStackBase", "iUndStackSize", "iAbtStackBase", "iAbtStackSize"]

SPEC = {
    "bootdefs": {
        "headers": ["e32def.h", "kernel/arm/bootdefs.h"],
        "const_src": ["include/kernel/arm/bootdefs.h"],
        "structs": [
            ("SDirectAlloc", "", ["iNextPageTable", "iLimit"]),
            ("SAllocData", "", ["iSkip", "iExtraSkip", "iNextPageTable"]),
            ("SAllocData::SPos", "SAllocData_SPos", ["iMask", "iFinal"]),
        ],
    },
    "kernboot": {
        "headers": ["e32def.h", "kernboot.h"],
        "const_src": ["include/kernel/kernboot.h"],
        "structs": [
            ("SSuperPageBase", "", SP_FIELDS),
            ("TStackInfo", "", STACK_FIELDS),
            ("SRamBank", "", ["iBase", "iSize"]),
            ("SRomBank", "", ["iBase", "iSize", "iWidth", "iLinBase"]),
            ("SSmrBank", "", []),   # only SSmrBank_sz is referenced
        ],
    },
    "e32rom": {
        "headers": ["e32def.h", "e32rom.h"],
        "const_src": ["include/e32rom.h"],
        "structs": [
            ("TRomHeader", "", ROMHDR_FIELDS),
            ("TExtensionRomHeader", "", EXTHDR_FIELDS),
            ("TRomImageHeader", "", IMGHDR_FIELDS),
            ("TRomEntry", "", ["iAddressLin"]),
        ],
    },
    "mmboot": {
        "headers": ["e32def.h", "memmodel/epoc/%s/arm/mmboot.h" % MM_MODEL],
        "const_src": ["include/memmodel/epoc/%s/arm/mmboot.h" % MM_MODEL],
        "structs": [],
    },
}

# SMP=1 (the PinePhone SMP rung): bootcpu.s's `IF SMP` part INCLUDEs arm_types.inc + arm_gic.inc -- the AP boot
# code fills an SFullArmRegSet and programs the GIC. Fields = the ones the 2010 bootstrap references.
if os.environ.get("SMP") == "1":
    SPEC["arm_types"] = {
        "headers": ["e32def.h", "kernel/arm/arm_types.h"],
        "const_src": [],
        "structs": [
            ("SFullArmRegSet", "", ["iN", "iA", "iB"]),
            ("SNormalRegs", "", ["iR1", "iR2", "iR3", "iR8Fiq", "iR13Svc", "iR13Abt", "iR13Und", "iR13Irq",
                                 "iR15", "iFlags"]),
            ("SBankedRegs", "", ["iSCTLR", "iACTLR", "iDACR", "iTTBR0", "iTTBR1", "iTTBCR", "iPRRR", "iNMRR",
                                 "iCTXIDR", "iRWRWTID", "iRWROTID", "iRWNOTID"]),
            ("SAuxiliaryRegs", "", ["iTEEHBR", "iCPACR"]),
        ],
    }
    SPEC["arm_gic"] = {
        "headers": ["e32def.h", "nkernsmp/arm/arm_gic.h"],
        "const_src": ["include/nkernsmp/arm/arm_gic.h"],
        "structs": [
            ("GicDistributor", "", ["iCtrl", "iType", "iIntSec", "iEnableClear", "iPendingClear", "iPriority",
                                    "iTarget", "iSoftIrq"]),
            ("GicCpuIfc", "", ["iCtrl", "iPriMask", "iBinaryPoint", "iAck", "iEoi"]),
        ],
    }


def scan_const_names(path):
    """Text-scan a header for candidate constant NAMES (values come from the compiler).

    Picks up: enum members, `const <type> NAME =`, and object-like `#define NAME value`.
    Names that turn out to live in an inactive #if branch are dropped later at compile time.
    """
    names = []
    seen = set()

    def add(n):
        if n and n not in seen and not n.startswith("__"):
            seen.add(n)
            names.append(n)

    text = open(path, encoding="utf-8", errors="replace").read()
    # strip // and /* */ comments so we don't harvest words out of prose
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)

    # const <type...> NAME =
    for m in re.finditer(r"\bconst\s+[A-Za-z_][\w:\s\*&]*?\b([A-Za-z_]\w*)\s*=", text):
        add(m.group(1))

    # object-like #define NAME value   (skip function-like NAME( and bare guards)
    for m in re.finditer(r"^\s*#\s*define\s+([A-Za-z_]\w*)(?=[ \t]+\S)", text, re.M):
        add(m.group(1))

    # enum bodies -> members
    for em in re.finditer(r"\benum\b[^{;]*\{(.*?)\}", text, re.S):
        for member in em.group(1).split(","):
            mm = re.match(r"\s*([A-Za-z_]\w*)", member)
            if mm:
                add(mm.group(1))
    return names


def build_probe(headers, const_names, structs):
    lines = ['#include <%s>' % h for h in headers]
    lines.append("void h2inc_probe(){")
    body = []
    # struct offsets + sizes
    for cpp_type, prefix, fields in structs:
        pref = prefix or cpp_type
        for f in fields:
            body.append((pref + "_" + f,
                         "(long)__builtin_offsetof(%s,%s)" % (cpp_type, f)))
        body.append((pref + "_sz", "(long)sizeof(%s)" % cpp_type))
    # auto constants
    for n in const_names:
        body.append((n, "(long)(%s)" % n))
    for sym, expr in body:
        lines.append('  __asm__ volatile(".equ %s, %%c0"::"i"(%s));' % (sym, expr))
    lines.append("}")
    return "\n".join(lines) + "\n", [s for s, _ in body]


UNDEF_RE = re.compile(
    r"error:.*?['\u2018]([A-Za-z_]\w*)['\u2019].*?"
    r"(?:was not declared|does not name|has not been declared|is not a member)")


def compile_probe(src, verbose=False):
    """Return (ok, asm_text_or_stderr, undefined_names)."""
    with tempfile.TemporaryDirectory() as d:
        cpp = os.path.join(d, "probe.cpp")
        asm = os.path.join(d, "probe.s")
        open(cpp, "w", encoding="utf-8").write(src)
        cmd = [GXX] + FLAGS + DEFS + INCS + ["-o", asm, cpp]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode == 0:
            return True, open(asm, encoding="utf-8", errors="replace").read(), []
        undef = set(UNDEF_RE.findall(p.stderr))
        if verbose and not undef:
            sys.stderr.write(p.stderr)
        return False, p.stderr, sorted(undef)


def harvest(asm_text):
    out = {}
    for m in re.finditer(r"^\s*\.equ\s+([A-Za-z_]\w*)\s*,\s*(-?\d+)", asm_text, re.M):
        out[m.group(1)] = int(m.group(2))
    return out


def gen_one(name, spec, out_dir, verbose=False):
    const_names = []
    for cs in spec["const_src"]:
        const_names += scan_const_names(os.path.join(EKA, cs))
    src, want = build_probe(spec["headers"], const_names, spec["structs"])

    dropped = []
    for _ in range(40):
        ok, res, undef = compile_probe(src, verbose)
        if ok:
            break
        if not undef:
            sys.stderr.write("[h2inc] %s: compile failed (no droppable symbol):\n" % name)
            sys.stderr.write(res)
            return None
        dropped += undef
        drop = set(undef)
        src = "\n".join(l for l in src.splitlines()
                        if not any(('.equ %s,' % u) in l for u in drop)) + "\n"
    else:
        sys.stderr.write("[h2inc] %s: did not converge\n" % name)
        return None

    values = harvest(res)
    struct_syms = set()
    for cpp_type, prefix, fields in spec["structs"]:
        pref = prefix or cpp_type
        for f in fields:
            struct_syms.add(pref + "_" + f)
        struct_syms.add(pref + "_sz")

    # sanity: every struct symbol we asked for must be present (correctness matters)
    missing = [s for s in struct_syms if s not in values]
    if missing:
        sys.stderr.write("[h2inc] %s: MISSING struct offsets: %s\n" % (name, ", ".join(missing)))
        return None

    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, name + ".inc")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("; %s.inc -- GENERATED by tools/armasm2gas/h2inc.py -- DO NOT EDIT\n" % name)
        f.write("; h2inc of %s (EPL-2010 source), Cortex-A8/ARMv7/flexible-MM.\n" %
                ", ".join(spec["const_src"]))
        f.write("; struct offsets = arm-none-eabi-g++ 14.2 layout (matches the kernel build).\n\n")
        if struct_syms:
            f.write("; --- struct field offsets and sizes ---\n")
            for s in sorted(struct_syms):
                f.write("%s\tEQU\t%d\n" % (s, values[s]))
            f.write("\n")
        consts = sorted(k for k in values if k not in struct_syms)
        if consts:
            f.write("; --- constants / enums / #defines ---\n")
            for s in consts:
                v = values[s]
                f.write("%s\tEQU\t0x%08x\n" % (s, v & 0xFFFFFFFF) if v >= 0
                        else "%s\tEQU\t%d\n" % (s, v))
    print("[h2inc] %-10s -> %s  (%d offsets, %d consts%s)" % (
        name, os.path.relpath(path, REPO), len(struct_syms), len(consts),
        ", dropped %d inactive" % len(set(dropped)) if dropped else ""))
    return path


def main():
    ap = argparse.ArgumentParser(description="h2inc for the Symbian bootstrap chain")
    ap.add_argument("--out-dir",
                    default=os.path.join(REPO, ".bootstrap-gen", "bootstrap-inc"))
    ap.add_argument("--only", help="comma-separated subset of: " + ",".join(SPEC))
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(GXX):
        sys.exit("[h2inc] arm-none-eabi-g++ not found at:\n  %s\n"
                 "  set SYMBIAN_GXX to override." % GXX)

    todo = args.only.split(",") if args.only else list(SPEC)
    rc = 0
    for name in todo:
        if name not in SPEC:
            sys.exit("[h2inc] unknown include '%s' (have: %s)" % (name, ",".join(SPEC)))
        if gen_one(name, SPEC[name], args.out_dir, args.verbose) is None:
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
