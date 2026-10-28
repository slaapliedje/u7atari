/* General MIDI output: the game's MT-32 MIDI, translated for a GM synth. See gm.c. */
#ifndef U7_GM_H
#define U7_GM_H

#include <stdint.h>

typedef void (*gm_output)(const uint8_t *message, int16_t length);

/* Forgets what the game wrote to the MT-32 and sends the GM synth its reset. */
void gm_reset(gm_output out);

/* Takes bytes the game sent the MT-32, in any pieces, and sends out what a GM synth needs. */
void gm_feed(const uint8_t *bytes, int16_t length, gm_output out);

#endif
