#!/usr/bin/env python3
"""Sums a Hatari CPU profile by function.

    tools/profile-funcs.py <hatari log> <nm -n output of the PRG> <TEXT base, hex> [rows]

The log must hold the disassembly Hatari prints for "profile save" (each executed address with
its instruction, cycle and cache counts). Local labels and object file names are skipped, so
an address counts toward the function it lies in. Names are demangled with c++filt.
"""
import bisect
import re
import subprocess
import sys

log, nm_file, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
rows = int(sys.argv[4]) if len(sys.argv) > 4 else 30

symbols = []
for line in open(nm_file):
    parts = line.split()
    if len(parts) == 3 and parts[1] in 'Tt' and not parts[2].startswith('.L') \
            and not parts[2].endswith(('.o', '.obj')):
        symbols.append((int(parts[0], 16) + base, parts[2]))
symbols.sort()
starts = [a for a, _ in symbols]

entry = re.compile(r'^([0-9a-f]{8}) .*% \((\d+), (\d+), (\d+), (\d+)\)$')
cycles, calls, total = {}, {}, 0
for line in open(log, errors='replace'):
    m = entry.match(line.rstrip())
    if not m:
        continue
    address, executed, used = int(m.group(1), 16), int(m.group(2)), int(m.group(3))
    i = bisect.bisect_right(starts, address) - 1
    name = symbols[i][1] if i >= 0 and address >= base else '(outside the program)'
    cycles[name] = cycles.get(name, 0) + used
    if i >= 0 and address == starts[i]:
        calls[name] = executed
    total += used

print('%d cycles, %.2f s at 32 MHz' % (total, total / 32e6))
print('    share       cycles      calls  function')
for name, used in sorted(cycles.items(), key=lambda kv: -kv[1])[:rows]:
    if name.startswith('__Z'):
        shown = subprocess.run(['c++filt', '-_'], input=name, capture_output=True,
                               text=True).stdout.strip() or name
    else:
        shown = name[1:] if name.startswith('_') else name   # a.out's leading underscore
    print('%8.2f%% %12d %10s  %s' % (100 * used / total, used, calls.get(name, ''), shown[:100]))
