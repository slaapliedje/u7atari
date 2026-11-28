/* The front: the original's main menu and its new-character screen (the original's MAINMENU),
 * from STATIC/MAINSHP.FLX and palette 6 of STATIC/INTROPAL.DAT, as the original lays them out
 * and fades them in. Driven a 60 Hz tick at a time with the pad's keys (engine.h, e_front_).
 *
 * The original's keyboard is the Jaguar's pad here: Up and Down move along the entries, A (Enter)
 * picks one. Its name is typed with a letter wheel (no keyboard): Up and Down turn the letter at
 * the cursor, Left and Right move the cursor, B rubs out the letter before it, A ends typing. On
 * SEX:, A or Left/Right turns it (the original: Space). */

#include <string.h>

#include "engine.h"

/* MAINSHP.FLX's entries */
#define SHP_PORTRAIT_MALE 0
#define SHP_PORTRAIT_FEMALE 1
#define SHP_BACKGROUND 2
#define SHP_INTRO 4
#define SHP_NEW_GAME 5
#define SHP_CREDITS 6
#define SHP_RETURN 7
#define SHP_JOURNEY 8
#define SHP_FONT 9
#define SHP_SEX 10
#define SHP_NAME 12
#define SHP_QUOTES 17
#define SHP_ENDGAME 18
#define SHP_MALE 22
#define SHP_FEMALE 23
#define SHAPES 24

/* the waits and fades of the original's entry (ticks; a fade's 60 steps a tick each here) */
#define WAIT1 100
#define WAIT2 250
#define WAIT3 230
#define FADE 60
#define IDLE 900
#define BLINK 20
#define NAME_MOST 14

static uint8_t *room;
static uint32_t room_size;
static struct EPicture pic[SHAPES];
static uint8_t have[SHAPES];
static uint8_t palette[768];

/* the main menu's six, top down (centred on x 159), and which are shown */
static const struct { uint8_t shape; int16_t y; } entries[6] = {
	{ SHP_ENDGAME, 115 }, { SHP_INTRO, 130 }, { SHP_NEW_GAME, 145 },
	{ SHP_JOURNEY, 160 }, { SHP_CREDITS, 175 }, { SHP_QUOTES, 190 }
};
static uint8_t shown[6];
static int game_exists;

/* the new-character screen's four that the pad moves along */
enum { C_NAME, C_SEX, C_BEGIN, C_RETURN };

enum { S_ENTRY, S_MENU, S_MESSAGE, S_CREATE, S_LEAVE, S_DONE };
static int state, after;        /* after: S_MESSAGE's and S_LEAVE's next (a screen, or the result) */
static int stage, wait;         /* the entry: 0-5 (wait, fade by turns), ticks left */
static int lit[3];              /* the three bands' fade steps (0-FADE): 0-47, 48-97, 97-254 */
static int leaving;             /* steps of the whole palette's fade to black */
static int pick, idle, input;
static int cpick, typing, cursor, blink, male;
static char name[NAME_MOST + 1];
static const char *message;
static int message_from;        /* S_MENU or S_CREATE: where the message goes back to */
static int result;

int e_front_open(int saved, int endgame_seen, int quotes_seen)
{
	uint32_t used = 0;

	room = e_spare_room(&room_size);
	if (room == NULL || !e_palette_read("STATIC", "INTROPAL.DAT", 6, palette))
		return 0;
	palette[255 * 3] = palette[255 * 3 + 1] = palette[255 * 3 + 2] = 0;        /* E_BLACK */
	for (int k = 0; k < SHAPES; k++) {
		have[k] = 0;
		if (k == 3 || k == 11 || (k >= 13 && k <= 16) || k == 19 || k == 20 || k == 21)
			continue;   /* (not the menu's) */
		if (!e_picture_load("STATIC", "MAINSHP.FLX", k, room + used, room_size - used, &pic[k]))
			continue;
		for (int p = 0; p < pic[k].parts; p++)
			pic[k].at[p] += used;
		used = e_picture_used(room, &pic[k]);
		have[k] = 1;
	}
	if (!have[SHP_BACKGROUND] || !have[SHP_FONT])
		return 0;
	game_exists = saved;
	shown[0] = (uint8_t) (endgame_seen != 0);
	shown[1] = shown[2] = shown[3] = shown[4] = 1;
	shown[5] = (uint8_t) (quotes_seen != 0);
	state = S_ENTRY;
	stage = 0;
	wait = WAIT1;
	lit[0] = lit[1] = lit[2] = 0;
	leaving = 0;
	pick = 1;                   /* View Introduction */
	idle = IDLE;
	input = 0;
	result = E_FRONT_RUNNING;
	male = 1;
	name[0] = 0;
	return 1;
}

static void all_lit(void)
{
	lit[0] = lit[1] = lit[2] = FADE;
	state = S_MENU;
	idle = IDLE;
	input = 0;
}

/* The pictures' fade out and in (the buttons' band only, the background staying) around a change
 * of screen: done by the leaving/entering steps of lit[2]. */
static int going_out, going_in, go_to;

static void change_to(int next)
{
	going_out = 1;
	go_to = next;
}

static void show_message(const char *text, int from)
{
	message = text;
	message_from = from;
	change_to(S_MESSAGE);
}

static int left_from;           /* the screen S_LEAVE fades out */

static void leave(int what)
{
	left_from = state == S_ENTRY ? S_MENU : state;
	state = S_LEAVE;
	after = what;
	leaving = 0;
}

static int next_shown(int from, int dir)
{
	for (int k = from + dir; k >= 0 && k < 6; k += dir)
		if (shown[k])
			return k;
	return from;
}

/* the letter wheel: what Up turns c to (Down the other way) */
static const char wheel[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-'!?";

static char turn(char c, int dir)
{
	const char *at = strchr(wheel, c);
	int n = (int) sizeof wheel - 1, i = at != NULL && c != 0 ? (int) (at - wheel) : 0;
	return wheel[((i + dir) % n + n) % n];
}

static void type_key(int key)
{
	int len = (int) strlen(name);

	switch (key) {
	case E_KEY_UP:
	case E_KEY_DOWN:
		if (cursor == len) {
			if (len == NAME_MOST)
				break;
			name[len] = key == E_KEY_UP ? 'A' : 'z';
			name[len + 1] = 0;
		} else
			name[cursor] = turn(name[cursor], key == E_KEY_UP ? 1 : -1);
		break;
	case E_KEY_LEFT:
		if (cursor > 0)
			cursor--;
		break;
	case E_KEY_RIGHT:
		if (cursor < len && cursor < NAME_MOST)
			cursor++;
		break;
	case E_KEY_BACK:
		if (cursor > 0) {
			memmove(name + cursor - 1, name + cursor, (size_t) (len - cursor + 1));
			cursor--;
		}
		break;
	case E_KEY_NEXT:
		typing = 0;
		cpick = C_SEX;
		break;
	}
	blink = 0;
}

static void menu_key(int key)
{
	if (key == E_KEY_UP)
		pick = next_shown(pick, -1);
	else if (key == E_KEY_DOWN)
		pick = next_shown(pick, 1);
	else if (key == E_KEY_NEXT) {
		switch (entries[pick].shape) {
		case SHP_INTRO: leave(E_FRONT_INTRO); break;
		case SHP_CREDITS: leave(E_FRONT_CREDITS); break;
		case SHP_QUOTES: leave(E_FRONT_QUOTES); break;
		case SHP_ENDGAME: leave(E_FRONT_ENDGAME); break;
		case SHP_JOURNEY:
			if (game_exists)
				leave(E_FRONT_JOURNEY);
			else
				show_message("You must start a new game first.", S_MENU);
			break;
		case SHP_NEW_GAME:
			male = 1;
			name[0] = 0;
			cursor = 0;
			cpick = C_NAME;
			typing = 1;
			blink = 0;
			change_to(S_CREATE);
			break;
		}
	}
}

static void create_key(int key)
{
	if (typing) {
		type_key(key);
		return;
	}
	switch (key) {
	case E_KEY_UP:
		if (cpick > C_NAME)
			cpick--;
		break;
	case E_KEY_DOWN:
		if (cpick < C_RETURN)
			cpick++;
		break;
	case E_KEY_LEFT:
	case E_KEY_RIGHT:
		if (cpick == C_SEX)
			male = !male;
		break;
	case E_KEY_NEXT:
		if (cpick == C_NAME) {
			typing = 1;
			blink = 0;
		} else if (cpick == C_SEX)
			male = !male;
		else if (cpick == C_BEGIN) {
			if (name[0] == 0)
				show_message("Avatar! You must choose a name.", S_CREATE);
			else
				leave(E_FRONT_NEW);
		} else {
			pick = 2;           /* back on Start New Game */
			change_to(S_MENU);
		}
		break;
	}
}

int e_front_tick(int key)
{
	if (key != 0)
		input = 1;
	switch (state) {
	case S_ENTRY:
		if (key != 0) {
			all_lit();
			return result;
		}
		if (stage & 1) {
			/* a fade: band stage / 2 lit a step more */
			if (++lit[stage / 2] >= FADE) {
				lit[stage / 2] = FADE;
				stage++;
				wait = stage == 2 ? WAIT2 : WAIT3;
				if (stage == 6)
					all_lit();
			}
		} else if (--wait <= 0)
			stage++;
		return result;
	case S_LEAVE:
		if (key != 0 || ++leaving >= FADE) {
			state = S_DONE;
			result = after;
		}
		return result;
	case S_DONE:
		return result;
	}
	/* a change of screen: the buttons' band out, the screen changed, in again */
	if (going_out) {
		if (--lit[2] <= 0 || key != 0) {
			lit[2] = 0;
			going_out = 0;
			going_in = 1;
			state = go_to;
			idle = IDLE;
		}
		return result;
	}
	if (going_in) {
		if (++lit[2] >= FADE || key != 0) {
			lit[2] = FADE;
			going_in = 0;
		}
		return result;
	}
	if (++blink >= 2 * BLINK)
		blink = 0;
	switch (state) {
	case S_MENU:
		if (key != 0)
			menu_key(key);
		else if (--idle <= 0) {
			/* the attract: the intro after 15 s with nothing pressed (15 more if anything was) */
			if (input) {
				idle = IDLE;
				input = 0;
			} else
				leave(E_FRONT_INTRO);
		}
		break;
	case S_CREATE:
		if (key != 0)
			create_key(key);
		break;
	case S_MESSAGE:
		if (key != 0)
			change_to(message_from);
		break;
	}
	return result;
}

/* Text in the menu's font (MAINSHP 9: frame n letter n) with its baseline at y from x on, each
 * letter's width plus gap apart: how wide. */
static int text(struct ESurface *frame, int16_t x, int16_t y, const char *s, int gap)
{
	int16_t r[4];
	int w = 0;

	for (; *s; s++) {
		if (!e_picture_reach(room, &pic[SHP_FONT], (uint8_t) *s, r))
			continue;
		if (frame != NULL)
			e_picture_draw(frame, room, &pic[SHP_FONT], (uint8_t) *s, (int16_t) (x + w + r[1]), y);
		w += r[0] + r[1] + 1 + gap;
	}
	return w;
}

/* A button or label (frame 1 when picked), its box's left at x (or centred on x), top at y. */
static void button(struct ESurface *frame, int n, int lit_up, int16_t x, int16_t y, int centred)
{
	int16_t r[4];

	if (!have[n] || !e_picture_reach(room, &pic[n], 0, r))
		return;
	int w = r[0] + r[1] + 1;
	int16_t left = centred ? (int16_t) (x - w / 2) : (int16_t) (x - r[1]);
	e_picture_draw(frame, room, &pic[n], lit_up ? 1 : 0, (int16_t) (left + r[1]), (int16_t) (y + r[2]));
}

void e_front_draw(struct ESurface *frame)
{
	eb_clear(frame, 0, 0, E_VIEW_W, E_VIEW_H);
	e_picture_draw(frame, room, &pic[SHP_BACKGROUND], 0, 0, 0);
	int s = state == S_LEAVE || state == S_DONE ? left_from : state;
	if (s == S_MESSAGE) {
		int w = text(NULL, 0, 0, message, 1);
		text(frame, (int16_t) (150 - w / 2), 150, message, 1);
	} else if (s == S_MENU || s == S_ENTRY) {
		for (int k = 0; k < 6; k++)
			if (shown[k])
				button(frame, entries[k].shape, k == pick && s == S_MENU, 159, entries[k].y, 1);
	} else if (s == S_CREATE) {
		button(frame, SHP_NAME, cpick == C_NAME, 40, 120, 0);
		button(frame, SHP_SEX, cpick == C_SEX, 40, 140, 0);
		button(frame, SHP_JOURNEY, cpick == C_BEGIN, 40, 180, 0);
		button(frame, SHP_RETURN, cpick == C_RETURN, 180, 180, 0);
		button(frame, male ? SHP_MALE : SHP_FEMALE, 0, 90, 140, 0);
		if (have[male ? SHP_PORTRAIT_MALE : SHP_PORTRAIT_FEMALE])
			e_picture_draw(frame, room, &pic[male ? SHP_PORTRAIT_MALE : SHP_PORTRAIT_FEMALE], 0, 240, 120);
		/* the name, each letter's width plus 3 apart (the original's spacing, added twice) */
		text(frame, 90, 126, name, 3);
		if (typing && blink < BLINK) {
			char before[NAME_MOST + 1];
			memcpy(before, name, (size_t) cursor);
			before[cursor] = 0;
			text(frame, (int16_t) (90 + text(NULL, 0, 0, before, 3)), 127, "_", 3);
		}
	}
}

void e_front_palette(uint8_t rgb[768])
{
	for (int c = 0; c < 256; c++) {
		int band = c < 48 ? 0 : c < 97 ? 1 : 2, l = lit[band];
		if (c == 255)
			l = FADE;
		if (state == S_LEAVE || state == S_DONE)
			l = l * (FADE - (state == S_DONE ? FADE : leaving)) / FADE;
		for (int i = 0; i < 3; i++)
			rgb[c * 3 + i] = (uint8_t) (palette[c * 3 + i] * l / FADE);
	}
}

void e_front_result(char out[15], int *is_male)
{
	memcpy(out, name, sizeof name);
	*is_male = male;
}
