/* What differs between the two games the backend runs: CMake defines U7_SERPENT_ISLE for
 * Serpent Isle, else the build is The Black Gate. */
#ifndef ATARI_GAME_H
#define ATARI_GAME_H

#include <stdint.h>

#ifdef U7_SERPENT_ISLE
#define GAME_NAME "Serpent Isle"
#define GAME_A_NAME "a Serpent Isle"
/* the executable, and the file of arguments beside it */
#define GAME_PRG "SERPENT.PRG"
#define GAME_ARG_FILE "SERPENT.ARG"
/* the sound setup the DOS installer wrote, and its speech line when it has none: Sound
 * Blaster Pro ('o') at 220, IRQ 7, DMA 1 */
#define GAME_SOUND_CONFIG "serpent.cfg"
#define GAME_DEFAULT_SPEECH "220 7 1 o"
#define GAME_MAIN SerpentMain
#else
#define GAME_NAME "Ultima VII"
#define GAME_A_NAME "an Ultima VII"
#define GAME_PRG "ULTIMA7.PRG"
#define GAME_ARG_FILE "ULTIMA7.ARG"
#define GAME_SOUND_CONFIG "u7.cfg"
#define GAME_DEFAULT_SPEECH "220 7 1"
#define GAME_MAIN Ultima7Main
#endif

#endif
