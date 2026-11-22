/* Shared state inside a backend (platform/atari, or platform/host for the PC lockstep reference)
 * and the parts they share (platform/common); the game sees only plat.h. */
#ifndef ATARI_BACKEND_H
#define ATARI_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	MACHINE_TT,      /* _MCH 0x00020000: TT low, 320x480x8 planar */
	MACHINE_FALCON,  /* _MCH 0x00030000: VIDEL 320x200x8 planar, line-doubled on VGA */
	MACHINE_OTHER
} machine_type;

extern machine_type atari_machine;

/* Each subsystem resets what it holds for the game before a program starts, and
 * restores the machine on exit. Shutdown must be safe when init never ran: plat_fatal
 * calls it from anywhere, including before the machine is taken over. */
void timer_init(void);
void timer_shutdown(void);
/* Lets go of every sound lock a program left by plat_exit was holding. */
void timer_drop_sound_lock(void);
/* --no-card: the TT's own video even with an ATW800/2 in the machine. */
extern bool video_no_card;
/* --fps: counts the frames presented; video_fps_report() writes U7FPS.TXT. */
extern bool video_fps;
void video_fps_report(void);
void video_init(void);
void video_shutdown(void);
void video_reset(void);
/* Shows the last presented frame again. */
void video_refresh(void);
/* Asks for the last frame to be shown again once a blank has passed: the game drew the pointer
 * into it. video_pump does it without waiting. */
void video_refresh_wanted(void);
/* Asks plat_pump to show rows top to bottom of the last frame again: the pointer moved there. */
void video_rows_wanted(int16_t top, int16_t bottom);
void video_pump(void);
/* The game's last frame, which the console draws text into; NULL before the first. */
uint8_t *video_frame(void);
/* Shows 80x25 text cells (character, attribute << 8) and the cursor in place of the game's
 * frames until video_show_game. */
void video_show_text(const uint16_t *cells, int16_t cursor_x, int16_t cursor_y);
void video_show_game(void);
/* A pointer the backend draws itself, in place of the game's (plat_cursor_*): video_pointer
 * is nonzero when there is one, from video_init on. */
int16_t video_pointer(void);
void video_pointer_shape(const uint8_t *pixels, int16_t width, int16_t height, int16_t hot_x,
	int16_t hot_y);
void video_pointer_show(int16_t visible);
/* The Atari's: what the game needs free when it starts, its arena (LINEAR_MEMORY_SIZE, set in
 * CMakeLists.txt) and its other blocks (main.c). */
#define MEMORY_NEEDED (LINEAR_MEMORY_SIZE + (UINT32_C(2) << 20))

/* The Atari's (pointer.c): drawn from the vertical blank on the screen on display. */
bool pointer_start(int16_t first_line, int16_t repeat, int32_t stride);
void pointer_stop(void);
bool pointer_running(void);
void pointer_screen(uint8_t *base);
void pointer_reset(void);
void pointer_shape(const uint8_t *pixels, int16_t width, int16_t height, int16_t hot_x,
	int16_t hot_y);
void pointer_show(bool show);
void pointer_vbl(void);
#ifdef U7_DEBUG_LOG
void pointer_into(uint8_t *pixels);
extern uint32_t pointer_draws;
#endif
/* The ATW800/2 graphics card (atw800.c). Start finds it and takes it over, in supervisor mode;
 * false when there is none. */
bool atw800_start(void);
void atw800_stop(void);
void atw800_palette(int16_t first, int16_t count, const uint8_t *rgb);
void atw800_frame(const uint8_t *pixels, uint8_t *shadow, bool full);
#ifdef U7_DEBUG_LOG
void atw800_check(const uint8_t *pixels, const uint8_t *colours);
#endif
void input_init(void);
void input_shutdown(void);
void input_reset(void);
void input_pump(void);
/* From an interrupt: the mouse as it is now, in the game's 0..639 x 0..199, with the moves the
 * pump has not applied yet; false while the pump is changing it. */
bool input_pointer_at(int16_t *x, int16_t *y);
#ifdef U7_DEBUG_LOG
/* --script: scripted keys and mouse for tests (script.c) */
void script_load(const char *path);
void script_pump(void);
void input_inject_key(uint16_t bios);
void input_inject_mouse(int16_t x, int16_t y, uint16_t buttons);
uint16_t input_mouse_buttons(void);
uint32_t script_pad(void);
void video_shot(const char *name);
/* for the script's mem step: what memory is left, or in use, as text */
void memory_report(char *text, size_t size);
#endif
/* The keys and mouse as the game sees them (inputq.c): backends hand over PC BIOS key words,
 * the shift state, and the mouse's position and buttons. */
void inputq_reset(void);
void inputq_key(uint16_t bios);
void inputq_modifiers(uint16_t state);
void inputq_mouse_position(int16_t *x, int16_t *y);
uint16_t inputq_mouse_buttons(void);
void inputq_mouse_to(int16_t x, int16_t y);
void inputq_mouse_set_buttons(uint16_t buttons);
void inputq_pad_buttons(uint16_t buttons);
int16_t inputq_key_count(void);

/* A Jaguar pad (pad.c): the backend hands over the buttons held, as these bits. */
#define PAD_UP 0x0001
#define PAD_DOWN 0x0002
#define PAD_LEFT 0x0004
#define PAD_RIGHT 0x0008
#define PAD_A 0x0010
#define PAD_B 0x0020
#define PAD_C 0x0040
#define PAD_OPTION 0x0080
#define PAD_PAUSE 0x0100
#define PAD_STAR 0x0200
#define PAD_HASH 0x0400
#define PAD_KEY(n) (UINT32_C(0x0800) << (n))     /* the keypad's 0 to 9 */
#define PAD_KEYS (PAD_KEY(10) - PAD_KEY(0))
void pad_pump(uint32_t bits);
void pad_reset(void);
void pad_ahead(uint32_t bits, int16_t *x, int16_t *y);
#ifdef U7_DEBUG_LOG
/* --lockstep (lockstep.c): a virtual clock and checksums, to compare hosts. */
extern bool lockstep;
void lockstep_start(void);
void lockstep_end(void);
/* The clock, not moving it: for the backend's own use. */
uint32_t lockstep_ms(void);
uint32_t lockstep_plat_ticks(void);
uint32_t lockstep_plat_milliseconds(void);
void lockstep_plat_sleep(uint32_t milliseconds);
void lockstep_plat_pump(void);
void lockstep_plat_yield(void);
void lockstep_wait_retrace(void);
void lockstep_timer_add(void (*fn)(void));
void lockstep_timer_remove(void (*fn)(void));
void lockstep_sound_tick_set(void (*fn)(void));
void lockstep_sound_lock(void);
void lockstep_sound_unlock(void);
void lockstep_drop_sound_lock(void);
void lockstep_palette(int16_t first, int16_t count, const uint8_t *rgb);
/* A frame shown: kind P from a present, R from a yield. */
void lockstep_frame(const uint8_t *pixels, char kind);
/* MIDI as it goes out, after any General MIDI translation. */
void lockstep_midi(const uint8_t *message, int16_t length);
void lockstep_pcm_start(uint32_t rate);
int32_t lockstep_pcm_queue(const uint8_t *samples, int32_t count);
int32_t lockstep_pcm_pending(void);
void lockstep_pcm_stop(void);
#endif
/* Restores the machine and exits the process. */
void backend_quit(int code);
void sound_init(void);
void sound_shutdown(void);
void sound_reset(void);
/* Each sound tick: counts the speech the DMA has played and silences it. */
void sound_service(void);
/* --line-in: the Falcon mixes its input jack into the output. */
extern bool sound_line_in;
/* --gm: MIDI for a General MIDI synth, not an MT-32 (gm.c). */
extern bool sound_gm;
#ifdef U7_DEBUG_LOG
extern uint32_t sound_midi_bytes;
extern uint32_t sound_pcm_samples;
#endif
/* The data folder every game file name is resolved against. */
void files_set_root(const char *dir);
const char *files_root(void);
/* False with a message for the player when the folder is not a usable installation. */
bool files_check_data(char *message, size_t size);
void files_reset(void);

/* diskcache.c: more GEMDOS sector buffers while the game runs (plain TOS) */
void disk_cache_init(void);
void disk_cache_shutdown(void);
void console_reset(void);

#endif
