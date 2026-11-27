/* The original's AdLib sound effects as DOSBox raw OPL captures (DRO 2.0), for the engine's
 * dataset: each effect of STATIC/ADLIBSFX.DAT played alone by the port's own copy of the
 * original's effects driver (upstream's game/sound/drv/adlib.cpp, linked as it is), its register
 * writes recorded at its 60 Hz tick. adlconvert renders them (engine/tools/mksfx.sh).
 *
 *   u7sfxdro <STATIC dir> <out dir>
 *
 * Writes <out>/sfxNNN.dro and <out>/index.txt (number, ticks played, 1 if still sounding when cut
 * at MOST_TICKS: a sound that goes on until stopped).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "adlib.h"



#define MOST_TICKS (60 * 8)     /* 8 seconds */
#define TAIL_TICKS 30           /* after the voice is freed, for the chip's own release */

/* the driver's hooks into the rest of the game: the music driver's channels (not playing here)
 * and the sound lock (no interrupts here) */
static void no_channel(int16_t) {}
void (*DriverReleaseChannelEntry)(int16_t) = no_channel;
void (*DriverClaimChannelEntry)(int16_t) = no_channel;
extern "C" void plat_sound_lock(void) {}
extern "C" void plat_sound_unlock(void) {}

struct Write {
	uint32_t ms;
	uint8_t reg, value;
};
static std::vector<Write> writes;
static uint32_t now_ms;
static int recording;
static uint8_t latched;
static uint8_t chip[256];       /* what reset left in each register */

extern "C" void plat_opl_write(uint16_t port, uint8_t value)
{
	if ((port & 1) == 0) {
		latched = value;
		return;
	}
	if (recording)
		writes.push_back(Write{ now_ms, latched, value });
	else
		chip[latched] = value;
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t) (p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24);
}

static void put16(FILE *f, unsigned v)
{
	fputc(v & 255, f);
	fputc(v >> 8 & 255, f);
}

static void put32(FILE *f, uint32_t v)
{
	put16(f, v & 0xffff);
	put16(f, v >> 16);
}

/* DRO 2.0: a header, a map of codes to registers, then (code, value) pairs; two codes are
 * delays (n + 1 ms, and (n + 1) * 256 ms). */
static int write_dro(const char *name, uint32_t length_ms)
{
	uint8_t code_of[256], map[128];
	int ncodes = 0;

	memset(code_of, 0xff, sizeof code_of);
	for (const Write &w : writes)
		if (code_of[w.reg] == 0xff) {
			if (ncodes == 126)
				return 0;
			code_of[w.reg] = (uint8_t) ncodes;
			map[ncodes++] = w.reg;
		}
	const uint8_t short_delay = (uint8_t) ncodes, long_delay = (uint8_t) (ncodes + 1);
	std::vector<uint8_t> data;
	uint32_t at = 0;
	for (size_t k = 0; k <= writes.size(); k++) {
		uint32_t to = k < writes.size() ? writes[k].ms : length_ms;
		while (to > at) {
			uint32_t gap = to - at;
			if (gap >= 256) {
				uint32_t n = gap / 256 > 256 ? 256 : gap / 256;
				data.push_back(long_delay);
				data.push_back((uint8_t) (n - 1));
				at += n * 256;
			} else {
				data.push_back(short_delay);
				data.push_back((uint8_t) (gap - 1));
				at += gap;
			}
		}
		if (k < writes.size()) {
			data.push_back(code_of[writes[k].reg]);
			data.push_back(writes[k].value);
		}
	}
	FILE *f = fopen(name, "wb");
	if (f == NULL)
		return 0;
	fwrite("DBRAWOPL", 1, 8, f);
	put16(f, 2);
	put16(f, 0);
	put32(f, (uint32_t) (data.size() / 2));
	put32(f, length_ms);
	fputc(0, f);                /* OPL2 */
	fputc(0, f);                /* interleaved */
	fputc(0, f);                /* not compressed */
	fputc(short_delay, f);
	fputc(long_delay, f);
	fputc(ncodes, f);
	fwrite(map, 1, (size_t) ncodes, f);
	fwrite(data.data(), 1, data.size(), f);
	return fclose(f) == 0;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: u7sfxdro <STATIC dir> <out dir>\n");
		return 2;
	}
	char name[1024];
	snprintf(name, sizeof name, "%s/ADLIBSFX.DAT", argv[1]);
	FILE *f = fopen(name, "rb");
	if (f == NULL) {
		perror(name);
		return 1;
	}
	std::vector<uint8_t> flex;
	int c;
	while ((c = fgetc(f)) != EOF)
		flex.push_back((uint8_t) c);
	fclose(f);
	if (flex.size() < 0x80)
		return 1;
	uint32_t count = rd32(&flex[0x54]), most = 0;
	for (uint32_t i = 0; i < count; i++)
		most = rd32(&flex[0x84 + i * 8]) > most ? rd32(&flex[0x84 + i * 8]) : most;
	/* as the game's buffers: four channels' worth, each the largest effect rounded to 16 */
	most = (most + 15) & ~15u;
	std::vector<uint8_t> area(most * 4);
	SetAdlibSoundArea(area.data(), (int32_t) area.size());

	snprintf(name, sizeof name, "%s/index.txt", argv[2]);
	FILE *index = fopen(name, "w");
	if (index == NULL) {
		perror(name);
		return 1;
	}
	for (uint32_t n = 0; n < count; n++) {
		uint32_t at = rd32(&flex[0x80 + n * 8]), size = rd32(&flex[0x84 + n * 8]);
		writes.clear();
		now_ms = 0;
		/* the chip as the game's reset leaves it: its registers not 0 written first */
		recording = 0;
		memset(chip, 0, sizeof chip);
		ResetAdlib();
		std::fill(area.begin(), area.end(), 0);
		if (size == 0 || at + size > flex.size()) {
			fprintf(index, "%u 0 0\n", n);
			continue;
		}
		memcpy(area.data(), &flex[at], size);
		recording = 1;
		for (int r = 0; r < 256; r++)
			if (chip[r] != 0)
				writes.push_back(Write{ 0, (uint8_t) r, chip[r] });
		int16_t voice = StartAdlibSound(area.data(), 127);
		int ticks = 0, freed = -1;
		for (; ticks < MOST_TICKS; ticks++) {
			now_ms = (uint32_t) (ticks * 1000 / 60);
			TickAdlibSounds();
			if (freed < 0 && (voice == 0 || GetAdlibVoiceState(voice) == 0))
				freed = ticks;
			if (freed >= 0 && ticks >= freed + TAIL_TICKS)
				break;
		}
		int looped = freed < 0;
		snprintf(name, sizeof name, "%s/sfx%03u.dro", argv[2], n);
		if (!write_dro(name, (uint32_t) (ticks * 1000 / 60))) {
			fprintf(stderr, "%s: not written\n", name);
			return 1;
		}
		fprintf(index, "%u %d %d\n", n, freed < 0 ? ticks : freed, looped);
	}
	fclose(index);
	return 0;
}
