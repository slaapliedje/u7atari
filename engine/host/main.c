/* The engine test on a PC: draws views of the world to pictures (PPM), for checking the core.
 *
 *   u7engine <folder with STATIC and GAMEDAT>/ <cell x> <cell y> [options]
 *     --walk dx dy n   move the view dx, dy cells a frame for n frames (default: one frame)
 *     --movers n       n NPCs wandering about the view
 *     --out prefix     pictures prefix.NNNN.ppm (default view); --every k: every k-th frame
 *     --layers         the movers on a layer of their own over the ring, as the Jaguar shows
 *                      them (the picture put together from both for saving)
 *     --avatar         the NPCs of NPC.DAT (the nearest 31 to the avatar) and the avatar, who
 *                      walks (--walk moves it, not the view; x y are ignored), the view on it,
 *                      roofs over it lifted, walls in its way, up and down stairs, shut doors
 *                      it walks into opened
 *     --path "dx,dy,n;..." with --avatar: legs of a walk, n cells each
 *     --from x,y[,z]   with --avatar: start the avatar there
 *     --time m         with --avatar: the time to start at, minutes from midnight (default 360)
 *     --route x,y[,z]  with --avatar: the avatar walks to x, y, z on a route, as an NPC walks one
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "engine.h"

/* ---- files ---- */

static FILE *files[16];

int16_t plat_file_open(const char *name, int16_t mode)
{
	for (int16_t i = 0; i < 16; i++)
		if (files[i] == NULL) {
			if ((files[i] = fopen(name, mode == PLAT_WRITE ? "wb" : "rb")) == NULL)
				return -1;
			if (getenv("U7_TRACE_FILES")) {
				static int opens;
				fprintf(stderr, "open %d: %s (%d)\n", ++opens, name, i);
			}
			return i;
		}
	return -1;
}

int32_t plat_file_write(int16_t f, const void *buffer, int32_t count)
{
	return (int32_t) fwrite(buffer, 1, (size_t) count, files[f]);
}

void plat_file_close(int16_t f)
{
	if (f >= 0 && f < 16 && files[f] != NULL) {
		fclose(files[f]);
		files[f] = NULL;
	}
}

int32_t plat_file_read(int16_t f, void *buffer, int32_t count)
{
	if (getenv("U7_TRACE_FILES")) {
		static int reads;
		fprintf(stderr, "read %d: file %d, %ld bytes at %ld, into %p\n", ++reads, f, (long) count,
			ftell(files[f]), buffer);
	}
	return (int32_t) fread(buffer, 1, (size_t) count, files[f]);
}

int32_t plat_file_seek(int16_t f, int32_t offset, int16_t origin)
{
	(void) origin;
	return fseek(files[f], offset, SEEK_SET) == 0 ? offset : -1;
}

int32_t plat_file_length(int16_t f)
{
	long at = ftell(files[f]), n;

	fseek(files[f], 0, SEEK_END);
	n = ftell(files[f]);
	fseek(files[f], at, SEEK_SET);
	return (int32_t) n;
}

/* ---- drawing ---- */

static uint8_t screen[E_VIEW_H][E_VIEW_W];
static uint8_t screen_rows[E_VIEW_H * 4];
static struct ESurface frame = { &screen[0][0], E_VIEW_W, E_VIEW_W, E_VIEW_H, screen_rows };

void eb_copy(struct ESurface *dst, int16_t dx, int16_t dy, const struct ESurface *src, int16_t sx,
	int16_t sy, int16_t w, int16_t h)
{
	for (int16_t r = 0; r < h; r++)
		memcpy(dst->pixels + (dy + r) * dst->pitch + dx, src->pixels + (sy + r) * src->pitch + sx,
			(size_t) w);
}

/* --layers: the ring is not copied; the picture is the ring's window with the layer over it */
static int layers;
static const struct ESurface *shown_ring;
static int16_t shown_x, shown_y;

int eb_view(struct ESurface *dst, const struct ESurface *ring, int16_t sx, int16_t sy)
{
	if (!layers) {
		e_copy_view(dst, ring, sx, sy);
		return 1;
	}
	shown_ring = ring;
	shown_x = sx;
	shown_y = sy;
	return 0;
}

int eb_movers(struct ESurface *frame, const struct EMoverDraw *list, int n, int16_t vx,
	int16_t vy, const void *const *shapes)
{
	(void) frame; (void) list; (void) n; (void) vx; (void) vy; (void) shapes;
	return 0;
}

void eb_clear(struct ESurface *dst, int16_t x, int16_t y, int16_t w, int16_t h)
{
	for (int16_t r = 0; r < h; r++)
		memset(dst->pixels + (y + r) * dst->pitch + x, 0, (size_t) w);
}

void eb_tiles(struct ESurface *dst, const struct EFlat flats[E_FLAT_TYPES],
	const struct ETileChunk *chunks, int n)
{
	(void) dst;
	for (int c = 0; c < n; c++)
		for (uint32_t row = 0; row < chunks[c].down; row++)
			for (uint32_t col = 0; col < chunks[c].across; col++) {
				const uint8_t *cell = chunks[c].cells + (row * 16 + col) * 2;
				uint16_t tf = (uint16_t) (cell[0] | cell[1] << 8), type = tf & 0x3ff;
				uint16_t frame = (tf >> 10) & 31;
				uint8_t *to = chunks[c].to + row * 8 * dst->pitch + col * 8;
				const uint8_t *from = type < E_FLAT_TYPES && flats[type].pixels != NULL
					&& frame < flats[type].frames ? flats[type].pixels + frame * 64 : NULL;
				for (int r = 0; r < 8; r++, to += dst->pitch)
					if (from != NULL)
						memcpy(to, from + r * 8, 8);
					else
						memset(to, 0, 8);
			}
}

static struct ESurface *target;
static const int16_t *bounds;

static inline void put(int x, int y, uint8_t c)
{
	if (x >= bounds[0] && x <= bounds[2] && y >= bounds[1] && y <= bounds[3])
		target->pixels[y * target->pitch + x] = c;
}

/* The engine's runs (engine.h): each a header long, then a fill's pixel or the raw pixels.
 * Flipped, a run goes down the screen: along is y. */
void eb_shape(struct ESurface *dst, const int16_t clip[4], const uint32_t *runs, int16_t x,
	int16_t y, uint8_t flipped)
{
	target = dst;
	bounds = clip;
	for (uint32_t h; (h = *runs++) != 0;) {
		int across = E_RUN_ACROSS(h), along = E_RUN_ALONG(h), len = (int) E_RUN_LENGTH(h);
		const uint8_t *px = (const uint8_t *) runs;
		int ax = flipped ? x + across : x + along, ay = flipped ? y + along : y + across;
		int sx = flipped ? 0 : 1, sy = flipped ? 1 : 0;
		for (int i = 0; i < len; i++)
			put(ax + i * sx, ay + i * sy, h & E_RUN_FILL ? px[0] : px[i]);
		runs += h & E_RUN_FILL ? 1 : (len + 3) / 4;
	}
}

uint32_t eb_convert(const uint8_t *raw, uint32_t size, uint8_t *out, uint32_t room)
{
	return e_convert_shape(raw, size, out, room);
}

void eb_finish(void)
{
}

/* Sounds: mixed as the Jaguar's DSP does (four channels, each sample times its side's volume,
 * the sum saturated), into a WAV (--wav), and listed. */
#define SOUND_CHANNELS 4
static struct {
	const uint8_t *at;
	uint32_t left;
	int vl, vr;
} channel[SOUND_CHANNELS];
static FILE *wav;
static uint32_t wav_frames;
static int sound_frame;

void eb_sound(const uint8_t *samples, uint32_t n, int volume, int pan)
{
	int c = 0;

	for (int k = 1; k < SOUND_CHANNELS; k++)
		if (channel[k].left < channel[c].left)
			c = k;
	channel[c].at = samples;
	channel[c].left = n;
	/* volume 0-127 to each side by the pan: 255 the whole */
	channel[c].vl = volume * (127 - pan) * 2 / 127;
	channel[c].vr = volume * pan * 2 / 127;
	printf("frame %d: sound of %u samples, volume %d, pan %d\n", sound_frame, n, volume, pan);
}

/* The music: the ring played by the engine's OPL2 a block at a time, as the Jaguar's DSP. */
static struct EOplStream music_stream;
static int16_t music_block[E_OPL_BLOCK];
static int music_left;

void eb_music_ring(uint8_t *ring, uint32_t size)
{
	music_stream.ring = ring;
	music_stream.mask = size - 1;
	eb_music_restart();
}

uint32_t eb_music_read(void)
{
	return music_stream.rd;
}

void eb_music_written(uint32_t wr)
{
	music_stream.wr = wr;
}

void eb_music_restart(void)
{
	e_opl_reset();
	music_stream.rd = music_stream.wr = music_stream.wait = music_stream.frac = 0;
	music_stream.ended = 0;
	music_left = 0;
}

static int music_gain = E_MUSIC_GAIN;

void eb_music_gain(int gain)
{
	music_gain = gain;
}

int eb_music_ended(void)
{
	return music_stream.ended;
}

static int music_sample(void)
{
	if (music_stream.ring == NULL)
		return 0;
	if (music_left == 0) {
		e_opl_block(&music_stream, music_block);
		music_left = E_OPL_BLOCK;
	}
	return music_block[E_OPL_BLOCK - music_left--];
}

static void wav_put(uint32_t v, int bytes)
{
	for (int k = 0; k < bytes; k++)
		fputc((int) (v >> (8 * k)) & 255, wav);
}

static void wav_header(void)
{
	fseek(wav, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, wav);
	wav_put(36 + wav_frames * 4, 4);
	fwrite("WAVEfmt ", 1, 8, wav);
	wav_put(16, 4);
	wav_put(1, 2);
	wav_put(2, 2);
	wav_put(E_SOUND_RATE, 4);
	wav_put(E_SOUND_RATE * 4, 4);
	wav_put(4, 2);
	wav_put(16, 2);
	fwrite("data", 1, 4, wav);
	wav_put(wav_frames * 4, 4);
}

/* The sound of blanks more blanks (at 60 a second). */
static void mix(int blanks)
{
	static uint32_t owed;

	owed += (uint32_t) blanks * E_SOUND_RATE;
	for (; owed >= 60; owed -= 60) {
		int32_t l = 0, r = 0, m = wav != NULL ? music_sample() * music_gain / E_MUSIC_GAIN : 0;
		for (int c = 0; c < SOUND_CHANNELS; c++) {
			if (channel[c].left == 0)
				continue;
			int16_t v = (int16_t) (channel[c].at[0] << 8 | channel[c].at[1]);
			channel[c].at += 2;
			channel[c].left--;
			l += v * channel[c].vl >> 8;
			r += v * channel[c].vr >> 8;
		}
		l += m;
		r += m;
		l = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
		r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
		if (wav != NULL) {
			wav_put((uint32_t) l & 0xffff, 2);
			wav_put((uint32_t) r & 0xffff, 2);
			wav_frames++;
		}
	}
}

uint32_t eb_clock(void)
{
	return (uint32_t) clock();
}

void eb_log(const char *text)
{
	fputs(text, stderr);
}

static const uint8_t *save_palette;    /* (the front's, when it is shown) */

static void save(const char *name)
{
	const uint8_t *pal = save_palette != NULL ? save_palette : e_palette();
	FILE *f = fopen(name, "wb");

	if (f == NULL)
		return;
	fprintf(f, "P6\n%d %d\n255\n", E_VIEW_W, E_VIEW_H);
	for (int y = 0; y < E_VIEW_H; y++)
		for (int x = 0; x < E_VIEW_W; x++) {
			uint8_t i = frame.pixels[y * frame.pitch + x];
			if (layers && i == 0)
				i = shown_ring->pixels[(shown_y + y) % shown_ring->h * shown_ring->pitch
					+ (shown_x + x) % shown_ring->w];
			const uint8_t *c = pal + i * 3;
			uint8_t rgb[3] = { (uint8_t) (c[0] << 2 | c[0] >> 4), (uint8_t) (c[1] << 2 | c[1] >> 4),
				(uint8_t) (c[2] << 2 | c[2] >> 4) };
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
}

/* --avatar: the avatar walks dx, dy a frame (a cell every other frame), the 31 NPCs nearest it
 * with it, picked again when it has gone 8 cells; the view on it; the ceiling over it */
static struct ENpc npcs[400];
static int nnpcs;
static int start_minutes = 6 * 60;

static void save(const char *name);

static const char *path;
/* --front keys: the main menu run a tick a key (. none, u d l r up down left right, a the pad's
 * A, b its B), then drawn into --out (or front.ppm) and what was chosen printed */
static const char *front_keys;
static int from_x = -1, from_y = -1, from_z = -1;
static int route_x = -1, route_y = -1, route_z;

/* --route: the avatar's step on its route to route_x, route_y (planned the first time), as an
 * NPC's (world.c): 1 and the step's dx, dy; 0 when it stays; -1 when there or given up. */
static int route_step(uint16_t tf, int16_t *ax, int16_t *ay, uint8_t *az, int *dx, int *dy, int f)
{
	static const int8_t way_x[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
	static const int8_t way_y[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
	static struct EWay way[E_ROUTE_WAYS];
	static int n = -1, next, failures;
	int door;

	*dx = *dy = 0;
	if (n < 0 || failures > 3) {
		n = e_route_plan(tf, *ax, *ay, *az, (int16_t) route_x, (int16_t) route_y, (uint8_t) route_z, 400, way);
		next = failures = 0;
		printf("frame %d: route from %d,%d,%d:", f, *ax, *ay, *az);
		for (int k = 0; k < n; k++)
			printf(" %d,%d,%d", way[k].x, way[k].y, way[k].z);
		printf("%s\n", n ? "" : " none");
		if (n == 0)
			return -1;
	}
	if (*ax == route_x && *ay == route_y && *az == route_z)
		return -1;
	if (next < n - 1 && *ax == way[next].x && *ay == way[next].y && *az == way[next].z)
		next++;
	int16_t x = *ax, y = *ay;
	uint8_t z = *az;
	int d = e_route_step(tf, &x, &y, &z, way[next].x, way[next].y, way[next].z, E_ROUTE_STOP, &door);
	if (d >= 0 && door) {
		int k = e_door_at(tf, (int16_t) (x + way_x[d]), (int16_t) (y + way_y[d]), z);
		if (k >= 0 && e_door_shut(k) && e_door_toggle(k)) {
			printf("frame %d: a door opened at %d,%d\n", f, x + way_x[d], y + way_y[d]);
			return 0;
		}
		d = e_route_step(tf, &x, &y, &z, way[next].x, way[next].y, way[next].z, E_ROUTE_AVOID, &door);
	}
	if (d < 0) {
		failures++;
		return 0;
	}
	*dx = way_x[d];
	*dy = way_y[d];
	*ax = x;
	*ay = y;
	*az = z;
	return 1;
}

static int list_npcs;           /* --npcs: those in view listed with each picture */
static int appear_frame;        /* --appear f: the avatar far off till frame f */
static int use_dx, use_dy, use_ahead;     /* --use-ahead dx,dy: the item ahead used (as A does) */
static int aim_dx, aim_dy, aim_given;  /* --aim dx,dy: pointed at, pixels from the avatar's feet */
static int put_type = -1;
static int party_numbers[8], nparty_numbers;
static int spawn_type = -1, spawn_n;    /* --spawn type,n: monsters put 6 cells south, fighting */
static int combat_on;
static int save_slot, load_slot;
static int show_equipment;
static int show_eggs;           /* --eggs: the eggs read in, and the monster eggs set or hatched */
static const char *avatar_name;  /* --name: the avatar's (as the front's new character) */
static int female;              /* --female: the avatar a woman (as the original's character made) */      /* --equipment: what the avatar and NPCs 1-4 wear */
static int save_at = -1, load_at = -1;  /* --save-at f, --load-at f: slot 9 saved, loaded, mid-walk */        /* --save n: saved at the end; --load n: loaded at the start */           /* --combat: the party in combat mode */   /* --party a,b,..: NPCs joining at the start */
static int uc_function = -1;
static int after_ticks;         /* --ticks n: the scripts run n ticks after (the functions they ask for) */    /* --uc-function n: run on the item ahead instead of its own */
static const char *inv_keys;    /* --inv k..: keys pressed in the container opened (l r u d a b c) */
static int inv_avatar;          /* --inv-avatar: the avatar's backpack opened */
static int find_types[4];       /* --find a,b,..: those shapes about the avatar listed */        /* --put type: an item of it put beside the avatar (to use) */
static int use_near;             /* --use-near: each usable item about the avatar used */
static int use_all;              /* --use-all: each shape's usecode run on an item of it */
static int talk_npc = -1;       /* --talk n: NPC n's conversation, printed (or with --out shown) */
static int out_pictures;        /* --out given */
static const char *talk_answers; /* --answers a,b,...: the answers picked */

static void walk_avatar(int dx, int dy, int n, const char *out, int every)
{
	int me = -1, leg_dx = dx, leg_dy = dy, leg_n = path ? 0 : n / 2;
	const char *next = path;

	nnpcs = e_npcs(npcs, 400);
	for (int i = 0; i < nnpcs; i++)
		if (npcs[i].number == 0)
			me = i;
	if (me < 0) {
		fprintf(stderr, "no avatar in NPC.DAT\n");
		return;
	}
	if (from_x >= 0) {
		npcs[me].x = (int16_t) from_x;
		npcs[me].y = (int16_t) from_y;
		if (from_z >= 0)
			npcs[me].z = (uint8_t) from_z;
	}
	if (avatar_name != NULL) {
		/* as the front's new character: the name */
		memset(npcs[me].name, 0, sizeof npcs[me].name);
		strncpy(npcs[me].name, avatar_name, sizeof npcs[me].name - 1);
	}
	if (female) {
		/* as the original's character made a woman: her shape (989), the flag off */
		npcs[me].male = 0;
		npcs[me].tf = (uint16_t) ((npcs[me].tf & ~0x3ffu) | 989);
	}
	int16_t ax = npcs[me].x, ay = npcs[me].y;
	uint8_t az = npcs[me].z;
	int16_t appear_x = ax, appear_y = ay;
	if (appear_frame > 0) {
		/* far off (as if elsewhere in the world) till then */
		ax = (int16_t) (ax > 1536 ? ax - 400 : ax + 400);
	}
	uint16_t type = npcs[me].tf & 0x3ff;
	int dir = dy < 0 ? 0 : dx > 0 ? 1 : dy > 0 ? 2 : 3;
	for (int m = 0; m < E_MOVERS; m++)
		e_mover_add(0, 0, 0, 0);
	e_mover_set(0, npcs[me].tf, ax, ay, az);
	e_world_init(npcs, nnpcs, start_minutes, 0);
	e_world_step(0, ax, ay);
	for (int n = 0; show_equipment && n <= 4; n++) {
		printf("NPC %d wears:", n);
		for (int sl = 0; sl <= 11; sl++) {
			int k = e_held_in_slot(n, sl);
			if (k >= 0)
				printf(" [%d] %d", sl, e_held_tf(k) & 0x3ff);
		}
		printf("  (weight %d)\n", e_held_weight(E_HELD_ON(n)));
	}
	if (load_slot > 0) {
		int ok = e_load_game(load_slot, 0);
		nnpcs = (int) e_save_count("NPCS");
		e_world_avatar_where(&ax, &ay, &az);
		e_mover_set(0, npcs[me].tf, ax, ay, az);
		printf("load %d: %d, avatar %d,%d,%d, %02d:%02d, held %d\n", load_slot, ok, ax, ay, az,
			e_world_minutes() / 60 % 24, e_world_minutes() % 60, e_held_used());
		if (save_slot > 0 && n <= 1) {
			/* (load then save, nothing between: the files the same) */
			printf("save %d: %d\n", save_slot, e_save_game(save_slot));
			return;
		}
	}
	for (int k = 0; k < nparty_numbers; k++)
		printf("party: NPC %d joins: %d\n", party_numbers[k], e_uc_join(party_numbers[k]));
	for (int k = 0; k < spawn_n; k++)
		printf("spawn: %d\n", e_world_spawn(spawn_type, (int16_t) (ax - spawn_n + 2 * k), (int16_t) (ay + 6), az));
	nnpcs = nnpcs + spawn_n;    /* (the world's own count went on) */
	if (combat_on)
		e_combat_mode(1);
	if (talk_npc >= 0 && out_pictures) {
		/* the conversation shown: a picture at each stop, the answers picked from the list (then
		 * bye, else the first offered) */
		const char *next = talk_answers;
		int pic = 0;
		e_set_view((int16_t) (ax - E_VIEW_CELLS_X / 2), (int16_t) (ay - E_VIEW_CELLS_Y / 2));
		e_follow(0);
		if (!e_talk_start(talk_npc)) {
			printf("no conversation\n");
			return;
		}
		while (e_talk_active() && pic < 300) {
			char name[256];
			e_draw(&frame);
			e_talk_draw(&frame);
			eb_finish();
			snprintf(name, sizeof name, "%s.talk%03d.ppm", out, pic++);
			save(name);
			printf("-- picture %d\n", pic - 1);
			for (int pl = 0; pl < 2; pl++)
				for (int l = 0; e_talk_line(pl, l); l++)
					printf("%d| %s\n", pl, e_talk_line(pl, l));
			char pick[64] = "";
			if (next != NULL && *next) {
				const char *c = strchr(next, ',');
				size_t len = c ? (size_t) (c - next) : strlen(next);
				snprintf(pick, sizeof pick, "%.*s", (int) len, next);
			}
			if (pick[0] && e_talk_choose(pick)) {
				printf("> %s\n", pick);
				const char *c = strchr(next, ',');
				next = c ? c + 1 : NULL;
			} else if (!pick[0] && e_talk_choose("bye"))
				printf("> bye\n");
			else
				e_talk_key(E_KEY_NEXT);
		}
		printf("(conversation over, %d pictures)\n", pic);
		return;
	}
	if (use_near) {
		/* each item about the avatar with usecode, used as the Jaguar's A uses it (the
		 * conversation screen's run: the first answer, the first number) */
		static struct EThing found[64];
		int16_t wx0, wy0, wx1, wy1;
		e_set_view((int16_t) (ax - E_VIEW_CELLS_X / 2), (int16_t) (ay - E_VIEW_CELLS_Y / 2));
		e_follow(0);
		e_draw(&frame);         /* (the world about read in) */
		e_near_window(&wx0, &wy0, &wx1, &wy1);
		for (int k = 0; k < 4 && find_types[k] > 0; k++) {
			static struct EThing f[32];
			int n = e_things((uint16_t) find_types[k], wx0, wy0, wx1, wy1, f, 32);
			for (int j = 0; j < n; j++)
				printf("found shape %d frame %d at %d,%d,%d (%+d,%+d from the avatar) quality %d\n",
					find_types[k], f[j].tf >> 10 & 31, f[j].x, f[j].y, f[j].z, f[j].x - ax, f[j].y - ay,
					e_thing_quality(&f[j]));
		}
		if (put_type >= 0)
			e_thing_add((uint16_t) put_type, (int16_t) (ax + 1), ay, az, NULL);
		if (use_ahead) {
			/* the item ahead, as the Jaguar's A finds it */
			struct EThing t;
			if (inv_avatar)
				e_inv_open_paperdoll(0);
			else if (!e_thing_ahead(ax, ay, az, use_dx, use_dy, e_inv_actionable, &t)) {
				printf("nothing usable ahead\n");
				return;
			} else {
				printf("== ahead: shape %d frame %d at %d,%d,%d\n", t.tf & 0x3ff, t.tf >> 10 & 31, t.x, t.y, t.z);
				if (uc_function >= 0)
					e_talk_run(uc_function, &t, 1);
				else if (!e_inv_open_thing(&t))
					e_talk_use(&t);
			}
			for (const char *k = inv_keys; e_inv_active(); k = k && *k ? k + 1 : k) {
				/* a container shown: its items, the keys pressed (then shut) */
				static int pic;
				int list[64], n = e_inv_items(list, 64);
				printf("  gumps %d, %d items (avatar's gold %d, held %d):", e_inv_depth(), n,
					e_held_count(E_HELD_ON(0), 644, -1, -1), e_held_used());
				for (int j = 0; j < n; j++)
					printf(" %s%d/%d", list[j] == e_inv_picked() ? "*" : "", e_held_tf(list[j]) & 0x3ff,
						e_held_quantity(list[j]));
				printf("%s\n", e_inv_refused() >= 0 ? " (refused)" : "");
				if (out_pictures) {
					char name[256];
					e_draw(&frame);
					e_inv_draw(&frame);
					eb_finish();
					snprintf(name, sizeof name, "%s.inv%02d.ppm", out, pic++);
					save(name);
				}
				char c = k && *k ? *k : 'b';
				printf("  key %c\n", c);
				if (c == 'o')
					e_inv_avatar_key();
				else
				e_inv_key(c == 'l' ? E_KEY_LEFT : c == 'r' ? E_KEY_RIGHT : c == 'u' ? E_KEY_UP
					: c == 'd' ? E_KEY_DOWN : c == 'a' ? E_KEY_NEXT : c == 'c' ? E_KEY_TAKE : E_KEY_BACK);
				for (int turns = 0; e_talk_active() && turns < 50; turns++) {
					for (int pl = 0; pl < 2; pl++)
						for (int l = 0; e_talk_line(pl, l); l++)
							printf("  %d| %s\n", pl, e_talk_line(pl, l));
					e_talk_key(E_KEY_NEXT);
				}
			}
			for (int turns = 0; e_talk_active() && turns < 50; turns++) {
				int16_t px, py;
				for (int pl = 0; pl < 2; pl++)
					for (int l = 0; e_talk_line(pl, l); l++)
						printf("  %d| %s\n", pl, e_talk_line(pl, l));
				if (e_talk_targeting(&px, &py)) {
					/* pointed at: from the avatar's feet on the screen (or let go) */
					struct EThing p;
					int m, sx = (E_VIEW_CELLS_X / 2) * 8 + 7 - az * 4 + aim_dx;
					int sy = (E_VIEW_CELLS_Y / 2) * 8 + 7 - az * 4 + aim_dy;
					e_talk_aim(sx, sy);
					int hit = e_pick((int16_t) sx, (int16_t) sy, &p, &m);
					printf("  target at %d,%d: %s shape %d frame %d at %d,%d,%d mover %d\n", sx, sy,
						hit ? "item" : "ground", p.tf & 0x3ff, p.tf >> 10 & 31, p.x, p.y, p.z, m);
					if (out_pictures) {
						char name[256];
						e_draw(&frame);
						e_talk_draw(&frame);
						eb_finish();
						snprintf(name, sizeof name, "%s.aim.ppm", out);
						save(name);
					}
					e_talk_key(aim_given ? E_KEY_NEXT : E_KEY_BACK);
					continue;
				}
				e_talk_key(E_KEY_NEXT);
			}
			printf("  (over)\n");
			for (int tick = 0; tick <= after_ticks; tick++) {
				/* then the scripts, the functions asked for (their questions: the first answer) */
				int16_t px, py;
				uint8_t pz;
				uint16_t ptf;
				int f, it, ev;
				if (e_world_avatar_pose(&px, &py, &pz, &ptf)) {
					printf("  tick %d: avatar at %d,%d,%d shape %d frame %d\n", tick, px, py, pz, ptf & 0x3ff,
						ptf >> 10 & 31);
					if (out_pictures) {
						char name[256];
						e_mover_set(0, ptf, px, py, pz);
						e_set_view((int16_t) (px - E_VIEW_CELLS_X / 2), (int16_t) (py - E_VIEW_CELLS_Y / 2));
						e_draw(&frame);
						eb_finish();
						snprintf(name, sizeof name, "%s.pose%03d.ppm", out, tick);
						save(name);
					}
				}
				static int was_faded;
				if (e_uc_faded() != was_faded)
					printf("  tick %d: %s, %02d:%02d\n", tick, (was_faded = e_uc_faded()) ? "faded out" : "faded in",
						e_world_minutes() / 60 % 24, e_world_minutes() % 60);
				if (!e_talk_active() && e_uc_request(&f, &it, &ev)) {
					printf("  tick %d: function 0x%x for %d (event %d)\n", tick, f, it, ev);
					e_talk_start_ref(f, it, ev);
				}
				for (int turns = 0; e_talk_active() && turns < 50; turns++) {
					for (int pl = 0; pl < 2; pl++)
						for (int l = 0; e_talk_line(pl, l); l++)
							printf("  %d| %s\n", pl, e_talk_line(pl, l));
					e_talk_key(E_KEY_NEXT);
				}
				e_uc_scripts_step();
			}
			return;
		}
		for (int type = 0; type < 1024; type++) {
			if (!e_usable((uint16_t) type))
				continue;
			int n = e_things((uint16_t) type, wx0, wy0, wx1, wy1, found, 64);
			for (int k = 0; k < n && k < 3; k++) {
				printf("== shape %d frame %d at %d,%d,%d\n", type, found[k].tf >> 10 & 31, found[k].x,
					found[k].y, found[k].z);
				int turns = 0;
				static int pic;
				e_talk_use(&found[k]);
				while (e_talk_active() && turns++ < 200) {
					for (int pl = 0; pl < 2; pl++)
						for (int l = 0; e_talk_line(pl, l); l++)
							printf("  %d| %s\n", pl, e_talk_line(pl, l));
					if (out_pictures) {
						char name[256];
						e_draw(&frame);
						e_talk_draw(&frame);
						eb_finish();
						snprintf(name, sizeof name, "%s.use%03d.ppm", out, pic++);
						save(name);
						printf("-- picture %d\n", pic - 1);
					}
					e_talk_key(E_KEY_NEXT);
				}
			}
		}
		return;
	}
	if (use_all) {
		/* each shape's function (0-1023) used on an item of that shape made for it (loose):
		 * the first answer, the first number, to its end */
		for (int type = 0; type < 1024; type++) {
			int i = e_held_add((uint16_t) type, 0, E_HELD_LOOSE);
			int r = e_uc_start(type, E_HELD_REF + i, 1), turns = 0;
			if (r == E_UC_DONE && i >= 0) {
				e_held_remove(i);
				continue;
			}
			printf("== shape %d\n", type);
			while (r != E_UC_DONE && turns++ < 200) {
				if (r == E_UC_SAY) {
					printf("  say: %.100s\n", e_uc_text());
					r = e_uc_resume(NULL);
				} else if (r == E_UC_NUMBER) {
					int lo, hi, st, in;
					e_uc_number(&lo, &hi, &st, &in);
					r = e_uc_resume_number(in);
				} else {
					const char *list[40];
					int n = e_uc_answers(list, 40);
					printf("  ask: %s\n", n > 0 ? list[0] : "-");
					r = e_uc_resume(n > 0 ? list[0] : "");
				}
			}
			e_held_remove(i);
		}
		return;
	}
	if (talk_npc >= 0) {
		/* the NPC's conversation (its usecode, as spoken to), the answers picked from the list
		 * (then "bye", else the first offered) */
		const char *next = talk_answers;
		int r = e_uc_start(0x400 + talk_npc, -talk_npc, 1), turns = 0;
		while (r != E_UC_DONE && turns++ < 400) {
			if (r == E_UC_SAY) {
				int frame, face = e_uc_speaker(&frame);
				printf("[face %d/%d] %s\n", face, frame, e_uc_text());
				r = e_uc_resume(NULL);
				continue;
			}
			if (r == E_UC_NUMBER) {
				/* a number: "#n" next in the list, else the first offered */
				int lo, hi, step, init, v;
				e_uc_number(&lo, &hi, &step, &init);
				v = init;
				if (next != NULL && *next == '#') {
					v = atoi(next + 1);
					const char *c = strchr(next, ',');
					next = c ? c + 1 : NULL;
				}
				printf("  number %d to %d by %d (%d): > %d\n", lo, hi, step, init, v);
				r = e_uc_resume_number(v);
				continue;
			}
			const char *list[40];
			char pick[64] = "";
			int n = e_uc_answers(list, 40);
			printf("  answers:");
			for (int k = 0; k < n; k++)
				printf(" [%s]", list[k]);
			printf("\n");
			static char tried[64][40];
			static int ntried;
			if (next != NULL && !strcmp(next, "auto")) {
				/* the newest answer not yet tried, forty at most, then bye */
				for (int k = n - 1; k >= 0 && !pick[0] && ntried < 40; k--) {
					int seen = !strcmp(list[k], "bye");
					for (int j = 0; j < ntried && !seen; j++)
						seen = !strcmp(tried[j], list[k]);
					if (!seen) {
						snprintf(pick, sizeof pick, "%s", list[k]);
						snprintf(tried[ntried++], sizeof tried[0], "%s", list[k]);
					}
				}
				if (!pick[0])
					snprintf(pick, sizeof pick, "%s", n > 0 ? "bye" : "");
			} else if (next != NULL && *next) {
				const char *c = strchr(next, ',');
				size_t len = c ? (size_t) (c - next) : strlen(next);
				snprintf(pick, sizeof pick, "%.*s", (int) len, next);
				next = c ? c + 1 : NULL;
			} else {
				for (int k = 0; k < n; k++)
					if (!strcmp(list[k], "bye"))
						snprintf(pick, sizeof pick, "bye");
				if (!pick[0] && n > 0)
					snprintf(pick, sizeof pick, "%s", list[0]);
			}
			printf("> %s\n", pick);
			r = e_uc_resume(pick);
		}
		printf("(conversation over; the avatar's gold %d, held items %d)\n",
			e_held_count(E_HELD_ON(0), 644, -1, -1), e_held_used());
		return;
	}
	if (path || route_x >= 0)
		n = 100000;
	for (int f = 0; f < n; f++) {
		if (f > 0 && (f & 1) == 0 && route_x >= 0) {
			e_mover_self(0);
			int r = route_step(e_walk_frame(type, 2, 0), &ax, &ay, &az, &dx, &dy, f);
			e_mover_self(-1);
			if (r < 0 || f > 4000) {
				printf("end: avatar %d,%d,%d; doors opened or shut %u\n", ax, ay, az,
					e_stats.doors_turned);
				break;
			}
			if (dx || dy)
				dir = ((e_way_of(dx, dy) + 1) & 7) >> 1;
		} else if (f > 0 && (f & 1) == 0) {
			while (leg_n == 0 && next != NULL && *next) {
				if (sscanf(next, "%d,%d,%d", &leg_dx, &leg_dy, &leg_n) != 3)
					break;
				next = strchr(next, ';');
				next = next ? next + 1 : NULL;
			}
			if (leg_n == 0) {
				if (path) {
					printf("end: avatar %d,%d,%d; doors opened or shut %u\n", ax, ay, az,
						e_stats.doors_turned);
					break;
				}
				leg_dx = leg_dy = 0;
			} else
				leg_n--;
			dx = leg_dx;
			dy = leg_dy;
			if (dx || dy)
				dir = dy < 0 ? 0 : dx > 0 ? 1 : dy > 0 ? 2 : 3;
			/* a step, veering round what is in the way, up or down stairs; a door opened */
			e_mover_self(0);
			int16_t was_x = ax, was_y = ay;
			if ((dx || dy) && e_walk(e_walk_frame(type, dir, 0), &ax, &ay, &az, dx, dy, 1)
				== E_STEP_DOOR)
				printf("frame %d: a door opened at %d,%d\n", f, ax + dx, ay + dy);
			e_mover_self(-1);
			/* in combat, left be: its own turn */
			static uint16_t swing_tf;
			swing_tf = 0;
			if (combat_on && !dx && !dy) {
				int fx, fy;
				int r = e_world_avatar_fight(&fx, &fy, &swing_tf);
				if (r == 1) {
					dir = fy < 0 ? 0 : fx > 0 ? 1 : fy > 0 ? 2 : 3;
					e_mover_self(0);
					e_walk(e_walk_frame(type, dir, 0), &ax, &ay, &az, fx, fy, 1);
					e_mover_self(-1);
				} else if (r == 2)
					printf("frame %d: the avatar swings\n", f);
			}
			/* the party after it */
			e_world_avatar_place(ax, ay, az, dir);
			e_world_party_step(ax != was_x || ay != was_y);
		}
		if (f == save_at) {
			e_world_avatar_place(ax, ay, az, dir);
			printf("frame %d: save 9: %d at %d,%d\n", f, e_save_game(9), ax, ay);
		}
		if (f == load_at) {
			int ok = e_load_game(9, (uint32_t) f * 4);
			e_world_avatar_where(&ax, &ay, &az);
			printf("frame %d: load 9: %d at %d,%d\n", f, ok, ax, ay);
			fflush(stdout);
		}
		sound_frame = f;
		if (appear_frame > 0 && f == appear_frame) {
			ax = appear_x;
			ay = appear_y;
		}
		e_music_service();
		mix(4);
		e_mover_set(0, dx || dy ? e_walk_frame(type, dir, f / 2) : npcs[me].tf, ax, ay, az);
		/* a frame here is 4 blanks (the avatar a cell every 2 frames, the Jaguar's 8 blanks) */
		e_world_step((uint32_t) f * 4, ax, ay);
		static uint32_t routes_seen;
		if (e_stats.routes_planned + e_stats.routes_failed != routes_seen) {
			routes_seen = e_stats.routes_planned + e_stats.routes_failed;
			printf("frame %d: routes %u (%u not found), steps tried %u, time %u worst %u\n", f,
				e_stats.routes_planned, e_stats.routes_failed,
				e_stats.route_steps, e_stats.route_time, e_stats.route_worst);
		}
		static uint32_t doors_seen;
		if (e_stats.doors_turned != doors_seen) {
			doors_seen = e_stats.doors_turned;
			printf("frame %d: doors opened or shut %u\n", f, doors_seen);
		}
		int ceiling = e_follow(0);
		e_set_view((int16_t) (ax - E_VIEW_CELLS_X / 2), (int16_t) (ay - E_VIEW_CELLS_Y / 2));
		static uint8_t layer_buf[3][E_VIEW_H * E_VIEW_W];
		if (layers)
			frame.pixels = layer_buf[f % 3];
		e_draw(&frame);
		if (f % every == 0 || f == n - 1) {
			char name[256];
			snprintf(name, sizeof name, "%s.%04d.ppm", out, f);
			save(name);
			printf("frame %d: avatar %d,%d,%d ceiling %d\n", f, ax, ay, az, ceiling);
			for (int i = 0; i < nnpcs; i++)
				for (int k = 0; k < nparty_numbers; k++)
					if (npcs[i].number == party_numbers[k])
						printf("  party %d at %d,%d,%d (%+d,%+d) frame %d hp %d%s\n", npcs[i].number, npcs[i].x, npcs[i].y,
							npcs[i].z, npcs[i].x - ax, npcs[i].y - ay, npcs[i].tf >> 10 & 31, (int8_t) npcs[i].hit_points,
							npcs[i].status & 0x8000 ? " dead" : npcs[i].status & 0x80 ? " out" : "");
			for (int i = 0; i < e_world_npc_count(); i++)
				if (npcs[i].number >= 1000 && !(npcs[i].status & 0x8000))
					printf("  monster %d type %d at %d,%d (%+d,%+d) hp %d%s\n", npcs[i].number, npcs[i].tf & 0x3ff,
						npcs[i].x, npcs[i].y, npcs[i].x - ax, npcs[i].y - ay, (int8_t) npcs[i].hit_points,
						npcs[i].status & 0x8000 ? " dead" : npcs[i].status & 0x80 ? " out" : "");
			if (show_eggs) {
				struct EEgg *t;
				int ne = e_eggs(&t), states[4] = { 0, 0, 0, 0 };
				for (int k = 0; k < ne; k++)
					states[t[k].state & 3]++;
				printf("  eggs %d: out %d asleep %d set %d away %d\n", ne, states[0], states[1], states[2], states[3]);
				printf("  music %d\n", e_music_current());
				for (int k = 0; k < ne; k++)
					if ((t[k].bits & 15) == 1 && t[k].state)
						printf("    egg %d,%d kind %d crit %d state %d type %d\n", t[k].x, t[k].y, t[k].bits & 15,
							t[k].bits >> 4 & 7, t[k].state, t[k].data2 & 0x3ff);
			}
			if (npcs[me].number == 0 && combat_on)
				printf("  avatar hp %d\n", (int8_t) npcs[me].hit_points);
			/* the NPCs in view: number, place, frame (10 sitting, 13 lying) */
			for (int i = 0; i < nnpcs && list_npcs; i++)
				if (i != me && npcs[i].x >= ax - 20 && npcs[i].x < ax + 20 && npcs[i].y >= ay - 13
					&& npcs[i].y < ay + 13)
					printf("  npc %d at %d,%d,%d frame %d\n", npcs[i].number, npcs[i].x,
						npcs[i].y, npcs[i].z, npcs[i].tf >> 10 & 31);
		}
	}
	if (save_slot > 0) {
		e_world_avatar_place(ax, ay, az, 2);
		printf("save %d: %d (avatar %d,%d,%d, %02d:%02d, held %d)\n", save_slot, e_save_game(save_slot), ax, ay,
			az, e_world_minutes() / 60 % 24, e_world_minutes() % 60, e_held_used());
	}
}

static uint32_t rng = 12345;
static int rnd(int n)
{
	rng = rng * 1103515245u + 12345u;
	return (int) ((rng >> 16) % (uint32_t) n);
}

/* --music n[,seconds] with --wav: song n played (as the usecode's PlayMusic) into the WAV for
 * the seconds (30 if not given), alone, after the engine is up. */
static int music_song = -1, music_seconds = 30;

int main(int argc, char **argv)
{
	static uint8_t memory[1600 * 1024];
	int x, y, dx = 0, dy = 0, n = 1, nm = 0, every = 1, sheet = -1, avatar = 0;
	const char *out = "view";
	struct { int16_t x, y; int dir; } walker[E_MOVERS];

	if (argc < 4) {
		fprintf(stderr, "u7engine <dir>/ <x> <y> [--walk dx dy n] [--movers n] [--out p] [--every k]\n");
		return 2;
	}
	x = atoi(argv[2]);
	y = atoi(argv[3]);
	for (int i = 4; i < argc; i++) {
		if (!strcmp(argv[i], "--walk") && i + 3 < argc) {
			dx = atoi(argv[++i]);
			dy = atoi(argv[++i]);
			n = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--movers") && i + 1 < argc)
			nm = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
			out = argv[++i];
			out_pictures = 1;
		}
		else if (!strcmp(argv[i], "--sheet") && i + 1 < argc)
			sheet = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--every") && i + 1 < argc)
			every = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--layers"))
			layers = 1;
		else if (!strcmp(argv[i], "--avatar"))
			avatar = 1;
		else if (!strcmp(argv[i], "--path") && i + 1 < argc)
			path = argv[++i];
		else if (!strcmp(argv[i], "--from") && i + 1 < argc)
			sscanf(argv[++i], "%d,%d,%d", &from_x, &from_y, &from_z);
		else if (!strcmp(argv[i], "--route") && i + 1 < argc)
			sscanf(argv[++i], "%d,%d,%d", &route_x, &route_y, &route_z);
		else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
			if ((wav = fopen(argv[++i], "wb")) == NULL) {
				perror(argv[i]);
				return 1;
			}
			wav_header();
		} else if (!strcmp(argv[i], "--music") && i + 1 < argc) {
			sscanf(argv[++i], "%d,%d", &music_song, &music_seconds);
		} else if (!strcmp(argv[i], "--use-ahead") && i + 1 < argc) {
			sscanf(argv[++i], "%d,%d", &use_dx, &use_dy);
			use_ahead = use_near = 1;
		} else if (!strcmp(argv[i], "--aim") && i + 1 < argc) {
			sscanf(argv[++i], "%d,%d", &aim_dx, &aim_dy);
			aim_given = 1;
		} else if (!strcmp(argv[i], "--find") && i + 1 < argc) {
			sscanf(argv[++i], "%d,%d,%d,%d", &find_types[0], &find_types[1], &find_types[2], &find_types[3]);
			use_near = 1;
		} else if (!strcmp(argv[i], "--inv") && i + 1 < argc)
			inv_keys = argv[++i];
		else if (!strcmp(argv[i], "--inv-avatar")) {
			inv_avatar = use_near = use_ahead = 1;
		} else if (!strcmp(argv[i], "--ticks") && i + 1 < argc)
			after_ticks = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--uc-function") && i + 1 < argc)
			uc_function = (int) strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--spawn") && i + 1 < argc)
			sscanf(argv[++i], "%d,%d", &spawn_type, &spawn_n);
		else if (!strcmp(argv[i], "--save") && i + 1 < argc)
			save_slot = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--name") && i + 1 < argc)
			avatar_name = argv[++i];
		else if (!strcmp(argv[i], "--female"))
			female = 1;
		else if (!strcmp(argv[i], "--eggs"))
			show_eggs = 1;
		else if (!strcmp(argv[i], "--equipment"))
			show_equipment = 1;
		else if (!strcmp(argv[i], "--save-at") && i + 1 < argc)
			save_at = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--load-at") && i + 1 < argc)
			load_at = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--load") && i + 1 < argc)
			load_slot = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--combat"))
			combat_on = 1;
		else if (!strcmp(argv[i], "--party") && i + 1 < argc) {
			for (char *t = strtok(argv[++i], ","); t != NULL && nparty_numbers < 8; t = strtok(NULL, ","))
				party_numbers[nparty_numbers++] = atoi(t);
		} else if (!strcmp(argv[i], "--put") && i + 1 < argc)
			put_type = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--uc-trace"))
			e_uc_trace = 1;
		else if (!strcmp(argv[i], "--use-near"))
			use_near = 1;
		else if (!strcmp(argv[i], "--use-all"))
			use_all = 1;
		else if (!strcmp(argv[i], "--talk") && i + 1 < argc)
			talk_npc = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--answers") && i + 1 < argc)
			talk_answers = argv[++i];
		else if (!strcmp(argv[i], "--appear") && i + 1 < argc)
			appear_frame = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--trace") && i + 1 < argc)
			e_world_trace = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--npcs"))
			list_npcs = 1;
		else if (!strcmp(argv[i], "--front") && i + 1 < argc)
			front_keys = argv[++i];
		else if (!strcmp(argv[i], "--time") && i + 1 < argc)
			start_minutes = atoi(argv[++i]);
	}
	if (music_song >= 0 && wav == NULL) {
		fprintf(stderr, "--music needs --wav\n");
		return 2;
	}
	e_surface_rows(&frame);
	int r = e_init(argv[1], memory, sizeof memory);
	if (r != 0) {
		fprintf(stderr, "e_init: %d\n", r);
		return 1;
	}
	if (front_keys != NULL) {
		static uint8_t pal[768];
		char chosen[15];
		int male, got = 0, ticks = 0;
		if (!e_front_open(0, 0, 0)) {
			fprintf(stderr, "e_front_open failed\n");
			return 1;
		}
		for (const char *k = front_keys; *k && !got; k++, ticks++) {
			static const char from[] = "udlrab";
			static const int to[] = { E_KEY_UP, E_KEY_DOWN, E_KEY_LEFT, E_KEY_RIGHT, E_KEY_NEXT, E_KEY_BACK };
			const char *at = strchr(from, *k);
			got = e_front_tick(at != NULL && *k ? to[at - from] : 0);
		}
		e_front_draw(&frame);
		eb_finish();
		e_front_palette(pal);
		save_palette = pal;
		save(out != NULL ? out : "front.ppm");
		e_front_result(chosen, &male);
		printf("front: %d after %d ticks, name '%s', %s\n", got, ticks, chosen, male ? "male" : "female");
		return 0;
	}
	if (music_song >= 0) {
		/* the song alone, into the WAV */
		e_music_play(music_song);
		for (long b = 0; b < (long) music_seconds * 60; b++) {
			e_music_service();
			mix(1);
		}
		wav_header();
		fclose(wav);
		return 0;
	}
	if (sheet >= 0) {
		/* the shape's 32 frames on a grid of 8 by 4: draw each as a mover in its own cell */
		static const int16_t whole[4] = { 0, 0, E_VIEW_W - 1, E_VIEW_H - 1 };
		memset(screen, 0, sizeof screen);
		for (int fr = 0; fr < 32; fr++) {
			const uint32_t *runs = e_frame_runs((uint16_t) sheet, (uint16_t) fr);
			if (runs != NULL)
				eb_shape(&frame, whole, runs, (int16_t) (fr % 8 * 40 + 32),
					(int16_t) (fr / 8 * 50 + 44), 0);
		}
		save("sheet.ppm");
		return 0;
	}
	if (avatar) {
		walk_avatar(dx, dy, n, out, every);
		if (wav != NULL) {
			wav_header();
			fclose(wav);
		}
		return 0;
	}
	for (int m = 0; m < nm && m < E_MOVERS; m++) {
		walker[m].x = (int16_t) (x + 4 + rnd(32));
		walker[m].y = (int16_t) (y + 4 + rnd(20));
		walker[m].dir = rnd(4);
		e_mover_add(721, walker[m].x, walker[m].y, 0);
	}
	clock_t t0 = clock();
	for (int f = 0; f < n; f++) {
		e_set_view((int16_t) (x + dx * f), (int16_t) (y + dy * f));
		for (int m = 0; m < nm && m < E_MOVERS; m++) {
			static const int8_t ddx[4] = { 0, 1, 0, -1 }, ddy[4] = { -1, 0, 1, 0 };
			if (rnd(8) == 0)
				walker[m].dir = rnd(4);
			walker[m].x = (int16_t) (walker[m].x + ddx[walker[m].dir]);
			walker[m].y = (int16_t) (walker[m].y + ddy[walker[m].dir]);
			e_mover_set(m, e_walk_frame(721, walker[m].dir, f), walker[m].x, walker[m].y, 0);
		}
		/* layers: three of them in turn, as the Jaguar's frames */
		static uint8_t layer_buf[3][E_VIEW_H * E_VIEW_W];
		if (layers)
			frame.pixels = layer_buf[f % 3];
		e_draw(&frame);
		if (f % every == 0 || f == n - 1) {
			char name[256];
			snprintf(name, sizeof name, "%s.%04d.ppm", out, f);
			save(name);
		}
	}
	double s = (double) (clock() - t0) / CLOCKS_PER_SEC;
	printf("%d frames, %.3f ms each; rebuilds %u, statics %u, edges %u, pair tests %u, drawn %u, "
		"shapes %u KB (%u loads)\n", n, 1000.0 * s / n, e_stats.rebuilds, e_stats.statics,
		e_stats.edges, e_stats.pair_tests, e_stats.drawn, e_stats.shape_bytes / 1024,
		e_stats.shape_loads);
	return 0;
}
