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
 * changed since are converted again (c2p.S, which takes rows of 320 pixels).
 *
 * Palette writes go to the hardware at once, as the VGA DAC took them; fades rely on that.
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
	uint8_t *base;              /* 256-byte aligned, in ST RAM */
	uint8_t *shadow;            /* the chunky frame this screen shows; NULL until it has one */
} screen;

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

static const uint8_t *last_pixels;
static uint8_t palette[768];               /* the game's, 6-bit VGA */
static bool refresh_wanted;

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

static void show(uint8_t *base)
{
	VsetScreen(-1L, base, -1, -1);
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

	old_logical = Logbase();
	old_physical = Physbase();
	if (atari_machine == MACHINE_TT ? !setup_tt() : !setup_falcon())
		plat_fatal(GAME_NAME " needs a colour or VGA monitor.");

	bytes = (size_t) screen_lines * LINE_BYTES;
	screen_memory = (void *) Mxalloc(2 * bytes + 256, 0);    /* ST RAM */
	if (screen_memory == NULL || (long) screen_memory < 0)
		plat_fatal("Not enough ST RAM for the screens.");
	for (int i = 0; i < 2; i++) {
		screens[i].base = (uint8_t *) (((uintptr_t) screen_memory + 255) & ~(uintptr_t) 255) + i * bytes;
		memset(screens[i].base, 0, bytes);
		screens[i].shadow = NULL;
	}
	console_memory = (void *) Mxalloc(bytes, 3);
	if (console_memory == NULL || (long) console_memory < 0)
		plat_fatal("Not enough memory for the console screen.");

	if (atari_machine == MACHINE_TT) {
		EsetShift(TT_LOW);
		VsetScreen(console_memory, screens[0].base, -1, -1);
	} else {
		VsetScreen(console_memory, screens[0].base, 3, falcon_mode);
	}
	hidden = 1;
	shown_at_vbl = vbl_count();
	active = true;
}

void video_shutdown(void)
{
	if (!active)
		return;
	active = false;
	if (atari_machine == MACHINE_TT) {
		EsetShift(old_mode);
		VsetScreen(old_logical, old_physical, -1, -1);
		EsetPalette(0, 256, old_tt_palette);
	} else {
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
	text_shown = false;
	if (active)
		plat_video_set_palette(0, 256, black);
}

static void write_palette(int16_t first, int16_t count, const uint8_t *rgb)
{
	if (atari_machine == MACHINE_TT) {
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

/* c2p.S: converts the groups of 16 pixels that differ from the shadow, or all of them. */
void c2p_frame(const uint8_t *in, uint8_t *shadow, uint8_t *out, int32_t rows, int32_t outStride,
	int32_t secondLine, int32_t full);

/* Converts the pixels that differ from what screen s shows. */
static void convert(screen *s, const uint8_t *pixels)
{
	bool full = s->shadow == NULL;

	if (full && (s->shadow = malloc(WIDTH * HEIGHT)) == NULL)
		plat_fatal("Out of memory for the screen copy.");
	c2p_frame(pixels, s->shadow, s->base + top_line * LINE_BYTES, HEIGHT, line_repeat * LINE_BYTES,
		line_repeat == 2 ? LINE_BYTES : 0, full);
}

static void show_frame(const uint8_t *pixels)
{
	screen *s;

	if (!active)
		return;
	/* The screen handed over last time is showing once a blank has passed; until then the
	 * hidden one may still be on display. */
	while (vbl_count() == shown_at_vbl)
		;
	s = &screens[hidden];
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
			snprintf(text, sizeof text, "present %lu: top colour %d x%u, tick %lu\r\n",
				(unsigned long) presents, top, counts[top], (unsigned long) plat_ticks());
			plat_log(text);
		}
	}
#endif
	convert(s, pixels);
	show(s->base);
	shown_at_vbl = vbl_count();
	hidden ^= 1;
}

void plat_video_present(const uint8_t *pixels)
{
	last_pixels = pixels;
	refresh_wanted = false;
#ifdef U7_DEBUG_LOG
	if (lockstep)
		lockstep_frame(pixels, 'P');
#endif
	if (!text_shown)
		show_frame(pixels);
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

	const uint8_t *pixels = text_shown ? text_frame : last_pixels;
	const uint8_t *colours = text_shown ? text_palette : palette;

	if (pixels == NULL || (f = fopen(name, "wb")) == NULL)
		return;
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
}
#endif

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

void video_pump(void)
{
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
