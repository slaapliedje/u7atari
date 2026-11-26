/* Shapes in the engine's form (engine.h): made from the original's, and turned to the other byte
 * order. Needs nothing of the engine or a backend, so the dataset tool (engine/tools/jagdata.c)
 * links it too. */
#include <string.h>

#include "engine.h"

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
static inline int16_t rds16(const uint8_t *p) { return (int16_t) rd16(p); }
static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static uint8_t *conv_out;           /* NULL: only measuring */
static uint32_t conv_n;

static void emit32(uint32_t v)
{
	if (conv_out != NULL)
		*(uint32_t *) (conv_out + conv_n) = v;
	conv_n += 4;
}

static void emit16(uint16_t v)
{
	if (conv_out != NULL)
		*(uint16_t *) (conv_out + conv_n) = v;
	conv_n += 2;
}

/* A run of raw pixels, split at 255; 0 when its place cannot be put in a run's header. */
static int emit_raw(int across, int along, const uint8_t *px, int len)
{
	while (len > 0) {
		int l = len > 255 ? 255 : len;
		if (across < -128 || across > 127 || along < -128 || along > 127)
			return 0;
		emit32((uint32_t) (uint8_t) across << 24 | (uint32_t) (uint8_t) along << 16
			| (uint32_t) l << 8);
		if (conv_out != NULL) {
			memcpy(conv_out + conv_n, px, (size_t) l);
			memset(conv_out + conv_n + l, 0, (size_t) ((4 - (l & 3)) & 3));
		}
		conv_n += (uint32_t) (l + 3) & ~3u;
		px += l;
		along += l;
		len -= l;
	}
	return 1;
}

/* engine.h. The Jaguar's GPU does the same (engine/jaguar/convert_gpu.s). */
uint32_t e_convert_shape(const uint8_t *s, uint32_t size, uint8_t *out, uint32_t room)
{
	static uint8_t pending[255];
	uint32_t first, frames;

	conv_out = out;
	conv_n = 0;
	if (size < 8 || rd32(s) != size)
		return 0;
	first = rd32(s + 4);
	if (first < 8 || first > size || (first & 3))
		return 0;
	frames = (first - 4) / 4;
	emit32(0);
	for (uint32_t f = 0; f < frames; f++)
		emit32(0);
	for (uint32_t f = 0; f < frames; f++) {
		uint32_t p = rd32(s + 4 + f * 4);
		if (out != NULL)
			((uint32_t *) out)[1 + f] = conv_n;
		if (p + 8 > size)
			return 0;
		for (int i = 0; i < 4; i++)
			emit16((uint16_t) rds16(s + p + i * 2));
		p += 8;
		for (;;) {
			if (p + 2 > size || (out != NULL && conv_n + E_CONVERT_SLACK > room))
				return 0;
			uint16_t head = rd16(s + p);
			if (head == 0)
				break;
			if (p + 6 > size)
				return 0;
			int len = head >> 1, along = rds16(s + p + 2), across = rds16(s + p + 4);
			p += 6;
			if (!(head & 1)) {
				if (p + (uint32_t) len > size || !emit_raw(across, along, s + p, len))
					return 0;
				p += (uint32_t) len;
				continue;
			}
			/* run-encoded: raw pieces and short fills gathered, long fills apart */
			int pos = 0, start = 0, n = 0;
			while (pos < len) {
				if (p >= size)
					return 0;
				int c = s[p] >> 1, fill = s[p] & 1;
				p++;
				if (fill && c >= E_FILL_SHORTEST) {
					if (p >= size)
						return 0;
					if (n > 0 && !emit_raw(across, along + start, pending, n))
						return 0;
					n = 0;
					if (across < -128 || across > 127 || along + pos < -128
						|| along + pos > 127 || c > 255)
						return 0;
					emit32((uint32_t) (uint8_t) across << 24
						| (uint32_t) (uint8_t) (along + pos) << 16 | (uint32_t) c << 8
						| E_RUN_FILL);
					emit32(s[p] * 0x01010101u);
					p++;
					start = pos + c;
				} else {
					if (p + (fill ? 1u : (uint32_t) c) > size)
						return 0;
					for (int i = 0; i < c; i++) {
						if (n == (int) sizeof pending) {
							if (!emit_raw(across, along + start, pending, n))
								return 0;
							start += n;
							n = 0;
						}
						pending[n++] = fill ? s[p] : s[p + (uint32_t) i];
					}
					p += fill ? 1u : (uint32_t) c;
				}
				pos += c;
			}
			if (n > 0 && !emit_raw(across, along + start, pending, n))
				return 0;
		}
		emit32(0);
	}
	if (out != NULL)
		((uint32_t *) out)[0] = conv_n;
	return conv_n;
}


static inline uint32_t swap32(uint32_t v)
{
	return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24;
}

/* engine.h. */
int e_swap_shape(uint8_t *s, uint32_t size, int foreign)
{
	uint32_t *l = (uint32_t *) s, first, frames;

	if (size < 8 || (size & 3))
		return 0;
	if (foreign) {
		l[0] = swap32(l[0]);
		l[1] = swap32(l[1]);
	}
	first = l[1];
	if (l[0] != size || first < 8 || first > size || (first & 3))
		return 0;
	frames = (first - 4) / 4;
	for (uint32_t f = 0; f < frames; f++) {
		uint32_t p;
		if (f > 0 && foreign)
			l[1 + f] = swap32(l[1 + f]);
		p = l[1 + f];
		if (f > 0 && !foreign)
			l[1 + f] = swap32(p);
		if ((p & 3) || p < first || p + 12 > size)
			return 0;
		for (int i = 0; i < 4; i++) {
			uint8_t t = s[p + i * 2];
			s[p + i * 2] = s[p + i * 2 + 1];
			s[p + i * 2 + 1] = t;
		}
		p += 8;
		for (;;) {
			uint32_t h;
			if (p + 4 > size)
				return 0;
			h = foreign ? swap32(l[p / 4]) : l[p / 4];
			l[p / 4] = swap32(l[p / 4]);
			p += 4;
			if (h == 0)
				break;
			p += h & E_RUN_FILL ? 4 : (E_RUN_LENGTH(h) + 3) & ~3u;
		}
	}
	if (!foreign) {
		l[0] = swap32(l[0]);
		l[1] = swap32(l[1]);
	}
	return 1;
}

int e_little_endian(void)
{
	const uint16_t one = 1;

	return *(const uint8_t *) &one;
}
