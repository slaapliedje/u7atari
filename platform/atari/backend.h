/* Shared state inside the Atari backend; the game sees only plat.h. */
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
void video_init(void);
void video_shutdown(void);
void video_reset(void);
/* Shows the last presented frame again. */
void video_refresh(void);
/* Asks for the last frame to be shown again once a blank has passed: the game drew the pointer
 * into it. video_pump does it without waiting. */
void video_refresh_wanted(void);
void video_pump(void);
void input_init(void);
void input_shutdown(void);
void input_reset(void);
void input_pump(void);
#ifdef U7_DEBUG_LOG
/* --script: scripted keys and mouse for tests (script.c) */
void script_load(const char *path);
void script_pump(void);
void input_inject_key(uint16_t bios);
void input_inject_mouse(int16_t x, int16_t y, uint16_t buttons);
uint16_t input_mouse_buttons(void);
void video_shot(const char *name);
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
void console_reset(void);

#endif
