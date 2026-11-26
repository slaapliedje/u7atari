/* Shape drawing on the GPU: the game's U7_SHAPE_SPANS_BACKEND hook (shapedraw.cpp in the fork)
 * comes here for every frame of every mode, and spans_gpu.s draws it. The GPU's code is copied
 * into its RAM the first time; then each frame is posted in its parameter block. While the game
 * asks for it (the world's items) the 68000 goes on and waits only before the next frame or
 * anything else that touches what the GPU uses; otherwise it waits for the GPU to stop. The GPU
 * outranks the 68000 on the bus, so the 68000's polling costs it little.
 */
#include <stdint.h>

extern const uint8_t gpu_spans[], gpu_spans_end[];

void shape_spans_finish(void);

#define G_FLAGS (*(volatile uint32_t *) 0xF02100)
#define G_PC (*(volatile uint32_t *) 0xF02110)
#define G_CTRL (*(volatile uint32_t *) 0xF02114)
#define G_RAM ((volatile uint32_t *) 0xF03000)
#define G_PARAMS ((volatile uint32_t *) 0xF03F80)
#define GPUGO 1

static int loaded, async;
/* The GPU has a frame of ours under way; blit.c, files.c and jaguar.c test it before touching
 * memory the GPU may be reading or writing. */
volatile int spans_busy;

static void load(void)
{
	const uint32_t *code = (const uint32_t *) gpu_spans;
	uint32_t n = (uint32_t) (gpu_spans_end - gpu_spans + 3) / 4;

	G_CTRL = 0;
	G_FLAGS = 0;                    /* no interrupts, register bank 0 */
	for (uint32_t i = 0; i < n; i++)
		G_RAM[i] = code[i];
	loaded = 1;
}

/* Draws a frame from its first span in mode (0 copy, 1 translate, 2 blend through table), flipped
 * or not, as DrawSpans<Mode, Flipped> would. clip is the view's x0, y0, x1, y1. Flipped, spans run
 * down the screen: the along axis is y, the across x. */
void shape_spans_draw(const uint8_t *source, int32_t x, int32_t y, const int16_t clip[4],
	const uint8_t *rowTable, uint8_t *linearBase, int32_t mode, int32_t flipped,
	const uint8_t *table)
{
	int a = flipped ? 1 : 0, c = 1 - a;

	if (!loaded)
		load();
	shape_spans_finish();
	G_PARAMS[0] = (uint32_t) source;
	G_PARAMS[1] = (uint32_t) (flipped ? y : x);
	G_PARAMS[2] = (uint32_t) (flipped ? x : y);
	G_PARAMS[3] = (uint32_t) (int32_t) clip[a];
	G_PARAMS[4] = (uint32_t) (int32_t) clip[c];
	G_PARAMS[5] = (uint32_t) (int32_t) clip[2 + a];
	G_PARAMS[6] = (uint32_t) (int32_t) clip[2 + c];
	G_PARAMS[7] = (uint32_t) rowTable;
	G_PARAMS[8] = (uint32_t) linearBase;
	G_PARAMS[9] = (uint32_t) table;
	G_PARAMS[10] = (uint32_t) mode;
	G_PARAMS[11] = (uint32_t) (flipped != 0);
	G_PARAMS[12] = flipped ? 320 : 1;
	G_PC = 0xF03000;
	G_CTRL = GPUGO;
	spans_busy = 1;
	if (!async)
		shape_spans_finish();
}

/* Waits for the GPU to finish the frame it is drawing, if any. */
void shape_spans_finish(void)
{
	if (spans_busy) {
		while (G_CTRL & GPUGO)
			;
		spans_busy = 0;
	}
}

/* On, a frame is left drawing while the 68000 goes on (the world's items: shapedraw.h); off,
 * waits for the last. */
void shape_spans_async(int on)
{
	async = on;
	if (!on)
		shape_spans_finish();
}
