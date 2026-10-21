#!/usr/bin/env python3
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

"""gcc2_aliases.py <link.log> <obj>... -- turn GCC 2.95-mangled undefined references into `--defsym old=new` aliases.

The 2010 kernel's .cia files call C++ functions through CSM_* string macros spelled in the GCC 2.95 (RVCT-era)
mangling, e.g. `Unlock__5NKern` for NKern::Unlock(). Modern GCC emits Itanium names (_ZN5NKern6UnlockEv). This
reads the linker's "undefined reference to `X'" lines, converts each old-style name, and prints one
`--defsym X=Y` per name whose Itanium spelling is DEFINED in the given objects (anything it cannot map is reported
on stderr and left for a human). Used by tools/kernel/link_ekern.sh for the SMP nanokernel (nkernsmp's .cia set
differs from the UP one, whose few aliases are spelled out by hand there).
"""
import re
import subprocess
import sys

NM = r"C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin/arm-none-eabi-nm"

# GCC 2.95 builtin type codes -> Itanium codes (the subset kernel signatures use)
TYPES = {"v": "v", "i": "i", "Ui": "j", "l": "l", "Ul": "m", "c": "c", "Uc": "h", "s": "s", "Us": "t",
         "x": "x", "Ux": "y", "b": "b", "w": "w"}


def parse_args(s):
    """GCC2 argument string -> Itanium argument string (builtins, P/R/C prefixes, <len>Class names)."""
    out = ""
    i = 0
    while i < len(s):
        pre = ""
        while i < len(s) and s[i] in "PRC":
            pre += {"P": "P", "R": "R", "C": "K"}[s[i]]
            i += 1
        if i < len(s) and s[i].isdigit():
            m = re.match(r"(\d+)", s[i:])
            n = int(m.group(1))
            j = i + len(m.group(1))
            out += pre + m.group(1) + s[j:j + n]
            i = j + n
            continue
        for code in ("Ui", "Ul", "Uc", "Us", "Ux"):
            if s.startswith(code, i):
                out += pre + TYPES[code]
                i += 2
                break
        else:
            out += pre + TYPES[s[i]]
            i += 1
    return out or "v"


def convert(old):
    m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*?)__(F?)(\d+)?(.*)$", old)
    if not m:
        return None
    name, free, clen, rest = m.groups()
    if free == "F":                                   # a free function: name__F<args>
        return "_Z%d%s%s" % (len(name), name, parse_args(rest))
    if not clen:
        return None
    cls, args = rest[:int(clen)], rest[int(clen):]
    return "_ZN%s%s%d%sE%s" % (clen, cls, len(name), name, parse_args(args))


def main():
    log, objs = sys.argv[1], sys.argv[2:]
    undef = sorted(set(re.findall(r"undefined reference to `([^']+)'", open(log, errors="replace").read())))
    defined = set()
    for line in subprocess.run([NM, "--defined-only"] + objs, capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            defined.add(parts[2])
    for old in undef:
        new = convert(old)
        if new and new in defined:
            print("--defsym %s=%s" % (old, new))
        elif "__" in old and not old.startswith("_Z"):
            print("gcc2_aliases: no mapping for %s (tried %s)" % (old, new), file=sys.stderr)


if __name__ == "__main__":
    main()
