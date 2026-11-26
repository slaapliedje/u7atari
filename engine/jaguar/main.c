/* The engine test on the Jaguar (engine/README.md): the world view from the U7 data on the
 * GameDrive's SD card (U7/STATIC, U7/GAMEDAT), drawn double-buffered in an 8-bit bitmap object:
 * terrain by the blitter, shapes by the GPU (platform/jaguar/spans_gpu.s, asynchronously).
 *
 * The pad scrolls the view a cell at a time; untouched, it walks a fixed tour so emulator runs
 * measure the same thing. Every 64 frames the frame rate goes to GD_DebugString.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "engine.h"
#include "gd.h"

void shape_spans_draw(const uint8_t *source, int32_t x, int32_t y, const int16_t clip[4],
	const uint8_t *rowTable, uint8_t *linearBase, int32_t mode, int32_t flipped,
	const uint8_t *table);
void shape_spans_finish(void);
void shape_spans_async(int on);
extern volatile int spans_busy;
extern const uint8_t gpu_tiles[], gpu_tiles_end[];
#define G_PC (*(volatile uint32_t *) 0xF02110)
#define G_CTRL (*(volatile uint32_t *) 0xF02114)
#define TILE_PARAMS ((volatile uint32_t *) 0xF03E00)
#define TILE_CODE ((volatile uint32_t *) 0xF03400)

#define REG16(a) (*(volatile uint16_t *) (a))
#define REG32(a) (*(volatile uint32_t *) (a))
#define OLP 0xF00020
#define VMODE 0xF00028
#define BORD1 0xF0002A
#define HDB1 0xF00038
#define HDB2 0xF0003A
#define HDE 0xF0003C
#define VDB 0xF00046
#define VDE 0xF00048
#define VI 0xF0004E
#define BG 0xF00058
#define INT1 0xF000E0
#define CLUT 0xF00400
#define CONFIG 0xF14002
#define LEVEL0 0x100
#define JOY_READ (*(volatile uint32_t *) 0xF14000)
#define JOY_WRITE (*(volatile uint16_t *) 0xF14000)

#define W E_VIEW_W
#define H E_VIEW_H
#define PHRASES (W / 8)

extern void jag_vbl(void);
extern volatile uint32_t *jag_vbl_object;
extern uint32_t jag_vbl_phrase[2];
extern volatile uint32_t jag_ticks;
/* startup.S's profiler: the pc and 32 longs of stack at each blank whose count has none of the
 * mask's bits, written to U7/PCPROF.BIN when the buffer is full (tools/jag-profile.py) */
extern uint8_t *volatile jag_prof_next;
extern uint8_t *volatile jag_prof_end;
extern volatile uint32_t jag_prof_mask;
#define PROFILE_RECORD 132
#define PROFILE_SAMPLES 900
static uint8_t profile[PROFILE_SAMPLES * PROFILE_RECORD] __attribute__((aligned(4)));
static int profile_saved;

static uint32_t gd_bios[1024];
static uint32_t object_list[12] __attribute__((aligned(16)));
static uint8_t frames[2][W * H] __attribute__((aligned(16)));
static uint8_t store[1260 * 1024] __attribute__((aligned(8)));
static uint8_t row_table[H * 4];
static struct ESurface surfaces[2] = {
	{ frames[0], W, W, H, row_table }, { frames[1], W, W, H, row_table },
};
static uint32_t stop_link;

void eb_log(const char *text)
{
	GD_DebugString(text);
}

/* startup.S's exception handler */
void jag_crash(int32_t long_frame, const uint16_t *frame)
{
	char text[64];
	const uint16_t *f = frame + 1 + (long_frame ? 4 : 0);

	snprintf(text, sizeof text, "crash: vector %u at %04x%04x\n", frame[0], f[1], f[2]);
	GD_DebugString(text);
	for (;;)
		;
}

/* ---- files: the GameDrive's, a handful open ---- */

#define FILES 6
static struct { uint16_t gd; uint8_t open; int32_t size; } files[FILES];

int16_t plat_file_open(const char *name, int16_t mode)
{
	(void) mode;
	for (int16_t i = 0; i < FILES; i++)
		if (!files[i].open) {
			uint16_t gd = GD_FOpen(name, GD_FOPEN_READ);
			if ((int16_t) gd < 0)
				return -1;
			files[i].gd = gd;
			files[i].open = 1;
			files[i].size = (int32_t) GD_FSize(gd);
			return i;
		}
	return -1;
}

void plat_file_close(int16_t f)
{
	if (f >= 0 && f < FILES && files[f].open) {
		GD_FClose(files[f].gd);
		files[f].open = 0;
	}
}

int32_t plat_file_read(int16_t f, void *buffer, int32_t count)
{
	shape_spans_finish();
	if ((int16_t) GD_FRead(files[f].gd, buffer, (uint32_t) count, GD_FREAD_CPU) < 0)
		return -1;
	return count;
}

int32_t plat_file_seek(int16_t f, int32_t offset, int16_t origin)
{
	(void) origin;
	if (offset > files[f].size || (int16_t) GD_FSeek(files[f].gd, GD_FSEEK_SET, offset) < 0)
		return -1;
	return offset;
}

int32_t plat_file_length(int16_t f)
{
	return files[f].size;
}

/* ---- the blitter ---- */

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
#define PIXEL8 0x00000018UL
/* The blitter's width field (bits 9-14): exponent << 2 | mantissa, for (4 + m) / 4 * 2^e; 0 for
 * a width it cannot have. */
static uint32_t width_code(uint16_t w)
{
	for (uint32_t e = 2; e < 16; e++)
		for (uint32_t m = 0; m < 4; m++)
			if (((4 + m) << e) >> 2 == w)
				return (e << 2 | m) << 9;
	return 0;
}
#define XADDPHR 0x00000000UL
#define SRCEN 0x00000001UL
#define UPDA1 0x00000200UL
#define UPDA2 0x00000400UL
#define LFU_REPLACE 0x01800000UL

static void blit_wait(void)
{
	while (!(BREG(B_CMD) & 1))
		;
}

void eb_copy(struct ESurface *dst, int16_t dx, int16_t dy, const struct ESurface *src, int16_t sx,
	int16_t sy, int16_t w, int16_t h)
{
	if (w <= 0 || h <= 0)
		return;
	shape_spans_finish();
	blit_wait();
	BREG(A1_BASE) = (uint32_t) dst->pixels;
	BREG(A1_FLAGS) = PIXEL8 | width_code(dst->pitch) | XADDPHR;
	BREG(A1_CLIP) = 0;
	BREG(A1_PIXEL) = (uint32_t) dy << 16 | (uint16_t) dx;
	BREG(A1_STEP) = 1UL << 16 | (uint16_t) -w;
	BREG(A2_BASE) = (uint32_t) src->pixels;
	BREG(A2_FLAGS) = PIXEL8 | width_code(src->pitch) | XADDPHR;
	BREG(A2_PIXEL) = (uint32_t) sy << 16 | (uint16_t) sx;
	BREG(A2_STEP) = 1UL << 16 | (uint16_t) -w;
	BREG(B_COUNT) = (uint32_t) h << 16 | (uint16_t) w;
	BREG(B_CMD) = SRCEN | UPDA1 | UPDA2 | LFU_REPLACE;
	blit_wait();
}

void eb_shape(struct ESurface *dst, const int16_t clip[4], const uint8_t *spans, int16_t x,
	int16_t y, uint8_t flipped)
{
	shape_spans_draw(spans, x, y, clip, dst->rows, dst->pixels, 0, flipped, NULL);
}

/* Terrain on the GPU (tiles_gpu.s): its code beside the span drawer's, loaded once; it runs while
 * the 68000 goes on, as a shape would (spans.c's busy flag, waited on before the next). */
static const uint8_t black_tile[64] __attribute__((aligned(8)));

void eb_tiles(struct ESurface *dst, const struct EFlat flats[E_FLAT_TYPES],
	const struct ETileChunk *chunks, int n)
{
	static int loaded;

	if (n <= 0)
		return;
	shape_spans_finish();
	if (!loaded) {
		const uint32_t *code = (const uint32_t *) gpu_tiles;
		uint32_t words = (uint32_t) (gpu_tiles_end - gpu_tiles + 3) / 4;
		for (uint32_t i = 0; i < words; i++)
			TILE_CODE[i] = code[i];
		loaded = 1;
	}
	TILE_PARAMS[0] = (uint32_t) flats;
	TILE_PARAMS[1] = dst->pitch;
	TILE_PARAMS[2] = (uint32_t) n;
	TILE_PARAMS[3] = (uint32_t) black_tile;
	for (int c = 0; c < n; c++) {
		TILE_PARAMS[4 + c * 4] = (uint32_t) chunks[c].cells;
		TILE_PARAMS[5 + c * 4] = (uint32_t) chunks[c].to;
		TILE_PARAMS[6 + c * 4] = chunks[c].across;
		TILE_PARAMS[7 + c * 4] = chunks[c].down;
	}
	G_PC = 0xF03400;
	G_CTRL = 1;
	spans_busy = 1;
}

void eb_finish(void)
{
	shape_spans_finish();
}

uint32_t eb_clock(void)
{
	return jag_ticks;
}

/* ---- video: the bitmap object, shown from one frame while the other is drawn ---- */

static void branch(uint32_t *object, uint32_t link, uint16_t ypos, uint32_t condition)
{
	object[0] = link >> 11;
	object[1] = link << 21 | condition << 14 | (uint32_t) ypos << 3 | 3;
}

/* From the next vertical blank on, the bitmap shows pixels. */
static void show(const uint8_t *pixels)
{
	uint32_t ticks = jag_ticks;

	jag_vbl_phrase[0] = (uint32_t) pixels << 8 | stop_link >> 11;
	while (jag_ticks == ticks)
		;
}

static void video_init(void)
{
	int ntsc = (REG16(CONFIG) & 0x10) != 0;
	uint16_t width = ntsc ? 1409 : 1381, hmid = ntsc ? 823 : 843;
	uint16_t height = ntsc ? 241 : 287, vmid = ntsc ? 266 : 322;
	uint16_t hde = (uint16_t) ((width / 2 - 1) | 0x400), hdb = (uint16_t) (hmid - width / 2 + 4);
	uint16_t vdb = (uint16_t) (vmid - height), vde = (uint16_t) (vmid + height);
	uint32_t stop = (uint32_t) &object_list[8], data = (uint32_t) frames[0];
	uint16_t ypos = (uint16_t) ((height - H + vdb) & ~1);
	uint16_t xpos = (uint16_t) ((width / 4 - W) / 2);

	stop_link = stop;
	REG16(HDB1) = hdb;
	REG16(HDB2) = hdb;
	REG16(HDE) = hde;
	REG16(VDB) = vdb;
	REG16(VDE) = 0xFFFF;
	REG32(BORD1) = 0;
	REG16(BG) = 0;
	branch(&object_list[0], stop, vde, 2);
	branch(&object_list[2], stop, vdb, 1);
	object_list[4] = data << 8 | stop >> 11;
	object_list[5] = stop << 21 | (uint32_t) H << 14 | (uint32_t) ypos << 3;
	object_list[6] = PHRASES >> 4;
	object_list[7] = (uint32_t) PHRASES << 28 | (uint32_t) PHRASES << 18 | 1 << 15 | 3 << 12 | xpos;
	object_list[8] = 0;
	object_list[9] = 4 | 8;
	jag_vbl_phrase[0] = object_list[4];
	jag_vbl_phrase[1] = object_list[5];
	jag_vbl_object = &object_list[4];
	volatile uint32_t *level0 = (volatile uint32_t *) LEVEL0;
	__asm__ ("" : "+a" (level0));
	*level0 = (uint32_t) jag_vbl;
	REG16(VI) = vde | 1;
	REG16(INT1) = 1;
	REG16(VMODE) = 0;
	REG32(OLP) = (uint32_t) object_list << 16 | (uint32_t) object_list >> 16;
	REG16(VMODE) = 0x6C7;                        /* RGB16 CLUT, 320 wide, on */
	__asm__ volatile ("andi.w #0xF8FF, %%sr" ::: "cc");
}

static void set_palette(const uint8_t *rgb)
{
	volatile uint16_t *clut = (volatile uint16_t *) CLUT;

	for (int i = 0; i < 256; i++, rgb += 3)
		clut[i] = (uint16_t) ((rgb[0] & 0x3e) << 10 | (rgb[2] & 0x3e) << 5 | (rgb[1] & 0x3f));
}

/* ---- the pad: directions only (jagwrestle's layout: U D L R at bits 20-23) ---- */

static uint32_t pad(void)
{
	JOY_WRITE = 0x81FE;
	return ~(JOY_READ >> 4) & 0x00F00000UL;
}

/* ---- the test ---- */

static uint32_t rng = 12345;
static int rnd(int n)
{
	rng = rng * 1103515245u + 12345u;
	return (int) ((rng >> 16) % (uint32_t) n);
}

void jag_main(void)
{
	/* a tour of Britain: east along a street, then south, then back */
	static const struct { int8_t dx, dy; int16_t n; } tour[] = {
		{ 1, 0, 64 }, { 0, 1, 48 }, { -1, 0, 64 }, { 0, -1, 48 },
	};
	char text[256];
	int16_t x = 880, y = 1330;
	struct { int16_t x, y; int dir; } walker[E_MOVERS];
	int nm = 12, leg = 0, step = 0, r;
	uint32_t frame = 0, mark_ticks, mark_frame = 0;

	GD_Install(gd_bios);
	e_surface_rows(&surfaces[0]);
	video_init();
	if ((r = e_init("U7/", store, sizeof store)) != 0) {
		snprintf(text, sizeof text, "e_init failed: %d\n", r);
		GD_DebugString(text);
		for (;;)
			;
	}
	set_palette(e_palette());
	for (int m = 0; m < nm; m++) {
		walker[m].x = (int16_t) (x + 4 + rnd(32));
		walker[m].y = (int16_t) (y + 4 + rnd(20));
		walker[m].dir = rnd(4);
		e_mover_add(721, walker[m].x, walker[m].y, 0);
	}
	shape_spans_async(1);
	jag_prof_mask = 1;
	jag_prof_end = profile + sizeof profile;
	jag_prof_next = profile;
	GD_DebugString("engine test: running\n");
	mark_ticks = jag_ticks;
	for (;;) {
		uint32_t held = pad();

		if (held) {
			x = (int16_t) (x + (held & (1UL << 23) ? 1 : 0) - (held & (1UL << 22) ? 1 : 0));
			y = (int16_t) (y + (held & (1UL << 21) ? 1 : 0) - (held & (1UL << 20) ? 1 : 0));
		} else {
			x = (int16_t) (x + tour[leg].dx);
			y = (int16_t) (y + tour[leg].dy);
			if (++step >= tour[leg].n) {
				step = 0;
				leg = (leg + 1) % 4;
			}
		}
		for (int m = 0; m < nm; m++) {
			static const int8_t ddx[4] = { 0, 1, 0, -1 }, ddy[4] = { -1, 0, 1, 0 };
			if (rnd(8) == 0)
				walker[m].dir = rnd(4);
			walker[m].x = (int16_t) (walker[m].x + ddx[walker[m].dir] + tour[leg].dx);
			walker[m].y = (int16_t) (walker[m].y + ddy[walker[m].dir] + tour[leg].dy);
			e_mover_set(m, e_walk_frame(721, walker[m].dir, (int) frame), walker[m].x,
				walker[m].y, 0);
		}
		if (!profile_saved && jag_prof_next >= jag_prof_end) {
			uint16_t f = GD_FOpen("U7/PCPROF.BIN", GD_FOPEN_WRITE | GD_FOPEN_CREATE_ALWAYS);
			profile_saved = 1;
			if ((int16_t) f >= 0) {
				GD_FWrite(f, profile, sizeof profile);
				GD_FClose(f);
				GD_DebugString("profile saved\n");
			}
		}
		struct ESurface *drawing = &surfaces[(frame + 1) & 1];
		e_set_view(x, y);
		e_draw(drawing);
		show(drawing->pixels);
		frame++;
		if ((frame & 63) == 0) {
			uint32_t now = jag_ticks, tenths = (frame - mark_frame) * 600 / (now - mark_ticks);
			snprintf(text, sizeof text, "fps %lu.%lu (frame %lu, blank %lu) rebuilds %lu statics %lu "
				"edges %lu drawn %lu shapes %luK loads %lu; blanks: build %lu copy %lu objects %lu "
				"finish %lu\n", (unsigned long) tenths / 10,
				(unsigned long) tenths % 10, (unsigned long) frame, (unsigned long) now,
				(unsigned long) e_stats.rebuilds, (unsigned long) e_stats.statics,
				(unsigned long) e_stats.edges, (unsigned long) e_stats.drawn,
				(unsigned long) e_stats.shape_bytes / 1024, (unsigned long) e_stats.shape_loads,
				(unsigned long) e_stats.build_time, (unsigned long) e_stats.copy_time,
				(unsigned long) e_stats.object_time, (unsigned long) e_stats.finish_time);
			e_stats.build_time = e_stats.copy_time = e_stats.object_time = e_stats.finish_time = 0;
			GD_DebugString(text);
			mark_ticks = now;
			mark_frame = frame;
		}
	}
}
