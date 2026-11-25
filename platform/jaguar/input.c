/* The Jaguar's input: joypad 1, read at each pump and handed to pad.c, which turns it into the
 * game's mouse and keys as on a Falcon with a Jagpad. A test script (--script) adds its steps.
 *
 * The pad is a matrix: a row is selected by writing JOYSTICK, and its buttons read back low.
 * The scan follows the SDK's joytest ReadJoypads, as jagwrestle's engine/input.c, whose bit
 * layout, checked on a real Jaguar, is
 *   xxAPxxBx RLDU147* xxCxxxOx 2580369#   (bit 31 first)
 */
#include <stdbool.h>
#include <stdint.h>

#include "plat.h"
#include "backend.h"

#define JOY_READ (*(volatile uint32_t *) 0xF14000)
#define JOY_WRITE (*(volatile uint16_t *) 0xF14000)
/* the bits not from the row read: kept high, so the inversion leaves them clear */
#define ROW_MASK 0xF0FFFFFCUL
/* row selects for port 1, with port 2 parked and the audio mute bit kept set */
#define ROW(n) (0x81F0 | (n))

static uint32_t ror32(uint32_t v, int n) { return v >> n | v << (32 - n); }
static uint32_t rol32(uint32_t v, int n) { return v << n | v >> (32 - n); }

static uint32_t scan_pad(void)
{
	uint32_t held = 0xFFFFFFFFUL;

	JOY_WRITE = ROW(0xE);                    /* the pad's directions, Pause, A */
	held &= ror32(JOY_READ | ROW_MASK, 4);
	JOY_WRITE = ROW(0xD);                    /* keys 1 4 7 *, B */
	held &= ror32(JOY_READ | ROW_MASK, 8);
	JOY_WRITE = ROW(0xB);                    /* keys 2 5 8 0, C */
	held &= rol32(JOY_READ | ROW_MASK, 12);
	JOY_WRITE = ROW(0x7);                    /* keys 3 6 9 #, Option */
	held &= rol32(JOY_READ | ROW_MASK, 8);
	return ~held;
}

/* The pad as pad.c's bits. */
static uint32_t read_pad(void)
{
	static const struct { uint32_t jaguar, pad; } map[] = {
		{ 1UL << 20, PAD_UP }, { 1UL << 21, PAD_DOWN }, { 1UL << 22, PAD_LEFT },
		{ 1UL << 23, PAD_RIGHT }, { 1UL << 29, PAD_A }, { 1UL << 25, PAD_B },
		{ 1UL << 13, PAD_C }, { 1UL << 9, PAD_OPTION }, { 1UL << 28, PAD_PAUSE },
		{ 1UL << 16, PAD_STAR }, { 1UL << 0, PAD_HASH },
		{ 1UL << 4, PAD_KEY(0) }, { 1UL << 19, PAD_KEY(1) }, { 1UL << 7, PAD_KEY(2) },
		{ 1UL << 3, PAD_KEY(3) }, { 1UL << 18, PAD_KEY(4) }, { 1UL << 6, PAD_KEY(5) },
		{ 1UL << 2, PAD_KEY(6) }, { 1UL << 17, PAD_KEY(7) }, { 1UL << 5, PAD_KEY(8) },
		{ 1UL << 1, PAD_KEY(9) },
	};
	uint32_t held = scan_pad(), bits = 0;

	for (unsigned i = 0; i < sizeof map / sizeof *map; i++) {
		if (held & map[i].jaguar)
			bits |= map[i].pad;
	}
	return bits;
}

void input_init(void) {}
void input_shutdown(void) {}
void input_reset(void) { inputq_reset(); }

void input_pump(void)
{
#ifdef U7_DEBUG_LOG
	script_pump();
	if (lockstep) {
		pad_pump(0);
		return;
	}
#endif
	pad_pump(read_pad());
}

/* The game draws its own pointer here, so nothing asks this from an interrupt. */
bool input_pointer_at(int16_t *x, int16_t *y)
{
	inputq_mouse_position(x, y);
	return true;
}
