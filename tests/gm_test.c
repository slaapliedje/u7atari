/* Checks the General MIDI translation. Runs on the build host:
 *   cc -O2 -Iplatform/atari tests/gm_test.c platform/atari/gm.c -o gm_test && ./gm_test
 */
#include <stdio.h>
#include <string.h>

#include "gm.h"

static uint8_t sent[4096];
static int sent_length;
static int failures;

static void capture(const uint8_t *message, int16_t length)
{
	memcpy(sent + sent_length, message, (size_t) length);
	sent_length += length;
}

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("%s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

/* Feeds bytes and checks exactly these came out. */
static void expect(const uint8_t *in, int in_length, const uint8_t *want, int want_length,
	int line)
{
	sent_length = 0;
	gm_feed(in, (int16_t) in_length, capture);
	if (sent_length != want_length || memcmp(sent, want, (size_t) want_length) != 0) {
		printf("%s:%d: sent", __FILE__, line);
		for (int i = 0; i < sent_length; i++)
			printf(" %02x", sent[i]);
		printf(", want");
		for (int i = 0; i < want_length; i++)
			printf(" %02x", want[i]);
		printf("\n");
		failures++;
	}
}

#define EXPECT(in, ...) do { \
	static const uint8_t in_[] = in; \
	static const uint8_t want_[] = { __VA_ARGS__ }; \
	expect(in_, sizeof in_, want_, sizeof want_, __LINE__); \
} while (0)
#define EXPECT_NOTHING(in) do { \
	static const uint8_t in_[] = in; \
	expect(in_, sizeof in_, NULL, 0, __LINE__); \
} while (0)
#define B(...) { __VA_ARGS__ }

/* A Roland data set to the MT-32, as the game's driver builds it. */
static int roland(uint8_t *m, uint32_t address, const uint8_t *data, int length)
{
	int n = 0, sum = 0;

	m[n++] = 0xf0;
	m[n++] = 0x41;
	m[n++] = 0x10;
	m[n++] = 0x16;
	m[n++] = 0x12;
	m[n++] = (uint8_t) (address >> 16);
	m[n++] = (uint8_t) (address >> 8);
	m[n++] = (uint8_t) address;
	for (int i = 0; i < length; i++)
		m[n++] = data[i];
	for (int i = 5; i < n; i++)
		sum += m[i];
	m[n++] = (uint8_t) (-sum & 0x7f);
	m[n++] = 0xf7;
	return n;
}

static void load_timbre(int slot, const char *name)
{
	uint8_t timbre[246] = { 0 }, m[300];
	int n;

	memset(timbre, ' ', 10);
	memcpy(timbre, name, strlen(name));
	n = roland(m, 0x080000 + ((uint32_t) slot << 9), timbre, sizeof timbre);
	expect(m, n, NULL, 0, __LINE__);
}

static void set_patch(int program, int slot, int key_shift, int bender_range)
{
	const uint8_t patch[8] = { 2, (uint8_t) slot, (uint8_t) key_shift, 50,
		(uint8_t) bender_range, 0, 1, 0 };
	uint8_t m[32];
	int n = roland(m, 0x050000 + ((uint32_t) (program >> 4) << 8) + ((program & 15) << 3),
		patch, 8);

	expect(m, n, NULL, 0, __LINE__);
}

int main(void)
{
	uint8_t m[300];
	int n;

	sent_length = 0;
	gm_reset(capture);
	CHECK(sent_length == 6 && memcmp(sent, "\xf0\x7e\x7f\x09\x01\xf7", 6) == 0);

	/* A preset patch: MT-32 Flute 1 (program 72) is GM Flute (73), with the MT-32's bender
	 * range of 12 semitones. Pitch bend passes. */
	EXPECT(B(0xc2, 72, 0x92, 60, 100, 0xe2, 0, 0x50, 0x82, 60, 0),
		0xc2, 73, 0xb2, 0x65, 0, 0xb2, 0x64, 0, 0xb2, 0x06, 12, 0xb2, 0x26, 0,
		0xb2, 0x65, 0x7f, 0xb2, 0x64, 0x7f, 0x92, 60, 100, 0xe2, 0, 0x50, 0x82, 60, 0);

	/* A loaded timbre by name, with the game's patch (bender range 24, no key shift). */
	load_timbre(0, "Gong");
	set_patch(4, 0, 24, 24);
	EXPECT(B(0xc1, 4),
		0xc1, 14, 0xb1, 0x65, 0, 0xb1, 0x64, 0, 0xb1, 0x06, 24, 0xb1, 0x26, 0,
		0xb1, 0x65, 0x7f, 0xb1, 0x64, 0x7f);
	/* the same bender range again is not resent */
	EXPECT(B(0xc1, 4), 0xc1, 14);

	/* A key shift of an octave up; the note off finds the note it started, after a program
	 * change and through running status. */
	set_patch(5, 0, 36, 24);
	EXPECT(B(0xc1, 5, 0x91, 60, 90), 0xc1, 14, 0x91, 72, 90);
	EXPECT(B(0xc1, 4, 0x91, 60, 0, 61, 80), 0xc1, 14, 0x91, 72, 0, 0x91, 61, 80);
	EXPECT(B(0x81, 61, 0), 0x81, 61, 0);

	/* An unknown timbre (a sound effect) is silent, start and stop. */
	load_timbre(1, "Footstep L");
	set_patch(6, 1, 24, 24);
	EXPECT_NOTHING(B(0xc3, 6, 0x93, 50, 100, 0x93, 50, 0));
	/* a timbre loaded over it is looked up again */
	load_timbre(1, "Churchy");
	EXPECT(B(0xc3, 6), 0xc3, 19, 0xb3, 0x65, 0, 0xb3, 0x64, 0, 0xb3, 0x06, 24, 0xb3, 0x26, 0,
		0xb3, 0x65, 0x7f, 0xb3, 0x64, 0x7f);

	/* Channels the MT-32 does not listen to are dropped; program changes on the rhythm
	 * channel too. */
	EXPECT_NOTHING(B(0x90, 60, 100, 0xbb, 7, 100, 0xcf, 3, 0xc9, 5));

	/* Drum keys given loaded timbres: Gong is a Chinese cymbal, an unknown one silent;
	 * keys left alone pass. The rhythm setup write is split into pieces, as an XMIDI sysex
	 * event sends it. */
	{
		const uint8_t setup[] = { 0, 0x5a, 7, 0, 1, 0x5a, 7, 0 };   /* keys 28, 29 */

		n = roland(m, 0x030120, setup, sizeof setup);
		expect(m, 1, NULL, 0, __LINE__);
		expect(m + 1, n - 1, NULL, 0, __LINE__);
	}
	EXPECT(B(0x99, 28, 100, 0x99, 29, 100, 0x99, 36, 100, 0x89, 28, 0, 0x89, 29, 0),
		0x99, 52, 100, 0x99, 36, 100, 0x89, 52, 0);

	/* Master volume 70 of 100; reverb and display text are dropped. */
	n = roland(m, 0x100016, (const uint8_t[]) { 70 }, 1);
	expect(m, n, (const uint8_t[]) { 0xf0, 0x7f, 0x7f, 0x04, 0x01, 0, 88, 0xf7 }, 8, __LINE__);
	n = roland(m, 0x100001, (const uint8_t[]) { 1, 3, 3 }, 3);
	expect(m, n, NULL, 0, __LINE__);
	n = roland(m, 0x200000, (const uint8_t *) "Origin Sound System!", 20);
	expect(m, n, NULL, 0, __LINE__);

	/* An MT-32 reset is a GM reset, and forgets the loaded timbres. */
	n = roland(m, 0x7f0000, (const uint8_t[]) { 0 }, 1);
	expect(m, n, (const uint8_t[]) { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 }, 6, __LINE__);
	/* the preset again, Elec Piano 2, and the GM default bender range again */
	EXPECT(B(0xc1, 4), 0xc1, 5, 0xb1, 0x65, 0, 0xb1, 0x64, 0, 0xb1, 0x06, 12, 0xb1, 0x26, 0,
		0xb1, 0x65, 0x7f, 0xb1, 0x64, 0x7f);

	printf("%s\n", failures ? "FAILED" : "ok");
	return failures != 0;
}
