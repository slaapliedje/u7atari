# Ultima VII for the Atari TT030 and Falcon030

A port of [Kitrinx/Ultima7_Decomp](https://github.com/Kitrinx/Ultima7_Decomp)'s
`ultima7-portable` build to the Atari TT030 and an accelerated Falcon030.

## Target machines

| | TT030 | Falcon030 (accelerated) |
|---|---|---|
| CPU | 68030 @ 32 MHz, 68882 | 68030 @ 50 MHz |
| ST RAM | 4 MB | 14 MB |
| Fast RAM | 256 MB TT-RAM | 128 MB fast RAM |
| 256-colour mode | TT low, 320x480, 8 planes | VIDEL 320x200 (VGA line-doubled), 8 planes |
| Music | MIDI port → real MT-32 | MIDI port → real MT-32 |
| Speech | 8-bit stereo DMA | 8/16-bit DMA |

A stock 16 MHz Falcon is not a target.

## Layout

```
upstream/            git submodule: Kitrinx/Ultima7_Decomp, pinned
cmake/               m68k-atari-mint toolchain file
platform/atari/      the backend: implements upstream's platform/plat.h
  main.c             main(), program runner (setjmp/longjmp in place of threads), exit
  timer.c            60 Hz ticks from _hz_200, game timers, sound tick and lock
  video.c            chunky-to-planar present, palette, retrace, 8x8 console
  input.c            IKBD keyboard and mouse, software pointer
  sound.c            MIDI out, DMA speech; AdLib reported absent
  files.c            GEMDOS file access for the game's DOS names
CMakeLists.txt       builds upstream's u7_game library + this backend → ULTIMA7.PRG
```

Upstream already separates the game from the host: game code calls only `plat_*`
functions (`upstream/ultima7-portable/Black Gate/platform/plat.h`), and the libmatoya
backend is just one implementation. This repo adds a second backend and links it against
upstream's unmodified `u7_game` object library.

## Building

Needs `m68k-atari-mint-gcc` (MiNT GCC 15.2 tested) and CMake 3.20 or newer.

```sh
git submodule update --init
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/m68k-atari-mint.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# → build/ULTIMA7.PRG
```

`-DU7_GAME="Serpent Isle"` selects the other game. Code is built with `-m68020-60 -mtune=68030`;
the game has no floating point, so an FPU is not required. The PRG header asks TOS to load
the program into TT-RAM and allocate from TT-RAM (flags `0x7`).

## Status

- [x] All of the game (351 objects) cross-compiles for the 68030 with no source changes
- [x] Links to `ULTIMA7.PRG` (1.6 MB code, 145 KB BSS) against a stub backend
- [ ] Runs to its first file open under Hatari (TT and Falcon)
- [ ] Files (GEMDOS), timing, input
- [ ] Video: c2p, palette, both machines
- [ ] Endian audit: the game reads correctly on a big-endian CPU
- [ ] MIDI to MT-32, speech on DMA sound
- [ ] Performance: 68030 assembly for the hot paths

## Known work

**Endianness.** The game's data and save files are little-endian. Upstream's linear arena
already goes through byte-order-safe accessors (`include/arena.h`, ~107 uses), but other code
reads raw structs from files (`DosRead` in 24 files, ~69 calls) and casts pointers into loaded
data. Each site needs a byte swap on load and save. Save files should stay little-endian so
they still work with PC saves. Fixing these means patching upstream, so `upstream/` will move
to a fork with an `atari` branch once the first patch is needed.

**Performance.** U7 was hand-tuned for a 386, with assembly for shape blitting, the depth
buffer and palette work. The portable build replaced that assembly with C, and every arena
access now pays for a byte swap. Expected hot spots:
- c2p of a 64,000-byte frame on every present, written to ST RAM; skip unchanged lines
- shape RLE decode and blit, depth sort and occlusion: candidates for 68030 assembly
- arena 16/32-bit access: `move.w` + `ror.w #8` instead of byte loads
- `#pragma pack(1)` everywhere: the 030 handles misaligned access but it costs bus cycles

The TT at 32 MHz is the tighter of the two machines; profile there first.

**Audio.** Munt (MT-32 emulation) and Nuked OPL3 are far too heavy for a 68030. MT-32 output
goes to the MIDI port at no CPU cost; AdLib is reported absent. If there is time later, a DSP56001
FM synth on the Falcon is a possible stretch goal.

**Memory.** Upstream allocates a fixed 32 MB arena (`LINEAR_MEMORY_SIZE`), which fits in
fast RAM on both targets. ST RAM holds only the screens (TT low: 150 KB each) and the DMA
sound buffer.

## Testing

Hatari emulates both targets: `--machine tt --ttram 256` and `--machine falcon` (with
`--ttram` for the fast RAM). Its frame timing is not trustworthy for profiling, so measure
on real hardware.
