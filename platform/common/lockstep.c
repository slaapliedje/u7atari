/* Lockstep (--lockstep, debug builds): the game on a virtual clock, so a script plays the same
 * game on any host, and checksums of what it shows and plays, to compare the Atari build with
 * the PC's line by line (tools/lockstep-compare.sh).
 *
 * Time moves only when the game asks for it or waits for it: each plat_ticks or
 * plat_milliseconds call moves it on by CALL_US, a yield by YIELD_US, a sleep by its length and
 * a retrace wait to the next 70 Hz refresh. A loop that only pumps (the target picker's polls
 * the mouse and nothing else) gets IDLE_PUMPS pumps per yield's worth, as time passes on a
 * real machine whatever the game does. The backend reads the clock without moving it
 * (lockstep_ms). Game timers and the sound tick both run from plat_pump, by the virtual clock;
 * the sound tick only when no sound lock is held. Speech plays at its own rate on the same
 * clock. So the game makes the same calls, and sees the same answers, whatever the host.
 *
 * The log lines all start "LS ":
 *   LS P|R <n> <ms> <frame> m<midi>/<bytes> s<speech>/<samples>
 * when the frame shown differs from the last one logged: P from a present, R from a yield
 * (at most once a tick), the frame and palette's checksum, and the running checksums of every
 * MIDI byte and speech sample so far.
 */
#ifdef U7_DEBUG_LOG

#include <stdio.h>
#include <string.h>

#include "plat.h"
#include "backend.h"

#define CALL_US 100
#define YIELD_US 1000
#define IDLE_PUMPS 64
#define MAX_GAME_TIMERS 8
/* Ticks the sound may fall behind before it skips ahead, as on the Atari. */
#define MAX_SOUND_CATCH_UP 6
/* Speech the backend takes ahead of what has played: 2.6 s, the Atari's ring. */
#define PCM_AHEAD_MS 2600

bool lockstep;

static uint64_t now_us;
static plat_timer_fn game_timers[MAX_GAME_TIMERS];
static uint32_t timers_ran_to;
static plat_timer_fn sound_tick;
static int16_t sound_lock_depth;
static uint32_t sound_ran_to;
static uint32_t yield_hashed_tick;
static uint64_t pumped_at_us;
static uint16_t idle_pumps;

static uint8_t palette[768];
static uint32_t frames, frame_sum, midi_sum, midi_bytes, pcm_sum, pcm_samples;

static uint32_t pcm_rate;                   /* 0: stopped */
static uint64_t pcm_written, pcm_base;      /* input samples */
static uint64_t pcm_base_us;

/* Adler-32, a byte at a time, so the order of bytes in a word does not matter. */
static uint32_t adler(uint32_t sum, const uint8_t *data, uint32_t length)
{
	uint32_t a = sum & 0xffff, b = sum >> 16;

	while (length > 0) {
		uint32_t n = length < 5552 ? length : 5552;

		length -= n;
		while (n-- > 0) {
			a += *data++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}
	return b << 16 | a;
}

static uint32_t ticks_at(uint64_t us)
{
	return (uint32_t) (us * PLAT_TICKS_PER_SECOND / 1000000);
}

uint32_t lockstep_ms(void)
{
	return (uint32_t) (now_us / 1000);
}

void lockstep_start(void)
{
	lockstep = true;
	midi_sum = pcm_sum = frame_sum = 1;
	plat_log("LS start\r\n");
}

/* ---- Time, as the game sees it ---- */

uint32_t lockstep_plat_ticks(void)
{
	now_us += CALL_US;
	return ticks_at(now_us);
}

uint32_t lockstep_plat_milliseconds(void)
{
	now_us += CALL_US;
	return lockstep_ms();
}

static void run_sound_ticks(void)
{
	uint32_t due = ticks_at(now_us);

	if (sound_tick == NULL) {
		sound_ran_to = due;
		return;
	}
	sound_lock_depth++;
	if (due - sound_ran_to > MAX_SOUND_CATCH_UP)
		sound_ran_to = due - MAX_SOUND_CATCH_UP;
	while (sound_ran_to < due && sound_tick != NULL) {
		sound_ran_to++;
		sound_tick();
	}
	sound_lock_depth--;
}

void lockstep_plat_pump(void)
{
	uint32_t now;

	if (now_us != pumped_at_us)
		idle_pumps = 0;
	else if (++idle_pumps >= IDLE_PUMPS) {
		idle_pumps = 0;
		now_us += YIELD_US;
	}
	pumped_at_us = now_us;
	now = ticks_at(now_us);

	input_pump();
	while (timers_ran_to < now) {
		timers_ran_to++;
		for (int i = 0; i < MAX_GAME_TIMERS; i++)
			if (game_timers[i] != NULL)
				game_timers[i]();
	}
	if (sound_lock_depth == 0)
		run_sound_ticks();
	video_pump();
}

void lockstep_plat_sleep(uint32_t milliseconds)
{
	uint64_t until = now_us + (uint64_t) milliseconds * 1000;

	while (now_us < until) {
		now_us += YIELD_US;
		lockstep_plat_pump();
	}
}

void lockstep_plat_yield(void)
{
	uint32_t tick;
	uint8_t *frame;

	now_us += YIELD_US;
	lockstep_plat_pump();
	video_refresh();
	tick = ticks_at(now_us);
	if (tick != yield_hashed_tick && (frame = video_frame()) != NULL) {
		yield_hashed_tick = tick;
		lockstep_frame(frame, 'R');
	}
}

void lockstep_wait_retrace(void)
{
	uint64_t refresh = now_us * 70 / 1000000 + 1;

	lockstep_plat_pump();
	now_us = (refresh * 1000000 + 69) / 70;
}

void lockstep_timer_add(plat_timer_fn fn)
{
	for (int i = 0; i < MAX_GAME_TIMERS; i++)
		if (game_timers[i] == NULL) {
			game_timers[i] = fn;
			return;
		}
	plat_fatal("Too many game timers");
}

void lockstep_timer_remove(plat_timer_fn fn)
{
	for (int i = 0; i < MAX_GAME_TIMERS; i++)
		if (game_timers[i] == fn)
			game_timers[i] = NULL;
}

void lockstep_sound_tick_set(plat_timer_fn fn)
{
	sound_tick = fn;
	sound_ran_to = ticks_at(now_us);
}

void lockstep_sound_lock(void)
{
	sound_lock_depth++;
}

void lockstep_sound_unlock(void)
{
	sound_lock_depth--;
}

void lockstep_drop_sound_lock(void)
{
	sound_lock_depth = 0;
}

/* ---- What the game shows and plays ---- */

void lockstep_palette(int16_t first, int16_t count, const uint8_t *rgb)
{
	if (first >= 0 && first + count <= 256)
		memcpy(palette + 3 * first, rgb, 3 * (size_t) count);
}

void lockstep_frame(const uint8_t *pixels, char kind)
{
	uint32_t sum = adler(adler(1, pixels, PLAT_SCREEN_WIDTH * PLAT_SCREEN_HEIGHT), palette,
		sizeof palette);
	char text[112];

	frames++;
	if (sum == frame_sum)
		return;
	frame_sum = sum;
	snprintf(text, sizeof text, "LS %c %lu %lu %08lx m%08lx/%lu s%08lx/%lu\r\n", kind,
		(unsigned long) frames, (unsigned long) lockstep_ms(), (unsigned long) sum,
		(unsigned long) midi_sum, (unsigned long) midi_bytes, (unsigned long) pcm_sum,
		(unsigned long) pcm_samples);
	plat_log(text);
}

void lockstep_midi(const uint8_t *message, int16_t length)
{
	midi_sum = adler(midi_sum, message, (uint32_t) length);
	midi_bytes += (uint32_t) length;
}

/* Moves what has played on to now; when it has all played, playing starts again from the next
 * sample queued. */
static void pcm_advance(void)
{
	uint64_t played;

	if (pcm_rate == 0)
		return;
	played = pcm_base + (now_us - pcm_base_us) * pcm_rate / 1000000;
	if (played >= pcm_written) {
		pcm_base = pcm_written;
		pcm_base_us = now_us;
	}
}

static uint64_t pcm_played(void)
{
	uint64_t played = pcm_base + (now_us - pcm_base_us) * pcm_rate / 1000000;

	return played < pcm_written ? played : pcm_written;
}

void lockstep_pcm_start(uint32_t rate)
{
	pcm_rate = rate;
	pcm_written = pcm_base = 0;
	pcm_base_us = now_us;
}

int32_t lockstep_pcm_queue(const uint8_t *samples, int32_t count)
{
	uint64_t room;
	int32_t taken;

	if (pcm_rate == 0)
		return count;
	pcm_advance();
	room = (uint64_t) pcm_rate * PCM_AHEAD_MS / 1000 - (pcm_written - pcm_played());
	taken = (uint64_t) count < room ? count : (int32_t) room;
	pcm_sum = adler(pcm_sum, samples, (uint32_t) taken);
	pcm_samples += (uint32_t) taken;
	pcm_written += (uint64_t) taken;
	return taken;
}

int32_t lockstep_pcm_pending(void)
{
	if (pcm_rate == 0)
		return 0;
	pcm_advance();
	return (int32_t) (pcm_written - pcm_played());
}

void lockstep_pcm_stop(void)
{
	pcm_rate = 0;
}

/* The totals, once: from the script's quit, or the program's exit. */
void lockstep_end(void)
{
	static bool ended;
	char text[112];

	if (ended)
		return;
	ended = true;

	snprintf(text, sizeof text, "LS end %lu %lu m%08lx/%lu s%08lx/%lu\r\n",
		(unsigned long) frames, (unsigned long) lockstep_ms(), (unsigned long) midi_sum,
		(unsigned long) midi_bytes, (unsigned long) pcm_sum, (unsigned long) pcm_samples);
	plat_log(text);
}

#endif
