/* The original's AdLib music as DOSBox raw OPL captures (DRO 2.0), for the engine's dataset: each
 * song of STATIC/ADLIBMUS.DAT (standard MIDI files) played by the port's own copy of the
 * original's song player and AdLib music driver (upstream's game/sound/drv/midiplay.cpp and
 * adlibdrv.cpp, linked as they are), with its timbres (STATIC/U7VOICE.FLX, record 1) and the
 * game's volume for the song, its register writes recorded at the 60 Hz sound tick. A song that
 * loops (the player's loop-start and loop-end controllers) is recorded to its first jump back;
 * where its loop starts is the tick the player first marks it.
 *
 *   u7musdro <STATIC dir> <out dir>
 *
 * Writes <out>/musNN.dro and <out>/index.txt: the song, its length (ms; for one that loops, to
 * the jump back), where its loop starts and ends (ms; 0 0 when it ends instead).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "midiplay.h"
#include "adlibdrv.h"

#define MOST_TICKS (60 * 60 * 8)        /* 8 minutes */
#define TAIL_TICKS 60                   /* after a song that ends, for the chip's own release */

typedef void (*Routine)();
extern SoundDriver *(*DriverDescribeEntry)(void);
extern int16_t (*DriverInitEntry)(void *);
extern void (*DriverShutdownEntry)(void);
extern Routine DriverTimerEntry;
extern void (*DriverNoteEntry)(int16_t, int16_t, int16_t);
extern void (*DriverControlEntry)(int16_t, int16_t, int16_t);
extern void (*DriverPitchEntry)(int16_t, int16_t, int16_t);
extern void (*DriverProgramEntry)(int16_t, int16_t);
extern void (*DriverPortEntry)(int16_t);
extern void (*DriverSysexEntry)(int32_t, int16_t, void *);
extern void (*DriverReleaseChannelEntry)(int16_t);
extern void (*DriverClaimChannelEntry)(int16_t);
extern int16_t MusicFlags, SongStopped;
#define TRACKS_SEEN 32                  /* (the player's MAX_TRACKS, at most) */
extern uint8_t *TrackPos[], *LoopTrackPos[];
void MusicTimerHandler(void);
extern "C" void ResetMidiplayGlobals(void);
extern "C" void ResetAdlibdrvGlobals(void);

/* the game's volume for each AdLib song (upstream's u7sound.cpp AdlibMusicVolumes: game data) */
static const uint8_t song_volume[60] = {
	180, 180, 180, 180, 255, 255, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180,
	180, 210, 200, 120, 120, 120, 220, 170, 56, 180, 180, 180, 200, 220, 200, 180,
	180, 180, 180, 180, 200, 200, 180, 180, 170, 195, 210, 210, 210, 210, 210, 210,
	210, 210, 210, 180, 255, 180, 180, 190, 190, 190, 190, 210
};

struct Write {
	uint32_t ms;
	uint8_t reg, value;
};
static std::vector<Write> writes;
static uint32_t now_ms;
static uint8_t latched;

extern "C" void plat_opl_write(uint16_t port, uint8_t value)
{
	if ((port & 1) == 0)
		latched = value;
	else
		writes.push_back(Write{ now_ms, latched, value });
}

extern "C" void plat_sound_lock(void) {}
extern "C" void plat_sound_unlock(void) {}

/* what the player links but this tool never reaches (the songs and timbres are read here; the
 * tick is run here; no MT-32): stubs */
#include "flex.h"
#include "memapi.h"
#include "mt32drv.h"
#include "plat.h"
void *AllocateFarHeap(int32_t size, int16_t) { return malloc((size_t) size); }
void FreeFarHeap(void *memory) { free(memory); }
void FlexHeader::clear() {}
uint8_t Flex::open(char *) { return 0; }
void Flex::close() {}
uint8_t Flex::getEntry(int16_t, FlexEntry *) { return 0; }
uint8_t Flex::readEntry(FlexEntry *, void *, uint8_t) { return 0; }
uint8_t Flex::readEntryToVoodoo(FlexEntry *, int32_t, uint8_t) { return 0; }
uint8_t Flex::readRecord(int16_t, void *, uint8_t) { return 0; }
void plat_yield(void) {}
void plat_sound_tick_set(plat_timer_fn) {}
SoundDriver *Mt32Describe(void) { return 0; }
int16_t Mt32Init(void *) { return 0; }
void Mt32Shutdown(void) {}
void Mt32Timer(void) {}
void Mt32Note(int16_t, int16_t, int16_t) {}
void Mt32Control(int16_t, int16_t, int16_t) {}
void Mt32Pitch(int16_t, int16_t, int16_t) {}
void Mt32Program(int16_t, int16_t) {}
void Mt32Port(int16_t) {}
void Mt32Sysex(int32_t, int16_t, void *) {}

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

/* DRO 2.0 (as sfxdro.cpp writes it). */
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
	fputc(0, f);
	fputc(0, f);
	fputc(0, f);
	fputc(short_delay, f);
	fputc(long_delay, f);
	fputc(ncodes, f);
	fwrite(map, 1, (size_t) ncodes, f);
	fwrite(data.data(), 1, data.size(), f);
	return fclose(f) == 0;
}

static std::vector<uint8_t> read_all(const char *dir, const char *file)
{
	char name[1024];
	std::vector<uint8_t> v;

	snprintf(name, sizeof name, "%s/%s", dir, file);
	FILE *f = fopen(name, "rb");
	if (f == NULL) {
		perror(name);
		exit(1);
	}
	int c;
	while ((c = fgetc(f)) != EOF)
		v.push_back((uint8_t) c);
	fclose(f);
	return v;
}

static void use_adlib(void)
{
	MusicDevice = MUSIC_DEVICE_ADLIB;
	DriverDescribeEntry = AdlibDescribe;
	DriverInitEntry = AdlibInit;
	DriverShutdownEntry = AdlibShutdown;
	DriverTimerEntry = AdlibTimer;
	DriverNoteEntry = AdlibNote;
	DriverControlEntry = AdlibControl;
	DriverPitchEntry = AdlibPitch;
	DriverProgramEntry = AdlibProgram;
	DriverPortEntry = AdlibPort;
	DriverSysexEntry = AdlibSysex;
	DriverReleaseChannelEntry = AdlibReleaseVoice;
	DriverClaimChannelEntry = AdlibClaimVoice;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: u7musdro <STATIC dir> <out dir>\n");
		return 2;
	}
	std::vector<uint8_t> songs = read_all(argv[1], "ADLIBMUS.DAT"), voices = read_all(argv[1], "U7VOICE.FLX");
	if (songs.size() < 0x80 || voices.size() < 0x90)
		return 1;
	std::vector<uint8_t> timbres(voices.begin() + rd32(&voices[0x88]),
		voices.begin() + rd32(&voices[0x88]) + rd32(&voices[0x8c]));
	char name[1024];
	snprintf(name, sizeof name, "%s/index.txt", argv[2]);
	FILE *index = fopen(name, "w");
	if (index == NULL) {
		perror(name);
		return 1;
	}
	uint32_t count = rd32(&songs[0x54]);
	for (uint32_t n = 0; n < count; n++) {
		uint32_t at = rd32(&songs[0x80 + n * 8]), size = rd32(&songs[0x84 + n * 8]);
		if (size == 0 || at + size > songs.size())
			continue;
		std::vector<uint8_t> song(songs.begin() + at, songs.begin() + at + size);
		std::vector<uint8_t> bank(timbres);
		/* the driver and player begun again for each song: the chip as the game leaves it */
		writes.clear();
		now_ms = 0;
		ResetMidiplayGlobals();
		ResetAdlibdrvGlobals();         /* (its data as the original's image: the F-numbers) */
		use_adlib();
		SoundDriverInfo = AdlibDescribe();
		if (AdlibInit(bank.data()) == 0) {
			fprintf(stderr, "song %u: the driver did not start\n", n);
			return 1;
		}
		SetMusicVolume(n < 60 ? song_volume[n] : 180);
		if (!PlaySong(song.data())) {
			fprintf(stderr, "song %u: not played\n", n);
			continue;
		}
		MusicFlags = 0;
		/* its loop (the player's loop controllers): where it starts, the first time the saved
		 * places change; where it ends, the first time a track's place goes back */
		uint32_t jump[2] = { 0, 0 };
		int jumps = 0, ticks = 0, ended = -1, tracks = TrackCount < TRACKS_SEEN ? TrackCount : TRACKS_SEEN;
		uint8_t *was[TRACKS_SEEN], *loop_was[TRACKS_SEEN];
		for (int t = 0; t < tracks; t++) {
			was[t] = TrackPos[t];
			loop_was[t] = LoopTrackPos[t];
		}
		for (; ticks < MOST_TICKS; ticks++) {
			now_ms = (uint32_t) ((uint64_t) ticks * 1000 / 60);
			AdlibTimer();
			MusicTimerHandler();
			int back = 0, marked = 0;
			for (int t = 0; t < tracks; t++) {
				back |= TrackPos[t] < was[t];
				marked |= LoopTrackPos[t] != loop_was[t];
				was[t] = TrackPos[t];
				loop_was[t] = LoopTrackPos[t];
			}
			if (marked && jumps == 0)
				jump[jumps++] = now_ms;
			if (ended < 0 && SongStopped)
				ended = ticks;
			else if (back && jumps == 1) {
				jump[jumps++] = now_ms;
				break;
			}
			if (ended >= 0 && ticks >= ended + TAIL_TICKS)
				break;
		}
		AdlibShutdown();
		uint32_t length = (uint32_t) ((uint64_t) ticks * 1000 / 60);
		snprintf(name, sizeof name, "%s/mus%02u.dro", argv[2], n);
		if (!write_dro(name, length)) {
			fprintf(stderr, "%s: not written\n", name);
			return 1;
		}
		fprintf(index, "%u %u %u %u\n", n, length, jumps == 2 ? jump[0] : 0, jumps == 2 ? jump[1] : 0);
		printf("song %2u: %6.1f s%s, %zu writes\n", n, length / 1000.0,
			jumps == 2 ? " (loops)" : ended >= 0 ? " (ends)" : " (cut)", writes.size());
	}
	fclose(index);
	return 0;
}
