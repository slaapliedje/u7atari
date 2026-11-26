/* Big memory copies and fills on Tom's blitter, in phrase mode (8 bytes a memory cycle), in place
 * of the 68000: the link wraps memcpy, memmove and memset (CMakeLists.txt), so every caller gets
 * them, the C library's own included. The register use follows jagwrestle's engine/blit.c.
 *
 * Blitted: 256 bytes or more, both ends in DRAM, source and destination at the same place in a
 * phrase (the 68000 does the bytes before the first phrase and after the last). The block goes
 * as rows of 4096 bytes and a last shorter row. The blitter goes up through memory, so a move
 * onto itself is blitted only downwards. Everything else, and every small one, goes to the C
 * library's own. The 68000 waits for each blit: it gets no bus while one runs anyway.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BREG(off) (*(volatile uint32_t *) (0xF02200UL + (off)))
#define A1_BASE 0x00
#define A1_FLAGS 0x04
#define A1_CLIP 0x08
#define A1_PIXEL 0x0C
#define A1_STEP 0x10
#define A2_BASE 0x24
#define A2_FLAGS 0x28
#define A2_PIXEL 0x30
#define A2_STEP 0x34
#define B_CMD 0x38
#define B_COUNT 0x3C
#define B_PATD 0x68

#define PIXEL8 0x00000018UL
#define WID4096 0x00006000UL        /* width 2^12: exponent 12, mantissa 0, in bits 9-14 */
#define XADDPHR 0x00000000UL
#define SRCEN 0x00000001UL
#define UPDA1 0x00000200UL
#define UPDA2 0x00000400UL
#define PATDSEL 0x00010000UL
#define LFU_REPLACE 0x01800000UL

#define ROW 4096
#ifndef JAG_BLIT_SMALLEST
#define JAG_BLIT_SMALLEST 256
#endif
#define SMALLEST JAG_BLIT_SMALLEST
#define DRAM_END 0x200000UL

void *__real_memcpy(void *dst, const void *src, size_t n);
void *__real_memmove(void *dst, const void *src, size_t n);
void *__real_memset(void *dst, int value, size_t n);

static void blit_wait(void)
{
	while (!(BREG(B_CMD) & 1))
		;
}

/* n bytes from src to dst, both phrase aligned, n a whole number of phrases */
static void blit_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	uint32_t rows = n / ROW, rest = n % ROW;

	blit_wait();
	BREG(A1_FLAGS) = PIXEL8 | WID4096 | XADDPHR;
	BREG(A1_CLIP) = 0;
	BREG(A2_FLAGS) = PIXEL8 | WID4096 | XADDPHR;
	if (rows > 0) {
		BREG(A1_BASE) = (uint32_t) dst;
		BREG(A1_PIXEL) = 0;
		BREG(A1_STEP) = 1UL << 16 | (uint16_t) -ROW;
		BREG(A2_BASE) = (uint32_t) src;
		BREG(A2_PIXEL) = 0;
		BREG(A2_STEP) = 1UL << 16 | (uint16_t) -ROW;
		BREG(B_COUNT) = rows << 16 | ROW;
		BREG(B_CMD) = SRCEN | UPDA1 | UPDA2 | LFU_REPLACE;
		blit_wait();
	}
	if (rest > 0) {
		BREG(A1_BASE) = (uint32_t) (dst + rows * ROW);
		BREG(A1_PIXEL) = 0;
		BREG(A2_BASE) = (uint32_t) (src + rows * ROW);
		BREG(A2_PIXEL) = 0;
		BREG(B_COUNT) = 1UL << 16 | rest;
		BREG(B_CMD) = SRCEN | LFU_REPLACE;
		blit_wait();
	}
}

static void blit_fill(uint8_t *dst, uint8_t value, uint32_t n)
{
	uint32_t pattern = value * 0x01010101UL, rows = n / ROW, rest = n % ROW;

	blit_wait();
	BREG(A1_FLAGS) = PIXEL8 | WID4096 | XADDPHR;
	BREG(A1_CLIP) = 0;
	BREG(B_PATD) = pattern;
	BREG(B_PATD + 4) = pattern;
	if (rows > 0) {
		BREG(A1_BASE) = (uint32_t) dst;
		BREG(A1_PIXEL) = 0;
		BREG(A1_STEP) = 1UL << 16 | (uint16_t) -ROW;
		BREG(B_COUNT) = rows << 16 | ROW;
		BREG(B_CMD) = PATDSEL | UPDA1;
		blit_wait();
	}
	if (rest > 0) {
		BREG(A1_BASE) = (uint32_t) (dst + rows * ROW);
		BREG(A1_PIXEL) = 0;
		BREG(B_COUNT) = 1UL << 16 | rest;
		BREG(B_CMD) = PATDSEL;
		blit_wait();
	}
}

static int in_dram(const void *p, size_t n)
{
	return (uint32_t) p < DRAM_END && n < DRAM_END - (uint32_t) p;
}

/* The bytes up to dst's first phrase, then the phrases on the blitter, then the rest. */
static void *copy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	size_t head = (size_t) (-(uint32_t) d & 7), body;

	__real_memmove(d, s, head);
	d += head;
	s += head;
	n -= head;
	body = n & ~(size_t) 7;
	blit_copy(d, s, body);
	__real_memmove(d + body, s + body, n - body);
	return dst;
}

static int blittable(const void *dst, const void *src, size_t n)
{
	return (((uint32_t) dst ^ (uint32_t) src) & 7) == 0 && in_dram(dst, n)
		&& in_dram(src, n);
}

/* Small ones go straight on: most are, and this is on every one's way. */
void *__wrap_memcpy(void *dst, const void *src, size_t n)
{
	if (n < SMALLEST || !blittable(dst, src, n))
		return __real_memcpy(dst, src, n);
	return copy(dst, src, n);
}

void *__wrap_memmove(void *dst, const void *src, size_t n)
{
	/* upwards over itself: the blitter would read what it had just written */
	if (n < SMALLEST || !blittable(dst, src, n) || ((uint8_t *) dst > (const uint8_t *) src
		&& (uint8_t *) dst < (const uint8_t *) src + n))
		return __real_memmove(dst, src, n);
	return copy(dst, src, n);
}

void *__wrap_memset(void *dst, int value, size_t n)
{
	uint8_t *d = dst;
	size_t head, body;

	if (n < SMALLEST || !in_dram(dst, n))
		return __real_memset(dst, value, n);
	head = (size_t) (-(uint32_t) d & 7);
	__real_memset(d, value, head);
	d += head;
	n -= head;
	body = n & ~(size_t) 7;
	blit_fill(d, (uint8_t) value, body);
	__real_memset(d + body, value, n - body);
	return dst;
}

/* ---- Rectangles ---- */

#define WID320 0x00004200UL         /* width 320: exponent 8, mantissa 1 (1.25 * 256) */

/* w by h pixels from sx, sy in src to dx, dy in dst, both 320 bytes a row, phrase mode: the game's
 * terrain picture into the viewport (drawtile.cpp). Every x and w are multiples of 8, so only the
 * buffers need be on a phrase boundary. */
bool plat_copy_rect(uint8_t *dst, int16_t dx, int16_t dy, const uint8_t *src, int16_t sx,
	int16_t sy, int16_t w, int16_t h)
{
	if ((((uint32_t) dst | (uint32_t) src) & 7) != 0 || !in_dram(dst, 320UL * (dy + h))
		|| !in_dram(src, 320UL * (sy + h)))
		return false;
	if (w <= 0 || h <= 0)
		return true;
	blit_wait();
	BREG(A1_BASE) = (uint32_t) dst;
	BREG(A1_FLAGS) = PIXEL8 | WID320 | XADDPHR;
	BREG(A1_CLIP) = 0;
	BREG(A1_PIXEL) = (uint32_t) dy << 16 | (uint16_t) dx;
	BREG(A1_STEP) = 1UL << 16 | (uint16_t) -w;
	BREG(A2_BASE) = (uint32_t) src;
	BREG(A2_FLAGS) = PIXEL8 | WID320 | XADDPHR;
	BREG(A2_PIXEL) = (uint32_t) sy << 16 | (uint16_t) sx;
	BREG(A2_STEP) = 1UL << 16 | (uint16_t) -w;
	BREG(B_COUNT) = (uint32_t) h << 16 | (uint16_t) w;
	BREG(B_CMD) = SRCEN | UPDA1 | UPDA2 | LFU_REPLACE;
	blit_wait();
	return true;
}
