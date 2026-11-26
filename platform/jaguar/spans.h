/* The GPU's shape drawing (spans.c). It may still be drawing a frame when the game goes on
 * (shapedraw.h); SPANS_FENCE waits for it before the 68000, the blitter or the drive touches
 * memory it may be using: anything copied or filled, the screen, a shape being read in. */
#ifndef JAGUAR_SPANS_H
#define JAGUAR_SPANS_H

extern volatile int spans_busy;
void shape_spans_finish(void);

#define SPANS_FENCE() do { if (spans_busy) shape_spans_finish(); } while (0)

#endif
