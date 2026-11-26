#!/usr/bin/env python3
"""Sums a Jaguar profile (PCPROF.BIN, from --profile) by function.

    tools/jag-profile.py <PCPROF.BIN> <program.elf> [rows]

PCPROF.BIN holds the program counter at every fourth vertical blank, as big-endian longs
(platform/jaguar/main.c). Each is put down to the function it lies in, by the program's symbols
(m68k-elf-nm, from $NM or the PATH), and the functions are listed by their share of the samples.
"""
import bisect
import collections
import os
import struct
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    data = open(sys.argv[1], "rb").read()
    rows = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    pcs = struct.unpack(">%dI" % (len(data) // 4), data[:len(data) // 4 * 4])
    nm = os.environ.get("NM", "m68k-elf-nm")
    out = subprocess.run([nm, "-n", "-C", sys.argv[2]], capture_output=True, text=True,
                         check=True).stdout
    symbols = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "TtWw":
            symbols.append((int(parts[0], 16), parts[2]))
    symbols.sort()
    starts = [a for a, _ in symbols]
    counts = collections.Counter()
    for pc in pcs:
        if pc >= 0x800000:
            i = bisect.bisect_right(starts, pc) - 1
            counts[symbols[i][1] if i >= 0 else "?"] += 1
        else:
            counts["(RAM: %s)" % ("GPU/DSP" if pc >= 0xF00000 else "DRAM")] += 1
    total = len(pcs)
    print(f"{total} samples")
    for name, n in counts.most_common(rows):
        print(f"{100 * n / total:6.2f}%  {n:6d}  {name[:100]}")


if __name__ == "__main__":
    main()
