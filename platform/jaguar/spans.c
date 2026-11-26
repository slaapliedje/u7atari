/* Shape drawing in copy mode on the GPU: the game's U7_SHAPE_SPANS_68K hook (shapedraw.cpp in the
 * fork) comes here, and spans_gpu.s draws the frame. The GPU's code is copied into its RAM the
 * first time; then each frame is posted in its parameter block and the 68000 waits for it to
 * stop, as the C it replaces would have kept the 68000 busy anyway. The GPU outranks the 68000
 * on the bus, so the 68000's polling costs it little.
 */
#include <stdint.h>

extern const uint8_t gpu_spans[], gpu_spans_end[];

#define G_FLAGS (*(volatile uint32_t *) 0xF02100)
#define G_PC (*(volatile uint32_t *) 0xF02110)
#define G_CTRL (*(volatile uint32_t *) 0xF02114)
#define G_RAM ((volatile uint32_t *) 0xF03000)
#define G_PARAMS ((volatile uint32_t *) 0xF03F80)
#define GPUGO 1

static int loaded;

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

void shape_spans_copy(const uint8_t *source, int32_t x, int32_t y, const int16_t clip[4],
	const uint8_t *rowTable, uint8_t *linearBase)
{
	if (!loaded)
		load();
	G_PARAMS[0] = (uint32_t) source;
	G_PARAMS[1] = (uint32_t) x;
	G_PARAMS[2] = (uint32_t) y;
	G_PARAMS[3] = (uint32_t) (int32_t) clip[0];
	G_PARAMS[4] = (uint32_t) (int32_t) clip[1];
	G_PARAMS[5] = (uint32_t) (int32_t) clip[2];
	G_PARAMS[6] = (uint32_t) (int32_t) clip[3];
	G_PARAMS[7] = (uint32_t) rowTable;
	G_PARAMS[8] = (uint32_t) linearBase;
	G_PC = 0xF03000;
	G_CTRL = GPUGO;
	while (G_CTRL & GPUGO)
		;
}
