// Checks platform/atari/shapes.S against the game's C shape drawing, on the Atari (or Hatari):
// draws every frame of every shape in SHAPES.VGA in copy mode, unflipped, at random positions
// and clip boxes, with DrawShapeFrame (built without U7_SHAPE_SPANS_68K) on one screen and
// shape_spans_copy on another, and compares the screens after each shape. Then times both.
//   tests/shapes_test.prg [path to SHAPES.VGA]
#include "u7port.h"
#include "arena.h"
#include "byteorder.h"
#include "view.h"
#include "shapedraw.h"
#include <time.h>

extern "C" void shape_spans_copy(const uint8_t *source, int32_t x, int32_t y, const int16_t clip[4],
	const uint8_t *rowTable, uint8_t *linearBase);

uint8_t *LinearBase;
uint32_t LinearSize;
View ScreenView, Viewport;

static uint32_t seed = 12345;

static uint32_t random_below(uint32_t n)
{
	seed = seed * 1103515245 + 12345;
	return (seed >> 8) % n;
}

static void draw_asm(View *view, int16_t x, int16_t y, int32_t shape, int16_t frame)
{
	uint16_t entry = (uint16_t) ((frame + 1) * 4);

	if (LinearGet16(shape + 4) < entry)
		return;
	shape_spans_copy(LINEAR(shape + (int32_t) LinearGet32(shape + entry) + 8), x, y, &view->clip.x0,
		LINEAR(view->rowTable), LinearBase);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "C:\\U7\\STATIC\\SHAPES.VGA";
	const int32_t screenA = 0x10000, screenB = 0x30000, rowsA = 0x50000, rowsB = 0x51000;
	const int32_t shapes = 0x100000;
	FILE *f = fopen(path, "rb");
	long frames = 0, draws = 0, bad = 0;
	uint32_t size, count;

	if (f == NULL) {
		printf("cannot open %s\n", path);
		return 2;
	}
	LinearSize = 8 << 20;
	LinearBase = (uint8_t *) calloc(LinearSize, 1);
	size = (uint32_t) fread(LINEAR(shapes), 1, LinearSize - shapes, f);
	fclose(f);
	for (int i = 0; i < 200; i++) {
		LinearPut32(rowsA + i * 4, screenA + i * 320);
		LinearPut32(rowsB + i * 4, screenB + i * 320);
	}
	for (int i = 0; i < 64000; i++)
		LINEAR(screenA)[i] = LINEAR(screenB)[i] = (uint8_t) random_below(256);

	// The Flex header: the entry count at 0x54, then offset and size pairs from 0x80.
	count = LinearGet32(shapes + 0x54);
	for (uint32_t s = 150; s < count; s++) {
		uint32_t off = LinearGet32(shapes + 0x80 + s * 8), length = LinearGet32(shapes + 0x84 + s * 8);
		int32_t shape = shapes + (int32_t) off;
		int nframes;

		if (off == 0 || length == 0 || off + length > size)
			continue;
		nframes = (int) (LinearGet32(shape + 4) - 4) / 4;
		for (int16_t fr = 0; fr < nframes; fr++, frames++) {
			for (int t = 0; t < 2; t++, draws++) {
				View a, b;

				a.rowTable = rowsA;
				b.rowTable = rowsB;
				if (t == 0) {
					a.clip = (Rect) { 0, 0, 319, 199 };
				} else {
					a.clip.x0 = (int16_t) random_below(160);
					a.clip.x1 = (int16_t) (a.clip.x0 + random_below(320 - a.clip.x0));
					a.clip.y0 = (int16_t) random_below(100);
					a.clip.y1 = (int16_t) (a.clip.y0 + random_below(200 - a.clip.y0));
				}
				b.clip = a.clip;
				int16_t x = (int16_t) ((int) random_below(400) - 40);
				int16_t y = (int16_t) ((int) random_below(280) - 40);
				DrawShapeFrame(&a, x, y, shape, fr, false, SHAPE_COPY, 0);
				draw_asm(&b, x, y, shape, fr);
			}
		}
		if (memcmp(LINEAR(screenA), LINEAR(screenB), 64000) != 0) {
			if (bad++ < 10)
				printf("mismatch after shape %lu\n", (unsigned long) s);
			memcpy(LINEAR(screenB), LINEAR(screenA), 64000);
		}
	}
	printf("%ld frames, %ld draws, %ld shapes mismatched\n", frames, draws, bad);

	// Timing: the same draws, full-screen clip, each way.
	for (int way = 0; way < 2; way++) {
		View v;
		clock_t start = clock();

		v.rowTable = rowsA;
		v.clip = (Rect) { 0, 0, 319, 199 };
		seed = 1;
		for (uint32_t s = 150; s < count; s++) {
			uint32_t off = LinearGet32(shapes + 0x80 + s * 8), length = LinearGet32(shapes + 0x84 + s * 8);
			int32_t shape = shapes + (int32_t) off;

			if (off == 0 || length == 0 || off + length > size)
				continue;
			for (int16_t fr = 0; fr < (int) (LinearGet32(shape + 4) - 4) / 4; fr++) {
				int16_t x = (int16_t) random_below(320), y = (int16_t) random_below(200);

				if (way == 0)
					DrawShapeFrame(&v, x, y, shape, fr, false, SHAPE_COPY, 0);
				else
					draw_asm(&v, x, y, shape, fr);
			}
		}
		printf("%s: %ld ticks of 1/%d s\n", way == 0 ? "C" : "asm", (long) (clock() - start),
			(int) CLOCKS_PER_SEC);
	}
	return bad != 0;
}
