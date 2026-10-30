ULTIMA VII FOR THE ATARI TT030 AND FALCON030
Version @VERSION@ - a test release

Ultima VII: The Black Gate (ULTIMA7.PRG) and Ultima VII Part Two:
Serpent Isle (SERPENT.PRG), running natively on a TT030 or a Falcon030.

This is a port of the "ultima7-portable" build of Kitrinx's Ultima VII
decompilation (github.com/Kitrinx/Ultima7_Decomp). No game data is
included: you need the DOS version of the game (the GOG release works).


WHAT YOU NEED
-------------

- An Atari TT030 with TT-RAM, or a Falcon030.
- About 6 MB of free memory. The game uses fast RAM (TT-RAM, or an
  accelerator's fast RAM) when there is enough, and ST RAM otherwise.
  It runs faster from fast RAM.
- A hard disk: The Black Gate's data is about 18 MB, Serpent Isle's
  about 22 MB, plus about 1 MB for each saved game.
- For music: a Roland MT-32 (or a CM-32L or a compatible module) on the
  MIDI OUT port. A General MIDI synth works too, with the --gm option
  below. With nothing on MIDI OUT, the game runs without music.
- Speech plays through the machine's own sound output.

The game is made for an accelerated Falcon (such as a DFB1X or a TF53x
at 50 MHz) and the TT. A stock 16 MHz Falcon runs it, but slowly. An
FPU is not needed. A TT with only its 4 MB of ST RAM does not have
enough memory.


INSTALLING
----------

1. Make a folder for the game on your hard disk, such as C:\U7.

2. Copy the STATIC folder from the DOS game into it. To carry on with
   a game saved under DOS, also copy the GAMEDAT folder and the saved
   games (GAME00.U7 and so on). The Atari version reads and writes
   the same files.

3. Copy ULTIMA7.PRG into the same folder, and run it.

Serpent Isle goes in a folder of its own, such as C:\SI, with its own
STATIC folder and SERPENT.PRG.


OPTIONS
-------

Put options in a text file named ULTIMA7.ARG (SERPENT.ARG for Serpent
Isle) next to the program, separated by spaces or on separate lines.
Or rename the program to ULTIMA7.TTP and type them when it starts.

  --gm            Music and sound effects for a General MIDI synth
                  instead of an MT-32. The game's music is written for
                  the MT-32, so this translates it: instruments are the
                  nearest General MIDI ones, and sound effects with no
                  close match are left out.

  --line-in       Falcon only: mixes the Falcon's sound input into its
                  output. Plug the synth's audio into the input, and
                  music and speech come out of one jack.

  --data <folder> Where the game's files are, if not in the program's
                  own folder. For example: --data D:\GAMES\U7


ACCELERATOR BOARDS
------------------

A DFB1X's or a TF53x's fast RAM is only usable once MAPROM has run from
the AUTO folder (MAPROM_C.PRG in the DFB1X package). If you boot with
Control held, AUTO is skipped, so the game runs \AUTO\MAPROM*.PRG from
the boot drive itself and then starts again. Without MAPROM it runs
from ST RAM.


CONTROLS
--------

The mouse works as on the PC. The Atari keyboard differs from a PC's:

  Undo            Esc
  Help            F1 (the game has no help screen)
  Keypad          as a PC keypad: 7 Home, 9 Page Up, 1 End,
                  3 Page Down, and 8 4 6 2 as arrows; Shift gives digits

In conversations, and after you press Space, the game lets you move
the pointer with the arrow keys and click with Control. Moving the
mouse switches back. If a mouse click seems to be ignored, move the
mouse a little and click again.


KNOWN PROBLEMS AND UNTESTED PARTS
---------------------------------

This is a test release. Most testing so far has been under the Hatari
emulator. On real machines, only the Falcon has run it so far.

- Speed on real hardware has not been measured.
- Speech, --line-in, and --gm on a real General MIDI synth are untested
  on real hardware.
- Serpent Isle has not been played far: combat, and speech that events
  trigger, are untested. Serpent Isle saves from DOS have not been
  tried.
- There is no AdLib or Sound Blaster music.

Please report what you find, with your machine, memory, TOS version,
and synth.


CREDITS
-------

Ultima VII and Serpent Isle are by Origin Systems.
Decompilation and ultima7-portable: Jamie Blanks (Kitrinx) and
contributors. See LICENSE.TXT.
Atari port: Slaapliedje, with Claude (Anthropic).
