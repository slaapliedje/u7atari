# The Atari Jaguar backend

Work in progress. For now this is a **lockstep build**: the game runs from a cartridge with its
data on the GameDrive's SD card, driven by the virtual clock and a test script as the PC
reference (`platform/host`) is, so a run can be compared with the PC's line for line. Video is
real (the game's 320x200 frame in an 8-bit Object Processor bitmap); time, input and sound are
the reference's.

## Layout

- The code runs in place from cartridge ROM at `$802000`, after the 8 KB boot header
  (`jaguar.ld`); `.data` is copied to RAM at `$4000` and `.bss` cleared by `startup.S`. The
  heap runs from there to the 64 KB stack at the top of the 2 MB.
- The game's arena is cut down to fit: build with `-DU7_ARENA_KB=1280 -DU7_FAR_HEAP_KB=384
  -DU7_SHAPE_POOL_KB=400` (the PC reference must use the same shape cache size: a smaller cache
  changes `FRAMES.FLG` in saved games and makes the game swap blocks out to `TEMP*.$$$` files).
- Files: `files.c`, on RetroHQ's GameDrive BIOS. The BIOS cannot delete, rename, truncate or
  make folders, so a removed file is cut to nothing (and a file of no bytes counts as absent),
  a rename is a copy, and the card needs its folders made beforehand.
- The log (every `plat_log` line, lockstep checksums included) goes to `GD_DebugString`.

## Building

Needs marsdev's m68k-elf toolchain (`~/dev/toolchains/mars`, or `MARSDEV`), rmac from the
jaguar-sdk, and two files that are not in this repository:

- RetroHQ's `gdbios_bindings.s` (no licence): `-DU7_GDBIOS_DIR=<folder>`;
- the universal homebrew boot header, 8 KB: `-DU7_JAG_HEADER=<file>`.

```
cmake -B build-jag -DCMAKE_TOOLCHAIN_FILE=cmake/m68k-jaguar.cmake -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DU7_ARENA_KB=1280 -DU7_FAR_HEAP_KB=384 -DU7_SHAPE_POOL_KB=400
cmake --build build-jag              # build-jag/ULTIMA7.rom
cmake --build build-jag --target jaguar_gdtest   # gdtest.rom: the GameDrive's file calls alone
```

## The card

`U7/` (`SI/` for Serpent Isle) holds `STATIC`, `GAMEDAT` (it must exist, even empty), and
`ULTIMA7.ARG` (`SERPENT.ARG`): one line of arguments, as the Atari's, e.g.
`--lockstep --script TEST.SCR --program u7 p` (`\ooo` in a word is a byte).

## Running it in an emulator

The libretro Virtual Jaguar (upstream develop) emulates the GameDrive's file calls against
`<save dir>/jaggd-sd`. With etjag's headless `lrrec` frontend the card is `./sysd/jaggd-sd`,
and the log comes out on stderr as `[JGD] debug: ...`:

```
lrrec virtualjaguar_libretro.so ULTIMA7.rom <frames> <outdir> virtualjaguar_jgd=enabled
```
