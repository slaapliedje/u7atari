/* Keyboard and mouse from the IKBD.
 *
 * Atari IKBD scan codes match PC set 1 for most keys, so plat_key_get can return the DOS
 * getch() sequence with a small fix-up table (Help, Undo, keypad). The mouse comes from
 * the IKBD relative packets (Kbdvbase()->mousevec), scaled to the DOS driver's 0..639 x
 * 0..199 range.
 *
 * TODO: hook ikbdsys/mousevec, keep a key queue and shift state, draw the pointer
 * in software (plat_cursor_is_hardware returns 0 so the game draws its own).
 */
#include <stddef.h>

#include "plat.h"
#include "backend.h"

static plat_mouse_fn mouse_handler;
static int16_t mouse_x, mouse_y;
static uint16_t mouse_buttons;

void input_init(void) {}
void input_shutdown(void) {}

void input_reset(void)
{
	mouse_handler = NULL;
}

void input_pump(void)
{
}

int16_t plat_key_available(void)
{
	return 0;
}

int16_t plat_key_get(void)
{
	return 0;
}

uint16_t plat_key_modifiers(void)
{
	return 0;
}

void plat_mouse_state(int16_t *x, int16_t *y, uint16_t *buttons)
{
	*x = mouse_x;
	*y = mouse_y;
	*buttons = mouse_buttons;
}

void plat_mouse_set_handler(plat_mouse_fn fn)
{
	mouse_handler = fn;
}

void plat_mouse_move_to(int16_t x, int16_t y)
{
	mouse_x = x;
	mouse_y = y;
}

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
