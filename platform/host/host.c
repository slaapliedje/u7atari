/* The PC lockstep reference's time, video, input and sound: lockstep.c does the work, and these
 * mirror the Atari backend's video.c, input.c and sound.c wherever the game could tell. */
#include <stdio.h>
#include <string.h>

#include "plat.h"
#include "backend.h"
#include "gm.h"

/* ---- Time: always the virtual clock ---- */

void timer_init(void) {}
void timer_shutdown(void) {}
void timer_drop_sound_lock(void) { lockstep_drop_sound_lock(); }
uint32_t plat_ticks(void) { return lockstep_plat_ticks(); }
uint32_t plat_milliseconds(void) { return lockstep_plat_milliseconds(); }
void plat_sleep(uint32_t milliseconds) { lockstep_plat_sleep(milliseconds); }
void plat_pump(void) { lockstep_plat_pump(); }
void plat_yield(void) { lockstep_plat_yield(); }
void plat_game_timer_add(plat_timer_fn fn) { lockstep_timer_add(fn); }
void plat_game_timer_remove(plat_timer_fn fn) { lockstep_timer_remove(fn); }
void plat_sound_tick_set(plat_timer_fn fn) { lockstep_sound_tick_set(fn); }
void plat_sound_lock(void) { lockstep_sound_lock(); }
void plat_sound_unlock(void) { lockstep_sound_unlock(); }

/* ---- Video: frames are kept, checksummed and saved on request ---- */

#define WIDTH PLAT_SCREEN_WIDTH
#define HEIGHT PLAT_SCREEN_HEIGHT

static bool active;
static const uint8_t *last_pixels;
static uint8_t palette[256 * 3];

void video_init(void) { active = true; }
void video_shutdown(void) { active = false; }

void video_reset(void)
{
	static const uint8_t black[256 * 3];

	last_pixels = NULL;
	if (active)
		plat_video_set_palette(0, 256, black);
}

void plat_video_set_palette(int16_t first, int16_t count, const uint8_t *rgb)
{
	if (!active)
		return;
	if (first < 0) {
		rgb -= 3 * first;
		count += first;
		first = 0;
	}
	if (first + count > 256)
		count = 256 - first;
	if (count <= 0)
		return;
	memcpy(palette + 3 * first, rgb, (size_t) count * 3);
	lockstep_palette(first, count, rgb);
}

void plat_video_present(const uint8_t *pixels)
{
	last_pixels = pixels;
	lockstep_frame(pixels, 'P');
}

void plat_video_wait_retrace(void) { lockstep_wait_retrace(); }
void video_refresh(void) {}
void video_refresh_wanted(void) {}
void video_rows_wanted(int16_t top, int16_t bottom) { (void) top; (void) bottom; }
void video_pump(void) {}
uint8_t *video_frame(void) { return (uint8_t *) last_pixels; }
/* Serpent Isle's debug screens: not compared. */
void video_show_text(const uint16_t *cells, int16_t cursor_x, int16_t cursor_y)
{
	(void) cells; (void) cursor_x; (void) cursor_y;
}
void video_show_game(void) {}

void video_shot(const char *name)
{
	static uint8_t row[WIDTH * 3];
	FILE *f;

	if (last_pixels == NULL || (f = fopen(name, "wb")) == NULL)
		return;
	fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
	for (int y = 0; y < HEIGHT; y++) {
		for (int x = 0; x < WIDTH; x++)
			for (int c = 0; c < 3; c++) {
				uint8_t v = palette[3 * last_pixels[y * WIDTH + x] + c] & 0x3f;

				row[3 * x + c] = (uint8_t) (v << 2 | v >> 4);
			}
		fwrite(row, 1, sizeof row, f);
	}
	fclose(f);
}

/* ---- Input: only the script's ---- */

void input_init(void) {}
void input_shutdown(void) {}
void input_reset(void) { inputq_reset(); }
void input_pump(void) { script_pump(); }

/* ---- Sound: MIDI and speech are only checksummed ---- */

bool sound_line_in;
bool sound_gm;
uint32_t sound_midi_bytes;
uint32_t sound_pcm_samples;

static void midi_out(const uint8_t *message, int16_t length)
{
	plat_sound_lock();
	sound_midi_bytes += (uint32_t) length;
	lockstep_midi(message, length);
	plat_sound_unlock();
}

/* As the Atari's: sustain off, all notes off, reset controllers, on every channel. */
static void midi_all_off(void)
{
	for (uint8_t channel = 0; channel < 16; channel++) {
		const uint8_t messages[3][3] = {
			{ 0xb0 | channel, 0x40, 0 },
			{ 0xb0 | channel, 0x7b, 0 },
			{ 0xb0 | channel, 0x79, 0 },
		};

		for (int i = 0; i < 3; i++)
			midi_out(messages[i], 3);
	}
}

void sound_init(void)
{
	midi_all_off();
	if (sound_gm)
		gm_reset(midi_out);
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
	plat_pcm_stop();
}

void sound_service(void) {}
int16_t plat_midi_available(void) { return 1; }

void plat_midi_send(const uint8_t *message, int16_t length)
{
	if (sound_gm) {
		plat_sound_lock();
		gm_feed(message, length, midi_out);
		plat_sound_unlock();
	} else {
		midi_out(message, length);
	}
}

bool plat_opl_available(void) { return false; }
void plat_opl_write(uint16_t port, uint8_t value) { (void) port; (void) value; }
uint8_t plat_opl_read(uint16_t port) { (void) port; return 0xff; }

void plat_pcm_start(uint32_t rate)
{
	char text[48];

	snprintf(text, sizeof text, "pcm start %lu Hz\r\n", (unsigned long) rate);
	plat_log(text);
	lockstep_pcm_start(rate);
}

int32_t plat_pcm_queue(const uint8_t *samples, int32_t count) { return lockstep_pcm_queue(samples, count); }
int32_t plat_pcm_pending(void) { return lockstep_pcm_pending(); }
void plat_pcm_stop(void) { lockstep_pcm_stop(); }
