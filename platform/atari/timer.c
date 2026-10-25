/* Time: 60 Hz game ticks from the 200 Hz system timer (MFP Timer C, _hz_200), game timers
 * run from plat_pump, and the sound tick.
 *
 * TODO: the sound tick should run from its own MFP interrupt (Timer A or B is free once
 * the game owns the machine) at 60 Hz, with plat_sound_lock raising the IPL.
 */
#include <mint/osbind.h>
#include <mint/sysvars.h>

#include <stddef.h>

#include "plat.h"
#include "backend.h"

#define MAX_GAME_TIMERS 8

static volatile uint32_t *const hz_200 = (volatile uint32_t *) 0x4ba;
static uint32_t start_200;
static plat_timer_fn game_timers[MAX_GAME_TIMERS];
static uint32_t timers_ran_to;
static plat_timer_fn sound_tick;
static int16_t sound_lock_depth;

void timer_init(void)
{
	start_200 = *hz_200;
	timers_ran_to = 0;
}

void timer_shutdown(void)
{
	sound_tick = NULL;
}

uint32_t plat_ticks(void)
{
	return (uint32_t) (((uint64_t) (*hz_200 - start_200) * PLAT_TICKS_PER_SECOND) / 200);
}

uint32_t plat_milliseconds(void)
{
	return (*hz_200 - start_200) * 5;
}

void plat_sleep(uint32_t milliseconds)
{
	uint32_t until = plat_milliseconds() + milliseconds;

	while ((int32_t) (until - plat_milliseconds()) > 0)
		plat_pump();
}

void plat_pump(void)
{
	uint32_t now = plat_ticks();

	input_pump();
	/* Catch up on missed ticks, as the DOS timer interrupt would have. */
	while (timers_ran_to < now) {
		timers_ran_to++;
		for (int i = 0; i < MAX_GAME_TIMERS; i++)
			if (game_timers[i] != NULL)
				game_timers[i]();
	}
	/* TODO: until the sound tick has its own interrupt, run it here. */
}

void plat_yield(void)
{
	plat_pump();
	/* TODO: re-present the last frame (the game may have drawn into it). */
}

void plat_game_timer_add(plat_timer_fn fn)
{
	for (int i = 0; i < MAX_GAME_TIMERS; i++)
		if (game_timers[i] == NULL) {
			game_timers[i] = fn;
			return;
		}
	plat_fatal("Too many game timers");
}

void plat_game_timer_remove(plat_timer_fn fn)
{
	for (int i = 0; i < MAX_GAME_TIMERS; i++)
		if (game_timers[i] == fn)
			game_timers[i] = NULL;
}

void plat_sound_tick_set(plat_timer_fn fn)
{
	sound_tick = fn;
}

void plat_sound_lock(void)
{
	/* TODO: raise IPL above the sound tick's MFP level on the first lock. */
	sound_lock_depth++;
}

void plat_sound_unlock(void)
{
	sound_lock_depth--;
}
