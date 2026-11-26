#!/usr/bin/env python3
"""Sums a Jaguar profile (PCPROF.BIN, from --profile) by function.

    tools/jag-profile.py [--frames] <PCPROF.BIN> <program.elf> [rows]

PCPROF.BIN holds a record for every 64th vertical blank under --bench, every 16th otherwise
(platform/jaguar/startup.S): the program counter interrupted and the 64 words of stack above it,
as 33 big-endian longs. Each pc is put down to the function it lies in, by the program's symbols
(m68k-elf-nm, from $NM or the PATH), and the functions are listed by their share of the samples.

A sample in a helper (libgcc's arithmetic, the memory copies) is also put down to the function
that called it: the first long on the stack that points into the code just after a call
instruction, and lies outside the helpers. Those are listed second.

--frames: the program was built with frame pointers (U7_JAG_FRAME_POINTERS), so each record has
32 more longs, the return addresses up a6's chain. Then every function on the way up is counted
(inclusive), and the calls are printed as a tree, outermost first, down to 1% of the samples.
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
    args = sys.argv[1:]
    frames = "--frames" in args
    if frames:
        args.remove("--frames")
    if len(args) < 2:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    data = open(args[0], "rb").read()
    rows = int(args[2]) if len(args) > 2 else 40
    record = RECORD + (128 if frames else 0)
    nm = os.environ.get("NM", "m68k-elf-nm")
    out = subprocess.run([nm, "-n", "-C", args[1]], capture_output=True, text=True,
                         check=True).stdout
    symbols = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        # local labels in libgcc's assembly (L4 and so on) belong to the routine before them
        if len(parts) == 3 and parts[1] in "TtWw" and not re.fullmatch(r"L\d+", parts[2]):
            symbols.append((int(parts[0], 16), parts[2]))
    symbols.sort()
    starts = [a for a, _ in symbols]
    text = load_text(args[1])
    base = 0x802000
    end = base + len(text)

    def function(pc):
        if not base <= pc < end:
            return None
        i = bisect.bisect_right(starts, pc) - 1
        return symbols[i][1] if i >= 0 else "?"

    counts = collections.Counter()
    inclusive = collections.Counter()
    callers = collections.Counter()
    pairs = collections.Counter()
    tree = {}
    total = len(data) // record
    helper_samples = 0
    for r in range(total):
        rec = data[r * record:(r + 1) * record]
        pc = struct.unpack(">I", rec[:4])[0]
        name = function(pc)
        if name is None:
            counts["(RAM: %s)" % ("GPU/DSP" if pc >= 0xF00000 else "DRAM")] += 1
            continue
        counts[name] += 1
        stack = rec[4:RECORD]
        seen = {name}
        if frames:
            # the chain, innermost first; a helper keeps no frame, so its caller is the first
            # return address on the stack
            path = [name]
            if is_helper(name):
                for o in range(0, len(stack) - 2, 2):
                    a = struct.unpack(">I", stack[o:o + 4])[0]
                    if not a & 1 and base < a < end and after_call(text, base, a):
                        if not is_helper(function(a)):
                            path.append(function(a))
                            break
            for o in range(RECORD, record, 4):
                a = struct.unpack(">I", rec[o:o + 4])[0]
                if a == 0:
                    break
                f = function(a)
                if f is None:
                    break
                if f != path[-1]:
                    path.append(f)
            seen.update(path)
            node = tree
            for f in reversed(path):
                entry = node.setdefault(f, [0, {}])
                entry[0] += 1
                node = entry[1]
        else:
            # every function with a return address in the stack kept, once a sample
            for o in range(0, len(stack) - 2, 2):
                a = struct.unpack(">I", stack[o:o + 4])[0]
                if not a & 1 and base < a < end and after_call(text, base, a):
                    seen.add(function(a))
        for f in seen:
            inclusive[f] += 1
        if not is_helper(name):
            continue
        helper_samples += 1
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
    if frames:
        print("\nfunctions on the way up the frames, or running")
    else:
        print("\nfunctions with a return address on the stack kept (128 bytes: shallow), or running")
    for name, n in inclusive.most_common(rows):
        print(f"{100 * n / total:6.2f}%  {n:6d}  {name[:100]}")
    print("\nhelper <- caller")
    for (h, c), n in pairs.most_common(rows):
        print(f"{100 * n / total:6.2f}%  {n:6d}  {h} <- {c[:80]}")
    if frames:
        print("\ncalls, outermost first (inclusive)")

        def show(node, depth):
            for f, (n, children) in sorted(node.items(), key=lambda e: -e[1][0]):
                if 100 * n / total < 1:
                    continue
                print(f"{100 * n / total:6.2f}%  {'  ' * depth}{f.split('(')[0][:90]}")
                show(children, depth + 1)
        show(tree, 0)


if __name__ == "__main__":
    main()
