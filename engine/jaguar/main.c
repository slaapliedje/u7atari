/* The engine test on the Jaguar (engine/README.md): the world view from the U7 data on the
 * GameDrive's SD card (U7/STATIC, U7/GAMEDAT), drawn double-buffered in an 8-bit bitmap object:
 * terrain and shapes by the GPU (tiles_gpu.s, shapes_gpu.s), while the 68000 goes on.
 *
 * The pad scrolls the view a cell at a time; untouched, it walks a fixed tour so emulator runs
 * measure the same thing. Every 64 frames the frame rate goes to GD_DebugString.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "engine.h"
#include "gd.h"

extern const uint8_t gpu_tiles[], gpu_tiles_end[], gpu_shapes[], gpu_shapes_end[];
extern const uint8_t gpu_convert[], gpu_convert_end[], gpu_movers[], gpu_movers_end[];
extern const uint8_t dsp_sound[], dsp_sound_end[];
#define G_FLAGS (*(volatile uint32_t *) 0xF02100)
#define G_PC (*(volatile uint32_t *) 0xF02110)
#define G_CTRL (*(volatile uint32_t *) 0xF02114)
#define TILE_PARAMS ((volatile uint32_t *) 0xF03E00)
#define TILE_CODE ((volatile uint32_t *) 0xF03400)
#define SHAPE_PARAMS ((volatile uint32_t *) 0xF03F80)
#define SHAPE_CODE ((volatile uint32_t *) 0xF03000)
#define CONVERT_PARAMS ((volatile uint32_t *) 0xF03F00)
#define CONVERT_CODE ((volatile uint32_t *) 0xF03500)
#define MOVER_PARAMS ((volatile uint32_t *) 0xF03F40)
#define MOVER_CODE ((volatile uint32_t *) 0xF03860)
#define GPUGO 1

static void gpu_finish(void);

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
#define VC 0xF00006
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
extern uint32_t *volatile jag_vbl_list;
extern uint32_t *volatile jag_vbl_live;
extern volatile uint32_t jag_ticks;
/* startup.S's profiler: the pc and 32 longs of stack at each tick of Tom's timer (set going in
 * jag_main), written to U7/PCPROF.BIN when the buffer is full (tools/jag-profile.py) */
extern uint8_t *volatile jag_prof_next;
extern uint8_t *volatile jag_prof_end;
extern volatile uint32_t jag_prof_mask;
extern volatile uint32_t jag_prof_pit;
extern volatile uint16_t jag_int_enable;
#ifdef JAG_PROF_FRAMES
#define PROFILE_RECORD 260      /* and 32 return addresses up the frame pointers' chain */
#define PROFILE_SAMPLES 450
#else
#define PROFILE_RECORD 132
#define PROFILE_SAMPLES 900
#endif
static uint8_t profile[PROFILE_SAMPLES * PROFILE_RECORD] __attribute__((aligned(4)));
static int profile_saved;

/* The tour scrolls a cell every SCROLL_FRAMES frames (1: 60 cells a second, far past walking) */
#ifndef SCROLL_FRAMES
#define SCROLL_FRAMES 1
#endif

static uint32_t gd_bios[1024];
/* The object list: two branches (above and below the picture: stop), up to OBJECTS bitmaps, a
 * stop. The bitmaps are put back at each blank from one of obj_lists (startup.S,
 * jag_vbl_list). */
#define OBJECTS 5
#define STOP_AT (4 + OBJECTS * 4)
static uint32_t object_list[STOP_AT + 2] __attribute__((aligned(16)));
static uint32_t obj_lists[2][1 + OBJECTS * 4];
static uint8_t list_next;
static uint16_t obj_x, obj_y;   /* the picture's top left: x in pixels, y in half-lines */
/* the ring's window to show under the frame (eb_view), or none (ring NULL: it was copied) */
static const struct ESurface *view_ring;
static int16_t view_sx, view_sy;
/* Three frames: one shown, one waiting for the next blank, one being drawn; so the 68000 goes on
 * drawing instead of waiting for a blank (ENGINE_DOUBLE_BUFFER: two, waiting). */
#ifdef ENGINE_DOUBLE_BUFFER
#define FRAMES 2
#else
#define FRAMES 3
#endif
static uint8_t frames[FRAMES][W * H] __attribute__((aligned(16)));
static uint8_t store[1396 * 1024] __attribute__((aligned(8)));
static uint8_t row_table[H * 4];
static struct ESurface surfaces[FRAMES] = {
	{ frames[0], W, W, H, row_table }, { frames[1], W, W, H, row_table },
#if FRAMES == 3
	{ frames[2], W, W, H, row_table },
#endif
};

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
	gpu_finish();
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
#define PATDSEL 0x00010000UL
#define B_PATD 0x68

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
	gpu_finish();
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
	/* not waited for: the GPU's shapes wait for the blitter before each run, and eb_finish */
}

/* ---- sounds: four channels mixed by the DSP (sound_dsp.s) at 15980 Hz ---- */

#define D_PC (*(volatile uint32_t *) 0xF1A110)
#define D_CTRL (*(volatile uint32_t *) 0xF1A114)
#define SCLK (*(volatile uint32_t *) 0xF1A150)
#define SMODE (*(volatile uint32_t *) 0xF1A154)
#define SOUND_CHANNELS 4
/* each: the next sample's address, the end's, the left volume, the right (0-255) */
#define SOUND_CHANNEL ((volatile uint32_t *) 0xF1CF00)

static void sound_init(void)
{
	const uint32_t *from = (const uint32_t *) dsp_sound;
	volatile uint32_t *to = (volatile uint32_t *) 0xF1B000;

	D_CTRL = 0;
	for (uint32_t i = 0; i < (uint32_t) (dsp_sound_end - dsp_sound + 3) / 4; i++)
		to[i] = from[i];
	for (int i = 0; i < SOUND_CHANNELS * 4; i++)
		SOUND_CHANNEL[i] = 0;
	/* the I2S clock: 26.59 MHz / (64 * (25 + 1)), 15980 Hz (E_SOUND_RATE); internal, word
	 * strobe on, the interrupt each word pair */
	SCLK = 25;
	SMODE = 0x15;
	D_PC = 0xF1B000;
	D_CTRL = 1;
}

void eb_sound(const uint8_t *samples, uint32_t n, int volume, int pan)
{
	int c = 0;
	uint32_t least = 0xffffffff;

	/* a free channel, else the one nearest its end */
	for (int k = 0; k < SOUND_CHANNELS; k++) {
		uint32_t at = SOUND_CHANNEL[k * 4], end = SOUND_CHANNEL[k * 4 + 1];
		uint32_t left = end > at ? end - at : 0;
		if (left < least) {
			least = left;
			c = k;
		}
	}
	volatile uint32_t *ch = &SOUND_CHANNEL[c * 4];
	ch[1] = 0;
	ch[2] = (uint32_t) (volume * (127 - pan) * 2 / 127);
	ch[3] = (uint32_t) (volume * pan * 2 / 127);
	ch[0] = (uint32_t) samples;
	ch[1] = (uint32_t) samples + n * 2;
}

/* ---- the GPU: shapes_gpu.s and tiles_gpu.s, each run while the 68000 goes on ----
 *
 * Shapes are put in a list. The GPU is started on the list whenever it is idle, and the next is
 * filled meanwhile; anything that touches what it draws or reads waits for it all (gpu_finish). */

#define LIST_ENTRIES 128
struct GpuShape {
	const uint32_t *runs;
	uint8_t *pixels;
	int32_t along, across, along_min, across_min, along_max, across_max, along_step, across_step;
};
/* the GPU loads longs: aligned past the 2 bytes the 68000's compiler gives */
static struct GpuShape lists[2][LIST_ENTRIES] __attribute__((aligned(8)));
static int filling, queued, gpu_busy;
/* rows of this many pixels or more go to the blitter */
static uint32_t blit_shortest = 16;

static int gpu_loaded;

static void gpu_load(void)
{
	if (gpu_loaded)
		return;
	G_CTRL = 0;
	G_FLAGS = 0;                    /* no interrupts, register bank 0 */
	for (uint32_t i = 0; i < (uint32_t) (gpu_shapes_end - gpu_shapes + 3) / 4; i++)
		SHAPE_CODE[i] = ((const uint32_t *) gpu_shapes)[i];
	for (uint32_t i = 0; i < (uint32_t) (gpu_tiles_end - gpu_tiles + 3) / 4; i++)
		TILE_CODE[i] = ((const uint32_t *) gpu_tiles)[i];
	for (uint32_t i = 0; i < (uint32_t) (gpu_convert_end - gpu_convert + 3) / 4; i++)
		CONVERT_CODE[i] = ((const uint32_t *) gpu_convert)[i];
	for (uint32_t i = 0; i < (uint32_t) (gpu_movers_end - gpu_movers + 3) / 4; i++)
		MOVER_CODE[i] = ((const uint32_t *) gpu_movers)[i];
	gpu_loaded = 1;
}

static void gpu_wait(void)
{
	if (gpu_busy) {
		while (G_CTRL & GPUGO)
			;
		gpu_busy = 0;
	}
}

static void gpu_start(uint32_t pc)
{
	G_PC = pc;
	G_CTRL = GPUGO;
	gpu_busy = 1;
}

/* The list filled so far handed to the GPU, once it is done with what it has. */
static void gpu_flush(void)
{
	if (queued == 0)
		return;
	gpu_wait();
	SHAPE_PARAMS[0] = (uint32_t) lists[filling];
	SHAPE_PARAMS[1] = (uint32_t) queued;
	SHAPE_PARAMS[2] = blit_shortest;
	gpu_start(0xF03000);
	filling ^= 1;
	queued = 0;
}

static void gpu_finish(void)
{
	gpu_flush();
	gpu_wait();
}

void eb_shape(struct ESurface *dst, const int16_t clip[4], const uint32_t *runs, int16_t x,
	int16_t y, uint8_t flipped)
{
	struct GpuShape *g;

#ifdef ENGINE_NO_FRAME_SHAPES
	if (dst->pitch == W)
		return;
#endif
	gpu_load();
	if (queued == LIST_ENTRIES)
		gpu_flush();
	g = &lists[filling][queued++];
	g->runs = runs;
	g->pixels = dst->pixels;
	if (!flipped) {
		g->along = x;
		g->across = y;
		g->along_min = clip[0];
		g->across_min = clip[1];
		g->along_max = clip[2];
		g->across_max = clip[3];
		g->along_step = 1;
		g->across_step = dst->pitch;
	} else {
		g->along = y;
		g->across = x;
		g->along_min = clip[1];
		g->across_min = clip[0];
		g->along_max = clip[3];
		g->across_max = clip[2];
		g->along_step = dst->pitch;
		g->across_step = 1;
	}
	if (!gpu_busy || !(G_CTRL & GPUGO))
		gpu_flush();
#ifdef ENGINE_SHAPES_WAIT
	gpu_finish();
#endif
}

/* Terrain (tiles_gpu.s): after the shapes asked for before it. */
static const uint8_t black_tile[64] __attribute__((aligned(8)));

void eb_tiles(struct ESurface *dst, const struct EFlat flats[E_FLAT_TYPES],
	const struct ETileChunk *chunks, int n)
{
	if (n <= 0)
		return;
	gpu_load();
	gpu_finish();
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
	gpu_start(0xF03400);
}

void eb_finish(void)
{
	gpu_finish();
	blit_wait();
}

/* The movers by the GPU (movers_gpu.s): their pictures, their order, and the list for them and
 * their fronts, then drawn (shapes_gpu.s); the 68000 goes on. ENGINE_C_MOVERS: the core does it. */
#ifndef ENGINE_C_MOVERS
#define MOVER_LIST 256
static struct GpuShape mover_list[MOVER_LIST] __attribute__((aligned(8)));
static uint32_t mover_seen[E_MOVERS * 16] __attribute__((aligned(8)));
#endif

int eb_movers(struct ESurface *frame, const struct EMoverDraw *list, int n, int16_t vx,
	int16_t vy, const void *const *shapes)
{
#ifdef ENGINE_C_MOVERS
	(void) frame; (void) list; (void) n; (void) vx; (void) vy; (void) shapes;
	return 0;
#else
	gpu_load();
	gpu_finish();
	MOVER_PARAMS[0] = (uint32_t) list;
	MOVER_PARAMS[1] = (uint32_t) n;
	MOVER_PARAMS[2] = (uint32_t) shapes;
	MOVER_PARAMS[3] = (uint32_t) (int32_t) vx;
	MOVER_PARAMS[4] = (uint32_t) (int32_t) vy;
	MOVER_PARAMS[5] = (uint32_t) frame->pixels;
	MOVER_PARAMS[6] = frame->pitch;
	MOVER_PARAMS[7] = (uint32_t) mover_list;
	MOVER_PARAMS[8] = MOVER_LIST;
	MOVER_PARAMS[9] = (uint32_t) mover_seen;
	MOVER_PARAMS[10] = blit_shortest;
	gpu_start((uint32_t) MOVER_CODE);
	return 1;
#endif
}

/* A shape converted by the GPU (convert_gpu.s), waited for: what is drawn next may use it. Built
 * with ENGINE_CHECK_CONVERT, the core's C does each again and a difference is logged. */
uint32_t eb_convert(const uint8_t *raw, uint32_t size, uint8_t *out, uint32_t room)
{
	static uint32_t dummy[2] __attribute__((aligned(8)));
	uint32_t made;

	gpu_load();
	gpu_finish();
	CONVERT_PARAMS[0] = (uint32_t) raw;
	CONVERT_PARAMS[1] = (uint32_t) (out != NULL ? out : (uint8_t *) dummy);
	CONVERT_PARAMS[2] = out != NULL ? 0xFFFFFFFFUL : 0;
	CONVERT_PARAMS[3] = 0;
	/* (measuring, the offsets are masked to 0: never past it) */
	CONVERT_PARAMS[4] = out != NULL && room > E_CONVERT_SLACK ? room - E_CONVERT_SLACK : 0;
	gpu_start(0xF03500);
	gpu_wait();
	made = CONVERT_PARAMS[3];
#ifdef ENGINE_CHECK_CONVERT
	static int checked, differ;
	if (out != NULL && checked < 24) {
		uint32_t gpu_sum = 0, c_sum = 0, again;
		char text[96];
		for (uint32_t i = 0; out != NULL && i < made; i++)
			gpu_sum = gpu_sum * 31 + out[i];
		again = e_convert_shape(raw, size, out, room);
		for (uint32_t i = 0; out != NULL && i < again; i++)
			c_sum = c_sum * 31 + out[i];
		if (again != made || gpu_sum != c_sum) {
			snprintf(text, sizeof text, "convert differs: size %lu gpu %lu c %lu\n",
				(unsigned long) size, (unsigned long) made, (unsigned long) again);
			GD_DebugString(text);
			differ++;
		}
		if (++checked == 24) {
			snprintf(text, sizeof text, "convert check: %d of 24 differ\n", differ);
			GD_DebugString(text);
		}
	}
#else
	(void) size;
#endif
	return made;
}

/* The clock: half-lines, from the blanks counted and the vertical count since the last one (the
 * blank's interrupt comes at VC = vc_irq; a field is vc_period half-lines, measured). Kept from
 * going back where the count has passed vc_irq but the interrupt has not yet been taken. */
static uint16_t vc_period = 525, vc_irq;

uint32_t eb_clock(void)
{
	static uint32_t last;
	uint32_t t, now;
	int phase;

	do {
		t = jag_ticks;
		phase = (int) (REG16(VC) & 0x7FF) - vc_irq;
	} while (t != jag_ticks);
	if (phase < 0)
		phase += vc_period;
	/* t * vc_period in two mulu.w (in C, a call to libgcc's 32-bit multiply) */
	uint32_t lo = (uint16_t) t, hi = (uint16_t) (t >> 16);
	__asm__ ("mulu.w %1,%0" : "+d" (lo) : "d" (vc_period));
	__asm__ ("mulu.w %1,%0" : "+d" (hi) : "d" (vc_period));
	now = lo + (hi << 16) + (uint32_t) phase;
	if (now < last)
		now = last;
	last = now;
	return now;
}

/* Clock units in tenths of a blank, for the log. */
static unsigned long tenths(uint32_t units)
{
	return (unsigned long) ((uint64_t) units * 10 / vc_period);
}

/* ---- video: the bitmap object, shown from one frame while the other is drawn ---- */

static void branch(uint32_t *object, uint32_t link, uint16_t ypos, uint32_t condition)
{
	object[0] = link >> 11;
	object[1] = link << 21 | condition << 14 | (uint32_t) ypos << 3 | 3;
}

/* Bitmap object k of n (8-bit pixels, pitch phrases a row; trans: colour 0 clear), at its place
 * in object_list (linked to the next, the last to the stop), made into o. */
static void bitmap(uint32_t *o, int k, int n, const uint8_t *data, uint16_t x, uint16_t y,
	uint16_t w, uint16_t h, uint16_t pitch, int trans)
{
	uint32_t link = (uint32_t) &object_list[k + 1 < n ? 4 + (k + 1) * 4 : STOP_AT];
	uint32_t iwidth = (uint32_t) w / 8, dwidth = (uint32_t) pitch / 8;

	o[0] = (uint32_t) data << 8 | link >> 11;
	o[1] = link << 21 | (uint32_t) h << 14 | (uint32_t) y << 3;
	o[2] = iwidth >> 4 | (trans ? 1UL << 15 : 0);
	o[3] = iwidth << 28 | dwidth << 18 | 1UL << 15 | 3UL << 12 | x;
}

/* The objects for a frame: the ring's window in up to four pieces where it wraps, then pixels
 * over it, 0 clear; or pixels alone (the window copied in). Into one of obj_lists. */
static uint32_t *make_list(const uint8_t *pixels)
{
	uint32_t *l = obj_lists[list_next];
	struct { const uint8_t *data; uint16_t x, y, w, h, pitch; } piece[OBJECTS];
	int n = 0;

	if (view_ring != NULL) {
		const struct ESurface *r = view_ring;
		uint16_t w1 = (uint16_t) (r->w - view_sx < W ? r->w - view_sx : W);
		uint16_t h1 = (uint16_t) (r->h - view_sy < H ? r->h - view_sy : H);
		for (int py = 0; py < 2; py++)
			for (int px = 0; px < 2; px++) {
				uint16_t w = px ? (uint16_t) (W - w1) : w1, h = py ? (uint16_t) (H - h1) : h1;
				if (w == 0 || h == 0)
					continue;
				piece[n].data = r->pixels + (py ? 0 : view_sy) * r->pitch + (px ? 0 : view_sx);
				piece[n].x = (uint16_t) (obj_x + (px ? w1 : 0));
				piece[n].y = (uint16_t) (obj_y + (py ? h1 * 2 : 0));
				piece[n].w = w;
				piece[n].h = h;
				piece[n].pitch = r->pitch;
				n++;
			}
	}
	piece[n].data = pixels;
	piece[n].x = obj_x;
	piece[n].y = obj_y;
	piece[n].w = W;
	piece[n].h = H;
	piece[n].pitch = W;
	n++;
	l[0] = (uint32_t) n * 4;
	for (int k = 0; k < n; k++)
		bitmap(&l[1 + k * 4], k, n, piece[k].data, piece[k].x, piece[k].y, piece[k].w,
			piece[k].h, piece[k].pitch, view_ring != NULL && k == n - 1);
	list_next ^= 1;
	return l;
}

/* From the next vertical blank on, the frame's objects are shown. With three frames, the ones
 * given last time have to have been put up first (a blank since): then the frame given before
 * them is no longer shown, and is the next to be drawn. With two, waits for the blank. */
static uint32_t shown_tick;

static void show(const uint8_t *pixels)
{
	uint32_t *l = make_list(pixels);
#if FRAMES == 3
	while (jag_ticks == shown_tick)
		;
	jag_vbl_list = l;
	shown_tick = jag_ticks;
#else
	uint32_t ticks = jag_ticks;

	jag_vbl_list = l;
	while (jag_ticks == ticks)
		;
#endif
}

/* The ring's window copied into the frame by the blitter; or, built with ENGINE_LAYERS, shown by
 * the object processor itself, the frame a layer over it. Not copying saves 0.2 blanks a frame,
 * but on the timing core the 68000 then runs the movers some 20% slower (the object processor
 * reading the ring's pieces and the layer, line by line), and the layered frames come out
 * slower where movers are in view. */
int eb_view(struct ESurface *frame, const struct ESurface *ring, int16_t sx, int16_t sy)
{
#ifdef ENGINE_LAYERS
	(void) frame;
	view_ring = ring;
	view_sx = sx;
	view_sy = sy;
	return 0;
#else
	view_ring = NULL;
	e_copy_view(frame, ring, sx, sy);
	return 1;
#endif
}

void eb_clear(struct ESurface *dst, int16_t x, int16_t y, int16_t w, int16_t h)
{
	if (w <= 0 || h <= 0)
		return;
	gpu_finish();
	blit_wait();
	BREG(A1_BASE) = (uint32_t) dst->pixels;
	BREG(A1_FLAGS) = PIXEL8 | width_code(dst->pitch) | XADDPHR;
	BREG(A1_CLIP) = 0;
	BREG(A1_PIXEL) = (uint32_t) y << 16 | (uint16_t) x;
	BREG(A1_STEP) = 1UL << 16 | (uint16_t) -w;
	BREG(B_PATD) = 0;
	BREG(B_PATD + 4) = 0;
	BREG(B_COUNT) = (uint32_t) h << 16 | (uint16_t) w;
	BREG(B_CMD) = PATDSEL | UPDA1;
	/* not waited for, as eb_copy */
}

static void video_init(void)
{
	int ntsc = (REG16(CONFIG) & 0x10) != 0;
	uint16_t width = ntsc ? 1409 : 1381, hmid = ntsc ? 823 : 843;
	uint16_t height = ntsc ? 241 : 287, vmid = ntsc ? 266 : 322;
	uint16_t hde = (uint16_t) ((width / 2 - 1) | 0x400), hdb = (uint16_t) (hmid - width / 2 + 4);
	uint16_t vdb = (uint16_t) (vmid - height), vde = (uint16_t) (vmid + height);
	uint32_t stop = (uint32_t) &object_list[STOP_AT];

	obj_y = (uint16_t) ((height - H + vdb) & ~1);
	obj_x = (uint16_t) ((width / 4 - W) / 2);
	REG16(HDB1) = hdb;
	REG16(HDB2) = hdb;
	REG16(HDE) = hde;
	REG16(VDB) = vdb;
	REG16(VDE) = 0xFFFF;
	REG32(BORD1) = 0;
	REG16(BG) = 0;
	branch(&object_list[0], stop, vde, 2);
	branch(&object_list[2], stop, vdb, 1);
	object_list[STOP_AT] = 0;
	object_list[STOP_AT + 1] = 4 | 8;
	/* to start, frame 0 alone */
	uint32_t *l = make_list(frames[0]);
	memcpy(&object_list[4], &l[1], l[0] * 4);
	jag_vbl_live = &object_list[4];
	jag_vbl_list = l;
	volatile uint32_t *level0 = (volatile uint32_t *) LEVEL0;
	__asm__ ("" : "+a" (level0));
	*level0 = (uint32_t) jag_vbl;
	REG16(VI) = vde | 1;
	vc_irq = vde | 1;
	REG16(INT1) = 1;
	REG16(VMODE) = 0;
	REG32(OLP) = (uint32_t) object_list << 16 | (uint32_t) object_list >> 16;
	REG16(VMODE) = 0x6C7;                        /* RGB16 CLUT, 320 wide, on */
	__asm__ volatile ("andi.w #0xF8FF, %%sr" ::: "cc");
	/* a field's length in half-lines: the highest count seen between two blanks */
	uint32_t t = jag_ticks;
	uint16_t top = 0;
	while (jag_ticks == t)
		;
	t = jag_ticks;
	while (jag_ticks == t) {
		uint16_t vc = REG16(VC) & 0x7FF;
		if (vc > top)
			top = vc;
	}
	vc_period = (uint16_t) (top + 1);
}

static void set_palette(const uint8_t *rgb)
{
	volatile uint16_t *clut = (volatile uint16_t *) CLUT;

	for (int i = 0; i < 256; i++, rgb += 3)
		clut[i] = (uint16_t) ((rgb[0] & 0x3e) << 10 | (rgb[2] & 0x3e) << 5 | (rgb[1] & 0x3f));
}

/* ---- the pad: directions only (jagwrestle's layout: U D L R at bits 20-23) ---- */

/* The pad's directions (bits 20-23: up, down, left, right) and A (bit 29), as the port's
 * platform/jaguar/input.c has them. */
#define PAD_A (1UL << 29)
static uint32_t pad(void)
{
	JOY_WRITE = 0x81FE;
	uint32_t v = JOY_READ;
	return (~(v >> 4) & 0x00F00000UL) | (v & 2 ? 0 : PAD_A);
}


#ifdef ENGINE_BENCH_SHAPES
/* Timings logged at the start: the walker's shape (721) converted, and a frame of it drawn 200
 * times, waiting each time, with the blitter given rows from various lengths. */
static uint8_t bench_raw[65536] __attribute__((aligned(8)));

static void bench_shapes(void)
{
	static const int16_t clip[4] = { 0, 0, W - 1, H - 1 };
	static const uint32_t shortest[] = { 8, 16, 24, 32, 1000 };
	uint8_t entry[8];
	char text[160];
	int16_t f = plat_file_open("U7/STATIC/SHAPES.VGA", PLAT_FILE_READ);
	uint32_t at, size, t0, t[2], made = 0, tm, tw;
	const uint32_t *runs = e_frame_runs(721, 0);
	uint8_t *out;

	plat_file_seek(f, 0x80 + 721 * 8, PLAT_SEEK_SET);
	plat_file_read(f, entry, 8);
	at = entry[0] | entry[1] << 8 | (uint32_t) entry[2] << 16 | (uint32_t) entry[3] << 24;
	size = entry[4] | entry[5] << 8 | (uint32_t) entry[6] << 16;
	if (runs == NULL || size > sizeof bench_raw / 2) {
		plat_file_close(f);
		return;
	}
	plat_file_seek(f, (int32_t) at, PLAT_SEEK_SET);
	plat_file_read(f, bench_raw, (int32_t) size);
	plat_file_close(f);
	out = bench_raw + ((size + 15) & ~15u);
	t0 = jag_ticks;
	for (int i = 0; i < 4; i++)
		made = eb_convert(bench_raw, size, NULL, 0);
	tm = jag_ticks - t0;
	t0 = jag_ticks;
	for (int i = 0; i < 4; i++)
		eb_convert(bench_raw, size, out, (uint32_t) (bench_raw + sizeof bench_raw - out));
	tw = jag_ticks - t0;
	snprintf(text, sizeof text, "bench: shape 721, %lu bytes to %lu; blanks for 4: measured %lu, "
		"written %lu\n", (unsigned long) size, (unsigned long) made, (unsigned long) tm,
		(unsigned long) tw);
	GD_DebugString(text);
	for (int k = 0; k < 5; k++) {
		blit_shortest = shortest[k];
		for (int flip = 0; flip < 2; flip++) {
			t0 = jag_ticks;
			for (int i = 0; i < 200; i++) {
				eb_shape(&surfaces[0], clip, runs, (int16_t) (60 + i % 200), 100,
					(uint8_t) flip);
				gpu_finish();
			}
			t[flip] = jag_ticks - t0;
		}
		snprintf(text, sizeof text, "bench: 200 walkers, blitted from %lu: %lu blanks, flipped "
			"%lu\n", (unsigned long) shortest[k], (unsigned long) t[0], (unsigned long) t[1]);
		GD_DebugString(text);
	}
	blit_shortest = 16;
}
#endif

/* ---- the test ---- */

#ifdef ENGINE_TOUR
static uint32_t rng = 12345;
static int rnd(int n)
{
	rng = rng * 1103515245u + 12345u;
	return (int) ((rng >> 16) % (uint32_t) n);
}
#endif

/* ---- the walk: the avatar and the NPCs of a new game (ENGINE/NPC.DAT), the pad walking it ---- */

#ifndef ENGINE_TOUR
#define WALK_BLANKS 8           /* a cell every 8 blanks: 7.5 a second */
static struct ENpc npcs[400];
static int nnpcs, me = -1;
static int16_t ax, ay;
#ifndef START_MINUTES
#define START_MINUTES (6 * 60)  /* the world's time to start at */
#endif
static uint8_t az, walk_dir;
static uint16_t walk_steps;
static uint32_t walk_tick;
/* untouched, it walks this (for runs without a pad): into the stables at Trinsic and out */
static const struct { int8_t dx, dy, n; } demo[] = {
	{ 0, -1, 6 }, { -1, 0, 9 }, { 0, -1, 9 }, { 1, 0, 5 }, { -1, 0, 5 }, { 0, 1, 9 }, { 1, 0, 9 },
	{ 0, 1, 6 },
};
static uint8_t demo_leg, demo_left, touched, a_held;

/* The world's clock: blanks; under ENGINE_FRAME_SUMS, 4 a frame, so the frames hang on the
 * frame count alone. */
#ifdef ENGINE_FRAME_SUMS
static uint32_t sum_frames;
#define world_clock() (sum_frames * 4)
#else
#define world_clock() jag_ticks
#endif

static int walk_init(void)
{
	nnpcs = e_npcs(npcs, 400);
	for (int i = 0; i < nnpcs; i++)
		if (npcs[i].number == 0)
			me = i;
	if (me < 0)
		return 0;
	ax = npcs[me].x;
	ay = npcs[me].y;
	az = npcs[me].z;
	for (int m = 0; m < E_MOVERS; m++)
		e_mover_add(0, 0, 0, 0);
	e_mover_set(0, npcs[me].tf, ax, ay, az);
	e_world_init(npcs, nnpcs, START_MINUTES, world_clock());
	e_world_step(world_clock(), ax, ay);
	e_follow(0);
#ifdef ENGINE_FRAME_SUMS
	walk_tick = 0;
#else
	walk_tick = jag_ticks;
#endif
	demo_left = (uint8_t) demo[0].n;
	return 1;
}

#ifdef ENGINE_ROUTE_BENCH
static void route_bench(void)
{
	/* routes from the avatar to every 6th cell it could stand on near the view, twice (the
	 * first reads the chunks in): blanks for all, the most one took, how many found */
	static struct EWay way[E_ROUTE_WAYS];
	static char text[160];
	int16_t x0, y0, x1, y1;
	uint16_t tf = e_walk_frame(npcs[me].tf & 0x3ff, 2, 0);
	e_set_view((int16_t) (ax - E_VIEW_CELLS_X / 2), (int16_t) (ay - E_VIEW_CELLS_Y / 2));
	e_near_window(&x0, &y0, &x1, &y1);
	for (int pass = 0; pass < 2; pass++) {
		uint32_t all = 0, most = 0, found = 0, tried = 0, steps = e_stats.route_steps;
		for (int ty = y0; ty <= y1; ty += 6)
			for (int tx = x0; tx <= x1; tx += 6) {
				if (!e_can_stand(tf, (int16_t) tx, (int16_t) ty, 0))
					continue;
				uint32_t t0 = eb_clock();
				found += e_route_plan(tf, ax, ay, az, (int16_t) tx, (int16_t) ty, 0, 150, way) > 0;
				uint32_t t = eb_clock() - t0;
				all += t;
				most = t > most ? t : most;
				tried++;
			}
		snprintf(text, sizeof text, "route bench %d: %lu tried %lu found, steps %lu, blanks "
			"%lu.%lu, worst %lu.%lu\n", pass, (unsigned long) tried, (unsigned long) found,
			(unsigned long) (e_stats.route_steps - steps), tenths(all) / 10, tenths(all) % 10,
			tenths(most) / 10, tenths(most) % 10);
		GD_DebugString(text);
	}
}
#endif

/* A cell for each WALK_BLANKS blanks gone: the pad's way, or the demo's; veering round what is in
 * the way, up and down stairs, a shut door in the way opened. A: the door ahead opened or shut.
 * The NPCs near it picked again when it has gone 8 cells; the ceiling over it. */
static void walk_update(void)
{
	uint16_t type = npcs[me].tf & 0x3ff;
	int moved = 0, walking = 0;

#ifdef ENGINE_FRAME_SUMS
	/* by frames, not time, so the frames hang on the frame count alone */
	uint32_t now = (sum_frames++ / 2) * WALK_BLANKS;
	while (now - walk_tick >= WALK_BLANKS) {
#else
	while (jag_ticks - walk_tick >= WALK_BLANKS) {
#endif
		uint32_t held = pad();
		int dx = 0, dy = 0;
		walk_tick += WALK_BLANKS;
		if (held)
			touched = 1;
		if ((held & PAD_A) && !a_held) {
			static const int8_t face_x[4] = { 0, 1, 0, -1 }, face_y[4] = { -1, 0, 1, 0 };
			int door = e_door_ahead(e_walk_frame(type, walk_dir, 0), ax, ay, az,
				face_x[walk_dir], face_y[walk_dir]);
			if (door >= 0)
				e_door_toggle(door);
		}
		a_held = (held & PAD_A) != 0;
		if (touched) {
			dx = (held & (1UL << 23) ? 1 : 0) - (held & (1UL << 22) ? 1 : 0);
			dy = (held & (1UL << 21) ? 1 : 0) - (held & (1UL << 20) ? 1 : 0);
		} else {
			dx = demo[demo_leg].dx;
			dy = demo[demo_leg].dy;
			if (--demo_left == 0) {
				demo_leg = (uint8_t) ((demo_leg + 1) % (sizeof demo / sizeof demo[0]));
				demo_left = (uint8_t) demo[demo_leg].n;
			}
		}
		if (dx == 0 && dy == 0)
			continue;
		walking = 1;
		walk_dir = (uint8_t) (dy < 0 ? 0 : dx > 0 ? 1 : dy > 0 ? 2 : 3);
		e_mover_self(0);            /* the avatar out of its own way */
		int step = e_walk(e_walk_frame(type, walk_dir, 0), &ax, &ay, &az, dx, dy, 1);
		e_mover_self(-1);
		if (step != E_STEP_MADE)
			continue;
		walk_steps++;
		moved = 1;
	}
	if (walking || moved)
		e_mover_set(0, e_walk_frame(type, walk_dir, walk_steps), ax, ay, az);
	e_world_step(world_clock(), ax, ay);
#ifdef ENGINE_BARK_TEST
	/* a line over mover 1 every two seconds, to see barks drawn */
	{
		static uint32_t said;
		if (world_clock() / 120 != said) {
			said = world_clock() / 120;
			e_bark(1, e_text(1, 128), 90);
		}
	}
#endif
	if (moved)
		e_follow(0);
}
#endif

void jag_main(void)
{
#ifdef ENGINE_TOUR
	/* a tour of Britain: east along a street, then south, then back */
	static const struct { int8_t dx, dy; int16_t n; } tour[] = {
		{ 1, 0, 64 }, { 0, 1, 48 }, { -1, 0, 64 }, { 0, -1, 48 },
	};
#endif
	char text[320];
	int16_t x = 880, y = 1330;
	struct { int16_t x, y; int dir; } walker[E_MOVERS];
	int nm = 12, leg = 0, step = 0, r;
	uint32_t frame = 0, mark_ticks, mark_frame = 0, last_shown, worst = 0;

	GD_Install(gd_bios);
	e_surface_rows(&surfaces[0]);
	shown_tick = jag_ticks - 1;
	video_init();
	if ((r = e_init("U7/", store, sizeof store)) != 0) {
		snprintf(text, sizeof text, "e_init failed: %d\n", r);
		GD_DebugString(text);
		for (;;)
			;
	}
	set_palette(e_palette());
	sound_init();
#ifdef ENGINE_BENCH_SHAPES
	bench_shapes();
#endif
#ifdef ENGINE_BENCH_CPU
	{
		/* the 68000's speed: 55,556 turns of subq.l and bne (8 + 10 cycles: 1,000,000), then
		 * 100,000 word reads of main RAM, each with a subq.l and a bne (26 cycles a turn if the
		 * bus is free: 2,600,000) */
		uint32_t t0 = eb_clock();
		__asm__ volatile ("move.l #55556,%%d0\n1: subq.l #1,%%d0\n bne 1b" ::: "d0", "cc");
		uint32_t t1 = eb_clock();
		__asm__ volatile ("move.l %0,%%a0\n move.l #100000,%%d0\n1: move.w (%%a0)+,%%d1\n"
			" subq.l #1,%%d0\n bne 1b" :: "g" (store) : "d0", "d1", "a0", "cc", "memory");
		uint32_t t2 = eb_clock();
		snprintf(text, sizeof text, "cpu: 1M cycles of loop in %lu half-lines, 2.6M of reads in %lu "
			"(a field %u)\n", (unsigned long) (t1 - t0), (unsigned long) (t2 - t1), vc_period);
		GD_DebugString(text);
	}
#endif
#ifdef ENGINE_TOUR
	for (int m = 0; m < nm; m++) {
		walker[m].x = (int16_t) (x + 4 + rnd(32));
		walker[m].y = (int16_t) (y + 4 + rnd(20));
		walker[m].dir = rnd(4);
		e_mover_add(721, walker[m].x, walker[m].y, 0);
	}
#else
	(void) walker;
	(void) nm;
	(void) leg;
	(void) step;
	if (!walk_init()) {
		GD_DebugString("no NPC.DAT: no avatar\n");
		for (;;)
			;
	}
#endif
	/* the profile's samples by Tom's timer, at 37 Hz (26.59 MHz / 1000 / 719), apart from the
	 * frames, which keep to the blanks */
	REG16(0xF00050) = 999;
	REG16(0xF00052) = 718;
	jag_int_enable = 1 | 8;
	jag_prof_pit = 1;
	jag_prof_end = profile + sizeof profile;
	jag_prof_next = profile;
#ifdef ENGINE_ROUTE_BENCH
	route_bench();
#endif
	GD_DebugString("engine test: running\n");
	mark_ticks = last_shown = jag_ticks;
	for (;;) {
#ifndef ENGINE_TOUR
		walk_update();
		x = (int16_t) (ax - E_VIEW_CELLS_X / 2);
		y = (int16_t) (ay - E_VIEW_CELLS_Y / 2);
#else
		uint32_t held = pad();

		if (held) {
			x = (int16_t) (x + (held & (1UL << 23) ? 1 : 0) - (held & (1UL << 22) ? 1 : 0));
			y = (int16_t) (y + (held & (1UL << 21) ? 1 : 0) - (held & (1UL << 20) ? 1 : 0));
		} else if (frame % SCROLL_FRAMES == 0) {
			x = (int16_t) (x + tour[leg].dx);
			y = (int16_t) (y + tour[leg].dy);
			if (++step >= tour[leg].n) {
				step = 0;
				leg = (leg + 1) % 4;
			}
		}
		for (int m = 0; m < nm; m++) {
			static const int8_t ddx[4] = { 0, 1, 0, -1 }, ddy[4] = { -1, 0, 1, 0 };
			/* a step every SCROLL_FRAMES frames, carried along with the view */
			if (frame % SCROLL_FRAMES == 0) {
				if (rnd(8) == 0)
					walker[m].dir = rnd(4);
				walker[m].x = (int16_t) (walker[m].x + ddx[walker[m].dir] + tour[leg].dx);
				walker[m].y = (int16_t) (walker[m].y + ddy[walker[m].dir] + tour[leg].dy);
			}
			e_mover_set(m, e_walk_frame(721, walker[m].dir, (int) frame), walker[m].x,
				walker[m].y, 0);
		}
#endif
		if (!profile_saved && jag_prof_next >= jag_prof_end) {
			uint16_t f = GD_FOpen("U7/PCPROF.BIN", GD_FOPEN_WRITE | GD_FOPEN_CREATE_ALWAYS);
			profile_saved = 1;
			if ((int16_t) f >= 0) {
				GD_FWrite(f, profile, sizeof profile);
				GD_FClose(f);
				GD_DebugString("profile saved\n");
			}
		}
		struct ESurface *drawing = &surfaces[(frame + 1) % FRAMES];
		e_set_view(x, y);
		e_draw(drawing);
#ifdef ENGINE_FRAME_SUMS
		/* each frame's pixels summed, to compare builds (the frames hang on the frame count
		 * alone, not on time) */
		if (frame < 400) {
			const uint32_t *p = (const uint32_t *) drawing->pixels;
			uint32_t sum = 0;
			for (int i = 0; i < W * H / 4; i++)
				sum = (sum << 1 | sum >> 31) ^ p[i];
			snprintf(text, sizeof text, "sum %lu %08lx\n", (unsigned long) frame,
				(unsigned long) sum);
			GD_DebugString(text);
		}
#endif
		show(drawing->pixels);
		frame++;
		if (jag_ticks - last_shown > worst && frame > 1)
			worst = jag_ticks - last_shown;
		last_shown = jag_ticks;
		if ((frame & 63) == 0) {
			uint32_t now = jag_ticks, fps10 = (frame - mark_frame) * 600 / (now - mark_ticks);
			/* the timings in blanks, to a tenth */
#define T(v) tenths(v) / 10, tenths(v) % 10
			snprintf(text, sizeof text, "fps %lu.%lu (frame %lu) areas %lu shapes %luK; blanks: "
				"build %lu.%lu (chunks %lu.%lu: regions %lu.%lu cells %lu.%lu; order %lu.%lu) "
				"copy %lu.%lu movers %lu.%lu (fronts %lu.%lu) finish %lu.%lu shapes read "
				"%lu.%lu; worst %lu; misses %lu finds %lu cands %lu cmp %lu\n",
				(unsigned long) fps10 / 10, (unsigned long) fps10 % 10, (unsigned long) frame,
				(unsigned long) e_stats.rebuilds, (unsigned long) e_stats.shape_bytes / 1024,
				T(e_stats.build_time), T(e_stats.chunk_time), T(e_stats.region_time),
				T(e_stats.cells_time), T(e_stats.order_time), T(e_stats.copy_time),
				T(e_stats.object_time), T(e_stats.front_time), T(e_stats.finish_time),
				T(e_stats.load_time), (unsigned long) worst, (unsigned long) e_stats.front_misses,
				(unsigned long) e_stats.near_finds, (unsigned long) e_stats.front_cands,
				(unsigned long) e_stats.front_compares);
			e_stats.near_finds = e_stats.front_cands = e_stats.front_compares = 0;
#undef T
			text[255] = 0;          /* GD_DebugString's most */
			e_stats.pair_tests = 0;
			e_stats.region_time = e_stats.cells_time = 0;
			worst = 0;
			e_stats.load_time = e_stats.chunk_time = e_stats.order_time = 0;
			e_stats.front_time = e_stats.front_misses = 0;
			e_stats.build_time = e_stats.copy_time = e_stats.object_time = e_stats.finish_time = 0;
			GD_DebugString(text);
			if (e_stats.routes_planned != 0) {
				/* routes planned (found), steps tried, blanks spent in all and the most one took */
				snprintf(text, sizeof text, "routes %lu steps %lu blanks %lu.%lu worst %lu.%lu\n",
					(unsigned long) e_stats.routes_planned, (unsigned long) e_stats.route_steps,
					tenths(e_stats.route_time) / 10, tenths(e_stats.route_time) % 10,
					tenths(e_stats.route_worst) / 10, tenths(e_stats.route_worst) % 10);
				GD_DebugString(text);
			}
			mark_ticks = now;
			mark_frame = frame;
		}
	}
}
