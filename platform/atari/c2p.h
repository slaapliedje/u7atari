/* Chunky to planar: 16 pixels of one byte each to the 8 interleaved bitplane words both the
 * TT (TT low) and the Falcon (8 bits a pixel) show.
 *
 * a, b, c and d hold pixels 0-3, 4-7, 8-11 and 12-15, the first pixel of each in the top byte
 * (how a 68000 reads four chunky bytes as a long). out[p] receives plane p: bit 15 is pixel 0.
 *
 * The 128 bits are addressed by 7 bits: which long (2) and which bit in it (5). The input is
 * (long = ~i3 ~i2 | bit = ~i1 ~i0 p2 p1 p0) for pixel i and plane p, taking the longs as d, c, b, a;
 * the output is (long = p0 p1 | bit = p2 ~i3 ~i2 ~i1 ~i0). Each step exchanges two address bits:
 * MERGE between two longs, SWAP_BITS within one.
 */
#ifndef C2P_H
#define C2P_H

#include <stdint.h>

/* Exchanges bit k of the long index with bit k of the position, s = 1 << k, mask = positions
 * with bit k clear; x is the long whose index bit is 0. */
#define C2P_MERGE(x, y, mask, s) do { \
	uint32_t t_ = (((x) >> (s)) ^ (y)) & (mask); \
	(y) ^= t_; \
	(x) ^= t_ << (s); \
} while (0)

/* Exchanges position bits k < m within one long, s = (1 << m) - (1 << k), mask = positions with
 * bit k set and bit m clear. */
#define C2P_SWAP_BITS(x, mask, s) do { \
	uint32_t t_ = (((x) >> (s)) ^ (x)) & (mask); \
	(x) ^= t_ ^ (t_ << (s)); \
} while (0)

static inline void c2p16(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint16_t out[8])
{
	uint32_t r0 = d, r1 = c, r2 = b, r3 = a;

	/* long bit 1 <-> position bit 0, then long bit 0 <-> position bit 1 */
	C2P_MERGE(r0, r2, 0x55555555u, 1);
	C2P_MERGE(r1, r3, 0x55555555u, 1);
	C2P_MERGE(r0, r1, 0x33333333u, 2);
	C2P_MERGE(r2, r3, 0x33333333u, 2);
	/* position bits 1 <-> 4, 0 <-> 3, 2 <-> 4 */
	C2P_SWAP_BITS(r0, 0x0000ccccu, 14);
	C2P_SWAP_BITS(r1, 0x0000ccccu, 14);
	C2P_SWAP_BITS(r2, 0x0000ccccu, 14);
	C2P_SWAP_BITS(r3, 0x0000ccccu, 14);
	C2P_SWAP_BITS(r0, 0x00aa00aau, 7);
	C2P_SWAP_BITS(r1, 0x00aa00aau, 7);
	C2P_SWAP_BITS(r2, 0x00aa00aau, 7);
	C2P_SWAP_BITS(r3, 0x00aa00aau, 7);
	C2P_SWAP_BITS(r0, 0x0000f0f0u, 12);
	C2P_SWAP_BITS(r1, 0x0000f0f0u, 12);
	C2P_SWAP_BITS(r2, 0x0000f0f0u, 12);
	C2P_SWAP_BITS(r3, 0x0000f0f0u, 12);

	/* plane p: long (p0 p1), high word when p2 */
	out[0] = (uint16_t) r0;
	out[1] = (uint16_t) r2;
	out[2] = (uint16_t) r1;
	out[3] = (uint16_t) r3;
	out[4] = (uint16_t) (r0 >> 16);
	out[5] = (uint16_t) (r2 >> 16);
	out[6] = (uint16_t) (r1 >> 16);
	out[7] = (uint16_t) (r3 >> 16);
}

#endif
