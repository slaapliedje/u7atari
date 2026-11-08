/* Sound.
 *
 * Music and sound effects: the game's MT-32 driver talks MIDI, and every TT and Falcon has
 * a MIDI port, so a real MT-32 (or CM-32L/compatible) plugs straight in at no CPU cost.
 * Emulating an MT-32 (Munt) or OPL3 (Nuked) is far beyond a 68030, so AdLib is reported
 * unavailable.
 *
 * MIDI goes straight to the MIDI ACIA: the sound tick sends from an interrupt, where a BIOS
 * call could land inside one the game is making. With --gm it goes through gm.c first, for a
 * General MIDI synth instead of an MT-32. The bytes wait in a queue, which the ACIA's transmit
 * interrupt empties (midi_system_vector in ikbd.S, in the MIDI system vector that TOS's ACIA
 * interrupt calls): at 31250 baud a byte takes 320 us to go out, which the game would otherwise
 * spend waiting.
 *
 * Speech: the game hands over unsigned 8-bit mono at Sound Blaster rates (about 11 to 22 kHz).
 * DMA sound plays signed 8-bit mono from a ring buffer in ST RAM, looping over it from start-up
 * to exit, at 25 kHz (TT 25033 Hz, Falcon 24585 Hz). Samples are resampled into it with linear
 * interpolation, behind the DMA's position. Each sound tick zeroes what has played, so when the
 * game stops feeding the ring plays silence, never old speech. Where the DMA is comes from its
 * frame address counter.
 *
 * Falcon line in (--line-in): the codec adds its input jack to the output, so a MIDI synth
 * plugged in there plays through the Falcon's speaker and line out with the speech. The codec
 * then runs at 49170 Hz, for the synth's sake. The TT has no audio input.
 */
#include <stdio.h>
#include <string.h>

#include <mint/osbind.h>
#include <mint/falcon.h>
#include <mint/sysvars.h>

#include "plat.h"
#include "backend.h"
#include "gm.h"
#include "resample.h"

#define MIDI_ACIA_STATUS ((volatile uint8_t *) 0xfffffc04)
#define MIDI_ACIA_DATA ((volatile uint8_t *) 0xfffffc06)
#define MIDI_ACIA_CONTROL MIDI_ACIA_STATUS      /* written */
#define ACIA_TDRE 0x02          /* transmit data register empty */
/* Receive interrupt on, RTS low, clock / 16, 8 bits, no parity, 1 stop bit, as TOS sets it; and
 * with the transmit interrupt on. */
#define ACIA_TRANSMIT_IDLE 0x95
#define ACIA_TRANSMIT_INTERRUPT 0xb5

/* Shared with midi_system_vector (ikbd.S). Each index has a single writer: in this code, out
 * the interrupt. */
#define MIDI_QUEUE_SIZE 1024    /* a power of two; ikbd.S has it too */
uint8_t midi_queue[MIDI_QUEUE_SIZE];
volatile uint16_t midi_queue_in, midi_queue_out;
volatile uint8_t midi_sending;  /* the transmit interrupt is on */
long (*midi_old_system_vector)(void);
long midi_system_vector(void);
static _KBDVECS *kbd_vectors;
static bool midi_queued;        /* the vector is in place */

#ifdef U7_DEBUG_LOG
uint32_t sound_midi_bytes;
uint32_t sound_pcm_samples;
#endif

bool sound_line_in;
bool sound_gm;

/* STE/TT DMA sound registers, which the Falcon keeps */
#define DMA_CONTROL ((volatile uint8_t *) 0xffff8901)     /* bit 0 play, bit 1 repeat */
#define DMA_START ((volatile uint8_t *) 0xffff8903)       /* high, mid, low on odd bytes */
#define DMA_COUNTER ((volatile uint8_t *) 0xffff8909)
#define DMA_END ((volatile uint8_t *) 0xffff890f)
#define DMA_MODE ((volatile uint8_t *) 0xffff8921)        /* bit 7 mono, bits 1-0 rate */
#define DMA_PLAY_REPEAT 0x03
#define DMA_MONO_25K 0x82

#define RING_SIZE 65536         /* 2.6 s at 25 kHz; a power of two */
#define RING_SIZE_LINE_IN 131072
/* Samples kept between where the DMA reads and where new ones are written: the DMA reads
 * ahead into its FIFO. */
#define PCM_LEAD 64

static int8_t *ring;
static uint32_t ring_mask;
static uint32_t out_rate;
static uint32_t pcm_written, pcm_played;   /* output samples, ever */
static uint32_t counter_seen;              /* the DMA's offset in the ring when last looked at */
static uint32_t in_rate;                   /* 0: stopped */
static resampler speech;

/* What the Falcon's codec had, to put back. */
static int16_t old_adder, old_adc_input;
static bool sound_locked;

static void midi_out(const uint8_t *message, int16_t length);

static void midi_queue_start(void)
{
	midi_queue_in = midi_queue_out = 0;
	midi_sending = 0;
	kbd_vectors = Kbdvbase();
	midi_old_system_vector = kbd_vectors->midisys;
	/* One long write, so the interrupt sees the old vector or the new. */
	kbd_vectors->midisys = midi_system_vector;
	midi_queued = true;
}

/* Lets what is queued go out, at most a second, and puts the vector back. */
static void midi_queue_stop(void)
{
	uint32_t until = *_hz_200 + 200;

	if (!midi_queued)
		return;
	while (midi_queue_out != midi_queue_in && (int32_t) (until - *_hz_200) > 0)
		;
	*MIDI_ACIA_CONTROL = ACIA_TRANSMIT_IDLE;
	midi_sending = 0;
	kbd_vectors->midisys = midi_old_system_vector;
	midi_queued = false;
	while (!(*MIDI_ACIA_STATUS & ACIA_TDRE) && (int32_t) (until - *_hz_200) > 0)
		;
}

/* The queue is full: waits for the interrupt to send a byte, or sends one itself when the
 * interrupt cannot come in. */
static void midi_wait_for_room(uint16_t next)
{
	while (next == midi_queue_out) {
		uint16_t sr;

		__asm__ volatile ("move.w %%sr,%0" : "=d" (sr));
		if ((sr & 0x0700) >= 0x0600 && (*MIDI_ACIA_STATUS & ACIA_TDRE)) {
			*MIDI_ACIA_DATA = midi_queue[midi_queue_out];
			midi_queue_out = (uint16_t) ((midi_queue_out + 1) & (MIDI_QUEUE_SIZE - 1));
		}
	}
}

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
			midi_out(messages[i], 3);
	}
}

static void pcm_init(void);
static void pcm_shutdown(void);

void sound_init(void)
{
	midi_queue_start();
	midi_all_off();
	if (sound_gm)
		gm_reset(midi_out);
	pcm_init();
}

void sound_shutdown(void)
{
	plat_sound_tick_set(NULL);
	midi_all_off();
	midi_queue_stop();
	pcm_shutdown();
}

void sound_reset(void)
{
	plat_sound_tick_set(NULL);
	midi_all_off();
	plat_pcm_stop();
}

int16_t plat_midi_available(void)
{
	return 1;
}

/* Under the sound lock, so a message from the game is never split by one from the tick. */
static void midi_out(const uint8_t *message, int16_t length)
{
	plat_sound_lock();
	for (int16_t i = 0; i < length; i++) {
		uint16_t next = (uint16_t) ((midi_queue_in + 1) & (MIDI_QUEUE_SIZE - 1));

		if (!midi_queued) {
			while (!(*MIDI_ACIA_STATUS & ACIA_TDRE))
				;
			*MIDI_ACIA_DATA = message[i];
			continue;
		}
		midi_wait_for_room(next);
		midi_queue[midi_queue_in] = message[i];
		midi_queue_in = next;
		if (!midi_sending) {
			midi_sending = 1;
			*MIDI_ACIA_CONTROL = ACIA_TRANSMIT_INTERRUPT;
		}
	}
#ifdef U7_DEBUG_LOG
	sound_midi_bytes += (uint32_t) length;
	if (lockstep)
		lockstep_midi(message, length);
#endif
	plat_sound_unlock();
}

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

/* ---- Speech ---- */

static uint32_t dma_offset(void)
{
	uint32_t a, b;

	/* The three bytes are read one at a time while the counter runs: read until two agree. */
	do {
		a = (uint32_t) DMA_COUNTER[0] << 16 | (uint32_t) DMA_COUNTER[2] << 8 | DMA_COUNTER[4];
		b = (uint32_t) DMA_COUNTER[0] << 16 | (uint32_t) DMA_COUNTER[2] << 8 | DMA_COUNTER[4];
	} while (a != b);
	return (a - (uint32_t) (uintptr_t) ring) & ring_mask;
}

static void zero_ring(uint32_t from, uint32_t count)
{
	from &= ring_mask;
	if (from + count > ring_mask + 1) {
		uint32_t first = ring_mask + 1 - from;

		memset(ring + from, 0, first);
		memset(ring, 0, count - first);
	} else {
		memset(ring + from, 0, count);
	}
}

/* Counts what the DMA has played since last time and silences it. Holds the sound lock. */
static void pcm_catch_up(void)
{
	uint32_t now = dma_offset();
	uint32_t played = (now - counter_seen) & ring_mask;

	zero_ring(counter_seen, played);
	counter_seen = now;
	pcm_played += played;
	if ((int32_t) (pcm_written - pcm_played) < 0)
		pcm_written = pcm_played;           /* ran dry */
}

/* Drops whatever is queued and not yet played. Holds the sound lock. */
static void pcm_discard(void)
{
	pcm_catch_up();
	zero_ring(pcm_played, pcm_written - pcm_played);
	pcm_written = pcm_played;
	resampler_start(&speech, in_rate != 0 ? in_rate : out_rate, out_rate);
}

void sound_service(void)
{
	if (ring != NULL)
		pcm_catch_up();
}

static void start_tt(uint32_t start, uint32_t end)
{
	DMA_CONTROL[0] = 0;
	DMA_START[0] = (uint8_t) (start >> 16);     /* high byte first: it clears the others */
	DMA_START[2] = (uint8_t) (start >> 8);
	DMA_START[4] = (uint8_t) start;
	DMA_END[0] = (uint8_t) (end >> 16);
	DMA_END[2] = (uint8_t) (end >> 8);
	DMA_END[4] = (uint8_t) end;
	DMA_MODE[0] = DMA_MONO_25K;
	DMA_CONTROL[0] = DMA_PLAY_REPEAT;
	out_rate = 25033;
}

static void start_falcon(uint32_t start, uint32_t end)
{
	sound_locked = Locksnd() == 1;
	old_adder = (int16_t) Soundcmd(ADDERIN, -1);
	old_adc_input = (int16_t) Soundcmd(ADCINPUT, -1);
	Buffoper(0);
	Setmode(MONO8);
	Devconnect(DMAPLAY, DAC, CLK25M, sound_line_in ? CLK50K : CLK25K, NO_SHAKE);
	if (sound_line_in) {
		Soundcmd(ADCINPUT, 0);                  /* both channels from the input jack */
		Soundcmd(ADDERIN, ADCIN | MATIN);
	} else {
		Soundcmd(ADDERIN, MATIN);
	}
	Setbuffer(SR_PLAY, (void *) (uintptr_t) start, (void *) (uintptr_t) end);
	Buffoper(SB_PLA_ENA | SB_PLA_RPT);
	out_rate = sound_line_in ? 49170 : 24585;
}

static void pcm_init(void)
{
	uint32_t size = atari_machine == MACHINE_FALCON && sound_line_in ? RING_SIZE_LINE_IN : RING_SIZE;

	ring = (int8_t *) Mxalloc(size, 0);         /* ST RAM, where the DMA reads */
	if (ring == NULL || (long) ring < 0) {
		ring = NULL;
		plat_log("u7: no ST RAM for speech\r\n");
		return;
	}
	memset(ring, 0, size);
	ring_mask = size - 1;
	if (atari_machine == MACHINE_FALCON)
		start_falcon((uintptr_t) ring, (uintptr_t) ring + size);
	else
		start_tt((uintptr_t) ring, (uintptr_t) ring + size);
	counter_seen = dma_offset();
	pcm_written = pcm_played = 0;
	in_rate = 0;
}

static void pcm_shutdown(void)
{
	int8_t *memory = ring;

	if (ring == NULL)
		return;
	plat_sound_lock();
	ring = NULL;                                /* the tick leaves it alone from here */
	plat_sound_unlock();
	if (atari_machine == MACHINE_FALCON) {
		Buffoper(0);
		Soundcmd(ADCINPUT, old_adc_input);
		Soundcmd(ADDERIN, old_adder);
		if (sound_locked)
			Unlocksnd();
	} else {
		DMA_CONTROL[0] = 0;
	}
	Mfree(memory);
}

void plat_pcm_start(uint32_t rate)
{
#ifdef U7_DEBUG_LOG
	{
		char text[48];

		snprintf(text, sizeof text, "pcm start %lu Hz\r\n", (unsigned long) rate);
		plat_log(text);
	}
	if (lockstep) {
		lockstep_pcm_start(rate);
		return;
	}
#endif
	if (ring == NULL)
		return;
	plat_sound_lock();
	in_rate = rate;
	pcm_discard();
	plat_sound_unlock();
}

int32_t plat_pcm_queue(const uint8_t *samples, int32_t count)
{
	int32_t taken;
	uint32_t room, at, written;

#ifdef U7_DEBUG_LOG
	if (lockstep)
		return lockstep_pcm_queue(samples, count);
#endif
	if (ring == NULL || in_rate == 0)
		return count;                       /* nowhere to play it: dropped */
	plat_sound_lock();
	pcm_catch_up();
	/* Starting afresh: leave the DMA's read-ahead alone. */
	if (pcm_written - pcm_played < PCM_LEAD)
		pcm_written = pcm_played + PCM_LEAD;
	room = ring_mask + 1 - PCM_LEAD - (pcm_written - pcm_played);
	at = pcm_written & ring_mask;
	taken = resample(&speech, samples, count, ring, ring_mask, &at, room, &written);
	pcm_written += written;
#ifdef U7_DEBUG_LOG
	sound_pcm_samples += (uint32_t) taken;
#endif
	plat_sound_unlock();
	return taken;
}

int32_t plat_pcm_pending(void)
{
	uint32_t pending;

#ifdef U7_DEBUG_LOG
	if (lockstep)
		return lockstep_pcm_pending();
#endif
	if (ring == NULL || in_rate == 0)
		return 0;
	plat_sound_lock();
	pcm_catch_up();
	pending = (uint32_t) (((uint64_t) (pcm_written - pcm_played) * in_rate) / out_rate);
	plat_sound_unlock();
	return (int32_t) pending;
}

void plat_pcm_stop(void)
{
#ifdef U7_DEBUG_LOG
	if (lockstep) {
		lockstep_pcm_stop();
		return;
	}
#endif
	if (ring == NULL)
		return;
	plat_sound_lock();
	pcm_discard();
	in_rate = 0;
	plat_sound_unlock();
}
