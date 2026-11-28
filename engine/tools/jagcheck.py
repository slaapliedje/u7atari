#!/usr/bin/env python3
"""Checks the engine test's Jaguar program after it is linked (a POST_BUILD step).

    jagcheck.py <u7engine_jag>       (NM: the toolchain's nm)

Fails the build when its statics end past $1F0000, where its 64 K stack starts (the stack then
runs over them: the display came out as stripes), or when the end of a GPU or DSP program it
copies into those processors' RAM is not in the program (a label left in the GPU's own address
space, after its .org: the copy then ran on into Tom's registers).
"""
import os
import subprocess
import sys

STACK = 0x1F0000


def main():
    nm = os.environ.get("NM", "nm")
    out = subprocess.run([nm, sys.argv[1]], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    bad = []
    if syms.get("__bss_end", 0) > STACK:
        bad.append(f"the statics end at ${syms['__bss_end']:X}, past the stack's ${STACK:X}")
    for name, end in syms.items():
        if not name.endswith("_end") or not (name.startswith("gpu_") or name.startswith("dsp_")):
            continue
        start = syms.get(name[:-4])
        if start is None:
            continue
        room = 0x2000 if name.startswith("dsp_") else 0x1000     # (their RAM: 8K, 4K)
        if not 0x800000 <= end < 0xE00000 or end < start or end - start > room:
            bad.append(f"{name} is ${end:X} ({name[:-4]} ${start:X}): not the program's end of it")
    for b in bad:
        print(f"jagcheck: {b}", file=sys.stderr)
    sys.exit(1 if bad else 0)


main()
