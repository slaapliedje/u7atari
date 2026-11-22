#!/usr/bin/env python3
"""Writes a Serpent Isle soak script, for a long run in lockstep against the PC reference.

    tools/si-soak.py <cycles> > si-soak.scr
    U7_SI=1 tools/lockstep-compare.sh <tt|falcon> <si-start> si-soak.scr <out> --program si p manimal

<si-start> is the start state tests/lockstep/run-all.sh makes (out-dir/start/si-start). Each
cycle takes about 92 s of game time (60 cycles: 92 minutes, some 6 hours under Hatari): a
teleport by the map (F3, cheats on), a walk there and back, combat mode for 10 s, a spell by the
cheat key (F5), a save over slot 2 and, every other cycle, that save loaded back, then 30 s idle
with NPC schedules running. Each cycle ends with a "mem" line, the memory left (Atari) or the heap
in use (PC), which should level off; the shots are there to look through for a stuck game.
"""
import sys

cycles = int(sys.argv[1])
# map (F3) clicks: Monitor, Fawn, Moonshade, the Sleeping Bull, the ice to the north, the
# Furnace; not the swamp, whose monsters kill the party, and Monk Isle's first resurrection is a
# long conversation the script cannot answer
places = [(280, 173), (286, 122), (434, 127), (384, 97), (354, 50), (274, 32)]
walks = [(500, 150), (150, 150), (500, 60), (150, 60), (500, 100), (150, 100)]
spells = ["3", "13", "19", "33"]
out = []


def at(t, step):
    out.append(f"{t} {step}")


t = 15000
for c in range(cycles):
    px, py = places[c % len(places)]
    # out of a conversation the cycle before left open: after a death, Monk Isle's choice "leave
    # Monk Isle" is here; in the world the click only looks at something
    for i in range(3):
        at(t + 2000 * i, "click 280 142")
    t += 6000
    at(t, "key f3")
    at(t + 2000, f"click {px} {py}")
    t += 7000
    at(t, f"shot C{c:03d}A.PPM")
    wx, wy = walks[c % len(walks)]
    at(t + 500, f"move {wx} {wy}")
    at(t + 600, "rdown")
    at(t + 6600, "rup")
    t += 7500
    at(t, f"move {640 - wx} {wy}")
    at(t + 100, "rdown")
    at(t + 6100, "rup")
    t += 7000
    at(t, f"shot C{c:03d}B.PPM")
    # combat mode for 10 s
    at(t + 500, "key c")
    at(t + 10500, "key c")
    t += 11500
    at(t, f"shot C{c:03d}C.PPM")
    # a spell by the cheat key, cast by the Avatar at a spot beside them
    at(t + 500, "key f5")
    for i, digit in enumerate(spells[c % len(spells)]):
        at(t + 700 + 200 * i, f"key {digit}")
    at(t + 1300, "key enter")
    at(t + 1800, "click 320 95")
    at(t + 3300, "click 420 150")
    t += 6000
    at(t, f"shot C{c:03d}D.PPM")
    # save to slot 2: named the first time, after that over the old one (SAVE, then YES)
    at(t + 1000, "key s")
    at(t + 3000, "click 200 41")
    if c == 0:
        for i, k in enumerate("Soak"):
            at(t + 4000 + 200 * i, f"key {k}")
        at(t + 5000, "key enter")
    else:
        at(t + 4000, "click 150 157")
        at(t + 5500, f"shot C{c:03d}Y.PPM")
        at(t + 6000, "click 296 109")
    t += 8000
    at(t, f"shot C{c:03d}S.PPM")
    at(t + 500, "key esc")
    t += 2000
    # every other cycle, load it back (LOAD, then YES)
    if c % 2 == 1:
        at(t, "key s")
        at(t + 2000, "click 200 41")
        at(t + 3500, "click 326 157")
        at(t + 5000, f"shot C{c:03d}Q.PPM")
        at(t + 5500, "click 296 109")
        t += 12000
        at(t, f"shot C{c:03d}L.PPM")
    # idle, NPC schedules running
    t += 30000
    at(t, f"shot C{c:03d}E.PPM")
    at(t + 100, "mem")
    t += 1000
at(t, "quit")
print(f"# Serpent Isle soak run of {cycles} cycles, written by tools/si-soak.py")
print("#@start si-start")
print("#@args --program si p manimal")
print("\n".join(out))
