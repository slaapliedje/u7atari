/* Scripted input for testing under Hatari, in builds with U7_DEBUG_LOG: --script <file>.
 *
 * Each line of the file is a time and an action, the time in milliseconds of the machine's own
 * clock since start-up (with --lockstep, the virtual clock), so steps land at the same point in
 * the game however fast the emulator runs:
 *   3000 key i          a key: a character, or esc, enter, space, bs, tab, up, down, left,
 *                       right, home, end, pgup, pgdn, f1 to f10, alt-<letter>
 *   3500 move 320 100   the mouse, in the game's 640x200 mouse space
 *   3600 down           left button; up, and rdown, rup for the right one
 *   3700 click 320 100  move there, then left down and, 100 ms on, up
 *   4000 shot inv.ppm   the frame on screen, as a 320x200 PPM in the game folder
 *   4100 log text       a line on the console
 *   4200 speech 3       starts speech track 3 as the game's usecode does (tests/speech_hook.cpp)
 *   9000 quit           restores the machine and exits
 * Lines starting with # are comments. Steps go in through the same paths as real keys and IKBD
 * mouse packets, after them.
 */
#ifdef U7_DEBUG_LOG

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "backend.h"

#define MAX_STEPS 512

void debug_play_speech(int16_t track);      /* tests/speech_hook.cpp */

enum action { KEY, MOVE, BUTTONS, SHOT, LOG, SPEECH, QUIT };

typedef struct {
	uint32_t at;
	enum action action;
	uint16_t word;              /* KEY: the PC BIOS word */
	int16_t x, y;               /* MOVE */
	uint16_t buttons;           /* BUTTONS: the new state */
	char text[64];              /* SHOT, LOG */
} step;

static step steps[MAX_STEPS];
static int step_count, next_step;
static uint16_t buttons;

/* The BIOS word for a key name; 0 for none. */
static uint16_t key_word(const char *name)
{
	static const struct { const char *name; uint16_t word; } named[] = {
		{ "esc", 0x011b }, { "enter", 0x1c0d }, { "space", 0x3920 }, { "bs", 0x0e08 },
		{ "tab", 0x0f09 }, { "up", 0x4800 }, { "down", 0x5000 }, { "left", 0x4b00 },
		{ "right", 0x4d00 }, { "home", 0x4700 }, { "end", 0x4f00 }, { "pgup", 0x4900 },
		{ "pgdn", 0x5100 },
	};
	/* PC scan codes of the letters, a to z */
	static const uint8_t letters[26] = {
		0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
		0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
	};

	for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
		if (strcmp(name, named[i].name) == 0)
			return named[i].word;
	if (name[0] == 'f' && name[1] >= '1' && name[1] <= '9') {
		int n = atoi(name + 1);

		return n >= 1 && n <= 10 ? (uint16_t) ((0x3a + n) << 8) : 0;
	}
	if (strncmp(name, "alt-", 4) == 0 && name[4] >= 'a' && name[4] <= 'z' && name[5] == '\0')
		return (uint16_t) (letters[name[4] - 'a'] << 8);
	if (name[0] != '\0' && name[1] == '\0') {
		if (name[0] >= 'a' && name[0] <= 'z')
			return (uint16_t) (letters[name[0] - 'a'] << 8 | (uint8_t) name[0]);
		if (name[0] >= 'A' && name[0] <= 'Z')
			return (uint16_t) (letters[name[0] - 'A'] << 8 | (uint8_t) name[0]);
		return (uint8_t) name[0];
	}
	return 0;
}

static step *add(uint32_t at, enum action action)
{
	step *s = &steps[step_count++];

	memset(s, 0, sizeof *s);
	s->at = at;
	s->action = action;
	return s;
}

/* Splits a line into up to four words, in place; without sscanf, whose MiNTLib version saves an
 * FPU register on entry and so stops a Falcon that has no FPU. */
static int split(char *line, char *word[4])
{
	int n = 0;

	for (char *w = strtok(line, " \t"); w != NULL && n < 4; w = strtok(NULL, " \t"))
		word[n++] = w;
	return n;
}

static void parse_line(char *line)
{
	char copy[128];
	char *word[4], *end;
	const char *action;
	unsigned long at;
	int n;
	step *s;

	snprintf(copy, sizeof copy, "%s", line);
	n = split(copy, word);
	if (line[0] == '#' || n < 2 || step_count > MAX_STEPS - 4)
		return;
	at = strtoul(word[0], &end, 10);
	if (*end != '\0')
		return;
	action = word[1];
	if (strcmp(action, "key") == 0 && n >= 3) {
		uint16_t key = key_word(word[2]);

		if (key != 0)
			add(at, KEY)->word = key;
	} else if ((strcmp(action, "move") == 0 || strcmp(action, "click") == 0) && n >= 4) {
		s = add(at, MOVE);
		s->x = (int16_t) strtol(word[2], NULL, 10);
		s->y = (int16_t) strtol(word[3], NULL, 10);
		if (action[0] == 'c') {
			add(at + 50, BUTTONS)->buttons = 1;
			add(at + 150, BUTTONS)->buttons = 0;
		}
	} else if (strcmp(action, "down") == 0) {
		add(at, BUTTONS)->buttons = buttons |= 1;
	} else if (strcmp(action, "up") == 0) {
		add(at, BUTTONS)->buttons = buttons &= ~1;
	} else if (strcmp(action, "rdown") == 0) {
		add(at, BUTTONS)->buttons = buttons |= 2;
	} else if (strcmp(action, "rup") == 0) {
		add(at, BUTTONS)->buttons = buttons &= ~2;
	} else if (strcmp(action, "shot") == 0 && n >= 3) {
		snprintf(add(at, SHOT)->text, sizeof steps[0].text, "%s", word[2]);
	} else if (strcmp(action, "log") == 0) {
		const char *text = strstr(line, "log") + 3;

		while (*text == ' ')
			text++;
		snprintf(add(at, LOG)->text, sizeof steps[0].text, "%s", text);
	} else if (strcmp(action, "speech") == 0 && n >= 3) {
		add(at, SPEECH)->x = (int16_t) strtol(word[2], NULL, 10);
	} else if (strcmp(action, "quit") == 0) {
		add(at, QUIT);
	}
}

void script_load(const char *path)
{
	char line[128];
	FILE *f = fopen(path, "r");

	if (f == NULL) {
		plat_log("script: cannot open it\r\n");
		return;
	}
	while (fgets(line, sizeof line, f) != NULL) {
		line[strcspn(line, "\r\n")] = '\0';
		parse_line(line);
	}
	fclose(f);
	buttons = 0;
}

/* Runs the steps that are due; from plat_pump, after the real input. */
void script_pump(void)
{
	/* in lockstep, without moving the clock */
	uint32_t now = lockstep ? lockstep_ms() : plat_milliseconds();

	while (next_step < step_count && steps[next_step].at <= now) {
		step *s = &steps[next_step++];
		char text[96];

		switch (s->action) {
		case KEY:
			input_inject_key(s->word);
			break;
		case MOVE:
			input_inject_mouse(s->x, s->y, input_mouse_buttons());
			break;
		case BUTTONS:
			input_inject_mouse(-1, -1, s->buttons);
			break;
		case SHOT:
			video_shot(s->text);
			snprintf(text, sizeof text, "script: shot %s\r\n", s->text);
			plat_log(text);
			break;
		case LOG:
			snprintf(text, sizeof text, "script: %s\r\n", s->text);
			plat_log(text);
			break;
		case SPEECH:
			snprintf(text, sizeof text, "script: speech %d\r\n", s->x);
			plat_log(text);
			debug_play_speech(s->x);
			break;
		case QUIT:
			plat_log("script: quit\r\n");
			if (lockstep)
				lockstep_end();
			backend_quit(0);
			break;
		}
	}
}

#endif
