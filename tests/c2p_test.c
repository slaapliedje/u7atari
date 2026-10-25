/* Checks c2p16 against a bit-by-bit reference. Runs on the build host:
 *   cc -O2 -Iplatform/atari tests/c2p_test.c -o c2p_test && ./c2p_test
 */
#include <stdio.h>
#include <stdlib.h>

#include "c2p.h"

static uint32_t load(const uint8_t *p)
{
	return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static int check(const uint8_t px[16])
{
	uint16_t got[8], want[8] = { 0 };

	for (int p = 0; p < 8; p++)
		for (int i = 0; i < 16; i++)
			if (px[i] >> p & 1)
				want[p] |= (uint16_t) (0x8000 >> i);
	c2p16(load(px), load(px + 4), load(px + 8), load(px + 12), got);
	for (int p = 0; p < 8; p++)
		if (got[p] != want[p]) {
			printf("plane %d: got %04x want %04x\n", p, got[p], want[p]);
			return 0;
		}
	return 1;
}

int main(void)
{
	uint8_t px[16];

	/* every single set bit, then random groups */
	for (int i = 0; i < 16; i++)
		for (int p = 0; p < 8; p++) {
			for (int k = 0; k < 16; k++)
				px[k] = 0;
			px[i] = (uint8_t) (1 << p);
			if (!check(px)) {
				printf("failed: pixel %d bit %d\n", i, p);
				return 1;
			}
		}
	srand(7);
	for (int n = 0; n < 1000000; n++) {
		for (int k = 0; k < 16; k++)
			px[k] = (uint8_t) rand();
		if (!check(px)) {
			printf("failed on random group %d\n", n);
			return 1;
		}
	}
	puts("c2p16: all checks passed");
	return 0;
}
