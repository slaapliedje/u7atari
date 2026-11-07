/* Checks platform/atari/c2p.S against c2p16 (c2p.h), on the Atari (or Hatari): converts random
 * frames whole, then changes random groups, runs of groups and rows and converts again, with and
 * without the doubled lines, comparing the planes and the shadow with c2p16's every time. Then
 * times a whole frame, an unchanged one, and one where every group changed, each way. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mint/osbind.h>

#include "c2p.h"

#define W 320
#define H 200

void c2p_frame(const uint8_t *in, uint8_t *shadow, uint8_t *out, int32_t rows, int32_t outStride,
	int32_t secondLine, int32_t full);

static uint32_t seed = 12345;

static uint32_t random_below(uint32_t n)
{
	seed = seed * 1103515245 + 12345;
	return (seed >> 8) % n;
}

static uint32_t load32(const uint8_t *p)
{
	return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

/* The planes c2p16 makes of the frame, line by line, each line repeat times. */
static void reference(const uint8_t *in, uint8_t *out, int repeat)
{
	for (int y = 0; y < H; y++)
		for (int r = 0; r < repeat; r++)
			for (int g = 0; g < W / 16; g++) {
				const uint8_t *p = in + y * W + g * 16;
				uint16_t words[8];

				c2p16(load32(p), load32(p + 4), load32(p + 8), load32(p + 12), words);
				memcpy(out + (y * repeat + r) * W + g * 16, words, 16);
			}
}

int main(void)
{
	static uint8_t frame[W * H], other[W * H], shadow[W * H], want[W * H * 2];
	/* the screen is in ST RAM, as in the game */
	uint8_t *out = (uint8_t *) Mxalloc(W * H * 2, 0);
	long checks = 0, bad = 0;

	if (out == NULL || (long) out < 0) {
		printf("no ST RAM\n");
		return 1;
	}

	for (int repeat = 1; repeat <= 2; repeat++) {
		int32_t stride = repeat * W, second = repeat == 2 ? W : 0;

		for (int i = 0; i < W * H; i++)
			frame[i] = (uint8_t) random_below(256);
		memset(out, 0, W * H * 2);
		c2p_frame(frame, shadow, out, H, stride, second, 1);
		for (int round = 0; round < 60; round++, checks++) {
			int changes = (int) random_below(40);

			if (round > 0)
				for (int c = 0; c < changes; c++) {
					int at = (int) random_below(W * H), length;

					switch (random_below(4)) {
					case 0: length = 1; break;                          /* one pixel */
					case 1: length = 1 + (int) random_below(48); break; /* a run */
					case 2: at -= at % W; length = W; break;            /* a row */
					default: at = W * H - 1; length = 1; break;         /* the last pixel */
					}
					for (int i = at; i < at + length && i < W * H; i++)
						frame[i] = (uint8_t) random_below(256);
				}
			if (round > 0)
				c2p_frame(frame, shadow, out, H, stride, second, 0);
			reference(frame, want, repeat);
			if (memcmp(out, want, (size_t) (W * H * repeat)) != 0 || memcmp(shadow, frame, W * H) != 0) {
				if (bad++ < 10)
					printf("mismatch: repeat %d round %d\n", repeat, round);
			}
		}
	}
	printf("%ld frames checked, %ld mismatched\n", checks, bad);

	for (int i = 0; i < W * H; i++)
		other[i] = (uint8_t) ~frame[i];
	for (int way = 0; way < 6; way++) {
		clock_t start = clock();

		for (int n = 0; n < 50; n++)
			switch (way) {
			case 0: reference(frame, want, 1); break;
			case 1: c2p_frame(frame, shadow, out, H, W, 0, 1); break;
			case 2: {   /* as video.c compared before c2p.S */
				const uint32_t *a = (const uint32_t *) frame, *b = (const uint32_t *) shadow;

				for (int g = 0; g < W * H / 16; g++, a += 4, b += 4)
					if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3])
						break;
				break;
			}
			case 3: c2p_frame(frame, shadow, out, H, W, 0, 0); break;
			/* walking: every group differs from the last frame */
			case 4: c2p_frame(n & 1 ? other : frame, shadow, out, H, W, 0, 0); break;
			case 5: c2p_frame(n & 1 ? other : frame, shadow, out, H, 2 * W, W, 0); break;
			}
		printf("%s: %ld ticks of 1/%d s for 50 frames\n",
			(const char *[]) { "C whole", "asm whole", "C compare unchanged", "asm unchanged",
				"asm all changed", "asm all changed, lines doubled" }[way],
			(long) (clock() - start), (int) CLOCKS_PER_SEC);
	}
	return bad != 0;
}
