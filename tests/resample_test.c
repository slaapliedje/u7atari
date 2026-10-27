/* Checks the speech resampler. Runs on the build host:
 *   cc -O2 -Iplatform/atari tests/resample_test.c -lm -o resample_test && ./resample_test
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resample.h"

#define IN_RATE 10752
#define OUT_RATE 25033
#define IN_COUNT 20000
#define RING 4096

static uint8_t input[IN_COUNT];
static int8_t whole[IN_COUNT * 3], ring[RING], pieces[IN_COUNT * 3];

int main(void)
{
	resampler r;
	uint32_t at = 0, written, total = 0, out_count;
	int32_t taken = 0;
	double worst = 0;
	int failures = 0;

	for (int i = 0; i < IN_COUNT; i++)
		input[i] = (uint8_t) lround(128 + 100 * sin(2 * M_PI * 1000.0 * i / IN_RATE));

	/* All at once into a buffer big enough. */
	resampler_start(&r, IN_RATE, OUT_RATE);
	taken = resample(&r, input, IN_COUNT, whole, 0xffffffff, &at, sizeof whole, &out_count);
	if (taken != IN_COUNT) {
		printf("took %d of %d\n", taken, IN_COUNT);
		failures++;
	}

	/* Output j stands for input time j * step - 1 (one input sample late). */
	for (uint32_t j = 4; j < out_count - 4; j++) {
		double t = (double) j * r.step / 65536.0 - 1.0;
		double want = 100 * sin(2 * M_PI * 1000.0 * t / IN_RATE);
		double error = fabs(whole[j] - want);

		if (error > worst)
			worst = error;
	}
	/* Linear interpolation of a 1 kHz tone at 10.75 kHz: about 4% of full scale, plus rounding. */
	printf("%u samples out, worst error %.2f of 100\n", (unsigned) out_count, worst);
	if (worst > 6)
		failures++;

	/* In random pieces into a small wrapping ring, drained as it fills: the same samples. */
	srand(1);
	resampler_start(&r, IN_RATE, OUT_RATE);
	at = 0;
	taken = 0;
	while (taken < IN_COUNT) {
		int32_t piece = 1 + rand() % 700;
		uint32_t room = 1 + (uint32_t) (rand() % 900), start = at;

		if (piece > IN_COUNT - taken)
			piece = IN_COUNT - taken;
		taken += resample(&r, input + taken, piece, ring, RING - 1, &at, room, &written);
		for (uint32_t k = 0; k < written; k++)
			pieces[total++] = ring[(start + k) & (RING - 1)];
	}
	/* Whatever the last input still owes. */
	resample(&r, input, 0, ring, RING - 1, &at, RING, &written);
	for (uint32_t k = 0; k < written; k++)
		pieces[total++] = ring[(at - written + k) & (RING - 1)];
	if (total != out_count || memcmp(pieces, whole, out_count) != 0) {
		printf("pieces gave %u samples, differing from the whole\n", (unsigned) total);
		failures++;
	}
	printf(failures ? "FAIL\n" : "ok\n");
	return failures != 0;
}
