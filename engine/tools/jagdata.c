/* The engine's dataset, made on the PC from the original's files (engine/README.md):
 *
 *     u7jagdata <static dir> <out dir>
 *
 * writes <out dir>/SHAPES.FLX: SHAPES.VGA with every object shape in the engine's form, in the
 * Jaguar's byte order (engine.h, E_SHAPES_TITLE). Put it on the card as U7/ENGINE/SHAPES.FLX.
 * The original's files are only read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

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
		fprintf(stderr, "usage: u7jagdata <static dir> <out dir>\n");
		return 2;
	}
	snprintf(path, sizeof path, "%s/SHAPES.VGA", argv[1]);
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
	return refused != 0;
}
