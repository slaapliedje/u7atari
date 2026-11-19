/* Video: the game draws a 320x200 chunky 8-bit frame; both machines show 256 colours only as 8
 * interleaved bitplanes, so every present is a chunky-to-planar conversion into ST RAM, the only
 * RAM the shifter or VIDEL can fetch from.
 *
 *   TT030:   TT low (EsetShift 0x0700), 320x480. Each game line is written twice: 400 lines,
 *            centred.
 *   Falcon:  VIDEL 8 bits a pixel, 40 columns. VGA: 320x240 line-doubled (VERTFLAG), centred;
 *            RGB/TV: 320x200.
 *
 * Two screens: a frame is converted into the hidden one, which is then shown from the next
 * vertical blank. Each screen remembers the frame it holds, so only the 16-pixel groups that
 * changed since are converted again (c2p.S, which takes rows of 320 pixels). On the Falcon, the
 * game's copy of its finished frame to the screen buffer (plat_video_copy_frame) converts it as
 * it copies, so the present that follows only shows it.
 *
 * Each screen is a window onto a taller area of screen memory, on the Falcon a wider one too
 * (VIDEL's line offset skips the rest of each line; the TT has none), and its shadow covers the
 * whole area, always matching what the planes there show. When the view scrolls, the frame is
 * mostly the last one moved: the window moves to where the area already holds it, by changing
 * the screen address, and only what really differs is converted. The window moves in whole
 * 16-pixel groups and game rows; when it nears an edge and a frame is mostly new anyway, it jumps
 * to the far side. On the TT it moves only up and down, in steps of 8 rows: 5120 bytes, so the
 * screen address keeps to the 256 bytes Setscreen takes.
 *
 * Palette writes go to the hardware at once, as the VGA DAC took them; fades rely on that.
 *
 * The mouse pointer is drawn onto the screen on display at each vertical blank (pointer.c), so it
 * moves at the display's rate whatever the game's.
 *
 * With an ATW800/2 graphics card on a TT (atw800.c), the frames and colours go there instead, and
 * the game draws the pointer into its frames.
 *
 * Serpent Isle's debug screens switch to 80x25 colour text (console.c); it is drawn into a frame
 * of its own, 4x8 pixels a cell, in the 16 text colours, with the game's palette put back after.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mint/osbind.h>
#include <mint/falcon.h>
#include <mint/sysvars.h>

#include "plat.h"
#include "backend.h"
#include "game.h"
#include "vgafont8.h"

#define WIDTH PLAT_SCREEN_WIDTH
#define HEIGHT PLAT_SCREEN_HEIGHT
#define TEXT_COLUMNS 80
#define TEXT_ROWS 25
/* 8 planes of 320 pixels */
#define LINE_BYTES WIDTH
_Static_assert(WIDTH == 320, "c2p.S converts rows of 320 pixels");

#define TT_LOW 0x0700

typedef struct {
	uint8_t *base;              /* the first line shown, in ST RAM */
	uint8_t *shadow;            /* the chunky frame this screen shows; NULL until it has one */
	uint8_t *area;              /* its screen memory, 256-byte aligned; base lies in it */
	uint8_t *shadow_area;       /* scrolling: the whole area's pixels, as the planes show them */
	int16_t x, y;               /* scrolling: where the frame lies in the area */
	int16_t last_dx, last_dy;   /* scrolling: the window's last move, tried first next time */
} screen;

/* The scrolling screens: the area each frame is a window of, in game rows, and on the Falcon
 * its width. */
#define SCROLL_LINE_BYTES 640
#define SCROLL_ROWS 480
#define SCROLL_REACH 48         /* the farthest a frame is looked for, in pixels and lines */
#define SAMPLES (17 * 8)        /* longs of the frame compared to find it */

bool video_no_card;
bool video_fps;
static bool card;
static bool active;
static void *screen_memory;
/* TOS's logical screen while the game runs: console text lands here, off the display, and a
 * desktop screen smaller than the game mode is never written past its end. */
static void *console_memory;
static screen screens[2];
static int hidden;              /* the screen the next frame goes into */
static uint32_t shown_at_vbl;   /* when the other screen was handed to the hardware */
static int16_t screen_lines;    /* lines in the hardware mode */
static int16_t top_line;        /* the first line the game's frame occupies */
static int16_t line_repeat;     /* 2 on the TT, which shows each game line twice */
static int32_t line_bytes = LINE_BYTES;    /* from one screen line to the next */
static int32_t shadow_stride = WIDTH;      /* from one row of a screen's shadow to the next */
static int32_t area_bytes;      /* each screen's memory */
static bool scrolling;
static void *shadow_memory;

static const uint8_t *last_pixels;
/* The screen plat_video_copy_frame converted the frame at prepared_pixels into, or NULL. */
static screen *prepared;
static const uint8_t *prepared_pixels;
static uint8_t palette[768];               /* the game's, 6-bit VGA */
static bool refresh_wanted;
/* Rows of the last frame to show again, where the game's pointer moved; none while bottom < 0. */
static int16_t rows_top, rows_bottom = -1;

/* Text mode: the cells drawn as a frame, shown instead of the game's. */
static bool text_shown;
static uint8_t text_frame[WIDTH * HEIGHT];
/* the VGA text colours, 6-bit */
static const uint8_t text_palette[16 * 3] = {
	0, 0, 0,  0, 0, 42,  0, 42, 0,  0, 42, 42,  42, 0, 0,  42, 0, 42,  42, 21, 0,  42, 42, 42,
	21, 21, 21,  21, 21, 63,  21, 63, 21,  21, 63, 63,  63, 21, 21,  63, 21, 63,  63, 63, 21,
	63, 63, 63,
};

/* What the desktop had, to put back. */
static void *old_logical, *old_physical;
static int16_t old_mode;
static uint16_t old_tt_palette[256];
static uint32_t old_falcon_palette[256];

static uint32_t vbl_count(void)
{
	return *_frclock;
}

/* Where the shifter or VIDEL is reading the screen now: the video address counter, read until
 * two reads agree, as it may move between the bytes. */
static uint32_t scan_address(void)
{
	volatile uint8_t *counter = (volatile uint8_t *) 0xffff8205;
	uint32_t a, b;

	do {
		a = (uint32_t) counter[0] << 16 | (uint32_t) counter[2] << 8 | counter[4];
		b = (uint32_t) counter[0] << 16 | (uint32_t) counter[2] << 8 | counter[4];
	} while ((a ^ b) & ~UINT32_C(0xff));
	return b;
}

/* The screen address, written straight to VIDEL (high, middle, then low byte, which must come
 * last), as Setscreen does under EmuTOS: the window moves in 16-byte steps, finer than the
 * 256 bytes older TOS calls promise. screenpt is cleared, so no TOS vertical blank puts an older
 * address back. Taken by the hardware at the next blank. */
static void set_base(uint8_t *base)
{
	volatile uint8_t *video = (volatile uint8_t *) 0xffff8201;
	uint32_t a = (uint32_t) (uintptr_t) base;
	uint16_t sr;

	__asm__ volatile ("move.w %%sr,%0; ori.w #0x0700,%%sr" : "=d" (sr) :: "memory");
	*(volatile uint32_t *) 0x45e = 0;    /* screenpt */
	video[0] = (uint8_t) (a >> 16);
	video[2] = (uint8_t) (a >> 8);
	video[12] = (uint8_t) a;
	__asm__ volatile ("move.w %0,%%sr" :: "d" (sr) : "memory");
}

static void show(uint8_t *base)
{
	/* the pointer goes there from the blank the hardware takes it at */
	pointer_screen(text_shown ? NULL : base);
	if (scrolling && atari_machine != MACHINE_TT)
		set_base(base);
	else
		VsetScreen(-1L, base, -1, -1);
}

/* The rows VGA shows above and below the frame stay black (colour 0, all planes clear). */
static void clear_row(screen *s, int32_t row)
{
	uint32_t *shadow = (uint32_t *) (s->shadow_area + row * shadow_stride + s->x);

	for (int i = 0; i < WIDTH / 4; i++)
		if (shadow[i] != 0) {
			memset(shadow, 0, WIDTH);
			for (int16_t k = 0; k < line_repeat; k++)
				memset(s->area + (row * line_repeat + k) * line_bytes + s->x, 0, WIDTH);
			return;
		}
}

/* Puts screen s's window with the frame at x, y (in rows) in its area. */
static void place(screen *s, int16_t x, int16_t y)
{
	int16_t top_rows = top_line / line_repeat, rows = screen_lines / line_repeat;

	s->x = x;
	s->y = y;
	s->base = s->area + ((int32_t) y * line_repeat - top_line) * line_bytes + x;
	s->shadow = s->shadow_area + (int32_t) y * shadow_stride + x;
	for (int32_t row = y - top_rows; row < y; row++)
		clear_row(s, row);
	for (int32_t row = y + HEIGHT; row < y - top_rows + rows; row++)
		clear_row(s, row);
}

/* How many of the sampled longs of the frame the area holds with the frame at x, y. */
static int16_t matches(const screen *s, const uint8_t *in, int16_t x, int16_t y)
{
	const uint8_t *area = s->shadow_area + (int32_t) y * shadow_stride + x + 20;
	int16_t n = 0;

	in += 20;
	for (int16_t row = 6; row < HEIGHT; row += 12) {
		const uint32_t *a = (const uint32_t *) (in + row * WIDTH);
		const uint32_t *b = (const uint32_t *) (area + row * shadow_stride);

		for (int16_t i = 0; i < WIDTH / 4 - 5; i += 10)
			n += a[i] == b[i];
	}
	return n;
}

/* Moves screen s's window to where its area best holds the frame already. */
static void follow_scroll(screen *s, const uint8_t *in)
{
	int16_t max_x = (int16_t) (shadow_stride - WIDTH);
	int16_t min_y = top_line / line_repeat;
	int16_t max_y = (int16_t) (SCROLL_ROWS - screen_lines / line_repeat + min_y);
	int16_t here = matches(s, in, s->x, s->y), best = here, best_x = s->x, best_y = s->y;

	if (here >= SAMPLES - SAMPLES / 8)
		return;
	/* the last move first: the view tends to keep going the way it went */
	for (int16_t k = -1; k < 13 * 7 && best < SAMPLES - SAMPLES / 8; k++) {
		int16_t dx = k < 0 ? s->last_dx : (int16_t) (k % 7 * 16 - SCROLL_REACH);
		int16_t dy = k < 0 ? s->last_dy : (int16_t) (k / 7 * 8 - SCROLL_REACH);
		int16_t x = (int16_t) (s->x + dx), y = (int16_t) (s->y + dy), n;

		if ((dx == 0 && dy == 0) || x < 0 || x > max_x || y < min_y || y > max_y)
			continue;
		n = matches(s, in, x, y);
		if (n > best) {
			best = n;
			best_x = x;
			best_y = y;
		}
	}
	if (best >= here + SAMPLES / 4) {
		s->last_dx = (int16_t) (best_x - s->x);
		s->last_dy = (int16_t) (best_y - s->y);
		place(s, best_x, best_y);
		return;
	}
	/* Nothing explains the frame, so it is mostly converted anyway: a window near an edge,
	 * where the view may have run out of room, jumps to the far side for nothing. */
	if (here < SAMPLES / 4) {
		int16_t x = s->x, y = s->y;

		if (x < SCROLL_REACH)
			x = max_x;
		else if (x > max_x - SCROLL_REACH)
			x = 0;
		if (y < min_y + SCROLL_REACH)
			y = max_y;
		else if (y > max_y - SCROLL_REACH)
			y = min_y;
		if (x != s->x || y != s->y)
			place(s, x, y);
	}
}

/* The scrolling screens, when there is memory enough for them and the game still. */
static bool start_scrolling(void)
{
	int32_t width = atari_machine == MACHINE_TT ? WIDTH : SCROLL_LINE_BYTES;
	size_t bytes = (size_t) width * line_repeat * SCROLL_ROWS;
	size_t shadow_bytes = (size_t) width * SCROLL_ROWS;

	screen_memory = (void *) Mxalloc(2 * bytes + 256, 0);    /* ST RAM */
	if (screen_memory == NULL || (long) screen_memory < 0)
		return false;
	shadow_memory = (void *) Mxalloc(2 * shadow_bytes, 3);
	if (shadow_memory == NULL || (long) shadow_memory < 0 ||
		(Mxalloc(-1L, 1) < (long) MEMORY_NEEDED && Mxalloc(-1L, 0) < (long) MEMORY_NEEDED)) {
		if (shadow_memory != NULL && (long) shadow_memory > 0)
			Mfree(shadow_memory);
		Mfree(screen_memory);
		return false;
	}
	line_bytes = width;
	shadow_stride = width;
	area_bytes = (int32_t) bytes;
	for (int i = 0; i < 2; i++) {
		screen *s = &screens[i];

		s->area = (uint8_t *) (((uintptr_t) screen_memory + 255) & ~(uintptr_t) 255) + i * bytes;
		s->shadow_area = (uint8_t *) shadow_memory + i * shadow_bytes;
		memset(s->area, 0, bytes);
		memset(s->shadow_area, 0, shadow_bytes);
		s->last_dx = s->last_dy = 0;
		/* an even row: on the TT, with steps of 8, the screen address stays 256-byte aligned */
		place(s, (width - WIDTH) / 2 & ~15, (int16_t) ((SCROLL_ROWS - HEIGHT) / 2) & ~1);
	}
	return true;
}

static bool setup_tt(void)
{
	old_mode = EgetShift();
	EgetPalette(0, 256, old_tt_palette);
	screen_lines = 480;
	line_repeat = 2;
	top_line = (screen_lines - HEIGHT * line_repeat) / 2;
	return true;
}

static int16_t falcon_mode;

static bool setup_falcon(void)
{
	int16_t current = VsetMode(-1);

	old_mode = current;
	VgetRGB(0, 256, old_falcon_palette);
	if (VgetMonitor() == 2) {
		falcon_mode = BPS8 | COL40 | VGA | VERTFLAG;
		screen_lines = 240;
	} else {
		falcon_mode = BPS8 | COL40 | (current & PAL);
		screen_lines = 200;
	}
	line_repeat = 1;
	top_line = (screen_lines - HEIGHT) / 2;
	return VgetMonitor() != 0;   /* no 256 colours on the mono monitor */
}

void video_init(void)
{
	size_t bytes;

	if (atari_machine == MACHINE_TT && !video_no_card && atw800_start()) {
		card = true;
		shown_at_vbl = vbl_count();
		active = true;
		return;
	}
	old_logical = Logbase();
	old_physical = Physbase();
	if (atari_machine == MACHINE_TT ? !setup_tt() : !setup_falcon())
		plat_fatal(GAME_NAME " needs a colour or VGA monitor.");

	bytes = (size_t) screen_lines * LINE_BYTES;
	scrolling = start_scrolling();
	if (!scrolling) {
		screen_memory = (void *) Mxalloc(2 * bytes + 256, 0);    /* ST RAM */
		if (screen_memory == NULL || (long) screen_memory < 0)
			plat_fatal("Not enough ST RAM for the screens.");
		area_bytes = (int32_t) bytes;
		for (int i = 0; i < 2; i++) {
			screens[i].area = (uint8_t *) (((uintptr_t) screen_memory + 255) & ~(uintptr_t) 255) + i * bytes;
			screens[i].base = screens[i].area;
			memset(screens[i].base, 0, bytes);
			screens[i].shadow = NULL;
		}
	}
	console_memory = (void *) Mxalloc(bytes, 3);
	if (console_memory == NULL || (long) console_memory < 0)
		plat_fatal("Not enough memory for the console screen.");

	if (atari_machine == MACHINE_TT) {
		EsetShift(TT_LOW);
		VsetScreen(console_memory, screens[0].base, -1, -1);
	} else {
		VsetScreen(console_memory, screens[0].base, 3, falcon_mode);
		if (scrolling) {
			/* the words VIDEL skips at the end of each line, past the 320 bytes shown */
			*(volatile uint16_t *) 0xffff820e = (uint16_t) ((SCROLL_LINE_BYTES - LINE_BYTES) / 2);
			set_base(screens[0].base);
		}
	}
	hidden = 1;
	shown_at_vbl = vbl_count();
	active = true;
#ifdef U7_DEBUG_LOG
	if (lockstep)
		return;     /* the PC's frames have the game's pointer in them */
#endif
	if (pointer_start(top_line, line_repeat, line_bytes))
		pointer_screen(screens[0].base);
}

void video_shutdown(void)
{
	if (!active)
		return;
	active = false;
	pointer_stop();
	if (card) {
		atw800_stop();
	} else if (atari_machine == MACHINE_TT) {
		EsetShift(old_mode);
		VsetScreen(old_logical, old_physical, -1, -1);
		EsetPalette(0, 256, old_tt_palette);
	} else {
		*(volatile uint16_t *) 0xffff820e = 0;
		VsetScreen(old_logical, old_physical, 3, old_mode);
		VsetRGB(0, 256, old_falcon_palette);
	}
	Vsync();
}

/* A new program starts with a black palette and no frame of its own. */
void video_reset(void)
{
	static const uint8_t black[256 * 3];

	last_pixels = NULL;
	prepared = NULL;
	text_shown = false;
	pointer_reset();
	if (active)
		plat_video_set_palette(0, 256, black);
}

static void write_palette(int16_t first, int16_t count, const uint8_t *rgb)
{
	if (card) {
		atw800_palette(first, count, rgb);
	} else if (atari_machine == MACHINE_TT) {
		uint16_t words[256];

		/* 0000 RRRR GGGG BBBB from 6-bit VGA */
		for (int16_t i = 0; i < count; i++)
			words[i] = (uint16_t) ((rgb[3 * i] & 0x3c) << 6 | (rgb[3 * i + 1] & 0x3c) << 2 |
				(rgb[3 * i + 2] & 0x3c) >> 2);
		EsetPalette(first, count, words);
	} else {
		uint32_t longs[256];

		/* 0x00RRGGBB, 6 bits widened to 8 */
		for (int16_t i = 0; i < count; i++) {
			uint32_t r = rgb[3 * i] & 0x3f, g = rgb[3 * i + 1] & 0x3f, b = rgb[3 * i + 2] & 0x3f;

			longs[i] = (r << 2 | r >> 4) << 16 | (g << 2 | g >> 4) << 8 | (b << 2 | b >> 4);
		}
		VsetRGB(first, count, longs);
	}
}

void plat_video_set_palette(int16_t first, int16_t count, const uint8_t *rgb)
{
	if (!active)
		return;
	if (first < 0) {
		rgb -= 3 * first;
		count += first;
		first = 0;
	}
	if (first + count > 256)
		count = 256 - first;
	if (count <= 0)
		return;
	memcpy(palette + 3 * first, rgb, (size_t) count * 3);
#ifdef U7_DEBUG_LOG
	if (lockstep)
		lockstep_palette(first, count, rgb);
#endif
	if (!text_shown)
		write_palette(first, count, rgb);
}

/* c2p.S: converts the groups of 16 pixels that differ from the shadow, or all of them, copying
 * the frame to copy as it reads it when copy is not NULL. */
void c2p_frame(const uint8_t *in, uint8_t *shadow, uint8_t *out, int32_t rows, int32_t outStride,
	int32_t secondLine, int32_t full, uint8_t *copy, int32_t shadowStride);

/* Converts the pixels that differ from what screen s shows, copying the frame to copy as it
 * goes when that is not NULL (the Falcon only). */
static void convert(screen *s, const uint8_t *pixels, uint8_t *copy)
{
	bool full = s->shadow == NULL;

	if (full && (s->shadow = malloc(WIDTH * HEIGHT)) == NULL)
		plat_fatal("Out of memory for the screen copy.");
	if (card) {
		atw800_frame(pixels, s->shadow, full);
		return;
	}
	if (scrolling)
		follow_scroll(s, pixels);
	c2p_frame(pixels, s->shadow, s->base + top_line * line_bytes, HEIGHT, line_repeat * line_bytes,
		line_repeat == 2 ? line_bytes : 0, full, copy, shadow_stride);
}

/* The screen the next frame goes into, once it may be written. */
static screen *hidden_screen(void)
{
	screen *s;

	/* The screen handed over last time is showing once a blank has passed; until then the
	 * hidden one may still be on display. */
	while (vbl_count() == shown_at_vbl)
		;
	s = &screens[card ? 0 : hidden];
	/* The blank does not always mean the hardware took the new address: when it still reads
	 * this screen, a conversion starting at the top is overtaken by the beam, and its top rows
	 * flicker. Wait for the next frame then, but not past a second blank. */
	if (!card) {
		uint32_t area = (uint32_t) (uintptr_t) s->area;

		while (scan_address() - area < (uint32_t) area_bytes && vbl_count() - shown_at_vbl < 2)
			;
	}
	return s;
}

static void show_frame(const uint8_t *pixels)
{
	screen *s;

	if (!active)
		return;
	s = hidden_screen();
#ifdef U7_DEBUG_LOG
	{
		static uint32_t presents;
		static uint16_t counts[256];
		int top = 0;
		char text[96];

		/* Only on the presents it reports: counting costs a frame's worth of time. Not in
		 * lockstep, where reading the clock moves it. */
		if (presents++ % 20 == 0 && !lockstep) {
			memset(counts, 0, sizeof counts);
			for (int i = 0; i < WIDTH * HEIGHT; i++)
				counts[pixels[i]]++;
			for (int i = 1; i < 256; i++)
				if (counts[i] > counts[top])
					top = i;
			snprintf(text, sizeof text, "present %lu: top colour %d x%u, tick %lu, pointer %lu\r\n",
				(unsigned long) presents, top, counts[top], (unsigned long) plat_ticks(),
				(unsigned long) pointer_draws);
			plat_log(text);
		}
	}
#endif
	if (s != prepared || pixels != prepared_pixels)
		convert(s, pixels, NULL);
	prepared = NULL;
	rows_bottom = -1;
	if (!card)
		show(s->base);
	shown_at_vbl = vbl_count();
	hidden ^= 1;
}

/* --fps: the frames presented in each 10 s, kept in memory, as writing a file mid-game would
 * stall it; video_fps_report() writes them out on leaving. */
#define FPS_PERIOD (10 * 200)       /* in _hz_200 ticks */
#define FPS_PERIODS 720             /* two hours */
static uint16_t fps_frames[FPS_PERIODS];
static uint16_t fps_periods;
static uint32_t fps_start, fps_period_start, fps_total;

static void count_frame(void)
{
	uint32_t now = *_hz_200;

	if (fps_start == 0)
		fps_start = fps_period_start = now;
	while (now - fps_period_start >= FPS_PERIOD && fps_periods < FPS_PERIODS) {
		fps_periods++;
		fps_period_start += FPS_PERIOD;
	}
	if (fps_periods < FPS_PERIODS)
		fps_frames[fps_periods]++;
	fps_total++;
}

void video_fps_report(void)
{
	FILE *f;
	uint32_t ticks;

	if (!video_fps || fps_start == 0 || (f = fopen("U7FPS.TXT", "w")) == NULL)
		return;
	ticks = *_hz_200 - fps_start;
	fprintf(f, "Frames a second, each 10 seconds from the first frame:\r\n");
	for (uint16_t i = 0; i < fps_periods; i++)
		fprintf(f, "%4u s  %3u.%u\r\n", i * 10u, fps_frames[i] / 10u, fps_frames[i] % 10u);
	if (ticks > 0)
		fprintf(f, "All: %lu frames in %lu.%02lu s, %lu.%02lu a second\r\n",
			(unsigned long) fps_total, (unsigned long) (ticks / 200),
			(unsigned long) (ticks % 200 / 2), (unsigned long) (fps_total * 200 / ticks),
			(unsigned long) ((uint64_t) fps_total * 20000 / ticks % 100));
	fclose(f);
}

void plat_video_present(const uint8_t *pixels)
{
	if (video_fps)
		count_frame();
	last_pixels = pixels;
	refresh_wanted = false;
#ifdef U7_DEBUG_LOG
	if (lockstep)
		lockstep_frame(pixels, 'P');
#endif
	if (!text_shown)
		show_frame(pixels);
}

void plat_video_copy_frame(uint8_t *dst, const uint8_t *src)
{
	/* The TT's doubled lines and the card convert as the frame is shown. */
	if (!active || card || text_shown || line_repeat != 1) {
		memmove(dst, src, WIDTH * HEIGHT);
		return;
	}
	prepared = hidden_screen();
	convert(prepared, src, dst);
	prepared_pixels = dst;
}

void plat_video_wait_retrace(void)
{
	uint32_t now = vbl_count();

#ifdef U7_DEBUG_LOG
	if (lockstep) {
		lockstep_wait_retrace();
		return;
	}
#endif
	plat_pump();
	while (vbl_count() == now)
		;
}

#ifdef U7_DEBUG_LOG
/* The frame on screen as a binary PPM, for scripted tests. */
void video_shot(const char *name)
{
	static uint8_t row[WIDTH * 3];
	FILE *f;

	static uint8_t with_pointer[WIDTH * HEIGHT];
	const uint8_t *pixels = text_shown ? text_frame : last_pixels;
	const uint8_t *colours = text_shown ? text_palette : palette;

	if (pixels == NULL || (f = fopen(name, "wb")) == NULL)
		return;
	if (!text_shown && pointer_running()) {
		uint16_t sr;
		long wrong = 0;
		int x0 = WIDTH, y0 = HEIGHT, x1 = -1, y1 = -1;
		const uint8_t *on = screens[hidden ^ 1].base;

		/* Checks the screen on display against the frame with the pointer, once the blank has
		 * put the pointer on it, then with the blank held off, so the pointer stays put. */
		while (vbl_count() == shown_at_vbl)
			;
		__asm__ volatile ("move.w %%sr,%0; move.w #0x2700,%%sr" : "=d" (sr) :: "memory");
		memcpy(with_pointer, pixels, sizeof with_pointer);
		pointer_into(with_pointer);
		for (int y = 0; y < HEIGHT; y++) {
			const uint16_t *planes = (const uint16_t *) (on + (top_line + y * line_repeat) * line_bytes);

			for (int x = 0; x < WIDTH; x++) {
				uint8_t colour = 0;

				for (int p = 0; p < 8; p++)
					colour |= (uint8_t) ((planes[x / 16 * 8 + p] >> (15 - x % 16) & 1) << p);
				if (colour != with_pointer[y * WIDTH + x]) {
					wrong++;
					if (x < x0) x0 = x;
					if (x > x1) x1 = x;
					if (y < y0) y0 = y;
					if (y > y1) y1 = y;
				}
			}
		}
		__asm__ volatile ("move.w %0,%%sr" :: "d" (sr) : "memory");
		{
			char text[128];
			int16_t px, py;

			inputq_mouse_position(&px, &py);
			snprintf(text, sizeof text, "shot %s: %ld pixels differ on screen, in %d,%d-%d,%d; mouse %d,%d; drawn %lu\r\n",
				name, wrong, x0, y0, x1, y1, px / 2, py, (unsigned long) pointer_draws);
			plat_log(text);
		}
		pixels = with_pointer;
	}
	fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
	for (int y = 0; y < HEIGHT; y++) {
		for (int x = 0; x < WIDTH; x++)
			for (int c = 0; c < 3; c++) {
				uint8_t v = colours[3 * pixels[y * WIDTH + x] + c] & 0x3f;

				row[3 * x + c] = (uint8_t) (v << 2 | v >> 4);
			}
		fwrite(row, 1, sizeof row, f);
	}
	fclose(f);
	if (card)
		atw800_check(pixels, colours);
}
#endif

int16_t video_pointer(void)
{
	return pointer_running();
}

void video_pointer_shape(const uint8_t *pixels, int16_t width, int16_t height, int16_t hot_x,
	int16_t hot_y)
{
	pointer_shape(pixels, width, height, hot_x, hot_y);
}

void video_pointer_show(int16_t visible)
{
	pointer_show(visible != 0);
}

/* Shows the last frame again: the game may have drawn into it since. */
void video_refresh(void)
{
	refresh_wanted = false;
	if (text_shown)
		show_frame(text_frame);
	else if (last_pixels != NULL)
		show_frame(last_pixels);
}

void video_refresh_wanted(void)
{
	refresh_wanted = true;
}

void video_rows_wanted(int16_t top, int16_t bottom)
{
	if (top < 0)
		top = 0;
	if (bottom > HEIGHT - 1)
		bottom = HEIGHT - 1;
	if (top > bottom)
		return;
	if (rows_bottom < 0 || top < rows_top)
		rows_top = top;
	if (bottom > rows_bottom)
		rows_bottom = bottom;
}

/* Converts the rows the pointer moved in straight into the screen on display, as TOS draws its
 * mouse: a frame's worth of comparing for a few rows would be wasted. */
static void show_rows(void)
{
	screen *s = &screens[hidden ^ 1];
	int16_t top = rows_top, rows = rows_bottom - rows_top + 1;

	rows_bottom = -1;
	if (!active || s->shadow == NULL || card) {
		refresh_wanted = true;
		return;
	}
	c2p_frame(last_pixels + top * WIDTH, s->shadow + top * shadow_stride,
		s->base + (top_line + top * line_repeat) * line_bytes, rows, line_repeat * line_bytes,
		line_repeat == 2 ? line_bytes : 0, 0, NULL, shadow_stride);
}

void video_pump(void)
{
	if (rows_bottom >= 0 && !refresh_wanted && !text_shown && last_pixels != NULL)
		show_rows();
	if (refresh_wanted && !text_shown && last_pixels != NULL && vbl_count() != shown_at_vbl) {
		refresh_wanted = false;
		show_frame(last_pixels);
	}
}

uint8_t *video_frame(void)
{
	/* The game hands over frames it owns; the BIOS wrote into that same screen memory. */
	return (uint8_t *) last_pixels;
}

/* Each 8-pixel glyph row as 4 pixels, a pixel set where either of its two is. */
static uint8_t narrow_font[256][8];

static void make_narrow_font(void)
{
	for (int ch = 0; ch < 256; ch++)
		for (int y = 0; y < 8; y++) {
			uint8_t row = VgaFont8[ch][y], out = 0;

			for (int x = 0; x < 4; x++)
				if (row & (0xc0 >> 2 * x))
					out |= (uint8_t) (8 >> x);
			narrow_font[ch][y] = out;
		}
}

void video_show_text(const uint16_t *cells, int16_t cursor_x, int16_t cursor_y)
{
	static bool font_made;

	if (!font_made) {
		make_narrow_font();
		font_made = true;
	}
	for (int row = 0; row < TEXT_ROWS; row++)
		for (int column = 0; column < TEXT_COLUMNS; column++) {
			uint16_t cell = cells[row * TEXT_COLUMNS + column];
			/* bit 7 of the attribute is blink, as the VGA's default had it; shown steady */
			uint8_t fore = (uint8_t) (cell >> 8 & 15), back = (uint8_t) (cell >> 12 & 7);
			uint8_t *at = text_frame + row * 8 * WIDTH + column * 4;

			for (int y = 0; y < 8; y++, at += WIDTH) {
				uint8_t bits = narrow_font[cell & 0xff][y];

				if (row == cursor_y && column == cursor_x && y == 7)
					bits = 15;      /* the cursor, an underline */
				for (int x = 0; x < 4; x++)
					at[x] = bits & (8 >> x) ? fore : back;
			}
		}
	if (!text_shown && active)
		write_palette(0, 16, text_palette);
	text_shown = true;
	show_frame(text_frame);
}

void video_show_game(void)
{
	if (!text_shown)
		return;
	text_shown = false;
	if (!active)
		return;
	write_palette(0, 16, palette);
	if (last_pixels != NULL)
		show_frame(last_pixels);
}
