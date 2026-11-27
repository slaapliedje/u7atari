#!/usr/bin/env python3
"""Packs the rendered sound effects into the engine's ENGINE/SFX.FLX (engine/core/engine.h).

    sfxpack.py <index.txt> <wav dir> <out SFX.FLX>

index.txt and the WAVs come from u7sfxdro and adlconvert (engine/tools/mksfx.sh): sfxNNN.wav,
16-bit stereo at 15980 Hz. Each entry: the effect in mono, 16-bit big-endian signed, at one gain
for all of them (4: the loudest effect just fits), cut after its last sample above 1/256 of the
loudest; one that sounds until stopped is kept to its first 2 seconds (not yet looped).
"""
import struct
import sys
import wave

RATE = 15980
GAIN = 4
TITLE = b"U7 engine sounds 1, 16-bit big-endian at 15980 Hz"


def mono(path):
    w = wave.open(path)
    if w.getframerate() != RATE or w.getsampwidth() != 2:
        sys.exit(f"{path}: not 16-bit at {RATE} Hz")
    raw = w.readframes(w.getnframes())
    c = w.getnchannels()
    a = struct.unpack(f"<{len(raw) // 2}h", raw)
    return [sum(a[i:i + c]) // c for i in range(0, len(a), c)]


def main():
    index, wavs, out = sys.argv[1:4]
    entries = []
    for line in open(index):
        n, ticks, looped = (int(x) for x in line.split())
        try:
            m = mono(f"{wavs}/sfx{n:03d}.wav") if ticks else []
        except FileNotFoundError:
            m = []
        if looped:
            m = m[:2 * RATE]
        loud = [i for i, x in enumerate(m) if abs(x) * 256 > 32767 // GAIN]
        m = m[:loud[-1] + 1] if loud else []
        pcm = b"".join(struct.pack(">h", max(-32768, min(32767, x * GAIN))) for x in m)
        while len(entries) < n:
            entries.append(b"")
        entries.append(pcm)
    head = bytearray(0x80)
    head[:len(TITLE)] = TITLE
    struct.pack_into("<III", head, 0x50, 0xFFFF1A00, len(entries), 0xCC)
    at = 0x80 + 8 * len(entries)
    table = bytearray()
    data = bytearray()
    for e in entries:
        pad = (-(at + len(data))) % 8
        data += bytes(pad)
        table += struct.pack("<II", at + len(data) if e else 0, len(e))
        data += e
    with open(out, "wb") as f:
        f.write(head + table + data)
    print(f"{out}: {len(entries)} sounds, {len(head) + len(table) + len(data)} bytes")


main()
