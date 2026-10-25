/* Shared state inside the Atari backend; the game sees only plat.h. */
#ifndef ATARI_BACKEND_H
#define ATARI_BACKEND_H

#include <stdint.h>

typedef enum {
	MACHINE_TT,      /* _MCH 0x00020000: TT low, 320x480x8 planar */
	MACHINE_FALCON,  /* _MCH 0x00030000: VIDEL 320x200x8 planar, line-doubled on VGA */
	MACHINE_OTHER
} machine_type;

extern machine_type atari_machine;

/* Each subsystem resets what it holds for the game before a program starts, and
 * restores the machine on exit. */
void timer_init(void);
void timer_shutdown(void);
void video_init(void);
void video_shutdown(void);
void video_reset(void);
void input_init(void);
void input_shutdown(void);
void input_reset(void);
void input_pump(void);
void sound_init(void);
void sound_shutdown(void);
void sound_reset(void);
void files_init(const char *data_dir);
void files_reset(void);
void console_reset(void);

#endif
