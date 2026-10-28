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
| Music | MIDI port → real MT-32, or a GM synth with `--gm` | MIDI port → real MT-32, or a GM synth with `--gm` |
| Speech | 8-bit mono DMA, 25033 Hz | 8-bit mono DMA, 24585 Hz (49170 Hz with `--line-in`) |

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
  c2p.S              chunky-to-planar for the frame, converting only the 16-pixel groups that changed
  shapes.S           shape drawing in copy mode, the game's hottest path (called from the fork)
  sound.c            MIDI out, DMA speech; AdLib reported absent
  gm.c               --gm: the game's MT-32 MIDI translated for a General MIDI synth (tests/gm_test.c)
  c2p.h              chunky-to-planar for 16 pixels in C, the reference for c2p.S (tests/c2p_test.c)
  resample.h         speech resampling into the DMA ring (tests/resample_test.c checks it)
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
- [x] MIDI to MT-32
- [x] Speech on DMA sound: plays the intro's speech sample-accurately under Hatari on both machines
      (not yet heard on hardware); `--line-in` mixing on the Falcon is untested
- [x] General MIDI output option, `--gm` (not yet heard on a GM synth)
- [x] Performance: 68030 assembly for shape drawing and c2p (2.6 times the frame rate, under Hatari)

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
buffer and palette work. The portable build replaced that assembly with C. Profiled with
Hatari's CPU profiler on the TT (see Testing), over the same 10 s of the opening scene in Trinsic:

| Change | Frames in 10 s |
|---|---|
| Start | 27 |
| Present compares 16-pixel groups as four longs, not with `memcmp` | 31 |
| `-mno-strict-align`: GCC copies unaligned data, and loads little-endian values, a long at a time | 35 |
| Shapes drawn straight from their runs, mode and direction fixed per frame (fork) | 47 |
| Little-endian loads through `memcpy` and a swap, run loops inlined (fork) | 50 |
| Copy-mode shapes in 68030 assembly, `shapes.S` (2.9 times the C's speed on its own) | 67 |
| c2p in assembly, `c2p.S`: each changed group written to both TT lines, no line copies | 68 |
| `DrawTile` takes its pointers once instead of reloading `LinearBase` every row (fork) | 70 |

The Falcon, emulated at 32 MHz, shows 68 frames in the same window. In the intro, the loop during
the Guardian's speech went from about 5 frames a second to 21 before the assembly and 28 after it
(the speech needs about 4). Count frames by `plat_video_present`'s calls: `plat_video_present.part.0`
is also entered by palette refreshes.

`tests/shapes_test.cpp` and `tests/c2p_frame_test.c` check the assembly against the C it replaces,
on the Atari or under Hatari (`cmake --build build --target shapes_test c2p_frame_test`, then run
the PRG with `tools/hatari-test.sh`; the shapes test reads `C:\U7\STATIC\SHAPES.VGA`).

What is left, none of it large on its own:
- finding what changed in the frame: about 11% of the game's time, comparing 64,000 bytes with the
  previous frame; under Hatari it is limited by memory, not instructions, so it needs checking on
  real hardware. The game does not say what it redrew.
- game logic called per item while drawing (`GetItemZAndStuff`, `Item_getX`/`Y`, the depth sort):
  each is short, but they are called 100,000 times in 10 s
- shapes drawn translucent (`DrawSpans<SHAPE_BLEND>`, about 4%), still in C
- MIDI output waits on the ACIA for each byte (a few per cent while music plays)
- speech resampling, about 9% of the TT while speech plays, twice that on a Falcon with `--line-in`

The TT at 32 MHz is the tighter of the two machines; profile there first.

**Keyboard.** The Atari keyboard has no Page Up, Page Down, End or Num Lock. The keypad works
as a PC keypad with Num Lock off (7 Home, 9 Page Up, 1 End, 3 Page Down, arrows on 8 4 6 2) and
gives digits with Shift. Help, Undo and the keypad brackets do nothing. Alt-Help's screen dump is
switched off while the game runs.

**Audio.** The music is written for the MT-32: the game uploads its own instruments to it and picks
them by number, so a General MIDI device plays it with the wrong instruments. With `--gm` (in
`ULTIMA7.ARG`) the backend translates for a GM synth (`platform/atari/gm.c`). It keeps a model of
the instruments and patches the game writes to the MT-32 and turns each program change into a GM
program: an MT-32 preset tone goes to the nearest GM one, and an instrument the game loaded goes by
its name, through a table of the ones Ultima VII loads. Patch key shifts transpose the notes, bender
ranges become pitch bend sensitivity (RPN 0), and the master volume becomes GM master volume. Drum
keys the game gave its own instruments play the nearest GM drum. Sound effects are MT-32
instruments too, and most have no GM equivalent: those are silent with `--gm` rather than played
on the wrong instrument (bells and clocks play). The choices are in the two tables at the top of
`gm.c`; none has been heard on a GM synth yet.

Speech plays from a looping DMA ring in ST RAM, resampled with linear interpolation from the
game's Sound Blaster rate (about 11 kHz). Each sound tick silences what the DMA has played, so the
ring never replays old speech when the game stops feeding it.

The intro streams its speech in 0.12 s halves and hands over the next one only from its animation
loop, so that loop must run faster than about 4 times a second or the speech stutters. Under
Hatari the TT's release-speed loop runs about 28 times a second.

On the Falcon, `--line-in` (in `ULTIMA7.ARG`) mixes the input jack into the output, so a synth
such as an MT32-pi plugged in there plays through the Falcon with the speech. The codec then runs
at 49170 Hz and the game's YM is not mixed in; the settings are put back at exit. The TT has no
audio input: mix its output and the synth's outside the machine.

Munt (MT-32 emulation) and Nuked OPL3 are far too heavy for a 68030. MT-32 output
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
frame statistics, keys, mouse events and a pump heartbeat to the console (`--program pcmtest`
then plays 6 s of the intro's speech through the speech path, for recording with `--sound 44100
--avirecord`), and use
`build/ULTIMA7.map` or `nm` on the PRG to turn Hatari debugger addresses into functions.

`tools/hatari-profile.sh <tt|falcon> <hd-dir> <start-vbl> <vbls> [symbol]` runs the same way with
Hatari's CPU profiler on for <vbls> frames from VBL <start-vbl> (or from the first call of
`symbol` after it, such as `_plat_pcm_start` for the intro's speech) and prints the time by
function, with call counts; `plat_video_present`'s count is the frames shown. With
`ULTIMA7.ARG` holding `--program u7 p` and a game in `GAMEDAT\`, VBLs 1800 to 2400 are the opening
scene in Trinsic; `--program intro ereiamjh` runs the intro alone.

Hatari emulates both targets: `--machine tt --ttram 256` and `--machine falcon` (with
`--ttram` for the fast RAM). Its cycle counts are approximate, so use them to compare builds
with each other and confirm frame rates on real hardware.
