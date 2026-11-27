#!/usr/bin/env python3
"""Packs the recorded music into the engine's ENGINE/MUSIC.FLX (engine/core/engine.h).

    muspack.py <index.txt> <dro dir> <out MUSIC.FLX>

index.txt and musNN.dro come from u7musdro (engine/tools/mkmusic.sh): each song's AdLib register
writes as the original's driver made them at its 60 Hz tick. Each entry (big-endian): the byte
offset in it where its loop starts again (0xFFFFFFFF: it ends instead), its length in ticks;
then its writes tick by tick: register, value pairs, 0xFF ending a tick, 0xFE n for n ticks with
none, 0xFD its end (then back to the loop, or silence).
"""
import struct
import sys

TITLE = b"U7 engine music 1, AdLib writes at 60 Hz"
END_TICK, EMPTY, END = 0xFF, 0xFE, 0xFD


def read_dro(path):
    d = open(path, "rb").read()
    if d[:8] != b"DBRAWOPL":
        sys.exit(f"{path}: not a DRO")
    short, long_, ncodes = d[23], d[24], d[25]
    codes = d[26:26 + ncodes]
    data = d[26 + ncodes:]
    ms, writes = 0, []
    for i in range(0, len(data), 2):
        c, v = data[i], data[i + 1]
        if c == short:
            ms += v + 1
        elif c == long_:
            ms += (v + 1) * 256
        else:
            writes.append(((ms * 60 + 500) // 1000, codes[c], v))
    return writes


def pack(writes, ticks, loop_tick):
    out, loop_at, tick, k = bytearray(), None, 0, 0
    while tick < ticks:
        if tick == loop_tick:
            loop_at = len(out)
        pairs = []
        while k < len(writes) and writes[k][0] <= tick:
            pairs += [writes[k][1], writes[k][2]]
            k += 1
        if pairs:
            out += bytes(pairs) + bytes([END_TICK])
            tick += 1
            continue
        # a run of empty ticks (not past the loop's start)
        n = 1
        while (tick + n < ticks and n < 255 and tick + n != loop_tick
               and not (k < len(writes) and writes[k][0] <= tick + n)):
            n += 1
        out += bytes([EMPTY, n]) if n > 1 else bytes([END_TICK])
        tick += n
    out += bytes([END])
    return out, loop_at


def main():
    index, dros, out = sys.argv[1:4]
    entries = []
    for line in open(index):
        n, length, loop_start, loop_end = (int(x) for x in line.split())
        writes = read_dro(f"{dros}/mus{n:02d}.dro")
        ticks = (length * 60 + 500) // 1000
        loop_tick = (loop_start * 60 + 500) // 1000 if loop_end else -1
        stream, loop_at = pack(writes, ticks, loop_tick)
        if loop_end and loop_at is None:
            loop_at = 0
        head = struct.pack(">II", 0xFFFFFFFF if loop_at is None else 8 + loop_at, ticks)
        while len(entries) < n:
            entries.append(b"")
        entries.append(head + stream)
    head = bytearray(0x80)
    head[:len(TITLE)] = TITLE
    struct.pack_into("<III", head, 0x50, 0xFFFF1A00, len(entries), 0xCC)
    at = 0x80 + 8 * len(entries)
    table, data = bytearray(), bytearray()
    for e in entries:
        pad = (-(at + len(data))) % 8
        data += bytes(pad)
        table += struct.pack("<II", at + len(data) if e else 0, len(e))
        data += e
    with open(out, "wb") as f:
        f.write(head + table + data)
    print(f"{out}: {len(entries)} songs, {len(head) + len(table) + len(data)} bytes")


main()
