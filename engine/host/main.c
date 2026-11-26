/* The engine test on a PC: draws views of the world to pictures (PPM), for checking the core.
 *
 *   u7engine <folder with STATIC and GAMEDAT>/ <cell x> <cell y> [options]
 *     --walk dx dy n   move the view dx, dy cells a frame for n frames (default: one frame)
 *     --movers n       n NPCs wandering about the view
 *     --out prefix     pictures prefix.NNNN.ppm (default view); --every k: every k-th frame
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

/* The original's spans: a word of length * 2 (+1 run-encoded; 0 ends), the offsets along and
 * across from the hot spot, then raw pixels or runs (count * 2, +1 for a fill of the byte after
 * it). Flipped, a span runs down the screen: along is y. */
void eb_shape(struct ESurface *dst, const int16_t clip[4], const uint8_t *s, int16_t x,
	int16_t y, uint8_t flipped)
{
	target = dst;
	bounds = clip;
	for (;;) {
		int head = s[0] | s[1] << 8, len = head >> 1;
		if (head == 0)
			return;
		int along = (int16_t) (s[2] | s[3] << 8), across = (int16_t) (s[4] | s[5] << 8);
		s += 6;
		int ax = flipped ? x + across : x + along, ay = flipped ? y + along : y + across;
		int sx = flipped ? 0 : 1, sy = flipped ? 1 : 0;
		if (!(head & 1)) {
			for (int i = 0; i < len; i++)
				put(ax + i * sx, ay + i * sy, s[i]);
			s += len;
		} else {
			for (int pos = 0; pos < len;) {
				int c = s[0] >> 1, fill = s[0] & 1;
				s++;
				for (int i = 0; i < c; i++)
					put(ax + (pos + i) * sx, ay + (pos + i) * sy, fill ? s[0] : s[i]);
				s += fill ? 1 : c;
				pos += c;
			}
		}
	}
}

void eb_finish(void)
{
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
			const uint8_t *c = pal + screen[y][x] * 3;
			uint8_t rgb[3] = { (uint8_t) (c[0] << 2 | c[0] >> 4), (uint8_t) (c[1] << 2 | c[1] >> 4),
				(uint8_t) (c[2] << 2 | c[2] >> 4) };
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
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
	int x, y, dx = 0, dy = 0, n = 1, nm = 0, every = 1, sheet = -1;
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
			const uint8_t *spans = e_frame_spans((uint16_t) sheet, (uint16_t) fr);
			if (spans != NULL)
				eb_shape(&frame, whole, spans, (int16_t) (fr % 8 * 40 + 32),
					(int16_t) (fr / 8 * 50 + 44), 0);
		}
		save("sheet.ppm");
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
		e_draw(&frame);
		if (f % every == 0 || f == n - 1) {
			char name[256];
			snprintf(name, sizeof name, "%s.%04d.ppm", out, f);
			save(name);
		}
	}
	double s = (double) (clock() - t0) / CLOCKS_PER_SEC;
	printf("%d frames, %.3f ms each; rebuilds %u, statics %u, edges %u, compares %u, drawn %u, "
		"shapes %u KB (%u loads)\n", n, 1000.0 * s / n, e_stats.rebuilds, e_stats.statics,
		e_stats.edges, e_stats.compares, e_stats.drawn, e_stats.shape_bytes / 1024,
		e_stats.shape_loads);
	return 0;
}
