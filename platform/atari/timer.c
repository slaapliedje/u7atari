/* Time: 60 Hz game ticks from the 200 Hz system timer (MFP Timer C, _hz_200), game timers
 * run from plat_pump, and the sound tick on its own interrupt.
 *
 * The sound tick runs from MFP Timer B in delay mode, as the DOS game's music ran from the PC
 * timer interrupt, so music keeps time through loads and long frames. TOS leaves Timer B
 * unused; Timer A stays free for DMA sound, which counts frames on it.
 *
 * The lock does not mask interrupts: the system timer, keyboard and MIDI must keep running
 * while the game holds it, sometimes for a whole MT-32 upload. A tick that falls due while the
 * lock is held is counted, and runs when the lock is let go.
 */
#include <mint/osbind.h>
#include <mint/sysvars.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "plat.h"
#include "backend.h"

#define MAX_GAME_TIMERS 8

static volatile uint32_t *const hz_200 = (volatile uint32_t *) 0x4ba;
static uint32_t start_200;
static plat_timer_fn game_timers[MAX_GAME_TIMERS];
static uint32_t timers_ran_to;
static plat_timer_fn volatile sound_tick;
/* Locks held, counting the one each running tick holds; ticks run only from 0. Changed by the
 * interrupt only around a tick it runs, so it finds the value again on return. */
static volatile int16_t sound_lock_depth;
/* Ticks counted by the interrupt, and run. Each has a single writer. */
static volatile uint16_t sound_ticks_due;
static uint16_t sound_ticks_run;
static void (*old_timer_b)(void);
static bool sound_timer_on;

/* Ticks the sound may fall behind before it skips ahead instead of catching up: a long hold of
 * the lock would otherwise replay seconds of music events at once. */
#define MAX_SOUND_CATCH_UP 6

/* MFP Timer B: 2.4576 MHz / 200 / 205 = 59.94 Hz */
#define XB_TIMERB 1
#define MFP_TIMER_B 8                   /* the MFP interrupt channel */
#define TIMER_B_VECTOR (0x40 + MFP_TIMER_B)
#define TIMER_DELAY_200 7
#define SOUND_TIMER_COUNT 205

void timer_sound_vector(void);          /* timerint.S */

void timer_init(void)
{
	start_200 = *hz_200;
	timers_ran_to = 0;
	sound_ticks_run = sound_ticks_due;
	old_timer_b = Setexc(TIMER_B_VECTOR, -1L);
	Xbtimer(XB_TIMERB, TIMER_DELAY_200, SOUND_TIMER_COUNT, timer_sound_vector);
	sound_timer_on = true;
}

void timer_shutdown(void)
{
	sound_tick = NULL;
	if (!sound_timer_on)
		return;
	sound_timer_on = false;
	Jdisint(MFP_TIMER_B);
	*(volatile uint8_t *) 0xfffffa1b = 0;     /* TBCR: stop the timer */
	Setexc(TIMER_B_VECTOR, old_timer_b);
}

/* Runs the ticks that are due. Called with no lock held, from the interrupt or from the last
 * unlock. A tick that falls due between the last check and the lock's release finds the lock
 * free and runs from its own interrupt, or is caught by the check after. */
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

/* From the Timer B interrupt: counts the tick, and says whether this interrupt should run the
 * ticks; it then calls timer_sound_run on its own stack with the other interrupts let in. */
int timer_sound_due(void)
{
	sound_ticks_due++;
	return sound_lock_depth == 0;
}

void timer_sound_run(void)
{
	run_sound_ticks();
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
#ifdef U7_DEBUG_LOG
	{
		static uint32_t last_report;

		if (now - last_report >= 5 * PLAT_TICKS_PER_SECOND) {
			last_report = now;
			char text[96];

			snprintf(text, sizeof text, "pump: sound ticks %u, MIDI bytes %lu, PCM %lu\r\n",
				(unsigned) sound_ticks_run, (unsigned long) sound_midi_bytes,
				(unsigned long) sound_pcm_samples);
			plat_log(text);
		}
	}
#endif
	/* Catch up on missed ticks, as the DOS timer interrupt would have. */
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
	plat_pump();
	video_refresh();
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
	plat_sound_lock();
	sound_tick = fn;
	sound_ticks_run = sound_ticks_due;    /* a new tick starts now, with no backlog */
	plat_sound_unlock();
}

void plat_sound_lock(void)
{
	sound_lock_depth++;
}

void plat_sound_unlock(void)
{
	if (--sound_lock_depth == 0 && sound_ticks_run != sound_ticks_due)
		run_sound_ticks();
}

/* The program left by plat_exit, maybe from inside a lock it will never let go. */
void timer_drop_sound_lock(void)
{
	sound_lock_depth = 0;
}
