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

/* The GameDrive's debug text: only when asked for (-DENGINE_GD_DEBUG; the emulator's runs). On
 * the console, with nothing on the USB side reading it, the second line never returns. */
#ifdef ENGINE_GD_DEBUG
#define DEBUG_TEXT(t) GD_DebugString(t)
#else
#define DEBUG_TEXT(t) ((void) (t))
#endif

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
#if defined JAG_PROF_FRAMES && !defined ENGINE_PROFILE
#define ENGINE_PROFILE
#endif
#ifdef ENGINE_PROFILE           /* (-DENGINE_PROFILE: its 116K are wanted for the store else) */
#ifdef JAG_PROF_FRAMES
#define PROFILE_RECORD 260      /* and 32 return addresses up the frame pointers' chain */
#define PROFILE_SAMPLES 450
#else
#define PROFILE_RECORD 132
#define PROFILE_SAMPLES 900
#endif
static uint8_t profile[PROFILE_SAMPLES * PROFILE_RECORD] __attribute__((aligned(4)));
static int profile_saved;
#endif

/* The tour scrolls a cell every SCROLL_FRAMES frames (1: 60 cells a second, far past walking) */
#ifndef SCROLL_FRAMES
#define SCROLL_FRAMES 1
#endif

static uint32_t gd_bios[1024] __attribute__((aligned(8)));   /* (long aligned, its README says) */
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
static uint8_t store[1476 * 1024] __attribute__((aligned(8)));
static uint8_t row_table[H * 4];
static struct ESurface surfaces[FRAMES] = {
	{ frames[0], W, W, H, row_table }, { frames[1], W, W, H, row_table },
#if FRAMES == 3
	{ frames[2], W, W, H, row_table },
#endif
};

void eb_log(const char *text)
{
	DEBUG_TEXT(text);
}

/* startup.S's exception handler */
void jag_crash(int32_t long_frame, const uint16_t *frame)
{
	char text[64];
	const uint16_t *f = frame + 1 + (long_frame ? 4 : 0);

	snprintf(text, sizeof text, "crash: vector %u at %04x%04x\n", frame[0], f[1], f[2]);
#ifdef ENGINE_STAGES
	/* the screen: orange a bus error, purple an address error, grey another */
	for (int i = 0; i < 256; i++)
		((volatile uint16_t *) 0xF00400)[i] = frame[0] == 2 ? 0xF810 : frame[0] == 3 ? 0x7FC0 : 0x8410;
#endif
	DEBUG_TEXT(text);
	for (;;)
		;
}


#ifdef ENGINE_FAULTS
/* (-DENGINE_FAULTS) A fault's frame logged: the 68000's bus and address errors push the access
 * address and the PC; the others the status and the PC. And the GPU not done in two seconds
 * logged, with its PC. */
void fault_report(const uint16_t *sp);
__asm__(".globl fault_handler\nfault_handler:\n\tmove.l %sp,-(%sp)\n\tjsr fault_report\n1:\tbra 1b\n");
void fault_handler(void);
void fault_report(const uint16_t *sp)
{
	char text[96];
	snprintf(text, sizeof text, "FAULT words %04x %04x %04x %04x %04x %04x %04x %04x\n", sp[0], sp[1], sp[2],
		sp[3], sp[4], sp[5], sp[6], sp[7]);
	DEBUG_TEXT(text);
	for (;;)
		;
}
static void faults_catch(void)
{
	for (int v = 2; v <= 11; v++)
		((void (**)(void)) 0)[v] = fault_handler;
}
#endif
/* ---- files: the GameDrive's, a handful open ---- */

/* The engine's files (up to FILES) on at most GD_LIVE of the GameDrive's: the console's own
 * hangs with a sixth open (the emulator's takes eight). One not open there is opened again
 * where it was when next read, the least lately used giving way; one being written stays. */
#define FILES 8
#define GD_LIVE 5
static struct {
	char name[48];
	uint16_t gd;
	uint8_t open, live, writing, rom;
	int32_t size, pos;
	uint32_t used;
} files[FILES];

/* ---- the shapes in the GameDrive's own memory ----
 *
 * The shape file (7.1 MB) is read from the card once, at the start, into the GameDrive's 16 MB,
 * just past this program (512K in bank 0): its place at in the file is the GameDrive's byte
 * ROM_START + at. The cartridge space ($800000-$DFFFFF) shows banks 0-5 as they are, so the
 * first 5.5 MB are there to be drawn from as they are (plat_file_direct: no copy, never given
 * way); the rest is copied out when read, page 5 shown the bank it is in meanwhile (the GPU
 * and the blitter idle). From the card, the shapes' ring (too small for a view's shapes) read
 * again all the time, and walking stopped for up to two seconds. If the GameDrive does not keep
 * what is written (read back), the card's as before. */
#define ROM_FILE "U7/ENGINE/SHAPES.FLX"
#define ROM_START 0x80000UL             /* (the program's 512K before it) */
#define ROM_SHOWN 0x5FFF00UL            /* banks 0-5 as they are, short of $DFFF00 (BUTCH's) */
#define ROM_END 0x1000000UL
static int32_t rom_size = -1;           /* -1: not there, the card's */
static void blit_wait(void);

/* n bytes at the file's place at into to, or from from into the GameDrive (to NULL) */
static void rom_copy(uint32_t at, uint8_t *to, const uint8_t *from, uint32_t n)
{
	uint32_t l = ROM_START + at;
	if (to != NULL && l + n <= ROM_SHOWN) {
		memcpy(to, (const uint8_t *) 0x800000 + l, n);
		return;
	}
	while (n > 0) {
		uint32_t in = l & 0xFFFFF, k = 0x100000 - in < n ? 0x100000 - in : n;
		GD_ROMSetPage(5, (uint16_t) (l >> 20));
		if (to != NULL) {
			memcpy(to, (const uint8_t *) 0xD00000 + in, k);
			to += k;
		} else {
			memcpy((uint8_t *) 0xD00000 + in, from, k);
			from += k;
		}
		l += k;
		n -= k;
	}
	GD_ROMSetPage(5, 5);
}

/* Where n bytes at the file's place at are to be read as they are, or NULL (the engine copies
 * them then): the shape file's first 5.5 MB, in the cartridge space. */
const void *plat_file_direct(int16_t f, uint32_t at, uint32_t n)
{
	if (f < 0 || f >= FILES || !files[f].rom || ROM_START + at + n > ROM_SHOWN)
		return NULL;
	return (const uint8_t *) 0x800000 + ROM_START + at;
}
static uint32_t file_clock;
#ifdef ENGINE_STAGES
/* (for tests on the console: the card's reads, their time (eb_clock's units), bytes, opens) */
static uint32_t io_reads, io_units, io_bytes, io_opens, io_seek, io_file_reads[FILES];
#endif

/* A GameDrive handle free, the least lately used (not being written) closed for it. */
static int gd_room(void)
{
	int live = 0, oldest = -1;
	for (int i = 0; i < FILES; i++)
		if (files[i].open && files[i].live) {
			live++;
			if (!files[i].writing && (oldest < 0 || files[i].used < files[oldest].used))
				oldest = i;
		}
	if (live < GD_LIVE)
		return 1;
	if (oldest < 0)
		return 0;
	GD_FClose(files[oldest].gd);
	files[oldest].live = 0;
	return 1;
}

/* The file open on the GameDrive at its place. */
static int gd_live(int16_t f)
{
	files[f].used = ++file_clock;
	if (files[f].live)
		return 1;
	if (!gd_room())
		return 0;
#ifdef ENGINE_STAGES
	io_opens++;
#endif
	uint16_t gd = GD_FOpen(files[f].name, GD_FOPEN_READ);
	if ((int16_t) gd < 0)
		return 0;
	files[f].gd = gd;
	files[f].live = 1;
	if (files[f].pos != 0 && (int16_t) GD_FSeek(gd, GD_FSEEK_SET, files[f].pos) < 0)
		return 0;
	return 1;
}

#ifdef ENGINE_STAGES
/* (on the console, without the GameDrive's debug text) how far the start has gone, as the whole
 * screen's colour: red the start, blue and cyan by turns at each file read, white the world
 * loaded, green past the first debug line, yellow the sound going, magenta the world failed */
static void __attribute__((unused)) stage(uint16_t rgb)
{
	for (int i = 0; i < 256; i++)
		((volatile uint16_t *) 0xF00400)[i] = rgb;
}
/* (the start's colours and bars only with ENGINE_LOAD_STAGES: they flash, and the start is
 * known good on the console now) */
#ifdef ENGINE_LOAD_STAGES
static uint8_t staging = 1;
#else
static uint8_t staging = 0;
#endif
/* and while the files are read: the files opened and the reads so far as two rows of 12 bars
 * (white a 1, purple a 0, the highest bit leftmost) on a background red while it waits for the
 * GPU, blue in the GameDrive's read, green between */
static uint16_t stage_opens, stage_reads;
static void stage_bars(int row, uint16_t v)
{
	static uint16_t was[3] = { 0xffff, 0xffff, 0xffff };
	uint16_t changed = (uint16_t) (v ^ was[row]);
	was[row] = v;
	for (int k = 0; k < 12; k++) {
		uint8_t c = v & (0x800 >> k) ? 1 : 2;
		if (!(changed & (0x800 >> k)))
			continue;
		for (int f = 0; f < FRAMES; f++)
			for (int y = 40 + row * 60; y < 76 + row * 60; y++)
				for (int x = 14 + k * 25; x < 30 + k * 25; x++)
					frames[f][y * W + x] = c;
	}
}
/* the third row: the GameDrive call it is in (1 opening, 2 opened, 3 sized; 4 closing, 5
 * closed; 6 seeking, 7 sought; 8 reading, 9 read) */
#define STEP(n) do { if (staging) stage_bars(2, n); } while (0)
static void blit_wait(void);
static void gpu_load(void);
static void gpu_start(uint32_t pc);

static void stage_clut(uint16_t back)
{
	volatile uint16_t *clut = (volatile uint16_t *) 0xF00400;
	clut[0] = back;
	clut[1] = 0xFFFF;
	clut[2] = 0x8410;
}
#ifdef ENGINE_LOAD_STAGES
#define STAGE(c) stage(c)
#else
#define STAGE(c) ((void) 0)
#endif
#else
#define STAGE(c) ((void) 0)
#define STEP(n) ((void) 0)
#endif

int16_t plat_file_open(const char *name, int16_t mode)
{
	for (int16_t i = 0; i < FILES; i++)
		if (!files[i].open && rom_size >= 0 && mode != PLAT_WRITE && strcmp(name, ROM_FILE) == 0) {
			/* (the GameDrive's memory's: no handle on the card) */
			strcpy(files[i].name, name);
			files[i].open = files[i].rom = 1;
			files[i].live = files[i].writing = 0;
			files[i].pos = 0;
			files[i].size = rom_size;
			return i;
		}
	for (int16_t i = 0; i < FILES; i++)
		if (!files[i].open) {
			if (strlen(name) >= sizeof files[i].name || !gd_room())
				return -1;
			STEP(1);
			uint16_t gd = GD_FOpen(name, mode == PLAT_WRITE ? GD_FOPEN_WRITE | GD_FOPEN_CREATE_ALWAYS
				: GD_FOPEN_READ);
			STEP(2);
			if ((int16_t) gd < 0) {
#ifdef ENGINE_STAGES
				/* (grey 2 s: the GameDrive would not open it) */
				volatile uint16_t *clut = (volatile uint16_t *) 0xF00400;
				uint16_t was = clut[0];
				clut[0] = 0x8410;
				for (uint32_t t = jag_ticks; jag_ticks - t < 120; )
					;
				clut[0] = was;
#endif
				return -1;
			}
			strcpy(files[i].name, name);
			files[i].gd = gd;
			files[i].open = files[i].live = 1;
			files[i].rom = 0;
			files[i].writing = mode == PLAT_WRITE;
			files[i].pos = 0;
			files[i].used = ++file_clock;
#ifdef ENGINE_STAGES
			if (staging)
				stage_bars(0, ++stage_opens);
#endif
			files[i].size = (int32_t) GD_FSize(gd);
			STEP(3);
			return i;
		}
	return -1;
}

void plat_file_close(int16_t f)
{
	if (f >= 0 && f < FILES && files[f].open) {
		STEP(4);
		if (files[f].live)
			GD_FClose(files[f].gd);
		STEP(5);
		files[f].open = files[f].live = files[f].rom = 0;
	}
}

/* The GameDrive's own reads and writes (on the console; not the emulator's) fail on a buffer
 * not long aligned: those go through this one. */
static uint8_t bounce[4096] __attribute__((aligned(8)));
#define READ_MOST 16384

int32_t plat_file_read(int16_t f, void *buffer, int32_t count)
{
#ifdef ENGINE_STAGES
	if (staging) {
		stage_bars(1, ++stage_reads);
		stage_clut(0xF800);
	}
#endif
	gpu_finish();
#ifdef ENGINE_STAGES
	if (staging)
		stage_clut(0x07C0);
	uint32_t io_t0 = eb_clock();
#endif
	STEP(8);
	if (files[f].rom) {
		blit_wait();            /* (page 5 may be shown another bank meanwhile) */
		if (count > files[f].size - files[f].pos)
			count = files[f].size - files[f].pos;
		rom_copy((uint32_t) files[f].pos, buffer, NULL, (uint32_t) count);
		files[f].pos += count;
#ifdef ENGINE_STAGES
		io_reads++;
		io_bytes += (uint32_t) count;
		io_file_reads[f]++;
		io_units += eb_clock() - io_t0;
#endif
		return count;
	}
	if (!gd_live(f))
		return -1;
	/* never past the file's end, nor more than READ_MOST at once (the console's GameDrive hangs
	 * on a 32K read past the end; the emulator's does not) */
	if (count > files[f].size - files[f].pos)
		count = files[f].size - files[f].pos;
	for (int32_t done = 0; done < count; ) {
		int32_t n = count - done > READ_MOST ? READ_MOST : count - done;
		uint8_t *to = (uint8_t *) buffer + done;
		if ((uint32_t) to & 3) {
			if (n > (int32_t) sizeof bounce)
				n = (int32_t) sizeof bounce;
			if ((int16_t) GD_FRead(files[f].gd, bounce, (uint32_t) n, GD_FREAD_CPU) < 0)
				return -1;
			memcpy(to, bounce, (size_t) n);
		} else if ((int16_t) GD_FRead(files[f].gd, to, (uint32_t) n, GD_FREAD_CPU) < 0)
			return -1;
		done += n;
	}
	files[f].pos += count;
	STEP(9);
#ifdef ENGINE_STAGES
	if (staging)
		stage_clut(0x003F);
	io_reads++;
	io_bytes += (uint32_t) count;
	io_file_reads[f]++;
	io_units += eb_clock() - io_t0;
#endif
	return count;
}

int32_t plat_file_write(int16_t f, const void *buffer, int32_t count)
{
	gpu_finish();
	if (!gd_live(f))
		return -1;
	files[f].pos = files[f].pos + count < files[f].size ? files[f].pos + count : files[f].size;
	if ((uint32_t) buffer & 3) {
		for (int32_t done = 0; done < count; ) {
			int32_t n = count - done > (int32_t) sizeof bounce ? (int32_t) sizeof bounce : count - done;
			memcpy(bounce, (const uint8_t *) buffer + done, (size_t) n);
			if ((int16_t) GD_FWrite(files[f].gd, bounce, (uint32_t) n) < 0)
				return -1;
			done += n;
		}
	} else if ((int16_t) GD_FWrite(files[f].gd, (void *) buffer, (uint32_t) count) < 0)
		return -1;
	return count;
}

int32_t plat_file_seek(int16_t f, int32_t offset, int16_t origin)
{
	(void) origin;
	if (offset > files[f].size)
		return -1;
	STEP(6);
	files[f].pos = offset;
#ifdef ENGINE_STAGES
	uint32_t io_t0 = eb_clock();
#endif
	if (files[f].live && !files[f].rom && (int16_t) GD_FSeek(files[f].gd, GD_FSEEK_SET, offset) < 0)
		return -1;
#ifdef ENGINE_STAGES
	io_seek += eb_clock() - io_t0;
#endif
	STEP(7);
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

#ifdef ENGINE_STAGES
static uint32_t wait_blit, wait_gpu;     /* (for tests: the time waited, eb_clock's units) */
uint32_t eb_clock(void);
#endif
static void blit_wait(void)
{
#ifdef ENGINE_STAGES
	if (!(BREG(B_CMD) & 1)) {
		uint32_t t0 = eb_clock();
		while (!(BREG(B_CMD) & 1))
			;
		wait_blit += eb_clock() - t0;
	}
#else
	while (!(BREG(B_CMD) & 1))
		;
#endif
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

/* the music's (sound_dsp.s): the OPL2's tables, and its controls (MCTL) */
#define DSP_TABLES ((volatile uint32_t *) 0xF1BC00)
#define MCTL ((volatile uint32_t *) 0xF1CF40)
enum { M_RING, M_MASK, M_WR, M_RD, M_CMD, M_ENDED, M_VOL, M_ORD, M_OWR, M_WAIT, M_FRAC, M_LFOAM,
	M_LFOVIB, M_WSE, M_BLOCKS, M_SHORT, M_CAP, M_CAPEND, M_ISRS, M_PING, M_PONG, M_SEEN, M_QUIET, M_REARMS, M_LAST, M_TURNS, M_RESTARTS, M_LASTM, M_STAGE, M_RET,
	M_BAD, M_BADSP, M_BADRET, M_BADSTAGE, M_LONGS };

static uint32_t music_ring, music_mask;

static void sound_init(void)
{
	const uint32_t *from = (const uint32_t *) dsp_sound;
	volatile uint32_t *to = (volatile uint32_t *) 0xF1B000;

	D_CTRL = 0;
	for (uint32_t i = 0; i < (uint32_t) (dsp_sound_end - dsp_sound + 3) / 4; i++)
		to[i] = from[i];
	for (int i = 0; i < SOUND_CHANNELS * 4; i++)
		SOUND_CHANNEL[i] = 0;
	/* the tables, one after another as struct EOplTables has them */
	const uint32_t *t = (const uint32_t *) e_opl_tables();
	for (uint32_t i = 0; i < sizeof (struct EOplTables) / 4; i++)
		DSP_TABLES[i] = t[i];
	for (int i = 0; i < M_LONGS; i++)
		MCTL[i] = 0;
	MCTL[M_VOL] = 1024;         /* (4x: the OPL2's own level is quiet beside the sounds) */
	MCTL[M_RING] = music_ring;
	MCTL[M_MASK] = music_mask;
	MCTL[M_CMD] = 1;            /* (its first: the synth silenced) */
	/* (the I2S clock, SCLK 25 and SMODE internal: the DSP sets them itself, sound_dsp.s) */
	D_PC = 0xF1B000;
	D_CTRL = 1;
}

void eb_music_ring(uint8_t *ring, uint32_t size)
{
	music_ring = (uint32_t) ring;
	music_mask = size - 1;
	MCTL[M_RING] = music_ring;
	MCTL[M_MASK] = music_mask;
	eb_music_restart();
}

uint32_t eb_music_read(void)
{
	return MCTL[M_RD];
}

void eb_music_written(uint32_t wr)
{
	MCTL[M_WR] = wr;
}

void eb_music_restart(void)
{
	MCTL[M_WR] = 0;
	MCTL[M_CMD] = 1;
	/* (looked at once a blank, a few at most: the DSP's RAM read from outside often, as a
	 * tight loop here did, is what stopped its main loop on the console) */
	for (uint32_t t = jag_ticks; (D_CTRL & 1) && MCTL[M_CMD] != 0 && jag_ticks - t < 4; ) {
		uint32_t b = jag_ticks;
		while (jag_ticks == b)
			;
	}
}

int eb_music_ended(void)
{
	return MCTL[M_ENDED] != 0;
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
#ifdef ENGINE_FAULTS
		uint32_t t0 = jag_ticks;
		while (G_CTRL & GPUGO)
			if (jag_ticks - t0 > 120) {
				char text[64];
				snprintf(text, sizeof text, "FAULT gpu stuck at pc %08lx\n", (unsigned long) REG32(0xF02110));
				DEBUG_TEXT(text);
				for (;;)
					;
			}
#elif defined ENGINE_STAGES
		if (G_CTRL & GPUGO) {
			uint32_t t0 = eb_clock();
			while (G_CTRL & GPUGO)
				;
			wait_gpu += eb_clock() - t0;
		}
#else
		while (G_CTRL & GPUGO)
			;
#endif
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
			DEBUG_TEXT(text);
			differ++;
		}
		if (++checked == 24) {
			snprintf(text, sizeof text, "convert check: %d of 24 differ\n", differ);
			DEBUG_TEXT(text);
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

#ifdef ENGINE_STAGES
/* (for tests: a frame's parts' times, eb_clock's units, and the longest frame's) */
static uint32_t part[4], part_keep[4], part_at, part_from, part_long;
static void part_mark(int k)
{
	uint32_t now = eb_clock();
	part[k] = now - part_at;
	part_at = now;
}
#endif
#ifdef ENGINE_STAGES
/* The DSP's state as colour 0 (the cells' colour), looked at each 32 calls. */
static void stage_status(void)
	{
		/* the main loop's turns, as the bars' top row (white the 1s), drawn into every frame */
		static uint16_t turns;
		/* the background: the DSP's state (red its loop not going, magenta its samples not
		 * taken by the I2S interrupt, green both going) */
		static uint32_t blocks, isrs, pinged;
		if ((++turns & 31) == 0) {
			uint32_t b = MCTL[M_BLOCKS], i = MCTL[M_ISRS], pong = MCTL[M_PONG];
			uint16_t c = b != blocks ? (i != isrs ? 0x003F : 0xFFC0) : (i != isrs ? 0xF810 : 0xF800);
			/* the ping: written now, read back (blue if not there), and the last one answered
			 * by the DSP (white if not) */
			if (pinged && pong != pinged)
				c = 0xFFFF;
			pinged = 0x5A000000 | turns;
			MCTL[M_PING] = pinged;
			if (MCTL[M_PING] != pinged)
				c = 0x07C0;
			((volatile uint16_t *) 0xF00400)[0] = c;
			blocks = b;
			isrs = i;
		}
	}

/* The DSP's state in the corner and as rows of 12 cells, into a frame. */
static void stage_cells(uint8_t *pixels)
{
	/* the DSP's state (colour 0, set above) as a square in the corner */
	for (int sy = H - 40; sy < H - 16; sy++)
		for (int sx = W - 64; sx < W - 36; sx++)
			pixels[sy * W + sx] = 0;
	{
		/* rows of 12 cells, the highest bit leftmost (the square's colour a 1, black a 0), over
		 * the last second: the frames, the longest frame (tenths of a blank), and in it: the
		 * card's reads, their bytes /256, the seeks' time and the reads' (tenths of a blank) */
		static uint32_t at, frames_n;
		static uint32_t rows[6];
		frames_n++;
		if (jag_ticks - at >= 60) {
			rows[0] = frames_n;
			rows[1] = (uint32_t) tenths(part_long);
			rows[2] = part_keep[0];
			rows[3] = part_keep[1] >> 8;
			rows[4] = (uint32_t) tenths(part_keep[2]);
			rows[5] = (uint32_t) tenths(part_keep[3]);
#ifdef ENGINE_STAGES_FILES
			/* (for the emulator: the shapes loaded, then files 0-3's reads, over the second) */
			{
				static uint32_t was_loads;
				extern uint32_t e_used_shapes, e_used_bytes;
				void e_used_reset(void);
				rows[1] = e_stats.shape_loads - was_loads;
				was_loads = e_stats.shape_loads;
				rows[2] = e_used_shapes;
				rows[3] = e_used_bytes >> 10;
				rows[4] = e_stats.shape_bytes >> 10;
				rows[5] = 0;
				e_used_reset();
			}
#endif
			frames_n = 0;
			part_long = 0;
			at = jag_ticks;
		}
		for (int r = 0; r < 6; r++)
			if (rows[r] > 0xFFF)
				rows[r] = 0xFFF;  /* (all lit: more) */
		for (int r = 0; r < 6; r++)
			for (int k = 0; k < 12; k++)
				for (int sy = H - 124 + r * 18; sy < H - 112 + r * 18; sy++)
					for (int sx = 24 + k * 18; sx < 38 + k * 18; sx++)
						pixels[sy * W + sx] = rows[r] & (0x800 >> k) ? 0 : 255;
	}
}
#endif

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
#define PAD_B (1UL << 25)
#define PAD_C (1UL << 26)
#define PAD_OPTION (1UL << 27)
#define PAD_PAUSE (1UL << 28)
static uint32_t pad_read(void)
{
	JOY_WRITE = 0x81FE;
	uint32_t v = JOY_READ;
	JOY_WRITE = 0x81FD;         /* the row with B (and keys 1 4 7 *) */
	uint32_t w = JOY_READ;
	JOY_WRITE = 0x81FB;         /* C (0 8 5 2) */
	uint32_t c = JOY_READ;
	JOY_WRITE = 0x81F7;         /* Option (# 9 6 3) */
	uint32_t o = JOY_READ;
	JOY_WRITE = 0x81FE;
	return (~(v >> 4) & 0x00F00000UL) | (v & 2 ? 0 : PAD_A) | (w & 2 ? 0 : PAD_B) | (v & 1 ? 0 : PAD_PAUSE)
		| (c & 2 ? 0 : PAD_C) | (o & 2 ? 0 : PAD_OPTION);
}

/* The keypad, port 1's other three rows (bits 24-27 of each: * 7 4 1, 0 8 5 2, # 9 6 3): bit d
 * for digit d, KEY_STAR, KEY_HASH. */
#define KEY_STAR (1u << 10)
#define KEY_HASH (1u << 11)
static uint32_t keypad_read(void)
{
	static const uint16_t rows[3] = { 0x81FD, 0x81FB, 0x81F7 };
	static const uint16_t keys[3][4] = {
		{ KEY_STAR, 1u << 7, 1u << 4, 1u << 1 },
		{ 1u << 0, 1u << 8, 1u << 5, 1u << 2 },
		{ KEY_HASH, 1u << 9, 1u << 6, 1u << 3 },
	};
	uint32_t got = 0;

	for (int r = 0; r < 3; r++) {
		JOY_WRITE = rows[r];
		uint32_t v = JOY_READ;
		for (int b = 0; b < 4; b++)
			if (!(v & 1UL << (24 + b)))
				got |= keys[r][b];
	}
	JOY_WRITE = 0x81FE;
	return got;
}

/* The pad read each blank (the VBL hook), its presses gathered: a press shorter than a slow
 * frame is not lost, and the rows are only ever selected here (no read cut in two). */
extern void (*jag_vbl_hook)(void);
static volatile uint32_t vbl_held, vbl_pressed, vbl_keys, vbl_typed;

static void pad_vbl(void)
{
	uint32_t h = pad_read(), k = keypad_read();

	vbl_pressed |= h & ~vbl_held;
	vbl_held = h;
	vbl_typed |= k & ~vbl_keys;
	vbl_keys = k;
}

static uint32_t pad(void)
{
	return vbl_held;
}

/* The presses gathered since last taken (cleared in one instruction: a blank may come between). */
static uint32_t pad_take(volatile uint32_t *gathered)
{
	uint32_t got = *gathered;
	__asm__ volatile ("and.l %1,%0" : "+m" (*gathered) : "d" (~got));
	return got;
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
	DEBUG_TEXT(text);
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
		DEBUG_TEXT(text);
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

#ifdef ENGINE_LOAD_STAGES
#define MARK(rgb) (((volatile uint16_t *) 0xF00400)[0] = (rgb))
#else
#define MARK(rgb) ((void) 0)
#endif
/* The new character the front made (its name and sex), when it made one. */
static char chosen_name[15];
static int chosen, chosen_male = 1;

/* The front (core/front.c): the original's main menu and new-character screen, a tick at a time
 * (each blank's, however long a frame takes), the pad's presses its keys, its palette set each
 * frame for its fades. Until the intro and the credits are made, those bring the menu back. */
static void front_run(void)
{
	static const uint32_t bit[6] = { 1UL << 20, 1UL << 21, 1UL << 22, 1UL << 23, PAD_A, PAD_B };
	static const int key[6] = { E_KEY_UP, E_KEY_DOWN, E_KEY_LEFT, E_KEY_RIGHT, E_KEY_NEXT, E_KEY_BACK };
	static uint8_t pal[768];
	uint32_t ticked = jag_ticks;
	int f = 0;

	if (!e_front_open(0, 0, 0))
		return;
	pad_take(&vbl_pressed);
	for (;;) {
		uint32_t pressed = pad_take(&vbl_pressed);
		int k = 0, r = E_FRONT_RUNNING;
		for (int b = 0; b < 6 && k == 0; b++)
			if (pressed & bit[b])
				k = key[b];
		/* the ticks gone since the last frame: the first takes the key */
		do {
			r = e_front_tick(k);
			k = 0;
		} while (r == E_FRONT_RUNNING && ++ticked != jag_ticks);
		ticked = jag_ticks;
		if (r == E_FRONT_NEW) {
			e_front_result(chosen_name, &chosen_male);
			chosen = 1;
			break;
		}
		if (r == E_FRONT_JOURNEY)
			break;
		if (r != E_FRONT_RUNNING && !e_front_open(0, 0, 0))
			break;
		struct ESurface *drawing = &surfaces[f = (f + 1) % FRAMES];
		e_front_draw(drawing);
		eb_finish();
		e_front_palette(pal);
		set_palette(pal);
		show(drawing->pixels);
		while (jag_ticks == ticked)
			;
	}
	/* black till the world's palette, and nothing shown that the world's palette would colour */
	memset(pal, 0, sizeof pal);
	set_palette(pal);
	struct ESurface *blank = &surfaces[f = (f + 1) % FRAMES];
	eb_clear(blank, 0, 0, W, H);
	eb_finish();
	show(blank->pixels);
	uint32_t t = jag_ticks;
	while (jag_ticks - t < 2)
		;
}

static int walk_init(void)
{
	nnpcs = e_npcs(npcs, 400);
	MARK(0xF800);               /* red: the NPCs read */
	for (int i = 0; i < nnpcs; i++)
		if (npcs[i].number == 0)
			me = i;
	if (me < 0)
		return 0;
#ifdef ENGINE_FEMALE
	chosen = 1;                 /* (a test: the avatar a woman) */
	chosen_male = 0;
#endif
	if (chosen) {
		/* the new character's (the front's): the name, and a woman her shape (989) and the
		 * flag off, as the original's game makes them from its GAMEARGS.DAT */
		if (chosen_name[0])
			memcpy(npcs[me].name, chosen_name, sizeof chosen_name);
		npcs[me].male = (uint8_t) (chosen_male != 0);
		if (!chosen_male)
			npcs[me].tf = (uint16_t) ((npcs[me].tf & ~0x3ffu) | 989);
	}
	ax = npcs[me].x;
	ay = npcs[me].y;
	az = npcs[me].z;
	for (int m = 0; m < E_MOVERS; m++)
		e_mover_add(0, 0, 0, 0);
	e_mover_set(0, npcs[me].tf, ax, ay, az);
	e_world_init(npcs, nnpcs, START_MINUTES, world_clock());
	MARK(0x7FC0);               /* purple: the world set going */
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
		DEBUG_TEXT(text);
	}
}
#endif

/* A cell for each WALK_BLANKS blanks gone: the pad's way, or the demo's; veering round what is in
 * the way, up and down stairs, a shut door in the way opened. A: the door ahead opened or shut.
 * The NPCs near it picked again when it has gone 8 cells; the ceiling over it. */
static uint32_t world_time;      /* blanks the world's step took (the stats' 64 frames) */
static int swing_ticks;         /* the avatar's swing shown, walk ticks left */

static void walk_update(void)
{
	uint16_t type = npcs[me].tf & 0x3ff;
	int moved = 0, walking = 0;
	static int posed;           /* the usecode has the avatar sitting or in bed */

	/* the avatar as the usecode has put it (sat down, into bed, stood up) */
	{
		uint16_t tf;
		if (e_world_avatar_pose(&ax, &ay, &az, &tf)) {
			int f = tf >> 10 & 15;
			posed = f == 10 || f == 13;
			e_mover_set(0, tf, ax, ay, az);
		}
	}

	/* presses, every frame (a short one may fall between the walk's ticks) */
	{
		uint32_t held = pad(), pressed = pad_take(&vbl_pressed);
#ifdef ENGINE_TALK_TEST
		/* NPC ENGINE_TALK_TEST's conversation (-DENGINE_TALK_TEST alone: Iolo's), two seconds in */
		static int frames_in;
		if (++frames_in == 40)
			e_talk_start(ENGINE_TALK_TEST);
#endif
#ifdef ENGINE_COMBAT_TEST
		/* Iolo, Shamino and Dupre join; ENGINE_COMBAT_TEST skeletons 6 cells south; combat on */
		static int fight_in;
		if (++fight_in == 40) {
			int joined = e_uc_join(1) + e_uc_join(3) + e_uc_join(4), made = 0;
			for (int k = 0; k < ENGINE_COMBAT_TEST; k++)
				made += e_world_spawn(528, (int16_t) (ax - ENGINE_COMBAT_TEST + 2 * k), (int16_t) (ay + 6), az) >= 0;
			e_combat_mode(1);
			touched = 1;
			char text[64];
			snprintf(text, sizeof text, "combat test: %d joined, %d skeletons\n", joined, made);
			DEBUG_TEXT(text);
		}
#endif
#ifdef ENGINE_USE_TEST
		/* the nearest item of shape ENGINE_USE_TEST used, two seconds in (to see a book, a
		 * scroll or a sign shown) */
		static int use_in;
		e_uc_trace = 1;
		if (++use_in == 40) {
			static struct EThing found[16];
			int n = e_things(ENGINE_USE_TEST, (int16_t) (ax - 20), (int16_t) (ay - 40), (int16_t) (ax + 20),
				(int16_t) (ay + 48), found, 16), best = -1, bd = 0;
			for (int k = 0; k < n; k++) {
				int d = (found[k].x > ax ? found[k].x - ax : ax - found[k].x)
					+ (found[k].y > ay ? found[k].y - ay : ay - found[k].y);
				if (best < 0 || d < bd)
					best = k, bd = d;
			}
			if (best < 0 && e_thing_add(ENGINE_USE_TEST, (int16_t) (ax + 1), ay, az, &found[0]))
				best = 0;       /* (none about: one put beside the avatar) */
			int used = best >= 0 && e_talk_use(&found[best]);
			char text[64];
			snprintf(text, sizeof text, "use test: %d found, used %d\n", n, used);
			DEBUG_TEXT(text);
		}
#endif
		int16_t aim_x, aim_y;
		static int aiming;      /* frames the cross-hairs have been out, the pad held */
		if (e_menu_active()) {
			/* the save menu: the pad picks, A does it, B (or Pause) goes on; a game loaded, the
			 * avatar where it was saved */
			static const uint32_t bit[6] = { 1UL << 20, 1UL << 21, 1UL << 22, 1UL << 23, PAD_A, PAD_B };
			static const int key[6] = { E_KEY_UP, E_KEY_DOWN, E_KEY_LEFT, E_KEY_RIGHT, E_KEY_NEXT, E_KEY_BACK };
			for (int k = 0; k < 6; k++)
				if ((pressed & bit[k]) && e_menu_key(key[k], world_clock())) {
					e_world_avatar_where(&ax, &ay, &az);
					nnpcs = (int) e_save_count("NPCS");
					posed = 0;
					swing_ticks = 0;
					e_mover_set(0, e_walk_frame(type, walk_dir, 0), ax, ay, az);
				}
			if (pressed & PAD_PAUSE)
				e_menu_key(E_KEY_BACK, 0);
			touched = 1;
			a_held = 1;
		} else if (e_uc_faded() && !e_talk_active()) {
			/* asleep: the pad does nothing */
		} else if (e_talk_targeting(&aim_x, &aim_y)) {
			/* something to point at: the cross-hairs, first a cell ahead of the avatar, moved
			 * by the pad (faster as it is held); A points, B lets go */
			static const int8_t face_x[4] = { 0, 1, 0, -1 }, face_y[4] = { -1, 0, 1, 0 };
			int fx = (E_VIEW_CELLS_X / 2) * 8 + 3 - az * 4, fy = (E_VIEW_CELLS_Y / 2) * 8 + 3 - az * 4;
			int dx = (held & (1UL << 23) ? 1 : 0) - (held & (1UL << 22) ? 1 : 0);
			int dy = (held & (1UL << 21) ? 1 : 0) - (held & (1UL << 20) ? 1 : 0);
			if (aiming == 0) {
				e_talk_aim(fx + face_x[walk_dir] * 16, fy + face_y[walk_dir] * 16);
				aiming = 1;
			} else if (dx != 0 || dy != 0) {
				int speed = aiming < 8 ? 1 : aiming < 24 ? 2 : 4;
				aiming++;
				e_talk_aim(aim_x + dx * speed, aim_y + dy * speed);
			} else
				aiming = 1;
			if (pressed & PAD_A)
				e_talk_key(E_KEY_NEXT);
			else if (pressed & PAD_B)
				e_talk_key(E_KEY_BACK);
			if (!e_talk_targeting(&aim_x, &aim_y))
				aiming = 0;
			touched = 1;
			a_held = 1;
		} else if (!e_talk_active() && e_inv_active()) {
			/* containers open: the pad steps through the top one's items; A uses one, C moves
			 * it across, B shuts it, Option all; the world waits */
			static const uint32_t bit[7] = { 1UL << 20, 1UL << 21, 1UL << 22, 1UL << 23, PAD_A, PAD_B, PAD_C };
			static const int key[7] = { E_KEY_UP, E_KEY_DOWN, E_KEY_LEFT, E_KEY_RIGHT, E_KEY_NEXT,
				E_KEY_BACK, E_KEY_TAKE };
			for (int k = 0; k < 7; k++)
				if (pressed & bit[k])
					e_inv_key(key[k]);
			if (pressed & PAD_OPTION)
				e_inv_avatar_key();
			touched = 1;
			a_held = 1;
		} else if (e_talk_active()) {
			/* talking: A on (or the answer picked), the pad moves the pointer; the world waits */
			if (pressed & PAD_A)
				e_talk_key(E_KEY_NEXT);
			if (pressed & (1UL << 20))
				e_talk_key(E_KEY_UP);
			if (pressed & (1UL << 21))
				e_talk_key(E_KEY_DOWN);
			if (pressed & (1UL << 22))
				e_talk_key(E_KEY_LEFT);
			if (pressed & (1UL << 23))
				e_talk_key(E_KEY_RIGHT);
			/* the keypad: digits type a number asked for, * clears it, # takes it (as A) */
			uint32_t typed = pad_take(&vbl_typed);
			for (int d = 0; d < 10; d++)
				if (typed & 1u << d)
					e_talk_key(E_KEY_DIGIT + d);
			if (typed & KEY_STAR)
				e_talk_key(E_KEY_CLEAR);
			if (typed & KEY_HASH)
				e_talk_key(E_KEY_NEXT);
			if (pressed || typed)
				touched = 1;
			if (e_talk_active() || pressed & PAD_A)
				a_held = 1;     /* (the A that ended it is not a door's) */
		} else if (pressed & PAD_A) {
			/* A: the door ahead opened or shut (the engine's, as the original's doors do it);
			 * else the item ahead used, as the original's double-click */
			static const int8_t face_x[4] = { 0, 1, 0, -1 }, face_y[4] = { -1, 0, 1, 0 };
			struct EThing t;
			int door = e_door_ahead(e_walk_frame(type, walk_dir, 0), ax, ay, az,
				face_x[walk_dir], face_y[walk_dir]);
			touched = 1;
			if (door >= 0) {
				/* (locked: its usecode says so) */
				if (!e_door_toggle(door) && e_ref_thing(door + 1, &t) && e_usable(t.tf))
					e_talk_use(&t);
			} else if (e_thing_ahead(ax, ay, az, face_x[walk_dir], face_y[walk_dir], e_inv_actionable, &t)
				&& !e_inv_open_thing(&t))
				e_talk_use(&t);
		} else if (pressed & PAD_PAUSE) {
			/* Pause: the save menu (the avatar where it is, to be saved) */
			touched = 1;
			if (!posed)
				e_world_avatar_place(ax, ay, az, walk_dir);
			e_menu_open();
		} else if (pressed & PAD_C) {
			/* C: combat mode on or off (the original's C key) */
			touched = 1;
			e_combat_mode(!e_combat_mode(-1));
		} else if (pressed & PAD_OPTION) {
			/* Option: the avatar's paperdoll (again: the next party member's) */
			touched = 1;
			e_inv_avatar_key();
		} else if (pressed & PAD_B) {
			/* B: the NPC ahead (or the nearest within 2 cells) spoken to */
			static const int8_t face_x[4] = { 0, 1, 0, -1 }, face_y[4] = { -1, 0, 1, 0 };
			int number = e_world_npc_near(ax, ay, face_x[walk_dir], face_y[walk_dir]);
			touched = 1;
			if (number > 0)
				e_talk_start(number);
		}
	}

#ifdef ENGINE_FRAME_SUMS
	/* by frames, not time, so the frames hang on the frame count alone */
	uint32_t now = (sum_frames++ / 2) * WALK_BLANKS;
	while (now - walk_tick >= WALK_BLANKS) {
#else
	/* a long frame (the card slow, a file opened again) not made up all at once: the avatar
	 * would jump */
	if (jag_ticks - walk_tick > 3 * WALK_BLANKS)
		walk_tick = jag_ticks - WALK_BLANKS;
	while (jag_ticks - walk_tick >= WALK_BLANKS) {
#endif
		uint32_t held = pad();
		int dx = 0, dy = 0;
		walk_tick += WALK_BLANKS;
		if (held)
			touched = 1;
		if (e_talk_active() || e_inv_active() || e_menu_active()) {
			a_held = (held & PAD_A) != 0;
			continue;
		}
		/* the items' scripts a tick on; a function they (or the schedules) ask for run; faded
		 * out (asleep), nothing else */
		e_uc_scripts_step();
		{
			int f, it, ev;
			if (e_uc_request(&f, &it, &ev)) {
				e_talk_start_ref(f, it, ev);
				continue;
			}
		}
		if (e_uc_faded())
			continue;
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
		if (dx == 0 && dy == 0 && !posed && e_combat_mode(-1)) {
			/* in combat, the pad leaving it be: the avatar fights on its own */
			int fx, fy;
			uint16_t tf;
			int r = e_world_avatar_fight(&fx, &fy, &tf);
			if (r == 1) {
				dx = fx;
				dy = fy;
			} else if (r == 2) {
				e_mover_set(0, tf, ax, ay, az);
				swing_ticks = 2;
			}
		}
		if (swing_ticks > 0 && --swing_ticks == 0)
			e_mover_set(0, e_walk_frame(type, walk_dir, 0), ax, ay, az);
		if (dx == 0 && dy == 0) {
			/* the party settles about it */
			e_world_avatar_place(ax, ay, az, walk_dir);
			e_world_party_step(0);
			continue;
		}
		if (posed) {
			/* up out of the chair or the bed first */
			e_world_avatar_stand();
			posed = 0;
			continue;
		}
		walking = 1;
		walk_dir = (uint8_t) (dy < 0 ? 0 : dx > 0 ? 1 : dy > 0 ? 2 : 3);
		e_mover_self(0);            /* the avatar out of its own way */
		int step = e_walk(e_walk_frame(type, walk_dir, 0), &ax, &ay, &az, dx, dy, 1);
		e_mover_self(-1);
		e_world_avatar_place(ax, ay, az, walk_dir);
		e_world_party_step(step == E_STEP_MADE);
		if (step != E_STEP_MADE)
			continue;
		walk_steps++;
		moved = 1;
	}
	if (walking || moved)
		e_mover_set(0, e_walk_frame(type, walk_dir, walk_steps), ax, ay, az);
	if (!e_talk_active() && !e_inv_active() && !e_menu_active())
	{
		if (!posed)
			e_world_avatar_place(ax, ay, az, walk_dir);
		uint32_t t0 = eb_clock();
		e_world_step(world_clock(), ax, ay);
		world_time += eb_clock() - t0;
	}
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

/* The shape file read into the GameDrive's memory (the store, not yet the engine's, the room it
 * comes through), a bar along the screen as it goes; then a little read back to see it kept. */
static void rom_load(void)
{
	uint16_t gd = GD_FOpen(ROM_FILE, GD_FOPEN_READ);
	if ((int16_t) gd < 0)
		return;
	uint32_t size = GD_FSize(gd);
	if (size == 0 || ROM_START + size > ROM_END) {
		GD_FClose(gd);
		return;
	}
	volatile uint16_t *clut = (volatile uint16_t *) 0xF00400;
	clut[0] = 0x0000;
	clut[1] = 0x8420;               /* (grey, 5R 5B 6G: the palette is not read yet) */
	uint8_t *pixels = surfaces[0].pixels;
	memset(pixels, 0, W * H);
	for (int x = 40; x < W - 40; x++)
		pixels[(H / 2 - 6) * W + x] = pixels[(H / 2 + 6) * W + x] = 1;
	for (int y = H / 2 - 6; y <= H / 2 + 6; y++)
		pixels[y * W + 40] = pixels[y * W + W - 41] = 1;
	show(pixels);
	GD_ROMWriteEnable(1);
	uint32_t done = 0;
	int ok = 1;
	while (done < size) {
		uint32_t n = size - done > READ_MOST ? READ_MOST : size - done;
		if ((int16_t) GD_FRead(gd, store, n, GD_FREAD_CPU) < 0) {
			ok = 0;
			break;
		}
		rom_copy(done, NULL, store, n);
		done += n;
		/* the bar */
		int filled = (int) ((uint64_t) done * (W - 84) / size);
		for (int y = H / 2 - 4; y <= H / 2 + 4; y++)
			memset(pixels + y * W + 42, 1, (size_t) filled);
		if ((done & 0x3FFFF) < READ_MOST)
			show(pixels);
	}
	GD_ROMWriteEnable(0);
	for (uint16_t page = 1; page <= 5; page++)
		GD_ROMSetPage(page, page);      /* (banks 0-5 as they are) */
	if (ok) {
		/* kept? a stretch at the start and at the end read back against the card's */
		static const uint32_t looks[2] = { 0, 1 };
		for (int k = 0; k < 2 && ok; k++) {
			uint32_t at = looks[k] ? (size > 4096 ? size - 4096 : 0) : 0, n = size - at < 4096 ? size - at : 4096;
			if ((int16_t) GD_FSeek(gd, GD_FSEEK_SET, (int32_t) at) < 0
				|| (int16_t) GD_FRead(gd, store, n, GD_FREAD_CPU) < 0) {
				ok = 0;
				break;
			}
			rom_copy(at, store + 8192, NULL, n);
			if (memcmp(store, store + 8192, n) != 0)
				ok = 0;
		}
	}
	GD_FClose(gd);
	if (ok)
		rom_size = (int32_t) size;
	memset(pixels, 0, W * H);
	show(pixels);
}

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
	STAGE(0xF800);
#ifdef ENGINE_FAULTS
	faults_catch();
#endif
	jag_vbl_hook = pad_vbl;     /* (the pad read each blank from now on) */
	rom_load();
	if ((r = e_init("U7/", store, sizeof store)) != 0) {
		snprintf(text, sizeof text, "e_init failed: %d\n", r);
		STAGE(0xFFC0);
		DEBUG_TEXT(text);
		for (;;)
			;
	}
#ifdef ENGINE_STAGES
	staging = 0;
#ifdef ENGINE_LOAD_STAGES
	stage(0xFFFF);
#endif
#endif
	snprintf(text, sizeof text, "shape pool %luK of %luK\n", (unsigned long) e_pool_size() / 1024,
		(unsigned long) sizeof store / 1024);
	DEBUG_TEXT(text);
#ifdef ENGINE_STAGES
	stage(0x07FF);              /* cyan: loaded (a second) */
	for (uint32_t t = jag_ticks; jag_ticks - t < 60; )
		;
#endif
	set_palette(e_palette());
	sound_init();
#ifdef ENGINE_STAGES
	stage(0xF83F);
	for (uint32_t t = jag_ticks; jag_ticks - t < 60; )
		;
	set_palette(e_palette());
#endif
#ifdef ENGINE_MUSIC_TEST
	/* song n alone, nothing else going on (its audio against the host's --music); its samples
	 * captured into the frames (not shown) for a RAM dump to hold against the host's */
#ifndef ENGINE_STAGES
	MCTL[M_CAPEND] = (uint32_t) frames + sizeof frames;
	MCTL[M_CAP] = (uint32_t) frames;
#else
	/* (on the console: no capture, the DSP's state shown instead, the song alone) */
	memset(frames[0], 128, sizeof frames[0]);
#endif
	e_music_play(ENGINE_MUSIC_TEST);
	for (;;) {
		uint32_t t0 = jag_ticks;
		e_music_service();
#ifdef ENGINE_STAGES
		stage_status();
		stage_cells(frames[0]);
		show(frames[0]);
#endif
		while (jag_ticks == t0)
			;
	}
#endif
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
		DEBUG_TEXT(text);
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
#ifdef ENGINE_STAGES
	((volatile uint16_t *) 0xF00400)[0] = 0x07C0;     /* blue: in walk_init */
#endif
#ifndef ENGINE_NO_FRONT
	front_run();
	set_palette(e_palette());
#endif
	if (!walk_init()) {
		DEBUG_TEXT("no NPC.DAT: no avatar\n");
		for (;;)
			;
	}
#endif
	/* the profile's samples by Tom's timer, at 37 Hz (26.59 MHz / 1000 / 719), apart from the
	 * frames, which keep to the blanks */
#ifdef ENGINE_STAGES
	((volatile uint16_t *) 0xF00400)[0] = 0xFFC0;     /* magenta: past it */
#endif
#ifdef ENGINE_PROFILE
	/* Tom's timer for the profile's samples (37 Hz, apart from the frames): only in a profile
	 * build, not yet tried on a console */
	REG16(0xF00050) = 999;
	REG16(0xF00052) = 718;
	jag_int_enable = 1 | 8;
	jag_prof_pit = 1;
	jag_prof_end = profile + sizeof profile;
	jag_prof_next = profile;
#endif
#ifdef ENGINE_ROUTE_BENCH
	route_bench();
#endif
#ifdef ENGINE_STAGES
	((volatile uint16_t *) 0xF00400)[0] = 0xFFFF;     /* white: the loop next */
#endif
	DEBUG_TEXT("engine test: running\n");
	mark_ticks = last_shown = jag_ticks;
	for (;;) {
#ifdef ENGINE_STAGES
		/* (the frame before's parts timed: the music, the walk and the world, the drawing, the
		 * rest (shown, the talk); the longest frame's kept for the cells) */
		{
			uint32_t now = eb_clock();
			static uint32_t was_reads, was_bytes, was_seek, was_io;
			part[3] = now - part_at;
			if (part_from && now - part_from > part_long) {
				part_long = now - part_from;
				part_keep[0] = io_reads - was_reads;
				part_keep[1] = io_bytes - was_bytes;
				part_keep[2] = io_seek - was_seek;
				part_keep[3] = io_units - was_io;
			}
			was_reads = io_reads;
			was_bytes = io_bytes;
			was_seek = io_seek;
			was_io = io_units;
			part_from = part_at = now;
		}
#endif
		e_music_service();
#ifdef ENGINE_STAGES
		stage_status();
		part_mark(0);
#endif
#ifndef ENGINE_TOUR
		walk_update();
#ifdef ENGINE_STAGES
		part_mark(1);
#endif
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
#ifdef ENGINE_PROFILE
		if (!profile_saved && jag_prof_next >= jag_prof_end) {
			uint16_t f = GD_FOpen("U7/PCPROF.BIN", GD_FOPEN_WRITE | GD_FOPEN_CREATE_ALWAYS);
			profile_saved = 1;
			if ((int16_t) f >= 0) {
				GD_FWrite(f, profile, sizeof profile);
				GD_FClose(f);
				DEBUG_TEXT("profile saved\n");
			}
		}
#endif
		/* talking, the world waits: once each frame holds what it shows, nothing is drawn
		 * till it changes (the letters, a shape each, would halve the frame rate) */
		static uint32_t talk_seen, talk_drawn;
		if (e_talk_active() || e_inv_active() || e_menu_active()) {
			if (e_talk_changes() + e_inv_changes() + e_menu_changes() != talk_seen) {
				talk_seen = e_talk_changes() + e_inv_changes() + e_menu_changes();
				talk_drawn = 0;
			} else if (talk_drawn >= FRAMES) {
				uint32_t t = jag_ticks;
				while (jag_ticks == t)
					;
				continue;
			}
			talk_drawn++;
		}
		struct ESurface *drawing = &surfaces[(frame + 1) % FRAMES];
		e_set_view(x, y);
		if (e_uc_faded())
			eb_clear(drawing, 0, 0, W, H);  /* (faded out: asleep) */
		else
			e_draw(drawing);
#ifdef ENGINE_STAGES
		eb_finish();
		part_mark(2);
		stage_cells(drawing->pixels);
#endif
		if (e_talk_active()) {
			e_talk_draw(drawing);
			eb_finish();
		} else if (e_inv_active()) {
			e_inv_draw(drawing);
			eb_finish();
		}
		if (e_menu_active()) {
			e_menu_draw(drawing);
			eb_finish();
		}
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
			DEBUG_TEXT(text);
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
			DEBUG_TEXT(text);
			snprintf(text, sizeof text, "world %lu.%lu blanks over 64 frames\n", tenths(world_time) / 10,
				tenths(world_time) % 10);
			DEBUG_TEXT(text);
			world_time = 0;
			if (e_stats.routes_planned != 0) {
				/* routes planned (found), steps tried, blanks spent in all and the most one took */
				snprintf(text, sizeof text, "routes %lu steps %lu blanks %lu.%lu worst %lu.%lu\n",
					(unsigned long) e_stats.routes_planned, (unsigned long) e_stats.route_steps,
					tenths(e_stats.route_time) / 10, tenths(e_stats.route_time) % 10,
					tenths(e_stats.route_worst) / 10, tenths(e_stats.route_worst) % 10);
				DEBUG_TEXT(text);
			}
			mark_ticks = now;
			mark_frame = frame;
		}
	}
}
