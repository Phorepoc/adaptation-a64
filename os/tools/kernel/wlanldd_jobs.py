#!/usr/bin/env python3
"""wlanldd_jobs.py -- OURS (2026-10-10): the wlanldd compile jobs for toolchain/cc_batch.py.

    py tools/kernel/wlanldd_jobs.py <group dir> <flags file> <jobs file> <out dir>

Parses the five wlanldd mmps (umac wlanosa wlanosadebug wlantrace wlanldd: SOURCEPATH/SOURCE lines, MACRO lines of
the mmp and of the .mmh files it #includes) and writes one cc_batch record per source:
    -k <src> <common flags from the flags file> -D<mmp macros> [the WLanLogicalDevice.cpp renames] -o <out>/wl_ldd_<lc>.o
The flags file is NUL-separated (the bash caller's EXTRA/MED/WLI/WLFLAGS arrays). Prints the job count. Replaces the
bash loop that spawned grep/cat/cut/basename/dirname/tr per TU (~1000 process starts = 3 minutes on this machine).
"""
import os, re, sys

MMPS = ["umac", "wlanosa", "wlanosadebug", "wlantrace", "wlanldd"]
RENAMES = {"WLanLogicalDevice.cpp": ["-DCreateLogicalDevice=CreateLogicalDevice_wlanldd",
                                     "-DKernelModuleEntry=KernelModuleEntry_wlanldd"]}


def read(p):
    return open(p, encoding="utf-8", errors="replace").read().replace("\r", "")


def macros(group, mmp_text):
    files = [mmp_text]
    for f in re.findall(r'#include *"([^"]*\.mmh)"', mmp_text):
        files.append(read(os.path.join(group, f)))
    out = []
    for t in files:
        out += ["-D" + m for m in re.findall(r"^[ \t]*MACRO[ \t]+([^\s/]+)", t, re.M)]
    return out


def main():
    group, flags_file, jobs_file, outd = sys.argv[1:5]
    flags = [f for f in open(flags_file, "rb").read().decode("utf-8").split("\0") if f]
    jobs, seen = [], {}
    for mmp in MMPS:
        text = read(os.path.join(group, mmp + ".mmp"))
        defs = macros(group, text)
        sp = ""
        for line in text.splitlines():
            m = re.match(r"\s*SOURCEPATH\s+(\S+)", line)
            if m:
                sp = m.group(1)
                continue
            m = re.match(r"\s*SOURCE\s+(\S+)", line)
            if not m:
                continue
            f = m.group(1)
            src = os.path.normpath(os.path.join(group, sp, f)).replace("\\", "/")
            obj = os.path.join(outd, "wl_ldd_" + os.path.splitext(f)[0].lower() + ".o").replace("\\", "/")
            if obj in seen:
                raise SystemExit("duplicate object name: %s (%s and %s)" % (obj, seen[obj], src))
            seen[obj] = src
            jobs.append(["-k", src] + flags + defs + RENAMES.get(f, []) + ["-o", obj])
    with open(jobs_file, "wb") as o:
        for j in jobs:
            o.write(("\0".join(j) + "\0\0").encode("utf-8"))
    print(len(jobs))


if __name__ == "__main__":
    main()
