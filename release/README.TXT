U7ATARI - ULTIMA VII FOR THE ATARI TT030 AND FALCON030
Version @VERSION@ - a test release

Ultima VII: The Black Gate (ULTIMA7.PRG) and Ultima VII Part Two:
Serpent Isle (SERPENT.PRG), running natively on a TT030 or a Falcon030.

u7atari is built on Jamie Blanks' (Kitrinx's) decompilation of Ultima
VII, github.com/Kitrinx/Ultima7_Decomp: the games' own code, recovered
from the DOS programs, in its "ultima7-portable" form. The game itself
is that code, and the credit for recovering it is theirs; u7atari adds
what the Atari needs to run it.

No game data is included: you need the DOS version of the game (the
GOG release works).


WHAT YOU NEED
-------------

- An Atari TT030 with TT-RAM, or a Falcon030 (68030 or 68060).
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
at 50 MHz) and the TT. A stock 16 MHz Falcon runs it, but slowly. A
68060 Falcon (CT60 or CT63, or the MiSTer's Falcon core) runs it well.
An FPU is not needed. A TT with only its 4 MB of ST RAM does not have
enough memory.

It runs from the desktop under TOS, and under FreeMiNT with XaAES
(memory protection may be on).


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


GAMES SAVED UNDER EXULT
-----------------------

A Black Gate or Serpent Isle game saved under Exult can be carried
over. On a PC with Python 3, convert it with EXULT2U7.PY, giving it the
Exult saved game, the game's STATIC folder, and the saved game to write:

  python3 EXULT2U7.PY exult00bg.sav STATIC GAME09.U7

(exult00si.sav and Serpent Isle's STATIC folder for Serpent Isle.)

GAME00.U7 to GAME09.U7 are the save slots, first to last; pick one you
are not using. Copy it into the game's folder and load it from the
game's menu. The game starts again and builds the game from it.

Your party, everyone's belongings, the world's objects, what you have
done, and the time of day carry over. Left out, as the original game
has no place for them: monsters roaming the map (new ones turn up), a
Light spell still shining, and what Exult adds (its notebook, Exult
Studio changes, and objects that only exist in Exult). In Serpent Isle
your keyring, skin colour and Mark spell carry over too; the Black
Gate's keyring is Exult's own and does not.


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

  --no-card       TT only: use the TT's own video even when an
                  ATW800/2 graphics card is fitted.

  --fps           Counts the frames shown and, on quitting, writes
                  them to U7FPS.TXT: frames a second for every 10
                  seconds, and the average. For comparing machines and
                  settings.

  --data <folder> Where the game's files are, if not in the program's
                  own folder. For example: --data D:\GAMES\U7


ACCELERATOR BOARDS
------------------

A DFB1X's or a TF53x's fast RAM is only usable once MAPROM has run from
the AUTO folder (MAPROM_C.PRG in the DFB1X package). If you boot with
Control held, AUTO is skipped, so the game runs \AUTO\MAPROM*.PRG from
the boot drive itself and then starts again. Without MAPROM it runs
from ST RAM.


ATW800/2 GRAPHICS CARD
----------------------

On a TT with an ATW800/2 card on the VME bus, the game shows on the
card's HDMI output, at 640x400 with every pixel doubled. The xVDI
driver may be loaded or not. On quitting, the card goes back to the
mode xVDI had set, or is turned off when xVDI is not loaded. The card
has been tested only under emulation; --no-card leaves it alone.


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
emulator. On real machines, only a Falcon030 (68030 at 50 MHz) and
the MiSTer's Falcon core with a 68060 have run it so far.

- Speed: a Falcon030 with a 50 MHz 68030 shows about 5-6 frames a
  second walking around (measured with --fps). Other machines have
  not been measured.
- The ATW800/2 card has not been tried on a real TT.
- Speech, --line-in, and --gm on a real General MIDI synth are untested
  on real hardware.
- Serpent Isle has not been played far: combat, and speech that events
  trigger, are untested. Serpent Isle saves from DOS have not been
  tried.
- On a real Falcon030, the screen has been seen to stay black after
  quitting. This version changes how the game quits, but that has not
  been tried there yet.
- Version 0.1.0 crashed at once on a 68060; this version runs there.
- There is no AdLib or Sound Blaster music.

Please report what you find, with your machine, memory, TOS version,
and synth.


CHANGES
-------

0.2.0
- Runs on a 68060 (0.1.0 stopped with a black screen).
- About 15-25% more frames a second on a Falcon030: faster screen
  conversion, no wait for the vertical blank, and a moving mouse
  pointer redraws only its own rows.
- Black Gate: music no longer hiccups when a sound effect's instrument
  is sent to the MT-32.
- The ATW800/2 graphics card on the TT, and --no-card.
- Quitting no longer crashes on the way out; the question whether to
  leave says TOS instead of DOS.
- When the game had to run MAPROM itself (booted with Control held),
  New Game showed the version and quit. Fixed.
- --fps, a frame counter.

0.1.0
- The first test release.


CREDITS
-------

Ultima VII and Serpent Isle are by Origin Systems.
The decompilation and ultima7-portable: Jamie Blanks (Kitrinx),
github.com/Kitrinx/Ultima7_Decomp. See LICENSE.TXT.
u7atari: Slaapliedje, with Claude (Anthropic), under the MIT licence.
