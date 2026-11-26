#!/usr/bin/env python3
"""Sums a Jaguar profile (PCPROF.BIN, from --profile) by function.

    tools/jag-profile.py <PCPROF.BIN> <program.elf> [rows]

PCPROF.BIN holds a record for every 64th vertical blank under --bench, every 16th otherwise
(platform/jaguar/startup.S): the program counter interrupted and the 64 words of stack above it,
as 33 big-endian longs. Each pc is put down to the function it lies in, by the program's symbols
(m68k-elf-nm, from $NM or the PATH), and the functions are listed by their share of the samples.

A sample in a helper (libgcc's arithmetic, the memory copies) is also put down to the function
that called it: the first long on the stack that points into the code just after a call
instruction, and lies outside the helpers. Those are listed second.
"""
import bisect
import collections
import os
import re
import struct
import subprocess
import sys
import tempfile

RECORD = 132
HELPERS = ("__udivsi3", "__divsi3", "__umodsi3", "__modsi3", "__mulsi3", "__udivdi3",
           "__divdi3", "__umoddi3", "__moddi3", "__muldi3", "__lshrdi3", "__ashldi3",
           "__ashrdi3", "memcpy", "memmove", "memset", "__wrap_memcpy", "__wrap_memmove",
           "__wrap_memset", "__real_memcpy")


def is_helper(name):
    base = name.split("(")[0]
    return base in HELPERS or any(base.startswith(h + "_") for h in HELPERS)


def load_text(elf):
    objcopy = os.environ.get("OBJCOPY", "m68k-elf-objcopy")
    with tempfile.NamedTemporaryFile() as f:
        subprocess.run([objcopy, "-O", "binary", "-j", ".text", elf, f.name], check=True)
        return open(f.name, "rb").read()


def after_call(text, base, a):
    """Whether the instruction before address a is a bsr or jsr."""
    def word(at):
        o = at - base
        return struct.unpack(">H", text[o:o + 2])[0] if 0 <= o <= len(text) - 2 else None
    w2, w4, w6 = word(a - 2), word(a - 4), word(a - 6)
    if w2 is not None and (w2 >> 8 == 0x61 and w2 & 0xFF not in (0, 0xFF) or
                           0x4E90 <= w2 <= 0x4E97):
        return True
    if w4 is not None and (w4 in (0x6100, 0x4EBA, 0x4EB8) or 0x4EA8 <= w4 <= 0x4EB7):
        return True
    return w6 is not None and w6 in (0x4EB9, 0x61FF)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    data = open(sys.argv[1], "rb").read()
    rows = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    nm = os.environ.get("NM", "m68k-elf-nm")
    out = subprocess.run([nm, "-n", "-C", sys.argv[2]], capture_output=True, text=True,
                         check=True).stdout
    symbols = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        # local labels in libgcc's assembly (L4 and so on) belong to the routine before them
        if len(parts) == 3 and parts[1] in "TtWw" and not re.fullmatch(r"L\d+", parts[2]):
            symbols.append((int(parts[0], 16), parts[2]))
    symbols.sort()
    starts = [a for a, _ in symbols]
    text = load_text(sys.argv[2])
    base = 0x802000
    end = base + len(text)

    def function(pc):
        if not base <= pc < end:
            return None
        i = bisect.bisect_right(starts, pc) - 1
        return symbols[i][1] if i >= 0 else "?"

    counts = collections.Counter()
    callers = collections.Counter()
    pairs = collections.Counter()
    total = len(data) // RECORD
    helper_samples = 0
    for r in range(total):
        rec = data[r * RECORD:(r + 1) * RECORD]
        pc = struct.unpack(">I", rec[:4])[0]
        name = function(pc)
        if name is None:
            counts["(RAM: %s)" % ("GPU/DSP" if pc >= 0xF00000 else "DRAM")] += 1
            continue
        counts[name] += 1
        if not is_helper(name):
            continue
        helper_samples += 1
        stack = rec[4:]
        caller = "(not found)"
        for o in range(0, len(stack) - 2, 2):
            a = struct.unpack(">I", stack[o:o + 4])[0]
            if a & 1 or not base < a < end or not after_call(text, base, a):
                continue
            f = function(a)
            if not is_helper(f):
                caller = f
                break
        callers[caller] += 1
        pairs[(name.split("(")[0], caller)] += 1

    print(f"{total} samples")
    for name, n in counts.most_common(rows):
        print(f"{100 * n / total:6.2f}%  {n:6d}  {name[:100]}")
    print(f"\n{helper_samples} samples in helpers, by caller")
    for name, n in callers.most_common(rows):
        print(f"{100 * n / total:6.2f}%  {n:6d}  {name[:100]}")
    print("\nhelper <- caller")
    for (h, c), n in pairs.most_common(rows):
        print(f"{100 * n / total:6.2f}%  {n:6d}  {h} <- {c[:80]}")


if __name__ == "__main__":
    main()
