/* The keys and the mouse as the game sees them, shared by the backends: a backend reads its
 * hardware and hands over PC BIOS key words and the mouse's position and buttons; this keeps
 * them and calls the game's mouse handler.
 *
 * Keys are the words the PC BIOS gave getch(): the PC scan code and the character. The mouse is
 * the DOS mouse driver's position in 0..639 x 0..199. The game draws its own pointer.
 */
#include <stdio.h>

#include "plat.h"
#include "backend.h"

#define KEY_BUFFER_SIZE 64
#define MOUSE_WIDTH 640
#define MOUSE_HEIGHT 200

static uint16_t key_buffer[KEY_BUFFER_SIZE];
static uint16_t key_head, key_count;
static int16_t pending_scan = -1;
static uint16_t modifiers;

static plat_mouse_fn mouse_handler;
static int16_t mouse_x, mouse_y;
static uint16_t mouse_buttons;

void inputq_reset(void)
{
	key_head = 0;
	key_count = 0;
	pending_scan = -1;
	mouse_handler = NULL;
	mouse_x = 0;
	mouse_y = 0;
	mouse_buttons = 0;
}

/* ---- Keys ---- */

void inputq_key(uint16_t bios)
{
	/* A full buffer drops the key, as the BIOS did. */
	if (bios != 0 && key_count < KEY_BUFFER_SIZE) {
		key_buffer[(key_head + key_count) % KEY_BUFFER_SIZE] = bios;
		key_count++;
	}
}

void inputq_modifiers(uint16_t state)
{
	modifiers = state;
}

int16_t plat_key_available(void)
{
	if (pending_scan < 0 && key_count == 0)
		plat_pump();
	return pending_scan >= 0 || key_count > 0;
}

int16_t plat_key_get(void)
{
	uint16_t next;

	while (!plat_key_available())
		plat_yield();
	if (pending_scan >= 0) {
		next = (uint16_t) pending_scan;
		pending_scan = -1;
		return (int16_t) next;
	}
	next = key_buffer[key_head];
	key_head = (uint16_t) ((key_head + 1) % KEY_BUFFER_SIZE);
	key_count--;
	if ((next & 0xFF) == 0) {
		pending_scan = (int16_t) (next >> 8);
		return 0;
	}
	return (int16_t) (next & 0xFF);
}

uint16_t plat_key_modifiers(void)
{
	return modifiers;
}

/* ---- Mouse ---- */

static int16_t clamp16(int32_t v, int32_t low, int32_t high)
{
	return (int16_t) (v < low ? low : v > high ? high : v);
}

static void deliver(uint16_t events)
{
#ifdef U7_DEBUG_LOG
	char text[48];

	snprintf(text, sizeof text, "mouse %02x %u %d,%d\r\n", events, mouse_buttons, mouse_x, mouse_y);
	plat_log(text);
#endif
	if (mouse_handler != NULL)
		mouse_handler(events, mouse_buttons, mouse_x, mouse_y);
	video_refresh_wanted();
}

void inputq_mouse_position(int16_t *x, int16_t *y)
{
	*x = mouse_x;
	*y = mouse_y;
}

uint16_t inputq_mouse_buttons(void)
{
	return mouse_buttons;
}

/* Moves the mouse there (clamped), telling the game when it moved. */
void inputq_mouse_to(int16_t x, int16_t y)
{
	x = clamp16(x, 0, MOUSE_WIDTH - 1);
	y = clamp16(y, 0, MOUSE_HEIGHT - 1);
	if (x != mouse_x || y != mouse_y) {
		mouse_x = x;
		mouse_y = y;
		deliver(PLAT_MOUSE_MOVED);
	}
}

/* Sets the buttons (1 left, 2 right), telling the game of each change. */
void inputq_mouse_set_buttons(uint16_t buttons)
{
	uint16_t changed = buttons ^ mouse_buttons;

	mouse_buttons = buttons;
	if (changed & 1)
		deliver(buttons & 1 ? PLAT_MOUSE_LEFT_DOWN : PLAT_MOUSE_LEFT_UP);
	if (changed & 2)
		deliver(buttons & 2 ? PLAT_MOUSE_RIGHT_DOWN : PLAT_MOUSE_RIGHT_UP);
}

void plat_mouse_state(int16_t *x, int16_t *y, uint16_t *buttons)
{
	if (x != NULL)
		*x = mouse_x;
	if (y != NULL)
		*y = mouse_y;
	if (buttons != NULL)
		*buttons = mouse_buttons;
}

void plat_mouse_set_handler(plat_mouse_fn fn)
{
	mouse_handler = fn;
}

void plat_mouse_move_to(int16_t x, int16_t y)
{
	mouse_x = clamp16(x, 0, MOUSE_WIDTH - 1);
	mouse_y = clamp16(y, 0, MOUSE_HEIGHT - 1);
}

/* ---- Pointer: the game draws its own ---- */

int16_t plat_cursor_is_hardware(void)
{
	return 0;
}

void plat_cursor_set_shape(const uint8_t *pixels, int16_t width, int16_t height,
	int16_t hot_x, int16_t hot_y)
{
	(void) pixels; (void) width; (void) height; (void) hot_x; (void) hot_y;
}

void plat_cursor_show(int16_t visible)
{
	(void) visible;
}

#ifdef U7_DEBUG_LOG
/* For script.c: a key word as if typed, and the mouse moved (x -1: where it is) with the
 * buttons in this state. */
void input_inject_key(uint16_t bios)
{
	inputq_key(bios);
}

void input_inject_mouse(int16_t x, int16_t y, uint16_t buttons)
{
	if (x >= 0)
		inputq_mouse_to(x, y);
	inputq_mouse_set_buttons(buttons);
}

uint16_t input_mouse_buttons(void)
{
	return mouse_buttons;
}
#endif
