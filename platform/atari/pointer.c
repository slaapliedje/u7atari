/* The game's mouse pointer, drawn by the backend from the vertical blank: the game hands over each
 * pointer frame it would have drawn (plat_cursor_set_shape) and leaves it out of its own frames,
 * and at each blank the pointer is put on the screen on display where the mouse is now. The game
 * only reads the mouse between frames, so a pointer it draws itself moves a few times a second
 * while the frame rate is low; this one moves at the display's rate. The frame it shows (the
 * arrow's direction) still changes when the game looks at the mouse.
 *
 * The pointer is drawn into the 8 bitplanes, with what was under it kept and put back before it
 * moves. It only ever sits on the screen the hardware was last told to show: the other one is
 * the one frames are converted into, and the screen it leaves is put back at the blank it
 * changes screens at, before a frame can be converted into it. So each screen's planes match
 * its shadow copy (video.c) wherever the pointer is not.
 *
 * Not with the ATW800/2 card, nor in lockstep tests, whose frames must have the game's pointer
 * in them as the PC's do.
 */
#include <stdbool.h>
#include <string.h>

#include <mint/sysvars.h>

#include "plat.h"
#include "backend.h"

#define WIDTH PLAT_SCREEN_WIDTH
#define HEIGHT PLAT_SCREEN_HEIGHT
#define GROUPS (WIDTH / 16)

/* The largest pointer frame kept; the game's are at most about 30 pixels a side. */
#define MAX_SIDE 64
#define MAX_CHUNKS (MAX_SIDE / 16)
#define MAX_GROUPS (MAX_CHUNKS + 1)
#define MAX_LINES (MAX_SIDE * 2)

#define NVBLS (*(volatile int16_t *) 0x454)
#define VBLQUEUE (*(void (*volatile **)(void)) 0x456)

void pointer_vbl_entry(void);   /* timerint.S */

/* A pointer frame: for each row and each 16 pixels across, the pixels that are the pointer's,
 * then the 8 planes' bits for them, as a 16-pixel group of the screen holds them. */
typedef struct {
	int16_t width, height, hot_x, hot_y, chunks;
	uint32_t generation;
	uint16_t bits[MAX_SIDE][MAX_CHUNKS][9];
	uint8_t pixels[MAX_SIDE][MAX_SIDE];     /* for test pictures */
} shape;

static bool running;
static void (*volatile *slot)(void);
static int16_t top_line, line_repeat;
static int32_t line_bytes;              /* from one screen line to the next */

static shape shapes[2];
static shape *volatile current;         /* NULL: none yet */
static uint32_t generations;
static volatile bool visible;
static volatile uint32_t hidden_at;     /* _frclock when hidden */
static uint8_t *volatile on_display;    /* the screen the pointer goes on; NULL: none */

/* Where the pointer is drawn, and what was under it. */
static uint8_t *drawn_on;               /* NULL: not drawn */
static int16_t drawn_x, drawn_y, drawn_line, drawn_lines, drawn_group, drawn_groups;
static uint32_t drawn_generation;
static const shape *drawn_shape;
static int16_t last_x, last_y;          /* the mouse, as the blank last saw it */
static uint16_t under[MAX_LINES][MAX_GROUPS * 8];
#ifdef U7_DEBUG_LOG
uint32_t pointer_draws;                 /* for the debug log: the times it was drawn */
#endif

bool pointer_start(int16_t first_line, int16_t repeat, int32_t stride)
{
	void (*volatile *queue)(void) = VBLQUEUE;

	running = false;
	current = NULL;
	visible = false;
	drawn_on = NULL;
	on_display = NULL;
	top_line = first_line;
	line_repeat = repeat;
	line_bytes = stride;
	if (queue == NULL)
		return false;
	for (int16_t i = 0; i < NVBLS; i++)
		if (queue[i] == NULL) {
			slot = &queue[i];
			running = true;
			*slot = pointer_vbl_entry;
			return true;
		}
	return false;
}

void pointer_stop(void)
{
	if (!running)
		return;
	running = false;
	*slot = NULL;
}

bool pointer_running(void)
{
	return running;
}

/* The screen the hardware is told to show next, from the next blank; NULL for none (the text
 * screen). */
void pointer_screen(uint8_t *base)
{
	on_display = base;
}

/* A new program: no pointer until it gives one. */
void pointer_reset(void)
{
	visible = false;
	current = NULL;
}

void pointer_shape(const uint8_t *pixels, int16_t width, int16_t height, int16_t hot_x,
	int16_t hot_y)
{
	shape *s;
	int16_t left = width, right = -1, top = height, bottom = -1;

	if (!running)
		return;
	/* only the pixels that are the pointer's */
	for (int16_t y = 0; y < height; y++)
		for (int16_t x = 0; x < width; x++)
			if (pixels[y * width + x] != 255) {
				if (x < left)
					left = x;
				if (x > right)
					right = x;
				if (y < top)
					top = y;
				bottom = y;
			}
	s = current == &shapes[0] ? &shapes[1] : &shapes[0];
	if (right < 0) {
		left = top = 0;
		right = bottom = -1;
	}
	if (right - left >= MAX_SIDE)
		right = left + MAX_SIDE - 1;
	if (bottom - top >= MAX_SIDE)
		bottom = top + MAX_SIDE - 1;
	s->width = right - left + 1;
	s->height = bottom - top + 1;
	s->hot_x = hot_x - left;
	s->hot_y = hot_y - top;
	s->chunks = (s->width + 15) / 16;
	s->generation = ++generations;
	memset(s->bits, 0, sizeof s->bits);
	for (int16_t y = 0; y < s->height; y++)
		for (int16_t x = 0; x < s->width; x++) {
			uint8_t colour = pixels[(top + y) * width + left + x];
			uint16_t *bits = s->bits[y][x / 16], bit = (uint16_t) (0x8000 >> (x % 16));

			s->pixels[y][x] = colour;
			if (colour == 255)
				continue;
			bits[0] |= bit;
			for (int p = 0; p < 8; p++)
				if (colour & (1 << p))
					bits[1 + p] |= bit;
		}
	/* one long write: the blank sees the old frame or the new */
	current = s;
}

void pointer_show(bool show)
{
	if (show)
		visible = true;
	else if (visible) {
		/* Kept for a blank: the game hides the pointer and shows it again around each frame
		 * it presents, and a blank between the two would make it flicker. */
		hidden_at = *_frclock;
		visible = false;
	}
}

static void put_back(void)
{
	uint8_t *line;

	if (drawn_on == NULL)
		return;
	line = drawn_on + drawn_line * line_bytes + drawn_group * 16;
	for (int16_t i = 0; i < drawn_lines; i++, line += line_bytes)
		memcpy(line, under[i], (size_t) drawn_groups * 16);
	drawn_on = NULL;
}

static void draw(uint8_t *base, const shape *s, int16_t x, int16_t y)
{
	int16_t first_group = (int16_t) (x >> 4), shift = (int16_t) (x & 15);    /* x may be < 0 */
	int16_t from_group = first_group < 0 ? 0 : first_group;
	int16_t to_group = (int16_t) ((x + s->width - 1) >> 4);
	int16_t from_row = y < 0 ? 0 : y, to_row = y + s->height - 1;

	if (to_group > GROUPS - 1)
		to_group = GROUPS - 1;
	if (to_row > HEIGHT - 1)
		to_row = HEIGHT - 1;
	if (from_group > to_group || from_row > to_row || s->width <= 0)
		return;
#ifdef U7_DEBUG_LOG
	pointer_draws++;
#endif
	drawn_on = base;
	drawn_x = x;
	drawn_y = y;
	drawn_generation = s->generation;
	drawn_shape = s;
	drawn_line = top_line + from_row * line_repeat;
	drawn_lines = (to_row - from_row + 1) * line_repeat;
	drawn_group = from_group;
	drawn_groups = to_group - from_group + 1;

	for (int16_t row = from_row, i = 0; row <= to_row; row++) {
		const uint16_t (*bits)[9] = s->bits[row - y];

		for (int16_t repeat = 0; repeat < line_repeat; repeat++, i++) {
			uint16_t *group = (uint16_t *) (base + (drawn_line + i) * line_bytes) + from_group * 8;

			memcpy(under[i], group, (size_t) drawn_groups * 16);
			for (int16_t g = from_group; g <= to_group; g++, group += 8) {
				int16_t k = g - first_group;
				uint16_t mask = 0, planes[8] = { 0 };

				if (k < s->chunks) {
					mask = bits[k][0] >> shift;
					for (int p = 0; p < 8; p++)
						planes[p] = bits[k][1 + p] >> shift;
				}
				if (shift != 0 && k > 0 && k - 1 < s->chunks) {
					mask |= (uint16_t) (bits[k - 1][0] << (16 - shift));
					for (int p = 0; p < 8; p++)
						planes[p] |= (uint16_t) (bits[k - 1][1 + p] << (16 - shift));
				}
				if (mask == 0)
					continue;
				for (int p = 0; p < 8; p++)
					group[p] = (uint16_t) ((group[p] & ~mask) | planes[p]);
			}
		}
	}
}

/* At each vertical blank, from the VBL queue. */
void pointer_vbl(void)
{
	const shape *s = current;
	uint8_t *base = on_display;
	int16_t x, y;

	if (!running)
		return;
	if (input_pointer_at(&x, &y)) {
		last_x = x;
		last_y = y;
	}
	if (s == NULL || base == NULL || (!visible && *_frclock - hidden_at >= 2)) {
		put_back();
		return;
	}
	x = (int16_t) ((last_x >> 1) - s->hot_x);
	y = (int16_t) (last_y - s->hot_y);
	if (drawn_on == base && x == drawn_x && y == drawn_y && s->generation == drawn_generation)
		return;
	put_back();
	draw(base, s, x, y);
}

#ifdef U7_DEBUG_LOG
/* For test pictures: the pointer drawn into a copy of the frame, where it is on screen. */
void pointer_into(uint8_t *pixels)
{
	const shape *s = drawn_shape;

	/* the frame on screen, unless a newer one has since been written over it */
	if (!running || drawn_on == NULL || s == NULL || s->generation != drawn_generation)
		return;
	for (int16_t y = 0; y < s->height; y++)
		for (int16_t x = 0; x < s->width; x++) {
			int16_t at_x = drawn_x + x, at_y = drawn_y + y;

			if (s->pixels[y][x] != 255 && at_x >= 0 && at_x < WIDTH && at_y >= 0 && at_y < HEIGHT)
				pixels[at_y * WIDTH + at_x] = s->pixels[y][x];
		}
}
#endif
