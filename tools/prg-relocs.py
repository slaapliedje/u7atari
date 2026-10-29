#!/usr/bin/env python3
"""Checks a PRG's relocation table the way the TOS loader reads it.

TOS's Pexec refuses a program with a fixup at an odd address (EPLFMT, -66; the desktop shows
"TOS error #35"), and Hatari's own loader for its GEMDOS drive does not check, so a program can
pass every Hatari test and still not load on the machine. Exits 1 and names the symbol each odd
fixup falls in (from the PRG's own symbol table, via m68k-atari-mint-nm) when there are any.

    tools/prg-relocs.py <file.PRG> [nm]
"""
import bisect
import struct
import subprocess
import sys


def fixups(data):
    """The text-relative offsets the loader patches, in order."""
    magic, tlen, dlen, _blen, slen = struct.unpack_from('>HIIII', data, 0)
    if magic != 0x601A:
        raise SystemExit('not a PRG')
    (absflag,) = struct.unpack_from('>H', data, 26)
    at = 28 + tlen + dlen + slen
    if absflag or at + 4 > len(data):
        return []
    (offset,) = struct.unpack_from('>I', data, at)
    if offset == 0:
        return []
    out = [offset]
    at += 4
    while data[at] != 0:
        step = data[at]
        at += 1
        offset += 254 if step == 1 else step
        if step != 1:
            out.append(offset)
    return out


def symbols(path, nm):
    result = subprocess.run([nm, '-n', path], capture_output=True, text=True, check=False)
    found = []
    for line in result.stdout.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3:
            try:
                found.append((int(parts[0], 16), parts[2]))
            except ValueError:
                pass
    return found


def main():
    path = sys.argv[1]
    nm = sys.argv[2] if len(sys.argv) > 2 else 'm68k-atari-mint-nm'
    with open(path, 'rb') as f:
        data = f.read()
    odd = [o for o in fixups(data) if o & 1]
    if not odd:
        return 0
    syms = symbols(path, nm)
    addresses = [a for a, _ in syms]
    print(f'{path}: {len(odd)} fixups at odd addresses; TOS will not load it (EPLFMT):',
          file=sys.stderr)
    for o in odd[:40]:
        i = bisect.bisect_right(addresses, o) - 1
        where = f'{syms[i][1]}+{o - syms[i][0]:#x}' if i >= 0 else '?'
        print(f'  {o:#08x}  {where}', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
