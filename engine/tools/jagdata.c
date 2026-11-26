/* The engine's dataset, made on the PC from the original's files (engine/README.md):
 *
 *     u7jagdata <data dir> <out dir>
 *
 * with <data dir> holding STATIC/, writes into <out dir> (engine.h):
 *   SHAPES.FLX  SHAPES.VGA with every object shape in the engine's form (E_SHAPES_TITLE)
 *   CHUNKS.DAT  every world chunk's fixed statics, binned, as the engine makes them: the engine's
 *               own core does it here, shown STATIC/ alone (E_CHUNKS_TITLE)
 * both in the Jaguar's byte order. They go on the card in U7/ENGINE/. The original's files are
 * only read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "engine.h"

/* ---- a backend for the core: files under the data dir, but for GAMEDAT/ and ENGINE/; no
 * drawing ---- */

static FILE *files[16];

int16_t plat_file_open(const char *name, int16_t mode)
{
	(void) mode;
	if (strstr(name, "GAMEDAT/") != NULL || strstr(name, "ENGINE/") != NULL)
		return -1;
	for (int16_t i = 0; i < 16; i++)
		if (files[i] == NULL)
			return (files[i] = fopen(name, "rb")) != NULL ? i : -1;
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

void eb_copy(struct ESurface *dst, int16_t dx, int16_t dy, const struct ESurface *src, int16_t sx,
	int16_t sy, int16_t w, int16_t h)
{
	(void) dst; (void) dx; (void) dy; (void) src; (void) sx; (void) sy; (void) w; (void) h;
}

void eb_shape(struct ESurface *dst, const int16_t clip[4], const uint32_t *runs, int16_t x,
	int16_t y, uint8_t flipped)
{
	(void) dst; (void) clip; (void) runs; (void) x; (void) y; (void) flipped;
}

int eb_view(struct ESurface *frame, const struct ESurface *ring, int16_t sx, int16_t sy)
{
	(void) frame; (void) ring; (void) sx; (void) sy;
	return 1;
}

void eb_clear(struct ESurface *dst, int16_t x, int16_t y, int16_t w, int16_t h)
{
	(void) dst; (void) x; (void) y; (void) w; (void) h;
}

void eb_tiles(struct ESurface *dst, const struct EFlat flats[E_FLAT_TYPES],
	const struct ETileChunk *chunks, int n)
{
	(void) dst; (void) flats; (void) chunks; (void) n;
}

uint32_t eb_convert(const uint8_t *raw, uint32_t size, uint8_t *out, uint32_t room)
{
	return e_convert_shape(raw, size, out, room);
}

void eb_finish(void)
{
}

void eb_log(const char *text)
{
	fputs(text, stderr);
}

uint32_t eb_clock(void)
{
	return (uint32_t) clock();
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) (v >> 24);
	p[1] = (uint8_t) (v >> 16);
	p[2] = (uint8_t) (v >> 8);
	p[3] = (uint8_t) v;
}

/* CHUNKS.DAT: the core's records of every world chunk. */
static int write_chunks(const char *data, const char *out_dir)
{
	static uint8_t memory[8 << 20];
	char prefix[1024], path[1024];
	static uint8_t head[E_CHUNKS_TABLE + (192 * 192 + 1) * 4], record[E_CHUNK_MOST];
	uint32_t at = sizeof head, most_statics = 0, total = 0;
	FILE *f;
	int r;

	snprintf(prefix, sizeof prefix, "%s/", data);
	if ((r = e_init(prefix, memory, sizeof memory)) != 0) {
		fprintf(stderr, "e_init: %d\n", r);
		return 1;
	}
	snprintf(path, sizeof path, "%s/CHUNKS.DAT", out_dir);
	if ((f = fopen(path, "wb")) == NULL || fwrite(head, 1, sizeof head, f) != sizeof head) {
		fprintf(stderr, "cannot write %s\n", path);
		return 1;
	}
	memset(head, 0, sizeof head);
	memcpy(head, E_CHUNKS_TITLE, strlen(E_CHUNKS_TITLE));
	for (int i = 0; i < 192 * 192; i++) {
		uint32_t n = e_chunk_record(i % 192, i / 192, record);
		uint32_t statics = (uint32_t) record[0] << 8 | record[1];
		if (statics > most_statics)
			most_statics = statics;
		total += statics;
		put_be32(head + E_CHUNKS_TABLE + i * 4, at);
		if (fwrite(record, 1, n, f) != n) {
			fprintf(stderr, "cannot write %s\n", path);
			return 1;
		}
		at += n;
	}
	put_be32(head + E_CHUNKS_TABLE + 192 * 192 * 4, at);
	if (fseek(f, 0, SEEK_SET) != 0 || fwrite(head, 1, sizeof head, f) != sizeof head
		|| fclose(f) != 0) {
		fprintf(stderr, "cannot write %s\n", path);
		return 1;
	}
	printf("%s: %u bytes; %u fixed statics, at most %u in a chunk\n", path, at, total,
		most_statics);
	return 0;
}

#define TILE_TYPES 150

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
	p[2] = (uint8_t) (v >> 16);
	p[3] = (uint8_t) (v >> 24);
}

static uint8_t *slurp(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;

	if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (*size = ftell(f)) < 0x80
		|| fseek(f, 0, SEEK_SET) != 0 || (b = malloc((size_t) *size)) == NULL
		|| fread(b, 1, (size_t) *size, f) != (size_t) *size) {
		fprintf(stderr, "cannot read %s\n", path);
		exit(1);
	}
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	char path[1024];
	long in_size;
	uint8_t *in, *out, *back;
	uint32_t count, at, made = 0, objects = 0, refused = 0, raw_total = 0;
	FILE *f;

	if (argc != 3) {
		fprintf(stderr, "usage: u7jagdata <data dir> <out dir>\n");
		return 2;
	}
	snprintf(path, sizeof path, "%s/STATIC/SHAPES.VGA", argv[1]);
	in = slurp(path, &in_size);
	count = rd32(in + 0x54);
	if (0x80 + count * 8u > (uint32_t) in_size) {
		fprintf(stderr, "%s: not a flex file\n", path);
		return 1;
	}
	/* no object shape grows past twice its size and a little (engine.c's bound) */
	size_t room = 0x80 + count * 8u;
	for (uint32_t t = 0; t < count; t++)
		room += rd32(in + 0x80 + t * 8 + 4) * 2u + 2 * E_CONVERT_SLACK + 8;
	if ((out = calloc(1, room)) == NULL || (back = malloc(2 * room)) == NULL)
		return 1;
	memcpy(out, in, 0x80);
	memset(out, 0, 0x50);
	memcpy(out, E_SHAPES_TITLE, strlen(E_SHAPES_TITLE));
	at = 0x80 + count * 8u;
	at = (at + 7) & ~7u;
	for (uint32_t t = 0; t < count; t++) {
		uint32_t from = rd32(in + 0x80 + t * 8), size = rd32(in + 0x80 + t * 8 + 4), n = 0;
		if (size != 0 && from + size > (uint32_t) in_size) {
			fprintf(stderr, "shape %u: past the file's end\n", t);
			return 1;
		}
		if (size == 0)
			n = 0;
		else if (t < TILE_TYPES) {
			memcpy(out + at, in + from, size);
			n = size;
		} else {
			objects++;
			raw_total += size;
			n = e_convert_shape(in + from, size, out + at, (uint32_t) (room - at));
			if (n == 0)
				refused++;
			else {
				/* to the Jaguar's order, checked by turning it back */
				if (e_little_endian()) {
					memcpy(back, out + at, n);
					if (!e_swap_shape(out + at, n, 0)) {
						fprintf(stderr, "shape %u: not well formed\n", t);
						return 1;
					}
					memcpy(back + n, out + at, n);
					if (!e_swap_shape(back + n, n, 1) || memcmp(back, back + n, n) != 0) {
						fprintf(stderr, "shape %u: does not come back the same\n", t);
						return 1;
					}
				}
				made += n;
			}
		}
		wr32(out + 0x80 + t * 8, n ? at : 0);
		wr32(out + 0x80 + t * 8 + 4, n);
		at = (at + n + 7) & ~7u;
	}
	snprintf(path, sizeof path, "%s/SHAPES.FLX", argv[2]);
	if ((f = fopen(path, "wb")) == NULL || fwrite(out, 1, at, f) != at || fclose(f) != 0) {
		fprintf(stderr, "cannot write %s\n", path);
		return 1;
	}
	printf("%s: %u bytes; %u object shapes, %u KB made from %u KB (%u refused)\n", path, at,
		objects, made / 1024, raw_total / 1024, refused);
	if (refused != 0)
		return 1;
	return write_chunks(argv[1], argv[2]);
}
