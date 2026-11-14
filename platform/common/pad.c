/* An Atari Jaguar pad (Jagpad), on the Falcon's enhanced joystick ports, as the game's mouse and
 * keys. The backend reads the pad; this turns it into what a player would do with the mouse and
 * keyboard:
 *   pad          walks, as the number keys 1-9 do: each queues three steps that way, the
 *                pace of the mouse held far from the Avatar
 *   A held       the pad moves the pointer instead, slowly at first and faster the longer it
 *                is held; a tap moves it one pixel
 *   Option       swaps the two: the pad moves the pointer, and A held walks
 *   C, B         the left and right mouse buttons, held while held (B with the pointer away
 *                from the Avatar walks that way, as with the mouse)
 *   keypad       hotkeys (pad_keys below); * is Esc and # is Enter
 * The pointer goes where it was put: walking doesn't move it.
 */
#include <stdbool.h>
#include <stdio.h>

#include "plat.h"
#include "backend.h"

#define DIRECTIONS (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)
#define KEYPAD (PAD_STAR | PAD_HASH | PAD_KEYS)

/* The pointer's speed in game pixels a second: from POINTER_SLOW, gaining POINTER_GAIN each
 * second held, up to POINTER_FAST. */
#define POINTER_SLOW 60
#define POINTER_GAIN 400
#define POINTER_FAST 320
/* A number key to walk goes in this long after the last at the soonest: the game flushes the
 * keyboard with "while (kbhit()) getch()", and each kbhit pumps, so a key put in at every pump
 * would never let that end. Three steps take longer than this at any frame rate. */
#define WALK_GAP 100

/* PC BIOS words: the scan code, then the character. */
static const uint16_t number_keys[10] = {
	0x0B30, 0x0231, 0x0332, 0x0433, 0x0534, 0x0635, 0x0736, 0x0837, 0x0938, 0x0A39
};

/* The number key that walks each way, by up, down, left, right: as on a keypad, 8 up and 3
 * down and right. */
static const uint8_t walk_number[16] = {
	0, 8, 2, 0,  4, 7, 1, 4,  6, 9, 3, 6,  0, 8, 2, 0
};

/* The keypad's 0 to 9: the game's hotkeys. Keys the Black Gate doesn't use stay off there: its
 * F, W, D and O are cheat toggles. */
static const uint16_t pad_keys[10] = {
#ifdef U7_SERPENT_ISLE
	0x1474,     /* 0 t: target, use an item at the pointer */
#else
	0,
#endif
	0x1769,     /* 1 i: inventory */
	0x2C7A,     /* 2 z: stats */
	0x2E63,     /* 3 c: combat on and off */
	0x1F73,     /* 4 s: save, load and options */
	0x1E61,     /* 5 a: sound on and off */
#ifdef U7_SERPENT_ISLE
	0x326D,     /* 6 m: map */
	0x3062,     /* 7 b: spellbook */
	0x256B,     /* 8 k: keyring */
	0x2166,     /* 9 f: feed */
#else
	0, 0, 0, 0,
#endif
};
#define KEY_ESC 0x011B
#define KEY_ENTER 0x1C0D

static uint32_t held;
static bool pointer_mode;           /* Option: the pad moves the pointer */
static uint32_t pointing_since, last_time, walk_sent;
static int32_t carry_x, carry_y;    /* game pixels x 1000 not yet moved */

static uint32_t clock_ms(void)
{
#ifdef U7_DEBUG_LOG
	/* in lockstep, without moving the clock */
	if (lockstep)
		return lockstep_ms();
#endif
	return plat_milliseconds();
}

/* -1, 0 or 1 along one axis, from the pad's two directions on it. */
static int axis(uint32_t bits, uint32_t less, uint32_t more)
{
	return ((bits & more) != 0) - ((bits & less) != 0);
}

static void move_pointer(uint32_t bits, uint32_t now, bool starting)
{
	int dx = axis(bits, PAD_LEFT, PAD_RIGHT), dy = axis(bits, PAD_UP, PAD_DOWN);
	int16_t x, y;
	int32_t speed, step;

	inputq_mouse_position(&x, &y);
	if (starting) {
		/* a tap: one pixel */
		pointing_since = now;
		carry_x = carry_y = 0;
		inputq_mouse_to((int16_t) (x + dx * 2), (int16_t) (y + dy));
		return;
	}
	speed = POINTER_SLOW + (int32_t) (now - pointing_since) * POINTER_GAIN / 1000;
	if (speed > POINTER_FAST)
		speed = POINTER_FAST;
	step = speed * (int32_t) (now - last_time);
	carry_x += dx * step;
	carry_y += dy * step;
	/* the mouse's x counts half pixels */
	inputq_mouse_to((int16_t) (x + carry_x / 1000 * 2), (int16_t) (y + carry_y / 1000));
	carry_x %= 1000;
	carry_y %= 1000;
}

/* From the backend's pump, with the buttons held now (PAD_ bits; 0 without a pad). */
void pad_pump(uint32_t bits)
{
	uint32_t pressed, now, directions;
	bool pointing;

#ifdef U7_DEBUG_LOG
	bits |= script_pad();
#endif
	if (bits == 0 && held == 0)
		return;
	now = clock_ms();
	pressed = bits & ~held;
#ifdef U7_DEBUG_LOG
	if (bits != held) {
		char text[32];

		snprintf(text, sizeof text, "pad %06lx\r\n", (unsigned long) bits);
		plat_log(text);
	}
#endif
	if (pressed & PAD_OPTION)
		pointer_mode = !pointer_mode;
	if (pressed & KEYPAD) {
		for (int n = 0; n < 10; n++) {
			if (pressed & PAD_KEY(n))
				inputq_key(pad_keys[n]);
		}
		if (pressed & PAD_STAR)
			inputq_key(KEY_ESC);
		if (pressed & PAD_HASH)
			inputq_key(KEY_ENTER);
	}
	if ((bits ^ held) & (PAD_B | PAD_C))
		inputq_pad_buttons((uint16_t) ((bits & PAD_C ? 1 : 0) | (bits & PAD_B ? 2 : 0)));

	directions = bits & DIRECTIONS;
	pointing = pointer_mode != ((bits & PAD_A) != 0);
	if (directions != 0 && pointing) {
		bool was = (held & DIRECTIONS) != 0 && pointer_mode != ((held & PAD_A) != 0);

		move_pointer(directions, now, !was);
	} else if (directions != 0 && walk_number[directions] != 0 && inputq_key_count() == 0
		&& now - walk_sent >= WALK_GAP) {
		/* one at a time: the game takes the next as the last steps run out */
		inputq_key(number_keys[walk_number[directions]]);
		walk_sent = now;
	}
	held = bits;
	last_time = now;
}

void pad_reset(void)
{
	held = 0;
	pointer_mode = false;
}
