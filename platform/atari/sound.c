/* Sound.
 *
 * Music and sound effects: the game's MT-32 driver talks MIDI, and every TT and Falcon has
 * a MIDI port, so a real MT-32 (or CM-32L/compatible) plugs straight in at no CPU cost.
 * Emulating an MT-32 (Munt) or OPL3 (Nuked) is far beyond a 68030, so AdLib is reported
 * unavailable.
 *
 * Speech: unsigned 8-bit mono. TT DMA sound plays signed 8-bit at 6.25/12.5/25/50 kHz;
 * Falcon adds 12.3/24.6/49.2 kHz. Samples are converted (^0x80) and rate-matched into a
 * ST-RAM ring buffer.
 *
 * TODO: PCM ring buffer on DMA sound (frame-end interrupt or polling), MIDI via direct
 * ACIA writes instead of Bconout if BIOS overhead shows up.
 */
#include <mint/osbind.h>

#include "plat.h"
#include "backend.h"

#define BIOS_MIDI 3

/* Silences every channel: sustain off, all notes off, reset controllers. A real MT-32 keeps
 * sounding whatever it was told last, so this runs at start-up (after a crash or a reset in a
 * previous run), between programs, and on every way out. */
static void midi_all_off(void)
{
	for (uint8_t channel = 0; channel < 16; channel++) {
		const uint8_t messages[3][3] = {
			{ 0xb0 | channel, 0x40, 0 },    /* sustain pedal off */
			{ 0xb0 | channel, 0x7b, 0 },    /* all notes off */
			{ 0xb0 | channel, 0x79, 0 },    /* reset all controllers */
		};

		for (int i = 0; i < 3; i++)
			plat_midi_send(messages[i], 3);
	}
}

void sound_init(void)
{
	midi_all_off();
}

void sound_shutdown(void)
{
	plat_sound_tick_set(NULL);
	midi_all_off();
}

void sound_reset(void)
{
	plat_sound_tick_set(NULL);
	midi_all_off();
}

int16_t plat_midi_available(void)
{
	return 1;
}

void plat_midi_send(const uint8_t *message, int16_t length)
{
	for (int16_t i = 0; i < length; i++)
		Bconout(BIOS_MIDI, message[i]);
}

bool plat_opl_available(void)
{
	return false;
}

void plat_opl_write(uint16_t port, uint8_t value)
{
	(void) port; (void) value;
}

uint8_t plat_opl_read(uint16_t port)
{
	(void) port;
	return 0xff;
}

void plat_pcm_start(uint32_t rate)
{
	(void) rate;
}

int32_t plat_pcm_queue(const uint8_t *samples, int32_t count)
{
	(void) samples;
	return count;   /* TODO: drop nothing once DMA playback exists */
}

int32_t plat_pcm_pending(void)
{
	return 0;
}

void plat_pcm_stop(void)
{
}
