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
  timer.c            60 Hz ticks from _hz_200, game timers, sound tick (MFP Timer B) and lock
  video.c            chunky-to-planar present, palette, retrace, 8x8 console
  input.c            keys from the BIOS buffer, mouse from the IKBD mousevec, PC key words
  ikbd.S             the mousevec wrapper and the Alt-Help screen-dump stub
  timerint.S         the Timer B interrupt that runs the sound tick on its own stack
  sound.c            MIDI out, DMA speech; AdLib reported absent
  c2p.h              chunky-to-planar for 16 pixels (tests/c2p_test.c checks it on the host)
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

`-DU7_GAME="Serpent Isle"` selects the other game. Code is built with `-m68020-60 -mtune=68030`,
which links MiNTLib's 68020-60 multilib; its startup code refuses to run without a 68881/68882,
so an FPU is required (both targets have one). The game itself has no floating point. The PRG header asks TOS to load
the program into TT-RAM and allocate from TT-RAM (flags `0x7`).

## Status

- [x] All of the game (351 objects) cross-compiles for the 68030 with no source changes
- [x] Links to `ULTIMA7.PRG` (1.6 MB code, 145 KB BSS) against a stub backend
- [x] Runs to its first file open under Hatari (TT and Falcon)
- [x] Files (MiNTLib on GEMDOS): boots, checks the data folder, launcher starts the intro
- [x] Input: keyboard (TOS layout and key repeat, PC BIOS key words) and mouse (IKBD packets
      through mousevec); the main menu and conversations respond under Hatari on both machines
- [x] Timing: sound tick on MFP Timer B at 59.94 Hz, independent of the frame rate
- [x] Video: c2p into double-buffered ST-RAM screens, palette, TT low and Falcon VGA/RGB;
      the game reaches Trinsic and draws it correctly under Hatari on both machines
- [x] Endian audit (30 findings), fixes in the fork: the main game builds a new game and loads its
      starting regions under Hatari on both machines
- [ ] Save formats still written in host order (round-trip on the Atari, not PC-compatible)
- [ ] MIDI to MT-32, speech on DMA sound
- [ ] Performance: 68030 assembly for the hot paths

## Known work

**Endianness.** The game's data and save files are little-endian. The fixes live in the fork
(`slaapliedje/Ultima7_Decomp`, branch `atari`) and compile to nothing on x86. `game/include/byteorder.h` holds:
- `LE16`/`LE32`/`SWAP_LE` for values that cross between files and native structs, where each site
  converts on load and save (Flex headers and entries, the item tables, schedules, maps, scripts).
- `LittleEndian<T>` (`le_int16` and friends): fields kept little-endian in memory and converted on every
  access. The item buffer, its list heads and the NPC records use these, because their unions and
  their raw save files only work in the PC's byte order.
- `U7_BITS(type, fields...)`: GCC fills bitfields from the high bit on m68k, so packed flag bytes are
  declared in reverse there. `tests/layout_check.cpp` checks these layouts at compile time.

Still to do: the smaller save files (timers, weather, missiles, party, routes, ITEMNODE/OSTIA
scalars) are written in host order. They round-trip on the Atari but are not PC-compatible yet.

**Performance.** U7 was hand-tuned for a 386, with assembly for shape blitting, the depth
buffer and palette work. The portable build replaced that assembly with C, and every arena
access now pays for a byte swap. Expected hot spots:
- c2p of a 64,000-byte frame on every present, written to ST RAM; skip unchanged lines
- shape RLE decode and blit, depth sort and occlusion: candidates for 68030 assembly
- arena 16/32-bit access: `move.w` + `ror.w #8` instead of byte loads
- `#pragma pack(1)` everywhere: the 030 handles misaligned access but it costs bus cycles

The TT at 32 MHz is the tighter of the two machines; profile there first.

**Keyboard.** The Atari keyboard has no Page Up, Page Down, End or Num Lock. The keypad works
as a PC keypad with Num Lock off (7 Home, 9 Page Up, 1 End, 3 Page Down, arrows on 8 4 6 2) and
gives digits with Shift. Help, Undo and the keypad brackets do nothing. Alt-Help's screen dump is
switched off while the game runs.

**Audio.** Munt (MT-32 emulation) and Nuked OPL3 are far too heavy for a 68030. MT-32 output
goes to the MIDI port at no CPU cost; AdLib is reported absent. If there is time later, a DSP56001
FM synth on the Falcon is a possible stretch goal.

**Memory.** Upstream allocates a fixed 32 MB arena (`LINEAR_MEMORY_SIZE`), which fits in
fast RAM on both targets. ST RAM holds only the screens (TT low: 150 KB each) and the DMA
sound buffer.

## Testing

`tools/hatari-test.sh <tt|falcon> <hd-dir>` runs the game headless (MIDI off unless
`U7_TEST_MIDI=1`); add `--avirecord --avi-vcodec png --avi-file f.avi` to record frames.
`U7_TIMEOUT=<s>` stops Hatari after that long and `U7_PRG` picks another build's PRG. To drive
input, add `--cmd-fifo <path>` and write Hatari commands to it: `hatari-event keypress 01` (Esc,
by scan code), `hatari-event doubleclick`, Alt (`keydown 56`) with the arrow keys to move the
mouse through TOS's mouse emulation, Alt with Insert (`keypress 82`) to click, and
`hatari-shortcut screenshot` with `--screenshot-dir`. Configure with `-DU7_DEBUG_LOG=ON` to log
frame statistics, keys, mouse events and a pump heartbeat to the console, and use
`build/ULTIMA7.map` or `nm` on the PRG to turn Hatari debugger addresses into functions.

Hatari emulates both targets: `--machine tt --ttram 256` and `--machine falcon` (with
`--ttram` for the fast RAM). Its frame timing is not trustworthy for profiling, so measure
on real hardware.
