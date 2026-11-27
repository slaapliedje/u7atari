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
	(void) mode;
	for (int16_t i = 0; i < 16; i++)
		if (files[i] == NULL) {
			if ((files[i] = fopen(name, "rb")) == NULL)
				return -1;
			return i;
		}
	return -1;
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
		int32_t l = 0, r = 0;
		for (int c = 0; c < SOUND_CHANNELS; c++) {
			if (channel[c].left == 0)
				continue;
			int16_t v = (int16_t) (channel[c].at[0] << 8 | channel[c].at[1]);
			channel[c].at += 2;
			channel[c].left--;
			l += v * channel[c].vl >> 8;
			r += v * channel[c].vr >> 8;
		}
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

static void save(const char *name)
{
	const uint8_t *pal = e_palette();
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
	int16_t ax = npcs[me].x, ay = npcs[me].y;
	uint8_t az = npcs[me].z;
	uint16_t type = npcs[me].tf & 0x3ff;
	int dir = dy < 0 ? 0 : dx > 0 ? 1 : dy > 0 ? 2 : 3;
	for (int m = 0; m < E_MOVERS; m++)
		e_mover_add(0, 0, 0, 0);
	e_mover_set(0, npcs[me].tf, ax, ay, az);
	e_world_init(npcs, nnpcs, start_minutes, 0);
	e_world_step(0, ax, ay);
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
			if ((dx || dy) && e_walk(e_walk_frame(type, dir, 0), &ax, &ay, &az, dx, dy, 1)
				== E_STEP_DOOR)
				printf("frame %d: a door opened at %d,%d\n", f, ax + dx, ay + dy);
			e_mover_self(-1);
		}
		sound_frame = f;
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
			/* the NPCs in view: number, place, frame (10 sitting, 13 lying) */
			for (int i = 0; i < nnpcs && list_npcs; i++)
				if (i != me && npcs[i].x >= ax - 20 && npcs[i].x < ax + 20 && npcs[i].y >= ay - 13
					&& npcs[i].y < ay + 13)
					printf("  npc %d at %d,%d,%d frame %d\n", npcs[i].number, npcs[i].x,
						npcs[i].y, npcs[i].z, npcs[i].tf >> 10 & 31);
		}
	}
}

static uint32_t rng = 12345;
static int rnd(int n)
{
	rng = rng * 1103515245u + 12345u;
	return (int) ((rng >> 16) % (uint32_t) n);
}

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
		else if (!strcmp(argv[i], "--out") && i + 1 < argc)
			out = argv[++i];
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
		} else if (!strcmp(argv[i], "--trace") && i + 1 < argc)
			e_world_trace = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--npcs"))
			list_npcs = 1;
		else if (!strcmp(argv[i], "--time") && i + 1 < argc)
			start_minutes = atoi(argv[++i]);
	}
	e_surface_rows(&frame);
	int r = e_init(argv[1], memory, sizeof memory);
	if (r != 0) {
		fprintf(stderr, "e_init: %d\n", r);
		return 1;
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
