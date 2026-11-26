/* Time on the Jaguar: the vertical blank (jag_vbl in startup.S) counts it, and runs the sound
 * tick, as platform/atari/timer.c does from the 200 Hz timer and MFP Timer B. On an NTSC Jaguar
 * the blank comes at 59.94 Hz, the Atari's sound tick rate; a PAL one's 50 Hz runs the music a
 * sixth slow (a Jerry timer will do for that later). Game timers run from plat_pump, catching up
 * on the ticks missed, as the DOS timer interrupt's would have.
 *
 * The lock does not mask interrupts: a tick that falls due while the game holds it is counted
 * and runs when the lock is let go. With --lockstep, lockstep.c's virtual clock is used instead.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "plat.h"
#include "backend.h"

#define MAX_GAME_TIMERS 8
#define MAX_SOUND_CATCH_UP 6
#define CONFIG 0xF14002

extern volatile uint32_t jag_ticks;
extern void (*volatile jag_vbl_hook)(void);

#ifdef U7_DEBUG_LOG
#define LOCKSTEP(call) do { if (lockstep) { call; return; } } while (0)
#define LOCKSTEP_VALUE(call) do { if (lockstep) return call; } while (0)
#else
#define LOCKSTEP(call) do { } while (0)
#define LOCKSTEP_VALUE(call) do { } while (0)
#endif

static uint32_t start_ticks;
/* The blank's rate as a fraction: NTSC 60000/1001 Hz (59.94), PAL 50/1. Time is worked out from
 * blanks with 32-bit arithmetic only, the 68000 having no 64-bit (nor 32-bit) divide: blanks
 * times 1001, split into whole seconds and the rest, keeps every product in range. */
static uint32_t rate_num = 60000, rate_den = 1001;
static plat_timer_fn game_timers[MAX_GAME_TIMERS];
static uint32_t timers_ran_to;
static plat_timer_fn volatile sound_tick;
static volatile int16_t sound_lock_depth;
static volatile uint16_t sound_ticks_due;
static uint16_t sound_ticks_run;

static uint32_t blanks(void)
{
	return jag_ticks - start_ticks;
}

static void run_sound_ticks(void)
{
	do {
		sound_lock_depth = 1;
		while (sound_ticks_run != sound_ticks_due) {
			if ((uint16_t) (sound_ticks_due - sound_ticks_run) > MAX_SOUND_CATCH_UP)
				sound_ticks_run = sound_ticks_due - MAX_SOUND_CATCH_UP;
			sound_ticks_run++;
			sound_service();
			if (sound_tick != NULL)
				sound_tick();
		}
		sound_lock_depth = 0;
	} while (sound_ticks_run != sound_ticks_due);
}

/* From the vertical blank. */
static void timer_vbl(void)
{
	sound_ticks_due++;
	if (sound_lock_depth == 0)
		run_sound_ticks();
}

void timer_init(void)
{
	start_ticks = jag_ticks;
	if (*(volatile uint16_t *) CONFIG & 0x10) {
		rate_num = 60000;
		rate_den = 1001;
	} else {
		rate_num = 50;
		rate_den = 1;
	}
	LOCKSTEP((void) 0);
	timers_ran_to = 0;
	sound_ticks_run = sound_ticks_due;
	jag_vbl_hook = timer_vbl;
}

void timer_shutdown(void)
{
	sound_tick = NULL;
	jag_vbl_hook = NULL;
}

/* t (blanks times rate_den) in whole seconds and what is left over, times unit, over rate_num */
static uint32_t scaled(uint32_t unit)
{
	uint32_t t = blanks() * rate_den;
	uint32_t seconds = t / rate_num, rest = t % rate_num;

	return seconds * unit + rest * unit / rate_num;
}

uint32_t timer_real_milliseconds(void)
{
	return scaled(1000);
}

uint32_t plat_ticks(void)
{
	LOCKSTEP_VALUE(lockstep_plat_ticks());
	return scaled(PLAT_TICKS_PER_SECOND);
}

uint32_t plat_milliseconds(void)
{
	LOCKSTEP_VALUE(lockstep_plat_milliseconds());
	return timer_real_milliseconds();
}

void plat_sleep(uint32_t milliseconds)
{
	LOCKSTEP(lockstep_plat_sleep(milliseconds));
	uint32_t until = plat_milliseconds() + milliseconds;

	while ((int32_t) (until - plat_milliseconds()) > 0)
		plat_pump();
}

void plat_pump(void)
{
	LOCKSTEP(lockstep_plat_pump());
	uint32_t now = plat_ticks();

	input_pump();
	while (timers_ran_to < now) {
		timers_ran_to++;
		for (int i = 0; i < MAX_GAME_TIMERS; i++)
			if (game_timers[i] != NULL)
				game_timers[i]();
	}
	video_pump();
}

void plat_yield(void)
{
	LOCKSTEP(lockstep_plat_yield());
	plat_pump();
	video_refresh();
}

void plat_game_timer_add(plat_timer_fn fn)
{
	LOCKSTEP(lockstep_timer_add(fn));
	for (int i = 0; i < MAX_GAME_TIMERS; i++)
		if (game_timers[i] == NULL) {
			game_timers[i] = fn;
			return;
		}
	plat_fatal("Too many game timers");
}

void plat_game_timer_remove(plat_timer_fn fn)
{
	LOCKSTEP(lockstep_timer_remove(fn));
	for (int i = 0; i < MAX_GAME_TIMERS; i++)
		if (game_timers[i] == fn)
			game_timers[i] = NULL;
}

void plat_sound_tick_set(plat_timer_fn fn)
{
	LOCKSTEP(lockstep_sound_tick_set(fn));
	plat_sound_lock();
	sound_tick = fn;
	sound_ticks_run = sound_ticks_due;
	plat_sound_unlock();
}

void plat_sound_lock(void)
{
	LOCKSTEP(lockstep_sound_lock());
	sound_lock_depth++;
}

void plat_sound_unlock(void)
{
	LOCKSTEP(lockstep_sound_unlock());
	if (--sound_lock_depth == 0 && sound_ticks_run != sound_ticks_due)
		run_sound_ticks();
}

void timer_drop_sound_lock(void)
{
	LOCKSTEP(lockstep_drop_sound_lock());
	sound_lock_depth = 0;
}
