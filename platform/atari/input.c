/* Keyboard and mouse from the IKBD.
 *
 * Keys come from the BIOS keyboard buffer (Bconin(2)), so TOS keeps doing the key repeat and the
 * national layout; conterm bit 3 makes each key carry the shift state of its moment. Each key is
 * turned into the word the PC BIOS gave getch(): the PC scan code and the character. The Atari
 * main block and function keys have the PC set 1 scan codes, so only the keypad and the cursor
 * block are mapped. There is no Num Lock: the keypad moves the cursor, as a PC keypad with Num
 * Lock off, and gives digits with Shift. That makes Page Up, Page Down and End reachable.
 * Undo is Esc, which cancels and closes, and Help is F1, the PC's help key (the cheat keys'
 * list, when cheats are on); the game has no help screen of its own.
 *
 * Alt-Help's screen dump is switched off while the game runs.
 *
 * The mouse comes from the IKBD's relative packets through the mousevec vector, which also keeps
 * them from the VDI, so the desktop pointer stays still. The vector queues the packets; plat_pump
 * applies them, as the DOS mouse driver's position in 0..639 x 0..199, and calls the game's
 * handler. The game draws its own pointer into the frame.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <mint/osbind.h>
#include <mint/ostruct.h>

#include "plat.h"
#include "backend.h"

#define KEY_BUFFER_SIZE 64
#define MOUSE_QUEUE_SIZE 64
#define MOUSE_WIDTH 640
#define MOUSE_HEIGHT 200
/* Columns and lines a mouse count moves: one game pixel each way. */
#define MOUSE_X_SCALE 2
#define MOUSE_Y_SCALE 1

/* Relative packet header: the left button is bit 1, the right bit 0. */
#define IKBD_LEFT 0x02
#define IKBD_RIGHT 0x01

/* conterm (0x484): bit 0 key click, bit 3 shift state in Bconin's top byte. */
#define CONTERM ((volatile uint8_t *) 0x484)
#define CONTERM_CLICK 0x01
#define CONTERM_SHIFT 0x08

#define CON_KEYBOARD 2

/* Alt-Help's screen dump: the VBL calls the scr_dump vector once _dumpflg reaches 0. */
#define SCR_DUMP ((void (*volatile *)(void)) 0x502)
#define DUMPFLG ((volatile int16_t *) 0x4EE)

/* PC key: set 1 scan code, with EXTENDED for the E0-prefixed keys. */
#define EXTENDED 0x100

void input_mouse_vector(void);   /* ikbd.S */
void input_no_dump(void);        /* ikbd.S */
void input_mouse_packet(const uint8_t *packet);

static bool installed;
static _KBDVECS *vectors;
static void (*old_mousevec)(void *);
static uint8_t old_conterm;
static void (*old_scr_dump)(void);
static _KEYTAB *layout;

/* Filled by the IKBD interrupt, emptied by plat_pump. */
static volatile uint8_t mouse_queue[MOUSE_QUEUE_SIZE][3];
static volatile uint16_t mouse_in, mouse_out;

static uint16_t key_buffer[KEY_BUFFER_SIZE];
static uint16_t key_head, key_count;
static int16_t pending_scan = -1;
static uint16_t modifiers;

static plat_mouse_fn mouse_handler;
static int16_t mouse_x, mouse_y;
static uint16_t mouse_buttons;

static void flush_keys(void)
{
	while (Bconstat(CON_KEYBOARD))
		Bconin(CON_KEYBOARD);
}

void input_init(void)
{
	layout = Keytbl((void *) -1, (void *) -1, (void *) -1);
	old_conterm = *CONTERM;
	*CONTERM = (uint8_t) ((old_conterm | CONTERM_SHIFT) & ~CONTERM_CLICK);
	flush_keys();
	old_scr_dump = *SCR_DUMP;
	*SCR_DUMP = input_no_dump;

	vectors = Kbdvbase();
	old_mousevec = vectors->mousevec;
	mouse_in = mouse_out = 0;
	/* One long write, so the interrupt sees the old vector or the new. */
	vectors->mousevec = (void (*)(void *)) input_mouse_vector;
	installed = true;
}

void input_shutdown(void)
{
	if (!installed)
		return;
	installed = false;
	vectors->mousevec = old_mousevec;
	flush_keys();
	*CONTERM = old_conterm;
	*DUMPFLG = -1;
	*SCR_DUMP = old_scr_dump;
}

void input_reset(void)
{
	key_head = 0;
	key_count = 0;
	pending_scan = -1;
	mouse_handler = NULL;
	mouse_x = 0;
	mouse_y = 0;
	mouse_buttons = 0;
}

/* Runs in the IKBD interrupt. A full queue drops the packet; the next one carries the buttons. */
void input_mouse_packet(const uint8_t *packet)
{
	uint16_t in = mouse_in, next = (uint16_t) ((in + 1) % MOUSE_QUEUE_SIZE);

	if (next == mouse_out)
		return;
	mouse_queue[in][0] = packet[0];
	mouse_queue[in][1] = packet[1];
	mouse_queue[in][2] = packet[2];
	mouse_in = next;
}

/* ---- Keys ---- */

static uint16_t word(uint8_t scan, uint8_t ascii)
{
	return (uint16_t) (scan << 8 | ascii);
}

/* The PC key at an Atari scan code; 0 for keys the PC has no match for (the keypad
 * brackets). */
static uint16_t pc_key(uint8_t scan)
{
	static const uint8_t keypad[] = {   /* Atari 0x67-0x71: 7 8 9 4 5 6 1 2 3 0 . */
		0x47, 0x48, 0x49, 0x4B, 0x4C, 0x4D, 0x4F, 0x50, 0x51, 0x52, 0x53
	};

	/* TOS gives some shifted keys the PC BIOS's codes: back to the key. */
	if (scan >= 0x54 && scan <= 0x5D)       /* Shift F1-F10 */
		return (uint16_t) (scan - 0x19);
	if (scan >= 0x78 && scan <= 0x83)       /* Alt 1-= */
		return (uint16_t) (scan - 0x76);
	switch (scan) {
	case 0x73: return EXTENDED | 0x4B;      /* Ctrl Left */
	case 0x74: return EXTENDED | 0x4D;      /* Ctrl Right */
	case 0x77: return EXTENDED | 0x47;      /* Ctrl Clr/Home */
	case 0x47: case 0x48: case 0x4B: case 0x4D: case 0x50: case 0x52: case 0x53:
		return EXTENDED | scan;             /* the cursor block */
	case 0x60: return 0x56;                 /* the ISO key beside left Shift */
	case 0x61: return 0x01;                 /* Undo: Esc */
	case 0x62: return 0x3B;                 /* Help: F1 */
	case 0x65: return EXTENDED | 0x35;      /* keypad / */
	case 0x66: return 0x37;                 /* keypad * */
	case 0x72: return EXTENDED | 0x1C;      /* keypad Enter */
	}
	if (scan >= 0x67 && scan <= 0x71)
		return keypad[scan - 0x67];
	if (scan == 0x4A || scan == 0x4E || (scan >= 0x01 && scan <= 0x44))
		return scan;
	return 0;
}

/* Ctrl with Home, arrows, PgUp and the rest; 0 where BIOS gave nothing. */
static uint8_t ctrl_navigation(uint8_t scan)
{
	switch (scan) {
	case 0x47: return 0x77;
	case 0x48: return 0x8D;
	case 0x49: return 0x84;
	case 0x4B: return 0x73;
	case 0x4D: return 0x74;
	case 0x4F: return 0x75;
	case 0x50: return 0x91;
	case 0x51: return 0x76;
	case 0x52: return 0x92;
	case 0x53: return 0x93;
	default: return 0;
	}
}

static uint16_t navigation_word(uint8_t scan, bool ctrl, bool alt)
{
	if (alt)
		return word((uint8_t) (scan + 0x50), 0);
	if (ctrl)
		return word(ctrl_navigation(scan), 0);
	return word(scan, 0);
}

/* A key of the main block, its characters from the TOS layout at the Atari scan code. */
static uint16_t main_block_word(uint8_t scan, uint8_t atari, bool shift, bool ctrl, bool alt,
	bool caps)
{
	uint8_t c = (uint8_t) layout->unshift[atari];
	bool letter = c >= 'a' && c <= 'z';

	if (c == 0)
		return 0;
	if (alt) {
		if (letter)
			return word(scan, 0);
		/* Alt-1 .. Alt-= are 0x78 .. 0x83. */
		if (scan >= 0x02 && scan <= 0x0D)
			return word((uint8_t) (scan + 0x76), 0);
		return c == ' ' ? word(scan, ' ') : 0;
	}
	if (ctrl) {
		if (letter)
			return word(scan, (uint8_t) (c - 'a' + 1));
		switch (c) {
		case '\033': return word(scan, 27);
		case '[': return word(scan, 0x1B);
		case '\\': return word(scan, 0x1C);
		case ']': return word(scan, 0x1D);
		case '6': return word(scan, 0x1E);
		case '-': return word(scan, 0x1F);
		case '2': return word(0x03, 0);
		case '\r': return word(scan, '\n');
		case '\b': return word(scan, 0x7F);
		case ' ': return word(scan, ' ');
		default: return 0;
		}
	}
	if (c == '\t' && shift)
		return word(scan, 0);
	if (letter && caps)
		shift = !shift;
	return word(scan, (uint8_t) (shift ? layout->shift[atari] : c));
}

static uint16_t function_key_word(uint8_t n, bool shift, bool ctrl, bool alt)
{
	if (alt)
		return word((uint8_t) (0x68 + n), 0);
	if (ctrl)
		return word((uint8_t) (0x5E + n), 0);
	if (shift)
		return word((uint8_t) (0x54 + n), 0);
	return word((uint8_t) (0x3B + n), 0);
}

/* The BIOS word for a key from Bconin: Atari scan code in bits 16-23, Kbshift state in 24-31. */
static uint16_t bios_word(uint32_t key)
{
	uint8_t atari = (uint8_t) (key >> 16);
	uint8_t state = (uint8_t) (key >> 24);
	uint16_t pc = pc_key(atari);
	uint8_t scan = (uint8_t) pc;
	bool shift = (state & 3) != 0;
	bool ctrl = (state & 4) != 0;
	bool alt = (state & 8) != 0;
	bool caps = (state & 0x10) != 0;

	if (pc == 0)
		return 0;
	if (pc & EXTENDED) {
		switch (scan) {
		case 0x1C: return word(0x1C, ctrl ? '\n' : '\r');
		case 0x35: return alt ? 0 : word(0x35, '/');
		default: return navigation_word(scan, ctrl, alt);
		}
	}
	if (atari == 0x61)                      /* Undo: Esc, whatever the shift keys */
		return word(0x01, 27);
	if (scan >= 0x3B && scan <= 0x44)
		return function_key_word((uint8_t) (scan - 0x3B), shift, ctrl, alt);
	if (scan >= 0x47 && scan <= 0x53) {
		static const char keypad_chars[] = "789-456+1230.";
		char c = keypad_chars[scan - 0x47];

		if (c == '-' || c == '+')
			return alt ? 0 : word(scan, (uint8_t) c);
		/* No Num Lock: Shift gives the digits. */
		if (shift && !ctrl && !alt)
			return word(scan, (uint8_t) c);
		return scan == 0x4C ? 0 : navigation_word(scan, ctrl, alt);
	}
	if (scan == 0x37)
		return word(scan, '*');
	/* The rest is the main block, whose characters TOS keeps at the Atari scan code (the ISO
	 * key's differs from the PC's). */
	if (scan <= 0x39 || scan == 0x56)
		return main_block_word(scan, atari, shift, ctrl, alt, caps);
	return 0;
}

static void read_keys(void)
{
	while (Bconstat(CON_KEYBOARD)) {
		uint32_t key = (uint32_t) Bconin(CON_KEYBOARD);
		uint16_t bios = bios_word(key);

#ifdef U7_DEBUG_LOG
		{
			char text[48];

			snprintf(text, sizeof text, "key %08lx -> %04x\r\n", (unsigned long) key, bios);
			plat_log(text);
		}
#endif

		/* A full buffer drops the key, as the BIOS did. */
		if (bios != 0 && key_count < KEY_BUFFER_SIZE) {
			key_buffer[(key_head + key_count) % KEY_BUFFER_SIZE] = bios;
			key_count++;
		}
	}
	modifiers = (uint16_t) (Kbshift(-1) & 0x0F);
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

/* Applies the queued packets: moves first, as one event, then each button change in order. */
static void read_mouse(void)
{
	bool moved = false;

	while (mouse_out != mouse_in) {
		const volatile uint8_t *packet = mouse_queue[mouse_out];
		uint16_t buttons = (uint16_t) ((packet[0] & IKBD_LEFT ? 1 : 0) |
			(packet[0] & IKBD_RIGHT ? 2 : 0));
		int16_t x = clamp16(mouse_x + (int8_t) packet[1] * MOUSE_X_SCALE, 0, MOUSE_WIDTH - 1);
		int16_t y = clamp16(mouse_y + (int8_t) packet[2] * MOUSE_Y_SCALE, 0, MOUSE_HEIGHT - 1);
		uint16_t changed = buttons ^ mouse_buttons;

		mouse_out = (uint16_t) ((mouse_out + 1) % MOUSE_QUEUE_SIZE);
		if (x != mouse_x || y != mouse_y) {
			mouse_x = x;
			mouse_y = y;
			moved = true;
		}
		if (changed == 0)
			continue;
		if (moved) {
			deliver(PLAT_MOUSE_MOVED);
			moved = false;
		}
		mouse_buttons = buttons;
		if (changed & 1)
			deliver(buttons & 1 ? PLAT_MOUSE_LEFT_DOWN : PLAT_MOUSE_LEFT_UP);
		if (changed & 2)
			deliver(buttons & 2 ? PLAT_MOUSE_RIGHT_DOWN : PLAT_MOUSE_RIGHT_UP);
	}
	if (moved)
		deliver(PLAT_MOUSE_MOVED);
}

void input_pump(void)
{
	if (!installed)
		return;
	read_keys();
	read_mouse();
#ifdef U7_DEBUG_LOG
	script_pump();
#endif
}

#ifdef U7_DEBUG_LOG
/* For script.c: a key word as if typed, and the mouse moved (x -1: where it is) with the
 * buttons in this state, as if from IKBD packets. */
void input_inject_key(uint16_t bios)
{
	if (key_count < KEY_BUFFER_SIZE) {
		key_buffer[(key_head + key_count) % KEY_BUFFER_SIZE] = bios;
		key_count++;
	}
}

void input_inject_mouse(int16_t x, int16_t y, uint16_t buttons)
{
	uint16_t changed = buttons ^ mouse_buttons;

	if (x >= 0 && (x != mouse_x || y != mouse_y)) {
		mouse_x = clamp16(x, 0, MOUSE_WIDTH - 1);
		mouse_y = clamp16(y, 0, MOUSE_HEIGHT - 1);
		deliver(PLAT_MOUSE_MOVED);
	}
	mouse_buttons = buttons;
	if (changed & 1)
		deliver(buttons & 1 ? PLAT_MOUSE_LEFT_DOWN : PLAT_MOUSE_LEFT_UP);
	if (changed & 2)
		deliver(buttons & 2 ? PLAT_MOUSE_RIGHT_DOWN : PLAT_MOUSE_RIGHT_UP);
}

uint16_t input_mouse_buttons(void)
{
	return mouse_buttons;
}
#endif

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
