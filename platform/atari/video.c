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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mint/osbind.h>
#include <mint/falcon.h>
#include <mint/sysvars.h>

#include "plat.h"
#include "backend.h"
#include "vgafont8.h"

#define WIDTH PLAT_SCREEN_WIDTH
#define HEIGHT PLAT_SCREEN_HEIGHT
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
#ifdef U7_DEBUG_LOG
static uint8_t shot_palette[768];          /* 6-bit VGA, for video_shot */
#endif
static bool refresh_wanted;

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
		plat_fatal("Ultima VII needs a colour or VGA monitor.");

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
	if (active)
		plat_video_set_palette(0, 256, black);
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
#ifdef U7_DEBUG_LOG
	memcpy(shot_palette + 3 * first, rgb, (size_t) count * 3);
#endif

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

void plat_video_present(const uint8_t *pixels)
{
	screen *s;

	last_pixels = pixels;
	refresh_wanted = false;
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

		/* Only on the presents it reports: counting costs a frame's worth of time. */
		if (presents++ % 20 == 0) {
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

void plat_video_wait_retrace(void)
{
	uint32_t now = vbl_count();

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

	if (last_pixels == NULL || (f = fopen(name, "wb")) == NULL)
		return;
	fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
	for (int y = 0; y < HEIGHT; y++) {
		for (int x = 0; x < WIDTH; x++)
			for (int c = 0; c < 3; c++) {
				uint8_t v = shot_palette[3 * last_pixels[y * WIDTH + x] + c] & 0x3f;

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
	if (last_pixels != NULL)
		plat_video_present(last_pixels);
}

void video_refresh_wanted(void)
{
	refresh_wanted = true;
}

void video_pump(void)
{
	if (refresh_wanted && last_pixels != NULL && vbl_count() != shown_at_vbl)
		plat_video_present(last_pixels);
}

/* ---- Console text ----
 * What the DOS game wrote with printf in graphics mode: the BIOS drew each character from its
 * 8x8 font into screen memory, colour 7 on 0, on a 40 by 25 grid, wrapping at the right edge and
 * scrolling up at the bottom. Drawn into the last frame, as upstream's matoya backend does.
 */

#define COLUMNS 40
#define ROWS 25
#define GLYPH 8
#define TEXT_COLOUR 7
#define BACK_COLOUR 0

static int16_t column, row;

void console_reset(void)
{
	column = 0;
	row = 0;
}

void plat_console_goto(int16_t x, int16_t y)
{
	column = x < 1 ? 0 : x > COLUMNS ? COLUMNS - 1 : x - 1;
	row = y < 1 ? 0 : y > ROWS ? ROWS - 1 : y - 1;
}

static void draw_char(uint8_t *frame, uint8_t ch)
{
	uint8_t *at = frame + (row * GLYPH) * WIDTH + column * GLYPH;

	for (int y = 0; y < GLYPH; y++, at += WIDTH) {
		for (int x = 0; x < GLYPH; x++)
			at[x] = VgaFont8[ch][y] & (0x80 >> x) ? TEXT_COLOUR : BACK_COLOUR;
	}
}

static void next_row(uint8_t *frame)
{
	if (++row < ROWS)
		return;
	row = ROWS - 1;
	memmove(frame, frame + GLYPH * WIDTH, (ROWS - 1) * GLYPH * WIDTH);
	memset(frame + (ROWS - 1) * GLYPH * WIDTH, BACK_COLOUR, GLYPH * WIDTH);
}

void plat_console_write(const char *text)
{
	/* The game hands over a frame it owns; the BIOS wrote into that same screen memory. */
	uint8_t *frame = (uint8_t *) last_pixels;

	if (frame == NULL) {
		plat_log(text);
		return;
	}
	for (; *text; text++) {
		switch (*text) {
		case '\r':
			column = 0;
			break;
		case '\n':
			next_row(frame);
			break;
		case '\b':
			if (column > 0)
				column--;
			break;
		case '\a':
			break;
		default:
			draw_char(frame, (uint8_t) *text);
			if (++column == COLUMNS) {
				column = 0;
				next_row(frame);
			}
		}
	}
	video_refresh();
}
