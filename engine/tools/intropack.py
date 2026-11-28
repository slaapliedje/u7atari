#!/usr/bin/env python3
"""Packs the intro's sounds into the engine's ENGINE/INTRO.FLX (engine/core/intro.c).

    intropack.py <isfx.txt> <wav dir> <STATIC dir> <out INTRO.FLX>

isfx.txt and isfxN.wav come from u7musdro and adlconvert (engine/tools/mkmusic.sh): the intro's
eight synth sound effects, 16-bit stereo at 15980 Hz. Entries 0-7 are those effects, entry 8 the
Guardian's speech (STATIC/INTROSND.DAT: a Creative Voice file, 8-bit unsigned at the rate its
time constant gives, 10752.7 Hz), resampled to 15980 Hz by straight lines. All in mono, 16-bit
big-endian signed (as eb_sound takes them); the effects at the game's effects' gain (4), cut
after their last sample above 1/256 of the loudest; the speech at full scale.
"""
import struct
import sys
import wave

RATE = 15980
GAIN = 4
TITLE = b"U7 engine intro 1, 16-bit big-endian at 15980 Hz"


def mono(path):
    w = wave.open(path)
    if w.getframerate() != RATE or w.getsampwidth() != 2:
        sys.exit(f"{path}: not 16-bit at {RATE} Hz")
    raw = w.readframes(w.getnframes())
    c = w.getnchannels()
    a = struct.unpack(f"<{len(raw) // 2}h", raw)
    return [sum(a[i:i + c]) // c for i in range(0, len(a), c)]


def speech(path):
    d = open(path, "rb").read()
    if not d.startswith(b"Creative Voice File\x1a"):
        sys.exit(f"{path}: not a Creative Voice file")
    at = struct.unpack_from("<H", d, 20)[0]
    if d[at] != 1:
        sys.exit(f"{path}: its first block is not sound data")
    size = d[at + 1] | d[at + 2] << 8 | d[at + 3] << 16
    tc, pack = d[at + 4], d[at + 5]
    if pack != 0:
        sys.exit(f"{path}: packed sound")
    rate = 1000000 / (256 - tc)
    src = d[at + 6:at + 4 + size]
    n = int(len(src) * RATE / rate)
    out = []
    for i in range(n):
        p = i * rate / RATE
        k = int(p)
        f = p - k
        a = src[k] - 128
        b = (src[k + 1] if k + 1 < len(src) else 128) - 128
        out.append(max(-32768, min(32767, int(round((a + (b - a) * f) * 256)))))
    return out, rate


def pcm(samples):
    return struct.pack(f">{len(samples)}h", *samples)


def main():
    index, wavs, static, out = sys.argv[1:5]
    entries = [b""] * 9
    for line in open(index):
        n, ticks = (int(x) for x in line.split())
        m = [max(-32768, min(32767, x * GAIN)) for x in mono(f"{wavs}/isfx{n}.wav")]
        loudest = max((abs(x) for x in m), default=0)
        end = len(m)
        while end > 0 and abs(m[end - 1]) * 256 <= loudest:
            end -= 1
        entries[n] = pcm(m[:end])
    s, rate = speech(f"{static}/INTROSND.DAT")
    entries[8] = pcm(s)
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
    print(f"{out}: 8 effects, speech {len(s)} samples from {rate:.1f} Hz, "
          f"{len(head) + len(table) + len(data)} bytes")


main()
