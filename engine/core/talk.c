/* A conversation shown, as the original's (engine.h, e_talk_): an NPC's usecode run (usecode.c),
 * what is said shown beside the speaker's face a page at a time, the answers offered under it by
 * the avatar's face, one picked. And what an item's usecode shows: a book or scroll read, a sign.
 *
 * The layout is Black Gate's: the first speaker's face at the top left and its text beside it,
 * a second speaker's below, the avatar's face and the answers between. Text is broken as the
 * original's FarString::show breaks it: lines as wide as the box, broken at spaces; '~' ends a
 * line, '*' a page, '^' capitalizes the letter after it; a page that runs over is cut at its last
 * line break, else its last sentence, else its last ',' ':' or ';', else after its lines. Each
 * page waits for the player; the last of what is said stays while the answers are offered.
 *
 * The answers are laid out as the original's AnswerPane: each after a bullet, along the line
 * while it fits (two of several words never side by side), and the one picked has the pointer
 * on it (the pad moves it; the original's mouse).
 *
 * A book (gump 32, two pages) or scroll (gump 55, one), font 4: what is said flows on from line
 * to line, page to page (a book's left page to its right), each say on a line of its own; '*'
 * ends a page; a full right page (a scroll's page) waits for the player, then is cleared. When
 * the usecode ends the last pages stay till the player lets them go. A sign (gumps 49-51, each
 * its font): its lines centred in it, till the player lets it go.
 *
 * Something to point at (the original's click on an item, a key's door, a bucket's well): the
 * green cross-hairs over the world (POINTERS.SHP frame 2), moved by the pad; on, what is drawn
 * under them, the frontmost (an item, an NPC, else the ground's cell), is given the usecode; let
 * go, nothing is.
 */
#include <stdio.h>
#include <string.h>

#include "engine.h"

#define BOX_LEFT 56             /* Black Gate's text boxes (the two faces') */
#define BOX_RIGHT 312
static const int16_t box_top[2] = { 10, 144 };
static const int16_t box_bottom[2] = { 70, 202 };
#define FACE_RIGHT 52           /* a face's right, from its box's top */
#define AVATAR_X 98             /* the avatar's face */
#define AVATAR_Y 121
#define ANSWER_X0 99            /* the answers' pane */
#define ANSWER_Y0 68
#define ANSWER_X1 284
#define ANSWER_Y1 142
#define PAGE_LINES 3            /* lines a page shows, as the original's break gives them */
#define BOX_LINES 24            /* most lines a box keeps (a book's page) */
#define LINE_TEXT 96
#define MOST_ANSWERS 40
#define SLIDER_X 104            /* where the usecode's slider is put (the original's) */
#define SLIDER_Y 57
#define READ_FONT 4             /* books' and scrolls' */

/* the book's and scroll's gumps and boxes (the original's conversation modes 3 and 2) */
static const struct Reading {
	int16_t gump, x, y, boxes;
	int16_t x0[2], y0, x1[2], y1;
} readings[2] = {
	{ 55, 12, 1, 1, { 64, 0 }, 36, { 206, 0 }, 146 },          /* a scroll */
	{ 32, 3, 13, 2, { 38, 178 }, 25, { 162, 302 }, 158 },      /* a book */
};
/* the signs' (gumps 49-51): where, the text's area, the font */
static const struct { int16_t x, y, x0, y0, x1, y1, font; } signs[3] = {
	{ 66, 41, 5, 3, 182, 91, 1 }, { 63, 73, 22, 19, 176, 109, 3 }, { 45, 35, 37, 10, 196, 92, 6 },
};

static int status;              /* E_UC_ the usecode stopped with, or E_UC_DONE: none */
static uint32_t changes;        /* counts what changes the picture */
static char said[2048];         /* what is being shown (a copy) */
static int said_place;          /* the face place it is shown by */
static int page_at, page_next;  /* this page's start in it, the next's (-1: the last) */
static int page_broke;          /* the page ended at a '*' (or ran over) */
static char lines[2][BOX_LINES][LINE_TEXT];    /* each place's (box's) lines as shown */
static int nlines[2];
static const char *answers[MOST_ANSWERS];
static int nanswers, picked;
static struct { int16_t x, line; } spot[MOST_ANSWERS];
static int low, high, step, value;      /* a number asked for (the original's slider) */
static int typed = -1;          /* the number typed on a keypad so far, or -1 */
static const struct Reading *reading;   /* a book or scroll open, or NULL */
static int box;                 /* the box what is said goes on in */
static int read_wait;           /* its pages full: they wait for the player */
static int read_end;            /* the usecode has ended: its last pages wait for the player */
static int sign_open = -1;      /* the sign's style (0-2) while one is shown, or -1 */
static char sign_lines[11][LINE_TEXT];
static int16_t aim_x = E_VIEW_W / 2, aim_y = E_VIEW_H / 2;   /* the cross-hairs (view pixels) */
#define AIM_FRAME 2             /* POINTERS.SHP's green cross-hairs */
#define AIM_STEP 8              /* a key moves them a cell */
static int nsign_lines;

/* Lays out a page of said from page_at, at most most lines width wide, into place's lines from
 * line first: page_next set (-1: the text ends on it); how many lines. */
static int make_page(int width, int most, int place, int first)
{
	int len = (int) strlen(said);
	int at = page_at, n = 0;
	int cut_line = -1, cut_sentence = -1, cut_punct = -1;     /* where it could be cut: text */
	int line_end[BOX_LINES + 1], line_start[BOX_LINES + 1];

	if (most > BOX_LINES - first)
		most = BOX_LINES - first;
	if (most < 1)
		most = 1;
	page_next = -1;
	page_broke = 0;
	/* line breaks at the page's start are let be */
	while (said[at] == '~')
		at++;
	while (n <= most) {
		int px = 0, k = at, space = -1, end = -1, next;
		while (at < len && said[at] == ' ' && n > 0)
			at++;
		k = at;
		for (;;) {
			char c = said[k];
			if (c == 0 || c == '~' || c == '*') {
				end = k;
				break;
			}
			if (c == ' ')
				space = k;
			else if (n >= most)
				;               /* (a line past the page: no place to cut it) */
			else if (c == ',' || c == ':' || c == ';')
				cut_punct = k + 1;
			else if (c == '.' || c == '!' || c == '?')
				cut_sentence = said[k + 1] == '"' || said[k + 1] == '\'' ? k + 2 : k + 1;
			px += c == '^' ? 0 : e_letter_width((uint8_t) c);
			if (px >= width)
				break;
			k++;
		}
		if (end >= 0) {
			line_start[n] = at;
			line_end[n] = end;
			n++;
			next = end + 1;
			if (said[end] == '~') {
				if (n <= most)
					cut_line = end + 1;
			} else if (n <= most) {
				page_broke = said[end] == '*';
				page_next = said[end] == '*' && end + 1 < len ? end + 1 : -1;
				break;
			}
		} else {
			int brk = space > at ? space : k;
			line_start[n] = at;
			line_end[n] = brk;
			n++;
			next = brk;
		}
		at = next;
		if (n > most) {
			/* runs over: cut at the last line break, sentence, mark, else after the lines */
			int cut = cut_line > page_at && cut_line <= line_end[most - 1] + 1 ? cut_line
				: cut_sentence > page_at && cut_sentence <= line_end[most - 1] ? cut_sentence
				: cut_punct > page_at && cut_punct <= line_end[most - 1] ? cut_punct
				: line_start[most];
			/* the lines up to it */
			n = 0;
			while (n < most && line_start[n] < cut) {
				if (line_end[n] > cut)
					line_end[n] = cut;
				n++;
			}
			while (cut < len && said[cut] == ' ')
				cut++;
			page_next = cut < len ? cut : -1;
			page_broke = 1;
			break;
		}
	}
	/* the lines as shown: spaces and line breaks off their ends, '^' capitalizing */
	if (n > most)
		n = most;
	for (int i = 0; i < n; i++) {
		int s = line_start[i], e = line_end[i], o = 0, up = 0;
		char *out = lines[place][first + i];
		while (e > s && (said[e - 1] == ' ' || said[e - 1] == '~'))
			e--;
		for (int k = s; k < e && o < LINE_TEXT - 1; k++) {
			char c = said[k];
			if (c == '^') {
				up = 1;
				continue;
			}
			if (c == '@')
				c = '"';
			if (up && c >= 'a' && c <= 'z')
				c = (char) (c - 'a' + 'A');
			up = 0;
			out[o++] = c;
		}
		out[o] = 0;
	}
	nlines[place] = first + n;
	return n;
}

/* The lines a reading's box takes (as the original's: its height in lines, less one). */
static int box_lines(void)
{
	int lh = e_line_height();

	return lh > 0 ? (reading->y1 - reading->y0 + 1) / lh - 1 : 1;
}

/* What is said laid on in the book or scroll from page_at, till it is all laid (0) or the pages
 * are full and wait (1). */
static int read_on(void)
{
	e_text_font(READ_FONT);
	int most = box_lines();
	while (page_at >= 0) {
		if (nlines[box] >= most) {
			/* the box full: a book's left page goes on to its right; else they wait */
			if (box + 1 < reading->boxes) {
				box++;
				continue;
			}
			e_text_font(0);
			return 1;
		}
		make_page(reading->x1[box] - reading->x0[box] + 1, most - nlines[box], box, nlines[box]);
		if (page_broke) {
			/* '*', or run over: the box is done */
			for (int i = nlines[box]; i < most; i++)
				lines[box][i][0] = 0;
			nlines[box] = most;
		}
		page_at = page_next;
	}
	e_text_font(0);
	return 0;
}

/* The answers laid out as the original's pane. */
static void lay_out(void)
{
	int x = 0, line = 0, width = ANSWER_X1 - ANSWER_X0 + 1;

	nanswers = e_uc_answers(answers, MOST_ANSWERS);
	for (int k = 0; k < nanswers; k++) {
		int w = e_letter_width(0x7f) + e_text_width(answers[k]) + 9;
		int both = k > 0 && strchr(answers[k - 1], ' ') && strchr(answers[k], ' ');
		if (k > 0 && (width < x + w || both)) {
			line++;
			x = 0;
		}
		spot[k].x = (int16_t) x;
		spot[k].line = (int16_t) line;
		x += w;
	}
	if (picked >= nanswers)
		picked = nanswers > 0 ? nanswers - 1 : 0;
}

static void close_reading(void)
{
	if (reading != NULL || sign_open >= 0)
		e_reading_close();
	reading = NULL;
	sign_open = -1;
	read_wait = read_end = 0;
}

/* What the usecode stopped with, taken up. */
static void took(int r)
{
	int function, item, event;

	changes++;
	for (;;) {
		/* a function the run asked for when it ended (its walk to the item not walked) */
		while (r == E_UC_DONE && e_uc_pending(&function, &item, &event))
			r = e_uc_start(function, item, event);
		if (r == E_UC_DONE && reading != NULL && (nlines[0] > 0 || nlines[1] > 0)) {
			/* the book's last pages stay till the player lets them go */
			read_end = 1;
			status = E_UC_SAY;
			return;
		}
		if (r == E_UC_SAY && e_uc_book() != 0) {
			/* read in the book or scroll: laid on, the usecode going on till the pages fill */
			const struct Reading *want = &readings[e_uc_book() == 2 ? 0 : 1];
			if (reading != want) {
				close_reading();
				if (!e_reading_open(want->gump, READ_FONT)) {
					e_reading_close();
					r = e_uc_resume(NULL);  /* (cannot be shown: let go) */
					continue;
				}
				reading = want;
				box = 0;
				nlines[0] = nlines[1] = 0;
			}
			snprintf(said, sizeof said, "%s", e_uc_text());
			page_at = 0;
			if (read_on()) {
				read_wait = 1;
				status = E_UC_SAY;
				return;
			}
			r = e_uc_resume(NULL);
			continue;
		}
		break;
	}
	status = r;
	if (r == E_UC_DONE) {
		close_reading();
		return;
	}
	if (r == E_UC_SIGN) {
		/* the sign's gump and font read in, its lines split */
		int style = e_uc_sign() - 49;
		const char *t = e_uc_text();
		if (style < 0 || style > 2 || !e_reading_open(e_uc_sign(), signs[style].font)) {
			e_reading_close();
			took(e_uc_resume(NULL));
			return;
		}
		sign_open = style;
		nsign_lines = 0;
		while (*t && nsign_lines < 11) {
			const char *e = strchr(t, '\r');
			size_t n = e ? (size_t) (e - t) : strlen(t);
			snprintf(sign_lines[nsign_lines++], LINE_TEXT, "%.*s", (int) n, t);
			t += n + (e != NULL);
		}
		return;
	}
	if (r == E_UC_SAY) {
		snprintf(said, sizeof said, "%s", e_uc_text());
		said_place = e_uc_speaking() == 1 ? 1 : 0;
		page_at = 0;
		make_page(BOX_RIGHT - BOX_LEFT + 1, PAGE_LINES, said_place, 0);
	} else if (r == E_UC_ASK) {
		picked = 0;
		lay_out();
	} else if (r == E_UC_TARGET) {
		aim_x = E_VIEW_W / 2;
		aim_y = E_VIEW_H / 2;
	} else if (r == E_UC_NUMBER) {
		e_uc_number(&low, &high, &step, &value);
		if (value < low)
			value = low;
		typed = -1;
	}
}

int e_talk_start(int number)
{
	close_reading();
	nlines[0] = nlines[1] = 0;
	took(e_uc_start(0x400 + number, number == 0 ? -356 : -number, 1));
	return status != E_UC_DONE;
}

#define BOOK 642                /* books: quality 100 on run the books' function */
#define BOOK_FUNCTION 0x638

int e_usable(uint16_t tf)
{
	return e_uc_has(tf & 0x3ff);
}

int e_talk_use(const struct EThing *t)
{
	int type = t->tf & 0x3ff;

	return e_talk_run(type == BOOK && e_thing_quality(t) >= 100 ? BOOK_FUNCTION : type, t, 1);
}

int e_talk_run(int function, const struct EThing *t, int event)
{
	int ref = e_thing_ref(t);

	if (ref == 0)
		return 0;
	close_reading();
	nlines[0] = nlines[1] = 0;
	took(e_uc_start(function, ref, event));
	return status != E_UC_DONE;
}

int e_talk_start_ref(int function, int ref, int event)
{
	close_reading();
	nlines[0] = nlines[1] = 0;
	took(e_uc_start(function, ref, event));
	return status != E_UC_DONE;
}

int e_talk_use_held(int i)
{
	int type = e_held_tf(i) & 0x3ff, function = type;

	if (!e_held_ok(i) || !e_uc_has(type))
		return 0;
	if (type == BOOK && e_held_quality(i) >= 100)
		function = BOOK_FUNCTION;
	close_reading();
	nlines[0] = nlines[1] = 0;
	took(e_uc_start(function, E_HELD_REF + i, 1));
	return status != E_UC_DONE;
}

int e_talk_active(void)
{
	return status != E_UC_DONE;
}

static void choose(const char *answer)
{
	took(e_uc_resume(answer));
}

int e_talk_choose(const char *answer)
{
	if (status != E_UC_ASK)
		return 0;
	choose(answer);
	return 1;
}

int e_talk_targeting(int16_t *x, int16_t *y)
{
	if (status != E_UC_TARGET)
		return 0;
	*x = aim_x;
	*y = aim_y;
	return 1;
}

void e_talk_aim(int x, int y)
{
	aim_x = (int16_t) (x < 0 ? 0 : x > E_VIEW_W - 1 ? E_VIEW_W - 1 : x);
	aim_y = (int16_t) (y < 0 ? 0 : y > E_VIEW_H - 1 ? E_VIEW_H - 1 : y);
	changes++;
}

/* What is under the cross-hairs given the usecode. */
static void target_picked(void)
{
	struct EThing t;
	int mover, ref = 0;

	if (e_pick(aim_x, aim_y, &t, &mover)) {
		int number = mover >= 0 ? e_world_mover_npc(mover) : -1;
		ref = mover >= 0 ? (number == 0 ? -356 : number > 0 ? -number : 0) : e_thing_ref(&t);
	}
	took(e_uc_resume_target(1, ref, t.x, t.y, t.z));
}

uint32_t e_talk_changes(void)
{
	return changes;
}

void e_talk_key(int key)
{
	changes++;
	if (reading != NULL && (read_wait || read_end)) {
		/* the pages let go: cleared, and what is said laid on (or the book shut) */
		if (key != E_KEY_NEXT)
			return;
		nlines[0] = nlines[1] = 0;
		box = 0;
		if (read_end) {
			close_reading();
			status = E_UC_DONE;
			return;
		}
		read_wait = 0;
		if (read_on()) {
			read_wait = 1;
			return;
		}
		took(e_uc_resume(NULL));
		return;
	}
	if (status == E_UC_TARGET) {
		if (key == E_KEY_NEXT)
			target_picked();
		else if (key == E_KEY_BACK)
			took(e_uc_resume_target(0, 0, 0, 0, 0));
		else if (key >= E_KEY_LEFT && key <= E_KEY_DOWN)
			e_talk_aim(aim_x + (key == E_KEY_LEFT ? -AIM_STEP : key == E_KEY_RIGHT ? AIM_STEP : 0),
				aim_y + (key == E_KEY_UP ? -AIM_STEP : key == E_KEY_DOWN ? AIM_STEP : 0));
		return;
	}
	if (status == E_UC_SIGN) {
		if (key == E_KEY_NEXT) {
			close_reading();
			took(e_uc_resume(NULL));
		}
		return;
	}
	if (status == E_UC_SAY) {
		if (key != E_KEY_NEXT)
			return;
		if (page_next >= 0) {
			page_at = page_next;
			make_page(BOX_RIGHT - BOX_LEFT + 1, PAGE_LINES, said_place, 0);
		} else
			took(e_uc_resume(NULL));
		return;
	}
	if (status == E_UC_NUMBER) {
		/* the slider: left (down) and right (up) a step, within its range; A takes it */
		if (key == E_KEY_NEXT)
			took(e_uc_resume_number(value));
		else if (key >= E_KEY_DIGIT && key <= E_KEY_DIGIT + 9) {
			/* typed (the Jaguar's keypad, not the original's): the digits make a number, set as
			 * the slider sets one dragged (down to its step, within its range); a digit past
			 * the range starts the number again */
			int d = key - E_KEY_DIGIT, n = typed < 0 ? d : typed * 10 + d;
			if (n > high && typed > 0)
				n = d;
			typed = n;
			n = n > high ? high : n / step * step;
			value = n < low ? low : n;
			return;
		} else if (key == E_KEY_CLEAR)
			value = low;
		else if ((key == E_KEY_LEFT || key == E_KEY_DOWN) && value - step >= low)
			value -= step;
		else if ((key == E_KEY_RIGHT || key == E_KEY_UP) && value + step <= high)
			value += step;
		typed = -1;
		return;
	}
	if (status != E_UC_ASK || nanswers == 0)
		return;
	switch (key) {
	case E_KEY_NEXT:
		choose(answers[picked]);
		return;
	case E_KEY_LEFT:
		picked = picked > 0 ? picked - 1 : nanswers - 1;
		return;
	case E_KEY_RIGHT:
		picked = picked + 1 < nanswers ? picked + 1 : 0;
		return;
	case E_KEY_UP:
	case E_KEY_DOWN: {
		/* the nearest across on the line above or below */
		int want = spot[picked].line + (key == E_KEY_DOWN ? 1 : -1), best = -1, best_d = 0;
		for (int k = 0; k < nanswers; k++) {
			int d = spot[k].x - spot[picked].x;
			d = d < 0 ? -d : d;
			if (spot[k].line == want && (best < 0 || d < best_d)) {
				best = k;
				best_d = d;
			}
		}
		if (best >= 0)
			picked = best;
		return;
	}
	}
}

const char *e_talk_line(int place, int line)
{
	if (sign_open >= 0)
		return place == 0 && line >= 0 && line < nsign_lines ? sign_lines[line] : NULL;
	return place >= 0 && place < 2 && line >= 0 && line < nlines[place] ? lines[place][line] : NULL;
}

void e_talk_draw(struct ESurface *frame)
{
	int16_t w, h;
	int lh = e_line_height();

	if (status == E_UC_DONE)
		return;
	if (status == E_UC_TARGET) {
		e_draw_pointer(frame, AIM_FRAME, aim_x, aim_y);
		return;
	}
	if (sign_open >= 0) {
		/* the sign, its lines centred in its area (the original's SignGump) */
		int s = sign_open, aw = signs[s].x1 - signs[s].x0, ah = signs[s].y1 - signs[s].y0;
		e_draw_reading(frame, signs[s].x, signs[s].y);
		e_text_font(signs[s].font);
		int slh = e_line_height(), top = (ah - nsign_lines * slh) >> 1;
		for (int i = 0; i < nsign_lines; i++) {
			int x = signs[s].x + ((aw - e_text_width(sign_lines[i])) >> 1) + signs[s].x0;
			int y = signs[s].y + i * slh + slh + signs[s].y0 + top;
			e_draw_text(frame, (int16_t) x, (int16_t) y, sign_lines[i], LINE_TEXT);
		}
		e_text_font(0);
		return;
	}
	if (reading != NULL) {
		/* the book or scroll, its pages */
		e_draw_reading(frame, reading->x, reading->y);
		e_text_font(READ_FONT);
		int rlh = e_line_height();
		for (int b = 0; b < reading->boxes; b++)
			for (int i = 0; i < nlines[b]; i++)
				e_draw_text(frame, reading->x0[b], (int16_t) (reading->y0 + i * rlh), lines[b][i], LINE_TEXT);
		e_text_font(0);
		if (status != E_UC_ASK && status != E_UC_NUMBER)
			return;
	} else
		/* the faces, their text */
		for (int place = 0; place < 2; place++) {
			int fr, face = e_uc_face(place, &fr);
			if (face < 0)
				continue;
			e_draw_face(NULL, face, fr, 0, 0, &w, &h);
			e_draw_face(frame, face, fr, FACE_RIGHT, (int16_t) (box_top[place] + h - 10), NULL, NULL);
			for (int i = 0; i < nlines[place] && i < PAGE_LINES; i++)
				if (box_top[place] + i * lh < box_bottom[place])
					e_draw_text(frame, BOX_LEFT, (int16_t) (box_top[place] + i * lh), lines[place][i], LINE_TEXT);
		}
	if (status == E_UC_NUMBER) {
		/* the original's slider: its back, the thumb along a line 55 long from 36 in, the
		 * buttons, the number in font 2 */
		int16_t x = SLIDER_X, y = SLIDER_Y, sw, sh;
		char n[8];
		e_draw_gump(NULL, 14, 0, 0, 0, &sw, &sh);
		if (sw + x > 319)
			x = (int16_t) (319 - sw);
		if (sh + y > 199)
			y = (int16_t) (199 - sh);
		int left = x + 36, range = high - low;
		int thumb = range <= 0 ? left : (int) ((55L * (value - low) + (long) left * range) / range);
		e_draw_gump(frame, 14, 0, x, y, NULL, NULL);
		e_draw_gump(frame, 15, 0, (int16_t) thumb, (int16_t) (y + 6), NULL, NULL);
		e_draw_gump(frame, 13, 0, (int16_t) (x + 128), (int16_t) (y + 16), NULL, NULL);
		e_draw_gump(frame, 2, 0, (int16_t) (x + 21), (int16_t) (y + 18), NULL, NULL);
		e_draw_gump(frame, 17, 0, (int16_t) (x + 31), (int16_t) (y + 14), NULL, NULL);
		e_draw_gump(frame, 16, 0, (int16_t) (x + 103), (int16_t) (y + 14), NULL, NULL);
		snprintf(n, sizeof n, "%d", value);
		e_draw_small(frame, (int16_t) (x + 108), (int16_t) (y + 15), n);
		return;
	}
	if (status != E_UC_ASK)
		return;
	/* the avatar's face (a woman's, frame 1) and the answers, the picked one under the pointer */
	e_draw_face(frame, 0, 0, AVATAR_X, AVATAR_Y, NULL, NULL);
	for (int k = 0; k < nanswers; k++) {
		char s[34];
		s[0] = 0x7f;
		snprintf(s + 1, sizeof s - 1, "%s", answers[k]);
		int16_t x = (int16_t) (ANSWER_X0 + spot[k].x), y = (int16_t) (ANSWER_Y0 + lh * spot[k].line + 8);
		if (y > ANSWER_Y1 + 8)
			break;
		e_draw_text(frame, x, y, s, 30);
		if (k == picked)
			e_draw_pointer(frame, 0, (int16_t) (x + e_letter_width(0x7f) + 4), (int16_t) (y - 2));
	}
}
