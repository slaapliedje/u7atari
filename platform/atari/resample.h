/* Linear-interpolation resampling of the game's unsigned 8-bit speech into the signed 8-bit DMA
 * ring. Header-only so tests/resample_test.c can check it on the build host. */
#ifndef ATARI_RESAMPLE_H
#define ATARI_RESAMPLE_H

#include <stdint.h>

typedef struct {
	uint32_t step;          /* input samples per output sample, 16.16 */
	uint32_t phase;         /* where the next output lies past previous, 16.16; 1.0 at a start */
	int16_t previous, current;
} resampler;

static inline void resampler_start(resampler *r, uint32_t in_rate, uint32_t out_rate)
{
	r->step = (uint32_t) (((uint64_t) in_rate << 16) / out_rate);
	r->phase = 0x10000;
	r->previous = r->current = 0;
}

/* Writes up to room samples into ring from *at (wrapping by mask), taking input as needed.
 * Returns how many input samples it took; *written gets how many it wrote. */
static inline int32_t resample(resampler *r, const uint8_t *samples, int32_t count,
	int8_t *ring, uint32_t mask, uint32_t *at, uint32_t room, uint32_t *written)
{
	int32_t taken = 0;
	uint32_t out = 0, pos = *at;

	while (out < room) {
		while (r->phase >= 0x10000) {
			if (taken == count)
				goto done;
			r->previous = r->current;
			r->current = (int16_t) (samples[taken++] - 128);
			r->phase -= 0x10000;
		}
		ring[pos] = (int8_t) (r->previous +
			(((int32_t) (r->current - r->previous) * (int32_t) r->phase) >> 16));
		pos = (pos + 1) & mask;
		r->phase += r->step;
		out++;
	}
done:
	*at = pos;
	*written = out;
	return taken;
}

#endif
