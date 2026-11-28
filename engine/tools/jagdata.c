/* The engine's dataset, made on the PC from the original's files (engine/README.md):
 *
 *     u7jagdata <data dir> <out dir>
 *
 * with <data dir> holding STATIC/, writes into <out dir> (engine.h):
 *   SHAPES.FLX  SHAPES.VGA with every object shape in the engine's form (E_SHAPES_TITLE)
 *   CHUNKS.DAT  every world chunk's fixed statics, binned, as the engine makes them: the engine's
 *               own core does it here, shown STATIC/ alone (E_CHUNKS_TITLE)
 *   NPC.DAT     a new game's NPCs, and U7IREGxx its movable items, as INITGAME.DAT holds them
 *   ENDSHAPE.FLX  the intro's pictures (STATIC/ENDSHAPE.FLX), each in the engine's form as a
 *               picture (E_PICTURES_TITLE: a 32-byte head, then its converted parts; a picture
 *               wider than the converter reaches cut in bands, its frame 0 only)
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
	if (mode == PLAT_WRITE)
		return -1;
	if (strstr(name, "GAMEDAT/") != NULL || strstr(name, "ENGINE/") != NULL)
		return -1;
	for (int16_t i = 0; i < 16; i++)
		if (files[i] == NULL)
			return (files[i] = fopen(name, "rb")) != NULL ? i : -1;
	return -1;
}

int32_t plat_file_write(int16_t f, const void *buffer, int32_t count)
{
	(void) f;
	(void) buffer;
	(void) count;
	return -1;                  /* (the tool writes nothing through the engine) */
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

int eb_movers(struct ESurface *frame, const struct EMoverDraw *list, int n, int16_t vx,
	int16_t vy, const void *const *shapes)
{
	(void) frame; (void) list; (void) n; (void) vx; (void) vy; (void) shapes;
	return 0;
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

void eb_music_ring(uint8_t *ring, uint32_t size)
{
	(void) ring;
	(void) size;
}

uint32_t eb_music_read(void)
{
	return 0;
}

void eb_music_written(uint32_t wr)
{
	(void) wr;
}

void eb_music_restart(void)
{
}

int eb_music_ended(void)
{
	return 0;
}

int eb_sound(const uint8_t *samples, uint32_t n, int volume, int pan)
{
	(void) samples;
	(void) n;
	(void) volume;
	(void) pan;
	return -1;
}

void eb_sound_stop(int channel)
{
	(void) channel;
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

static uint32_t rd32(const uint8_t *p);
static uint8_t *slurp(const char *path, long *size);

/* NPC.DAT: INITGAME.DAT's entry of that name (each entry its 13-byte name, then the file). */
static int write_npcs(const char *data, const char *out_dir)
{
	char path[1024];
	long size;
	uint8_t *in;
	FILE *f;

	snprintf(path, sizeof path, "%s/STATIC/INITGAME.DAT", data);
	in = slurp(path, &size);
	uint32_t count = rd32(in + 0x54);
	int written = 0;
	for (uint32_t i = 0; i < count && 0x80 + i * 8 + 8 <= (uint32_t) size; i++) {
		uint32_t at = rd32(in + 0x80 + i * 8), n = rd32(in + 0x84 + i * 8);
		char name[14];
		if (n < 13 || at + n > (uint32_t) size)
			continue;
		memcpy(name, in + at, 13);
		name[13] = 0;
		for (char *c = name; *c; c++)
			if (*c >= 'a' && *c <= 'z')
				*c = (char) (*c - 'a' + 'A');
		if (strcmp(name, "NPC.DAT") != 0 && strncmp(name, "U7IREG", 6) != 0)
			continue;
		snprintf(path, sizeof path, "%s/%s", out_dir, name);
		if ((f = fopen(path, "wb")) == NULL || fwrite(in + at + 13, 1, n - 13, f) != n - 13
			|| fclose(f) != 0) {
			fprintf(stderr, "cannot write %s\n", path);
			return 1;
		}
		written++;
	}
	free(in);
	printf("%s/NPC.DAT and U7IREG: %d files from INITGAME.DAT\n", out_dir, written);
	return written == 0;
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

static void wr16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
}

static void be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t) (v >> 8);
	p[1] = (uint8_t) v;
}

static void be32(uint8_t *p, uint32_t v)
{
	be16(p, (uint16_t) (v >> 16));
	be16(p + 2, (uint16_t) v);
}

/* The intro's pictures: STATIC/ENDSHAPE.FLX's shapes made pictures (engine.h, e_picture_), each
 * entry a head (big-endian: parts, width, height, 0, each part's place in the room after the head
 * and its hot spot's from the picture's) and the room, its parts in the Jaguar's order. */
static int write_pictures(const char *data, const char *dir)
{
	char path[1024];
	long in_size;
	FILE *f;

	snprintf(path, sizeof path, "%s/STATIC/ENDSHAPE.FLX", data);
	uint8_t *in = slurp(path, &in_size);
	uint32_t count = rd32(in + 0x54), room_size = 0x80 + count * 8u, largest = 0;
	for (uint32_t t = 0; t < count; t++) {
		uint32_t size = rd32(in + 0x80 + t * 8 + 4);
		room_size += size * 3u + 4 * E_CONVERT_SLACK + E_PICTURE_HEAD + 8;
		if (size > largest)
			largest = size;
	}
	uint8_t *out = calloc(1, room_size), *raw = malloc(largest + 8), *cut = malloc(largest * 2 + 1024);
	if (out == NULL || raw == NULL || cut == NULL)
		return 1;
	memcpy(out, in, 0x80);
	memset(out, 0, 0x50);
	memcpy(out, E_PICTURES_TITLE, strlen(E_PICTURES_TITLE));
	uint32_t at = (0x80 + count * 8u + 7) & ~7u, pictures = 0;
	for (uint32_t t = 0; t < count; t++) {
		uint32_t from = rd32(in + 0x80 + t * 8), size = rd32(in + 0x80 + t * 8 + 4), n = 0;
		if (size >= 16 && from + size <= (uint32_t) in_size) {
			memcpy(raw, in + from, size);
			/* (the original reads its frame offsets in real mode: their low 20 bits; one of
			 * ENDSHAPE's has stray high bits) */
			uint32_t frames = (rd32(raw + 4) & 0xfffff) / 4 - 1;
			for (uint32_t k = 0; k < frames; k++)
				wr32(raw + 4 + 4 * k, rd32(raw + 4 + 4 * k) & 0xfffff);
			struct EPicture p;
			uint8_t *room = out + at + E_PICTURE_HEAD;
			if (!e_picture_convert(raw, size, cut, largest * 2 + 1024, room, room_size - at - E_PICTURE_HEAD, &p)) {
				fprintf(stderr, "ENDSHAPE %u: not made a picture\n", t);
				return 1;
			}
			n = E_PICTURE_HEAD + e_picture_used(room, &p);
			for (int k = 0; k < p.parts; k++) {
				uint32_t psize = ((uint32_t *) (room + p.at[k]))[0];
				if (e_little_endian() && !e_swap_shape(room + p.at[k], psize, 0)) {
					fprintf(stderr, "ENDSHAPE %u: not well formed\n", t);
					return 1;
				}
			}
			uint8_t *h = out + at;
			be16(h, p.parts);
			be16(h + 2, (uint16_t) p.w);
			be16(h + 4, (uint16_t) p.h);
			be16(h + 6, 0);
			for (int k = 0; k < E_PICTURE_PARTS; k++) {
				be32(h + 8 + 4 * k, k < p.parts ? p.at[k] : 0);
				be16(h + 16 + 2 * k, (uint16_t) (k < p.parts ? p.dx[k] : 0));
				be16(h + 20 + 2 * k, (uint16_t) (k < p.parts ? p.dy[k] : 0));
			}
			be16(h + 24, (uint16_t) p.left);
			be16(h + 26, (uint16_t) p.above);
			be16(h + 28, 0);
			be16(h + 30, 0);
			pictures++;
		}
		wr32(out + 0x80 + t * 8, n ? at : 0);
		wr32(out + 0x80 + t * 8 + 4, n);
		at = (at + n + 7) & ~7u;
	}
	(void) wr16;
	snprintf(path, sizeof path, "%s/ENDSHAPE.FLX", dir);
	if ((f = fopen(path, "wb")) == NULL || fwrite(out, 1, at, f) != at || fclose(f) != 0) {
		fprintf(stderr, "cannot write %s\n", path);
		return 1;
	}
	printf("%s: %u bytes, %u pictures\n", path, at, pictures);
	return 0;
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
	return write_chunks(argv[1], argv[2]) || write_npcs(argv[1], argv[2]) || write_pictures(argv[1], argv[2]);
}
