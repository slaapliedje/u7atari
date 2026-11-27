/* A conversation shown, as the original's (engine.h, e_talk_): an NPC's usecode run (usecode.c),
 * what is said shown beside the speaker's face a page at a time, the answers offered under it by
 * the avatar's face, one picked.
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
#define LINE_TEXT 96
#define MOST_ANSWERS 40
#define SLIDER_X 104            /* where the usecode's slider is put (the original's) */
#define SLIDER_Y 57

static int status;              /* E_UC_ the usecode stopped with, or E_UC_DONE: none */
static uint32_t changes;        /* counts what changes the picture */
static char said[2048];         /* what is being shown (a copy) */
static int said_place;          /* the face place it is shown by */
static int page_at, page_next;  /* this page's start in it, the next's (-1: the last) */
static char lines[2][PAGE_LINES][LINE_TEXT];   /* each place's page as shown */
static int nlines[2];
static const char *answers[MOST_ANSWERS];
static int nanswers, picked;
static struct { int16_t x, line; } spot[MOST_ANSWERS];
static int low, high, step, value;      /* a number asked for (the original's slider) */
static int typed = -1;          /* the number typed on a keypad so far, or -1 */

/* Lays out the next page of said from page_at: its lines into the place's, page_next set. */
static void make_page(void)
{
	int width = BOX_RIGHT - BOX_LEFT + 1, len = (int) strlen(said);
	int at = page_at, n = 0;
	int cut_line = -1, cut_sentence = -1, cut_punct = -1;     /* where it could be cut: text */
	int line_end[PAGE_LINES + 1], line_start[PAGE_LINES + 1];

	page_next = -1;
	/* line breaks at the page's start are let be */
	while (said[at] == '~')
		at++;
	while (n <= PAGE_LINES) {
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
			else if (n >= PAGE_LINES)
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
				if (n <= PAGE_LINES)
					cut_line = end + 1;
			} else {
				if (n <= PAGE_LINES) {
					page_next = said[end] == '*' && end + 1 < len ? end + 1 : -1;
					break;
				}
			}
		} else {
			int brk = space > at ? space : k;
			line_start[n] = at;
			line_end[n] = brk;
			n++;
			next = brk;
		}
		at = next;
		if (n > PAGE_LINES) {
			/* runs over: cut at the last line break, sentence, mark, else after the lines */
			int cut = cut_line > page_at && cut_line <= line_end[PAGE_LINES - 1] + 1 ? cut_line
				: cut_sentence > page_at && cut_sentence <= line_end[PAGE_LINES - 1] ? cut_sentence
				: cut_punct > page_at && cut_punct <= line_end[PAGE_LINES - 1] ? cut_punct
				: line_start[PAGE_LINES];
			/* the lines up to it */
			n = 0;
			while (n < PAGE_LINES && line_start[n] < cut) {
				if (line_end[n] > cut)
					line_end[n] = cut;
				n++;
			}
			while (cut < len && said[cut] == ' ')
				cut++;
			page_next = cut < len ? cut : -1;
			break;
		}
	}
	/* the lines as shown: spaces and line breaks off their ends, '^' capitalizing */
	int place = said_place;
	nlines[place] = n > PAGE_LINES ? PAGE_LINES : n;
	for (int i = 0; i < nlines[place]; i++) {
		int s = line_start[i], e = line_end[i], o = 0, up = 0;
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
			lines[place][i][o++] = c;
		}
		lines[place][i][o] = 0;
	}
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

/* What the usecode stopped with, taken up. */
static void took(int r)
{
	changes++;
	status = r;
	if (r == E_UC_SAY) {
		snprintf(said, sizeof said, "%s", e_uc_text());
		said_place = e_uc_speaking() == 1 ? 1 : 0;
		page_at = 0;
		make_page();
	} else if (r == E_UC_ASK) {
		picked = 0;
		lay_out();
	} else if (r == E_UC_NUMBER) {
		e_uc_number(&low, &high, &step, &value);
		if (value < low)
			value = low;
		typed = -1;
	}
}

int e_talk_start(int number)
{
	nlines[0] = nlines[1] = 0;
	took(e_uc_start(0x400 + number, number == 0 ? -356 : -number, 1));
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

uint32_t e_talk_changes(void)
{
	return changes;
}

void e_talk_key(int key)
{
	changes++;
	if (status == E_UC_SAY) {
		if (key != E_KEY_NEXT)
			return;
		if (page_next >= 0) {
			page_at = page_next;
			make_page();
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
	return place >= 0 && place < 2 && line >= 0 && line < nlines[place] ? lines[place][line] : NULL;
}

void e_talk_draw(struct ESurface *frame)
{
	int16_t w, h;
	int lh = e_line_height();

	if (status == E_UC_DONE)
		return;
	/* the faces, their text */
	for (int place = 0; place < 2; place++) {
		int fr, face = e_uc_face(place, &fr);
		if (face < 0)
			continue;
		e_draw_face(NULL, face, fr, 0, 0, &w, &h);
		e_draw_face(frame, face, fr, FACE_RIGHT, (int16_t) (box_top[place] + h - 10), NULL, NULL);
		for (int i = 0; i < nlines[place]; i++)
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
