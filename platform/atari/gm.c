/* General MIDI output: the game's MT-32 MIDI, translated for a GM synth (--gm).
 *
 * The game writes its own instruments into the MT-32's timbre memory, points patches (program
 * numbers) at them, and assigns some of them to drum keys. A GM synth has none of that, so this
 * keeps a model of what the game wrote and turns each program change into the GM program
 * nearest to what the MT-32 would play:
 *   - a patch on one of the MT-32's 128 preset tones goes to the GM program nearest that tone;
 *   - a patch on a timbre the game loaded goes by the timbre's name (the first 10 bytes of its
 *     data) through a table of the instruments Ultima VII loads; a name not in it, which is most
 *     sound effects, is muted rather than played on the wrong instrument;
 *   - a drum key the game gave one of its own timbres plays the GM drum nearest it, or nothing.
 * The patch's key shift transposes the notes, and its bender range is sent as the channel's
 * pitch bend sensitivity (RPN 0). The master volume becomes GM master volume. Every other
 * MT-32 sysex (reverb, display text, part setup) is dropped.
 *
 * Only MIDI channels 2 to 10 pass, as on an MT-32 (parts 1 to 8 and the rhythm part); a GM
 * synth would play the others.
 */
#include <string.h>

#include "gm.h"

#define SYSEX_MAX 300           /* a Roland data set carries 256 bytes at most */
#define FIRST_RHYTHM_KEY 24     /* the MT-32's rhythm setup covers keys 24 to 87 */
#define RHYTHM_KEYS 64
#define RHYTHM_CHANNEL 9
#define NOT_SOUNDING 0xff
#define DEFAULT_RHYTHM 0xff     /* a key with the MT-32's own drum */

/* The GM program nearest each MT-32 preset tone (patch groups A then B). */
static const uint8_t PresetPrograms[128] = {
	0, 1, 0, 4, 5, 4, 5, 3,             /* pianos, honky-tonk */
	16, 17, 18, 18, 19, 19, 20, 21,     /* organs, accordion */
	6, 6, 6, 7, 7, 7, 8, 8,             /* harpsichords, clavis, celestas */
	62, 63, 62, 63, 38, 39, 38, 39,     /* synth brass, synth bass */
	88, 89, 52, 92, 97, 99, 98, 85,     /* Fantasy .. Funny Vox */
	102, 96, 68, 102, 86, 10, 91, 80,   /* Echo Bell .. Square Wave */
	48, 48, 49, 45, 40, 40, 42, 42,     /* strings, pizzicato, violins, cellos */
	43, 46, 46, 24, 25, 26, 27, 104,    /* contrabass, harps, guitars, sitar */
	32, 32, 33, 34, 36, 37, 35, 35,     /* basses */
	73, 73, 72, 72, 74, 75, 64, 65,     /* flutes, piccolos, recorder, pan pipes, saxes */
	66, 67, 71, 71, 68, 69, 70, 22,     /* saxes, clarinets, oboe, English horn, bassoon, harmonica */
	56, 56, 57, 57, 60, 60, 58, 61,     /* trumpets, trombones, French horns, tuba, brass */
	61, 11, 11, 12, 112, 9, 14, 13,     /* brass, vibes, Syn Mallet, Windbell, glock, tube bell, xylophone */
	12, 107, 111, 77, 78, 78, 76, 75,   /* marimba, koto, sho, shakuhachi, whistles, bottle, Breathpipe */
	47, 117, 118, 118, 118, 116, 115, 119, /* timpani, melodic tom, snare, electric percussion, taiko, cymbal */
	115, 112, 55, 124, 123, 81, 98, 115, /* castanets, triangle, orchestra hit, telephone, bird .. */
};

/* The instruments Ultima VII loads into the MT-32 (in-game, intro and endgame music, and the
 * few sound effects with a fair GM match), by name: the GM program for a channel, and the GM
 * drum for a drum key (0: silent there). */
static const struct {
	char name[11];
	uint8_t program, drum;
} Named[] = {
	{ "Gong", 14, 52 }, { "Analgong", 14, 52 }, { "Afrika", 116, 64 }, { "Afrika2", 116, 64 },
	{ "Taikocym", 116, 49 }, { "Revophat", 119, 46 }, { "Lowcrash", 119, 49 },
	{ "Windbells", 112, 81 }, { "Toybell", 10, 81 }, { "Pushy", 55, 57 },
	{ "Elsnare", 118, 40 }, { "Milsnare", 118, 38 }, { "Wood 2", 115, 76 },
	{ "Del Clave", 115, 75 },
	{ "Stringflu", 49, 0 }, { "Stringflu2", 49, 0 }, { "String2", 48, 0 }, { "String3", 49, 0 },
	{ "PhilhStr1", 48, 0 }, { "PhilhStr8", 48, 0 }, { "Pizzaf", 45, 0 },
	{ "Dana's", 89, 0 }, { "Sigmunds", 89, 0 }, { "Churchy", 19, 0 }, { "Matrix", 90, 0 },
	{ "Matrix2", 90, 0 }, { "Fairlight3", 54, 0 }, { "Alah", 52, 0 }, { "Chodepth", 52, 0 },
	{ "MMMMMMMMMM", 53, 0 }, { "Bussy", 61, 0 }, { "Major bud", 61, 0 }, { "Trumpets", 56, 0 },
	{ "Strike!!!", 55, 0 }, { "Solo", 81, 0 }, { "Acid", 38, 0 }, { "Growly", 39, 0 },
	{ "Pickbass", 34, 0 }, { "Whistle 1", 78, 0 }, { "Del Bottle", 76, 0 },
	{ "Del Blow", 76, 0 },
	{ "Rainman", 96, 0 }, { "Drops", 96, 0 }, { "Wind", 122, 0 }, { "Snow Wind", 122, 0 },
	{ "Surf's up", 122, 0 }, { "Sea wave", 122, 0 }, { "Canari", 123, 0 },
	{ "Canari 2", 123, 0 }, { "Seaguls", 123, 0 },
	{ "Bell Ding", 14, 0 }, { "Bell Dong", 14, 0 }, { "Chime", 14, 0 },
	{ "Clock tick", 115, 76 }, { "Clock tock", 115, 77 },
};

#define MUTED 0xff

/* What the game wrote to the MT-32. */
static struct {
	uint8_t group;              /* 0, 1 preset tones; 2 timbre memory; 3 rhythm */
	uint8_t timbre;
	uint8_t key_shift;          /* 24 is none */
	uint8_t bender_range;       /* semitones */
} patches[128];
static char timbre_names[64][10];
static uint8_t timbre_program[64], timbre_drum[64];  /* from the names; MUTED or 0 when unknown */
static uint64_t names_changed;
static uint8_t rhythm_timbre[RHYTHM_KEYS];

/* What has been sent to the GM synth. */
static struct {
	uint8_t muted;
	int8_t shift;
	uint8_t bend_range;
	uint8_t sounding[128];      /* the GM note each of the game's notes went out as */
} channels[16];

/* The byte stream being taken apart. */
static uint8_t status, data[2], data_count;
static uint8_t sysex[SYSEX_MAX];
static int16_t sysex_length;    /* -1: not in a sysex */

static int16_t data_bytes(uint8_t s)
{
	switch (s & 0xf0) {
	case 0xc0:
	case 0xd0:
		return 1;
	default:
		return 2;
	}
}

static void clear_notes(int ch)
{
	memset(channels[ch].sounding, NOT_SOUNDING, sizeof channels[ch].sounding);
}

static void forget(void)
{
	for (int p = 0; p < 128; p++) {
		patches[p].group = (uint8_t) (p >> 6);
		patches[p].timbre = (uint8_t) (p & 63);
		patches[p].key_shift = 24;
		patches[p].bender_range = 12;
	}
	memset(timbre_names, ' ', sizeof timbre_names);
	memset(timbre_program, MUTED, sizeof timbre_program);
	memset(timbre_drum, 0, sizeof timbre_drum);
	names_changed = 0;
	memset(rhythm_timbre, DEFAULT_RHYTHM, sizeof rhythm_timbre);
	for (int ch = 0; ch < 16; ch++) {
		channels[ch].muted = 0;
		channels[ch].shift = 0;
		channels[ch].bend_range = 2;    /* the GM default */
		clear_notes(ch);
	}
}

void gm_reset(gm_output out)
{
	static const uint8_t gm_on[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };

	forget();
	status = 0;
	data_count = 0;
	sysex_length = -1;
	out(gm_on, sizeof gm_on);
}

static void look_up_names(void)
{
	for (int t = 0; t < 64; t++) {
		int length = 10;

		if (!(names_changed >> t & 1))
			continue;
		while (length > 0 && timbre_names[t][length - 1] == ' ')
			length--;
		timbre_program[t] = MUTED;
		timbre_drum[t] = 0;
		for (size_t i = 0; i < sizeof Named / sizeof Named[0]; i++) {
			if (strlen(Named[i].name) == (size_t) length
					&& memcmp(Named[i].name, timbre_names[t], (size_t) length) == 0) {
				timbre_program[t] = Named[i].program;
				timbre_drum[t] = Named[i].drum;
				break;
			}
		}
	}
	names_changed = 0;
}

/* One byte of a Roland data set, at its address with the 7-bit fields run together. */
static void write_memory(uint32_t address, uint8_t value, gm_output out)
{
	uint32_t offset = address & 0x3fff;

	switch (address >> 14) {
	case 0x03:                          /* rhythm setup: timbre, level, pan, reverb per key */
		if (offset >= 0x90 && offset < 0x90 + RHYTHM_KEYS * 4 && (offset & 3) == 0)
			rhythm_timbre[(offset - 0x90) >> 2] = value;
		break;
	case 0x05:                          /* patch memory: 8 bytes a patch */
		if (offset < 128 * 8) {
			int p = (int) (offset >> 3);

			switch (offset & 7) {
			case 0: patches[p].group = value & 3; break;
			case 1: patches[p].timbre = value & 63; break;
			case 2: patches[p].key_shift = value; break;
			case 4: patches[p].bender_range = value; break;
			}
		}
		break;
	case 0x08:                          /* timbre memory: 256 bytes a timbre, the name first */
		if ((offset & 0xff) < 10 && (offset >> 8) < 64) {
			timbre_names[offset >> 8][offset & 0xff] = (char) value;
			names_changed |= UINT64_C(1) << (offset >> 8);
		}
		break;
	case 0x10:                          /* system: master volume 0 to 100 */
		if (offset == 0x16) {
			uint8_t volume[] = { 0xf0, 0x7f, 0x7f, 0x04, 0x01, 0, 0, 0xf7 };

			volume[6] = (uint8_t) ((value > 100 ? 100 : value) * 127 / 100);
			out(volume, sizeof volume);
		}
		break;
	case 0x7f:                          /* reset */
		gm_reset(out);
		break;
	}
}

static void roland_sysex(const uint8_t *s, int16_t length, gm_output out)
{
	uint32_t address;

	/* 41 device 16 12 address(3) data.. checksum */
	if (length < 9 || s[0] != 0x41 || s[2] != 0x16 || s[3] != 0x12)
		return;
	address = (uint32_t) s[4] << 14 | (uint32_t) s[5] << 7 | s[6];
	for (int16_t i = 7; i < length - 1; i++)
		write_memory(address++, s[i], out);
}

static void send_bend_range(int ch, uint8_t range, gm_output out)
{
	const uint8_t b = (uint8_t) (0xb0 | ch);
	const uint8_t rpn[18] = {
		b, 0x65, 0, b, 0x64, 0,             /* RPN 0, pitch bend sensitivity */
		b, 0x06, range, b, 0x26, 0,
		b, 0x65, 0x7f, b, 0x64, 0x7f,       /* RPN null */
	};

	for (int i = 0; i < 18; i += 3)
		out(rpn + i, 3);
	channels[ch].bend_range = range;
}

static void program_change(int ch, uint8_t program, gm_output out)
{
	uint8_t gm = MUTED;
	uint8_t message[2];

	if (ch == RHYTHM_CHANNEL)
		return;
	switch (patches[program].group) {
	case 0:
	case 1:
		gm = PresetPrograms[patches[program].group << 6 | patches[program].timbre];
		break;
	case 2:
		look_up_names();
		gm = timbre_program[patches[program].timbre];
		break;
	}
	channels[ch].muted = gm == MUTED;
	if (gm == MUTED)
		return;
	message[0] = (uint8_t) (0xc0 | ch);
	message[1] = gm;
	out(message, 2);
	channels[ch].shift = (int8_t) (patches[program].key_shift - 24);
	if (patches[program].bender_range != channels[ch].bend_range)
		send_bend_range(ch, patches[program].bender_range, out);
}

/* The GM note for a note the game starts, or -1 for none. */
static int gm_note(int ch, uint8_t note)
{
	int n;

	if (ch == RHYTHM_CHANNEL) {
		uint8_t t;

		if (note < FIRST_RHYTHM_KEY || note >= FIRST_RHYTHM_KEY + RHYTHM_KEYS)
			return note;
		t = rhythm_timbre[note - FIRST_RHYTHM_KEY];
		if (t >= 64)                    /* its own drum */
			return note;
		look_up_names();
		return timbre_drum[t] != 0 ? timbre_drum[t] : -1;
	}
	if (channels[ch].muted)
		return -1;
	n = note + channels[ch].shift;
	return n >= 0 && n <= 127 ? n : -1;
}

static void channel_message(gm_output out)
{
	const int ch = status & 0x0f;
	uint8_t message[3] = { status, data[0], data[1] };
	uint8_t *sounding;

	if (ch < 1 || ch > 9)
		return;
	sounding = &channels[ch].sounding[data[0]];
	switch (status & 0xf0) {
	case 0x90:
		if (data[1] != 0) {
			int n = gm_note(ch, data[0]);

			if (*sounding != NOT_SOUNDING) {        /* the same note again: stop the old one */
				const uint8_t off[3] = { message[0], *sounding, 0 };

				out(off, 3);
				*sounding = NOT_SOUNDING;
			}
			if (n < 0)
				return;
			*sounding = (uint8_t) n;
			message[1] = (uint8_t) n;
			out(message, 3);
			return;
		}
		/* velocity 0 is a note off */
		/* fall through */
	case 0x80:
		if (*sounding == NOT_SOUNDING)
			return;
		message[1] = *sounding;
		*sounding = NOT_SOUNDING;
		out(message, 3);
		return;
	case 0xb0:
		if (data[0] == 0x7b || data[0] == 0x78 || data[0] >= 0x7c)    /* notes off, sound off */
			clear_notes(ch);
		out(message, 3);
		return;
	case 0xc0:
		program_change(ch, data[0], out);
		return;
	case 0xe0:
		out(message, 3);
		return;
	}
	/* aftertouch: the MT-32 takes none */
}

void gm_feed(const uint8_t *bytes, int16_t length, gm_output out)
{
	for (int16_t i = 0; i < length; i++) {
		const uint8_t b = bytes[i];

		if (b >= 0xf8)                  /* real time */
			continue;
		if (b == 0xf0) {
			sysex_length = 0;
			status = 0;
			continue;
		}
		if (b & 0x80) {
			if (b == 0xf7 && sysex_length >= 0)
				roland_sysex(sysex, sysex_length, out);
			sysex_length = -1;
			status = b < 0xf0 ? b : 0;
			data_count = 0;
			continue;
		}
		if (sysex_length >= 0) {
			if (sysex_length < SYSEX_MAX)
				sysex[sysex_length++] = b;
			continue;
		}
		if (status == 0)
			continue;
		data[data_count++] = b;
		if (data_count == data_bytes(status)) {
			channel_message(out);
			data_count = 0;
		}
	}
}
