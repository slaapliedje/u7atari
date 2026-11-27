/* Containers shown (engine.h, e_inv_): a chest, a barrel, a bag, the avatar's backpack opened as
 * the original opens them, its gump with what it holds laid out in it; a container in it opened
 * over it (up to E_PANELS).
 *
 * The original's (its ContainerGump and ItemGrid): each kind of container its gump and the area
 * its contents are laid out in; the contents in their order, each placed after the one before
 * along a row by its picture's size, a row too long going on below, one too low starting again
 * from the top a little further in. What opens so (the original's Item_canBeOpened): the
 * container types, bodies.
 *
 * Not the original's (it has the mouse): the pad steps through the top gump's items, the
 * gauntlet on the one picked, its name under the gump. A uses it (a container: opened over it,
 * else its usecode run, shown as a conversation); B shuts the top gump; C moves it across: out
 * of a container in the world to the avatar (its first container with room), or from the
 * avatar's into the world's container under it. A refusal shows the original's word for it
 * (the pointer's TOO HEAVY, WON'T FIT, or the red X) till the next key.
 */
#include <stdio.h>
#include <string.h>

#include "engine.h"

#define ITEMS 64                /* laid out in a gump */
#define POINTER_HAND 0          /* POINTERS.SHP's gauntlet */
#define POINTER_NO 1            /* the red X */
#define POINTER_HEAVY 3
#define POINTER_FULL 6
#define STACK_MOST 100          /* a pile's most */

/* each kind's contents area from the gump's corner (the original's ContainerContentAreas) */
static const int16_t areas[11][4] = {
	{ 46, 28, 124, 63 }, { 49, 19, 117, 61 }, { 50, 50, 120, 86 }, { 44, 40, 118, 79 },
	{ 43, 32, 119, 58 }, { 0, 0, 0, 0 }, { 52, 18, 139, 51 }, { 39, 20, 102, 58 },
	{ 22, 20, 90, 73 }, { 31, 11, 120, 91 }, { 42, 15, 109, 48 },
};

static struct Open {
	int in;                     /* e_held_'s in: a held container, or the world's */
	int16_t kind, gump;
	int16_t x, y;               /* its corner on the view */
	int n;                      /* its items, laid out */
	int16_t item[ITEMS], ix[ITEMS], iy[ITEMS];
	int16_t cx[ITEMS], cy[ITEMS];  /* the middle of each one's picture */
} open[E_PANELS];
static int nopen, picked, refused = -1;
static uint32_t changes;

/* What opens as a container (the original's Item_canBeOpened; not the spellbook, 761, which
 * has a gump of its own). */
int e_inv_opens(uint16_t tf)
{
	switch (tf & 0x3ff) {
	case 283: case 400: case 405: case 406: case 407: case 414: case 416: case 507: case 679:
	case 762: case 778: case 799: case 800: case 801: case 802: case 803: case 804: case 819:
	case 892:
		return 1;
	}
	return 0;
}

/* Its kind and gump (the original's ContainerGump::initialize). */
static void kind_of(int type, int16_t *kind, int16_t *gump)
{
	switch (type) {
	case 799: *kind = 0; *gump = 0; break;      /* unsealed box */
	case 802: *kind = 1; *gump = 9; break;      /* bag */
	case 801: *kind = 3; *gump = 10; break;     /* backpack */
	case 803: *kind = 4; *gump = 11; break;     /* basket */
	case 804: *kind = 6; *gump = 1; break;      /* crate */
	case 800: *kind = 7; *gump = 22; break;     /* chest */
	case 819: *kind = 8; *gump = 8; break;      /* barrel */
	case 405: *kind = 9; *gump = 26; break;     /* ship's hold */
	case 283: case 406: case 407: case 416: case 679:   /* desk, nightstand, drawers */
		*kind = 10; *gump = 27; break;
	default: *kind = 2; *gump = 53; break;
	}
}

/* Its contents laid out (the original's ItemGrid::place, row by row). */
static void lay_out(struct Open *o)
{
	const int16_t *a = areas[o->kind];
	int rw = a[2] - a[0], rh = a[3] - a[1];
	int column = 0, row = 0, row_h = 0, inset = 0;

	o->n = 0;
	for (int i = e_held_find(o->in, -1, -1, -1, -1); i >= 0 && o->n < ITEMS;
		i = e_held_find(o->in, -1, -1, -1, i)) {
		int16_t box[4] = { 0, 0, 0, 0 };
		if (e_held_in(i) != o->in)
			continue;           /* (deeper: in one of its containers) */
		int w = 0, h = 0, x, y;
		if (e_item_box(e_held_tf(i), box)) {
			w = box[0] + box[1] + 1;
			h = box[2] + box[3] + 1;
		}
		if (rw <= w || rh <= h) {
			x = w;
			y = h;
		} else
			for (;;) {
				x = column + w + inset;
				y = row + h + inset;
				if (rw < x) {
					x = w;
					row += row_h;
					y = row + h;
					row_h = 0;
				}
				if (rh >= y)
					break;
				row = column = row_h = 0;
				inset += 8;
				if (inset > rh || inset > rw)
					inset = 0;
			}
		if (y > row_h)
			row_h = y;
		column = x;
		o->item[o->n] = (int16_t) i;
		o->ix[o->n] = (int16_t) x;
		o->iy[o->n] = (int16_t) y;
		o->cx[o->n] = (int16_t) (x + (w ? (box[0] - box[1]) / 2 : 0));
		o->cy[o->n] = (int16_t) (y + (h ? (box[3] - box[2]) / 2 : 0));
		o->n++;
	}
}

static void check(void);

static int push(int in, int type)
{
	if (nopen == E_PANELS)
		return 0;
	struct Open *o = &open[nopen];
	o->in = in;
	kind_of(type, &o->kind, &o->gump);
	o->x = (int16_t) (16 + nopen * 24);
	o->y = (int16_t) (8 + nopen * 16);
	nopen++;
	picked = 0;
	refused = -1;
	changes++;
	return 1;
}

int e_inv_open_thing(const struct EThing *t)
{
	if (!e_inv_opens(t->tf))
		return 0;
	int d = e_container_fill(t);
	return d >= 0 && push(E_HELD_WORLD(d), t->tf & 0x3ff);
}

int e_inv_open_held(int i)
{
	return e_held_ok(i) && e_inv_opens(e_held_tf(i)) && push(i, e_held_tf(i) & 0x3ff);
}

void e_inv_avatar_key(void)
{
	/* its backpack opened over what is open; open already, all shut */
	for (int k = 0; k < nopen; k++)
		if (open[k].in >= 0 && e_held_in(open[k].in) == E_HELD_ON(0) && (e_held_tf(open[k].in) & 0x3ff) == 801) {
			e_inv_close();
			return;
		}
	e_inv_open_avatar();
}

int e_inv_open_avatar(void)
{
	/* (no paperdoll yet: the backpack it carries) */
	for (int i = e_held_find(E_HELD_ON(0), 801, -1, -1, -1); i >= 0; i = e_held_find(E_HELD_ON(0), 801, -1, -1, i))
		if (e_held_in(i) == E_HELD_ON(0))
			return e_inv_open_held(i);
	return 0;
}

int e_inv_actionable(uint16_t tf)
{
	return e_inv_opens(tf) || e_usable(tf);
}

int e_inv_items(int *out, int most)
{
	if (nopen == 0)
		return 0;
	check();
	int n = open[nopen - 1].n < most ? open[nopen - 1].n : most;
	for (int k = 0; k < n; k++)
		out[k] = open[nopen - 1].item[k];
	return n;
}

int e_inv_picked(void)
{
	return nopen > 0 && open[nopen - 1].n > 0 ? open[nopen - 1].item[picked] : -1;
}

int e_inv_depth(void)
{
	return nopen;
}

int e_inv_refused(void)
{
	return refused;
}

int e_inv_active(void)
{
	return nopen > 0;
}

void e_inv_close(void)
{
	nopen = 0;
	changes++;
}

uint32_t e_inv_changes(void)
{
	return changes;
}

/* The gumps still there (a container used up, taken away: it and those over it shut), each
 * laid out again. */
static void check(void)
{
	for (int k = 0; k < nopen; k++) {
		int in = open[k].in;
		if (in >= 0 && (!e_held_ok(in) || e_held_root(in) == E_HELD_LOOSE)) {
			nopen = k;
			break;
		}
	}
	for (int k = 0; k < nopen; k++)
		lay_out(&open[k]);
	if (nopen > 0 && picked >= open[nopen - 1].n)
		picked = open[nopen - 1].n > 0 ? open[nopen - 1].n - 1 : 0;
}

/* The "in" outside all of a gump's containers: the world's container, an NPC. */
static int root_of(const struct Open *o)
{
	return o->in >= 0 ? e_held_root(o->in) : o->in;
}

/* Item i moved across (C): 0, or the pointer frame saying why not. */
static int move_across(int i)
{
	int r;

	if (E_HELD_IS_WORLD(root_of(&open[nopen - 1])))
		r = e_held_place(i, E_HELD_ON(0), e_uc_strength(0));   /* to the avatar */
	else {
		/* into the world's container under it */
		int k = nopen - 2;
		while (k >= 0 && !E_HELD_IS_WORLD(root_of(&open[k])))
			k--;
		if (k < 0)
			return POINTER_NO;
		r = e_held_place(i, open[k].in, 0);
	}
	if (r == 0 && e_shape_class(e_held_tf(i) & 0x3ff) == 3) {
		/* a counted one joins a pile of its kind there (as the original's stacking) */
		int in = e_held_in(i), type = e_held_tf(i) & 0x3ff;
		int frame = e_held_frame_counts(type) >= 0 ? -1 : e_held_tf(i) >> 10 & 31;
		for (int k = e_held_find(in, type, -1, frame, -1); k >= 0; k = e_held_find(in, type, -1, frame, k))
			if (k != i && e_held_in(k) == in && e_held_quantity(k) + e_held_quantity(i) <= STACK_MOST) {
				e_held_set_quantity(k, e_held_quantity(k) + e_held_quantity(i));
				e_held_remove(i);
				break;
			}
	}
	return r == 0 ? 0 : r == 1 ? POINTER_HEAVY : r == 4 ? POINTER_FULL : POINTER_NO;
}

void e_inv_key(int key)
{
	if (nopen == 0)
		return;
	changes++;
	check();
	refused = -1;
	struct Open *top = &open[nopen - 1];
	int n = top->n;
	switch (key) {
	case E_KEY_BACK:
		nopen--;
		picked = 0;
		return;
	case E_KEY_LEFT:
		if (n > 0)
			picked = picked > 0 ? picked - 1 : n - 1;
		return;
	case E_KEY_RIGHT:
		if (n > 0)
			picked = picked + 1 < n ? picked + 1 : 0;
		return;
	case E_KEY_UP:
	case E_KEY_DOWN: {
		/* the nearest item above or below (its middle a few pixels or more up or down) */
		int best = -1, best_d = 0;
		for (int k = 0; k < n; k++) {
			int dy = top->cy[k] - top->cy[picked], dx = top->cx[k] - top->cx[picked];
			if (key == E_KEY_UP ? dy > -4 : dy < 4)
				continue;
			int d = (dy < 0 ? -dy : dy) * 2 + (dx < 0 ? -dx : dx);
			if (best < 0 || d < best_d) {
				best = k;
				best_d = d;
			}
		}
		if (best >= 0)
			picked = best;
		return;
	}
	}
	if (n == 0)
		return;
	int i = top->item[picked];
	if (key == E_KEY_NEXT) {
		if (e_inv_opens(e_held_tf(i)))
			e_inv_open_held(i);
		else
			e_talk_use_held(i);
	} else if (key == E_KEY_TAKE) {
		refused = move_across(i);
		if (refused == 0)
			refused = -1;
		check();
	}
}

/* An item's name as the original gives it (its ProduceItemDisplayName): TEXT.FLX's entry for
 * its type, or for its frame among section 2's (reagents, amulets, food, sextants, desk items);
 * "article/base/singular/plural", one without '/' all base; a counted one of more than one
 * "count base plural", else "article base singular". */
static void name_of(int i, char *out, size_t room)
{
	char name[96], part[4][64];
	int type = e_held_tf(i) & 0x3ff, frame = e_held_tf(i) >> 10 & 31;
	int count = e_shape_class(type) == 3 ? e_held_quantity(i) : 0;
	static const struct { int16_t type, count, first; } framed[4] = {
		{ 842, 8, 0 }, { 955, 3, 8 }, { 377, 32, 11 }, { 675, 21, 45 },
	};

	snprintf(name, sizeof name, "%.60s", e_text(0, type));
	for (int k = 0; k < 4; k++)
		if (framed[k].type == type)
			snprintf(name, sizeof name, "%.60s",
				e_text(2, framed[k].first + (frame < framed[k].count ? frame : framed[k].count - 1)));
	if (type == 650)
		snprintf(name, sizeof name, "%.60s", e_text(2, frame == 1 ? 43 : 44));
	memset(part, 0, sizeof part);
	if (strchr(name, '/') == NULL)
		snprintf(part[1], sizeof part[1], "%.60s", name);
	else {
		const char *p = name;
		for (int k = 0; k < 4 && p != NULL; k++) {
			const char *e = strchr(p, '/');
			size_t n = e ? (size_t) (e - p) : strlen(p);
			snprintf(part[k], sizeof part[k], "%.*s", (int) n, p);
			p = e ? e + 1 : NULL;
		}
	}
	if (count > 1)
		snprintf(out, room, "%d %.30s%.15s", count, part[1], part[3]);
	else
		snprintf(out, room, "%.15s%s%.30s%.15s", part[0], part[0][0] ? " " : "", part[1], part[2]);
}

void e_inv_draw(struct ESurface *frame)
{
	int16_t gumps[E_PANELS];

	if (nopen == 0)
		return;
	check();
	for (int k = 0; k < nopen; k++)
		gumps[k] = open[k].gump;
	if (!e_panels_load(gumps, nopen))
		return;
	for (int k = 0; k < nopen; k++) {
		struct Open *o = &open[k];
		int16_t w, h;
		if (e_panel_size(k, &w, &h)) {
			/* (within the view) */
			if (o->x + w > E_VIEW_W)
				o->x = (int16_t) (E_VIEW_W - w);
			if (o->y + h > E_VIEW_H)
				o->y = (int16_t) (E_VIEW_H - h);
		}
		e_draw_panel(frame, k, o->x, o->y);
		for (int j = 0; j < o->n; j++)
			e_draw_item(frame, e_held_tf(o->item[j]), (int16_t) (o->x + areas[o->kind][0] + o->ix[j]),
				(int16_t) (o->y + areas[o->kind][1] + o->iy[j]));
	}
	struct Open *top = &open[nopen - 1];
	if (top->n > 0) {
		/* the gauntlet on the item picked, its name under the gump */
		int16_t box[4], w, h;
		int i = top->item[picked];
		int x = top->x + areas[top->kind][0] + top->ix[picked];
		int y = top->y + areas[top->kind][1] + top->iy[picked];
		if (e_item_box(e_held_tf(i), box)) {
			/* (its finger at the picture's lower right, not over a small one) */
			x += box[0];
			y += box[3];
		}
		e_draw_pointer(frame, refused >= 0 ? refused : POINTER_HAND, (int16_t) x, (int16_t) y);
		char name[80];
		name_of(i, name, sizeof name);
		if (e_panel_size(nopen - 1, &w, &h)) {
			int ny = top->y + h + e_line_height();
			if (ny > E_VIEW_H - 2)
				ny = top->y - 2;
			e_draw_text(frame, top->x, (int16_t) ny, name, (int) sizeof name);
		}
	} else if (refused >= 0)
		e_draw_pointer(frame, refused, (int16_t) (top->x + 40), (int16_t) (top->y + 40));
}
