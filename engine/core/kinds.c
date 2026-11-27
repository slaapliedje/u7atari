/* What each kind of schedule does (kinds.h), as the original's schedules do it.
 *
 * As the original, each kind is a set of numbered states run one a tick while the NPC is not
 * walking or showing a script; a kind may push another over itself (walk to the place, sit,
 * walk to an item, pause, loiter, ...), which runs until it pops, and then the one under it goes
 * on at its next state, the popped one's result (-1: it could not) to look at. A walk started
 * (start_path) and a script posted (poses, turns, waits, sounds, a tick each) end with the state
 * moved on, as the original's ContinueScheduleWalk and schedule scripts do. The states, ranges,
 * waits and chances are the original's (the decompilation's main/schedule/ files).
 *
 * Not yet: usecode (but what using shutters and lights does), held weapons and missiles (no
 * weapons drawn, so archery and fencing are the poses and sounds alone, and the arrows hit),
 * combat, the thief's pickpocketing, waking to chase the avatar.
 */
#include <stdio.h>
#include <string.h>

#include "kinds.h"

/* things looked for */
#define T_BED 1011
#define T_BED_2 696
#define T_FUTON 312
#define T_FUTON_2 363
#define T_CHAIR 873
#define T_SEAT 292
#define T_PLATE 717
#define T_FOOD 377
#define T_DESK 283
#define T_DESK_2 407
#define T_BOOK 642
#define T_PATH 607
#define T_PODIUM 697
#define T_ICON 724
#define T_TOMBSTONE 715
#define T_TARGET 735
#define T_DUMMY 860

/* the original's pose frames */
#define F_STAND 0
#define F_READY 3
#define F_RAISE1 4
#define F_EXTEND1 5
#define F_THRUST1 6
#define F_RAISE2 7
#define F_EXTEND2 8
#define F_THRUST2 9
#define F_SIT 10
#define F_BEND 11
#define F_KNEEL 12
#define F_LIE 13
#define F_UP 14
#define F_OUT 15

/* script ops, each with its argument */
#define S_POSE 1                /* show frame n (a tick) */
#define S_FACE 2                /* face way n, 0-7 (a tick) */
#define S_WAIT 3                /* n ticks */
#define S_SOUND 4               /* sound n, at once */
#define S_LOOP 5                /* back to the mark (or the start) again, n times */
#define S_MARK 6                /* where a loop goes back to */
#define P(n) S_POSE, (n)
#define FACE(n) S_FACE, (n)
#define WAIT(n) S_WAIT, (n)
#define SOUND(n) S_SOUND, (n)

static const int8_t way_x[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int8_t way_y[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

static struct Level *cur(struct Doing *d)
{
	return &d->level[d->depth - 1];
}

static int chance(int n)       /* the original's RollChance: 1 in n */
{
	return rnd(n) == 0;
}

static int range(int lo, int hi)
{
	return lo + rnd(hi - lo + 1);
}

static void push(int i, int kind, int x, int y)
{
	struct Doing *d = &doing[i];

	if (d->depth == LEVELS)
		d->depth--;
	struct Level *l = &d->level[d->depth++];
	memset(l, 0, sizeof *l);
	l->kind = (uint8_t) kind;
	l->x = (int16_t) x;
	l->y = (int16_t) y;
}

static void pop(int i, int result)
{
	struct Doing *d = &doing[i];

	d->result = (int16_t) result;
	if (d->depth > 1) {
		d->depth--;
		cur(d)->state++;
	} else
		cur(d)->state = 0;      /* the outermost: from the start again */
}

/* NPC i now does kind alone, from its first state. */
static void set_kind(int i, int kind)
{
	struct Doing *d = &doing[i];

	d->depth = 0;
	push(i, kind, -1, -1);
}

static int show(int i)
{
	struct Doing *d = &doing[i];
	uint16_t tf = e_pose_frame(npc[i].tf & 0x3ff, d->dir, d->pose);

	if (npc[i].tf == tf)
		return 0;
	npc[i].tf = tf;
	return 1;
}

static void face(int i, int way)
{
	doing[i].dir = (uint8_t) (((way + 1) & 7) >> 1);
}

/* A script of (op, argument) pairs shown from the next tick; the state moves on. */
static void post(int i, const uint8_t *steps, int n)
{
	struct Doing *d = &doing[i];

	if (n > (int) sizeof d->script)
		n = (int) sizeof d->script;
	memcpy(d->script, steps, (size_t) n);
	d->script_len = (uint8_t) n;
	d->pc = 0;
	d->loops = 0;
	d->mark = 0;
	cur(d)->state++;
	d->wait = 1;
}
#define POST(i, ...) do { static const uint8_t s_[] = { __VA_ARGS__ }; post(i, s_, sizeof s_); } while (0)

static int run_script(int i)
{
	struct Doing *d = &doing[i];

	while (d->pc < d->script_len) {
		uint8_t op = d->script[d->pc], arg = d->script[d->pc + 1];
		d->pc += 2;
		switch (op) {
		case S_POSE:
			d->pose = arg;
			d->wait = TICK_BLANKS;
			return show(i);
		case S_FACE:
			face(i, arg);
			d->wait = TICK_BLANKS;
			return show(i);
		case S_WAIT:
			d->wait = (int16_t) (arg * TICK_BLANKS);
			return 0;
		case S_SOUND:
			e_sound(arg, npc[i].x, npc[i].y);
			break;
		case S_LOOP:
			if (d->loops < arg) {
				d->loops++;
				d->pc = d->mark;
			}
			break;
		case S_MARK:
			d->mark = d->pc;
			break;
		}
	}
	d->wait = 1;
	return 0;
}

/* NPC k says one of count lines from the text's barks (section 1), first on (the original's
 * barkOnItem, 15 of its ticks). */
#define BARK_BLANKS (15 * TICK_BLANKS)
static int in_view_slot(int k);

static void bark(int k, int first, int count)
{
	int s = in_view_slot(k);

	if (s)
		e_bark(s, e_text(1, first + rnd(count)), BARK_BLANKS);
}

/* Where NPC k is (the avatar's from the world step). */
static int avatar = -1;

static int in_view_slot(int k)
{
	return k == avatar ? 0 : in_slot(k);
}

int avatar_index(void)
{
	return avatar;
}

static void place_of(int k, int *x, int *y)
{
	if (k == avatar) {
		*x = me_x;
		*y = me_y;
	} else {
		*x = npc[k].x;
		*y = npc[k].y;
	}
}

/* The nearest of type within r cells of x, y (and of level z0 to z1; of two as near, the lower;
 * frame, unless -1) into t: 1; 0 for none. */
static int nearest(uint16_t type, int x, int y, int r, int z0, int z1, int frame, struct EThing *t)
{
	static struct EThing found[64];
	int n = e_things(type, (int16_t) (x - r), (int16_t) (y - r), (int16_t) (x + r), (int16_t) (y + r),
		found, 64), best = -1, best_far = 0;

	for (int k = 0; k < n; k++) {
		int far = far_of(x, y, found[k].x, found[k].y) * 16 + found[k].z;
		if (found[k].z < z0 || found[k].z > z1 || (frame >= 0 && (found[k].tf >> 10 & 31) != frame)
			|| (best >= 0 && far >= best_far))
			continue;
		best = k;
		best_far = far;
	}
	if (best < 0)
		return 0;
	*t = found[best];
	return 1;
}

#include "usespots.h"

/* A type-frame argument (as the original's TypeFrame): the frame counts when bit 15 is set. */
#define TF(type, frame) ((int16_t) ((type) | (frame) << 10 | 0x8000))
#define SPEC_TYPE(spec) ((uint16_t) ((spec) & 0x3ff))
#define SPEC_FRAME(spec) ((spec) & 0x8000 ? (spec) >> 10 & 31 : -1)

/* How many levels an item stands. */
static int height_of(uint16_t tf)
{
	struct EBody b;
	int h = 0;

	e_body(tf, &b);
	for (uint16_t l = b.levels; l; l >>= 1)
		h += l & 1;
	return h;
}

/* A spot NPC i could stand on to use t from (Operate::findUseSpot): 1, with it and the way to
 * face. With top (a type, else 0), one of it must fit on t's top ahead of the spot too. */
static int use_spot(int i, const struct EThing *t, int *x, int *y, int *z, int *way, uint16_t top)
{
	uint16_t type = t->tf & 0x3ff, tf = e_walk_frame(npc[i].tf & 0x3ff, 2, 0);
	int frame = t->tf >> 10 & 31, k, first, last;
	int8_t spot[4];

	if (type == T_CHAIR || type == T_PODIUM) {
		first = 0;
		last = 1;
	} else {
		for (k = 0; k < USE_TYPES && use_types[k] != type; k++)
			;
		if (k == USE_TYPES)
			return 0;
		first = use_starts[k];
		last = use_starts[k + 1];
	}
	for (int j = first; j < last; j++) {
		if (type == T_CHAIR) {
			spot[3] = (int8_t) ((frame & 3) * 2);
			spot[0] = way_x[spot[3]];
			spot[1] = way_y[spot[3]];
			spot[2] = 0;
		} else if (type == T_PODIUM) {
			if (frame != 0 && frame != 2)
				return 0;
			spot[0] = -1;
			spot[1] = (int8_t) -(frame == 2);
			spot[2] = 2;            /* this level alone */
			spot[3] = (int8_t) (frame ? 4 : 2);
		} else
			memcpy(spot, use_spots[j], 4);
		int zlo = t->z, zhi = t->z;
		if (spot[2] == -1)
			zlo = t->z - 2;
		else if (spot[2] == 1)
			zhi = t->z + 2;
		if (type == T_CHAIR)
			zlo = zhi = t->z;
		zlo = zlo < 0 ? 0 : zlo;
		zhi = zhi > 15 ? 15 : zhi;
		*x = t->x + spot[0];
		*y = t->y + spot[1];
		*way = spot[3];
		if (!in_near_window(*x, *y))
			continue;
		for (*z = zlo; *z <= zhi; (*z)++) {
			if (!e_can_stand(tf, (int16_t) *x, (int16_t) *y, (uint8_t) *z))
				continue;
			if (top == 0 || e_can_stand(top, (int16_t) (*x + way_x[*way]), (int16_t) (*y + way_y[*way]),
				(uint8_t) (t->z + height_of(t->tf))))
				return 1;
		}
	}
	return 0;
}

/* A walk to the free cell beside NPC (or the avatar) k nearest NPC i, in at most limit steps:
 * 1 when it is walking there (or already beside it, or it may try again), 0 when not. The state
 * stays as it is (the original's PathNextToItem). */
static int path_next_to(int i, int k, int limit)
{
	int tx, ty, best = -1, bx = 0, by = 0;
	uint16_t tf = e_walk_frame(npc[i].tf & 0x3ff, 2, 0);

	place_of(k, &tx, &ty);
	if (far_of(npc[i].x, npc[i].y, tx, ty) <= 1)
		return 1;
	for (int w = 0; w < 8; w++) {
		int x = tx + way_x[w], y = ty + way_y[w], far = far_of(npc[i].x, npc[i].y, x, y);
		if ((best < 0 || far < best) && in_near_window(x, y)
			&& e_can_stand(tf, (int16_t) x, (int16_t) y, npc[i].z)) {
			best = far;
			bx = x;
			by = y;
		}
	}
	if (best < 0)
		return 0;
	int r = start_path(i, bx, by, npc[i].z, limit);
	if (r == 0)
		cur(&doing[i])->state--;        /* walking ends it with the state moved on: undone */
	return r != 2;
}

/* Another NPC in the cell way w from NPC i. */
static int npc_ahead(int i, int w)
{
	int x = npc[i].x + way_x[w & 7], y = npc[i].y + way_y[w & 7];

	if (me_x == x && me_y == y)
		return 1;
	for (int s = 1; s < E_MOVERS; s++)
		if (slot_npc[s] >= 0 && slot_npc[s] != i && npc[slot_npc[s]].x == x && npc[slot_npc[s]].y == y)
			return 1;
	return 0;
}

/* A step way w (0-7) as the original's StepItem: 1 when made. */
static int step(int i, int w)
{
	struct Doing *d = &doing[i];
	struct ENpc *p = &npc[i];

	face(i, w);
	if (e_step(e_walk_frame(p->tf & 0x3ff, d->dir, 0), &p->x, &p->y, &p->z, way_x[w & 7],
		way_y[w & 7]) != E_STEP_MADE)
		return 0;
	d->steps++;
	d->pose = 0;
	p->tf = e_walk_frame(p->tf & 0x3ff, d->dir, d->steps);
	d->wait = STEP_BLANKS;
	return 1;
}

/* An NPC (not the avatar unless party) in front of NPC i, facing east or south, at random (the
 * original's ChooseNpcInFront): its index, or -1. */
static int npc_in_front(int i, int party)
{
	int x0, y0, x1, y1, n = 0, k;

	if (doing[i].dir == 1) {
		x0 = npc[i].x + 1, y0 = npc[i].y - 7, x1 = npc[i].x + 20, y1 = npc[i].y + 7;
	} else if (doing[i].dir == 2) {
		x0 = npc[i].x - 7, y0 = npc[i].y + 1, x1 = npc[i].x + 7, y1 = npc[i].y + 20;
	} else
		return -1;
	for (int pass = 0; pass < 2; pass++) {
		int pick = pass ? rnd(n) : -1;
		for (k = 0; k < nnpc; k++) {
			int x, y;
			if (k == i || (k == avatar && !party))
				continue;
			place_of(k, &x, &y);
			if (x < x0 || x > x1 || y < y0 || y > y1)
				continue;
			if (pass == 0)
				n++;
			else if (pick-- == 0)
				return k;
		}
		if (n == 0)
			return -1;
	}
	return -1;
}

/* ---- the kinds pushed by others ---- */

/* Walk to the place (WalkToScheduleSchedule): world.c's. */
static int goto_place(int i)
{
	struct Doing *d = &doing[i];

	if (npc[i].x == d->place_x && npc[i].y == d->place_y)
		pop(i, 0);
	else
		start_place(i);
	return 0;
}

/* Walk to the use spot of the nearest item x (a type-frame) within 25 cells (near one of y,
 * unless -1), and face it; pops its type (WalkToItemSchedule). The state's 0x80 (a put down's
 * second try): the nearest x itself, the item y carries fitting on top of it. */
static int goto_item(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t, a;
	int x, y, z, way, r, retry = 0;

	if (l->state & 0x80) {
		retry = 1;
		l->state &= 0x7f;
	}
	switch (l->state) {
	case 0:
		if (planned || searched) {
			if (retry)
				l->state |= 0x80;
			d->wait = 1;
			return 0;
		}
		searched = 1;
		if (l->y == -1 || retry ? !nearest(SPEC_TYPE(l->x), npc[i].x, npc[i].y, 25, 0, 15, SPEC_FRAME(l->x), &t)
			: !nearest(SPEC_TYPE(l->y), npc[i].x, npc[i].y, 25, 0, 15, SPEC_FRAME(l->y), &a)
				|| !nearest(SPEC_TYPE(l->x), a.x, a.y, 25, 0, 15, SPEC_FRAME(l->x), &t)) {
			pop(i, -1);
			return 0;
		}
		if (!use_spot(i, &t, &x, &y, &z, &way, retry ? SPEC_TYPE(l->y) : 0)
			|| (r = start_path(i, x, y, z, 100)) == 2) {
			pop(i, -1);
			return 0;
		}
		if (r == 1) {
			if (retry)
				l->state |= 0x80;
			d->wait = 1;
			return 0;
		}
		d->face8 = (uint8_t) way;
		l->state++;
		return 0;
	case 1:
		l->state++;
		return 0;
	case 2:
		if (d->result < 0) {
			pop(i, -1);
			return 0;
		}
		face(i, d->face8);
		l->state++;
		d->wait = TICK_BLANKS;
		return show(i);
	case 3:
		pop(i, SPEC_TYPE(l->x));
		return 0;
	}
	return 0;
}

/* Sit (DoWorkSit): a chair (else a seat) near an item of type x within 25 cells, or (x -1) one
 * within 12 cells picked at random, else the first that will do; in front of it, facing its way,
 * standing, bending, sitting. */
static int sit(int i)
{
	static struct EThing found[48];
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct ENpc *p = &npc[i];
	struct EThing a, t;
	int x, y, z, way, r = 2;

	switch (l->state) {
	case 0:
	case 4:
		if (planned || searched) {
			d->wait = 1;
			return 0;
		}
		searched = 1;
		if (l->state == 0 && l->x == -1) {
			l->state = 4;
			d->wait = 1;
			return 0;
		}
		for (int pass = 0; pass < 2; pass++) {
			uint16_t type = pass ? T_SEAT : T_CHAIR;
			int n;
			if (l->state == 0) {
				n = nearest((uint16_t) l->x, p->x, p->y, 25, 0, 15, -1, &a)
					&& nearest(type, a.x, a.y, 12, 0, 15, -1, &found[0]);
			} else
				n = e_things(type, (int16_t) (p->x - 12), (int16_t) (p->y - 12), (int16_t) (p->x + 12),
					(int16_t) (p->y + 12), found, 48);
			for (int k = n > 0 ? -1 : 0; k < n; k++) {
				/* first the one picked at random, then each in turn */
				t = found[k < 0 ? (l->state == 0 ? 0 : rnd(n)) : k];
				if (!use_spot(i, &t, &x, &y, &z, &way, 0))
					continue;
				r = start_path(i, x, y, t.z, 100);
				if (r == 1) {
					d->wait = 1;
					return 0;
				}
				if (r == 0) {
					d->face8 = (uint8_t) way;
					l->state = l->state == 0 ? 8 : 5;
					return 0;
				}
				if (l->state == 0)
					break;
			}
		}
		pop(i, -1);
		return 0;
	case 6:
		l->state = d->result < 0 ? 4 : 9;
		return 0;
	case 9:
		if (d->result < 0) {
			pop(i, -1);
			return 0;
		}
		face(i, d->face8);
		POST(i, P(F_STAND), WAIT(2), P(F_BEND), WAIT(2), P(F_SIT), WAIT(2));
		return show(i);
	case 10:
		pop(i, 0);
		return 0;
	}
	return 0;
}

/* Pause x ticks. */
static int pause_kind(int i)
{
	struct Level *l = cur(&doing[i]);

	if (l->state == 0) {
		uint8_t s[2] = { S_WAIT, (uint8_t) (l->x > 255 ? 255 : l->x < 1 ? 1 : l->x) };
		post(i, s, 2);
	} else
		pop(i, 0);
	return 0;
}

/* Read (DoWorkRead): sit at a book, open it (frames in threes: the first shown while read), read
 * a while, stand up, shut it. */
static int find_book(int i, struct EThing *t)
{
	for (int k = 0; k <= 9; k++) {
		int w = (doing[i].dir * 2 + k) & 7, x = npc[i].x + way_x[w], y = npc[i].y + way_y[w];
		if (e_things(T_BOOK, (int16_t) x, (int16_t) y, (int16_t) x, (int16_t) y, t, 1))
			return 1;
	}
	return 0;
}

static int read_kind(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;

	switch (l->state) {
	case 0:
		push(i, K_SIT, T_BOOK, -1);
		return 0;
	case 1:
		if (d->result < 0) {
			pop(i, -1);
			return 0;
		}
		if (find_book(i, &t) && (t.tf >> 10 & 31) % 3 != 0)
			e_thing_set(&t, (uint16_t) ((t.tf & ~(31u << 10)) | ((t.tf >> 10 & 31) / 3 * 3) << 10));
		{
			uint8_t s[2] = { S_WAIT, (uint8_t) range(25, 40) };
			post(i, s, 2);
		}
		return 0;
	case 2:
		POST(i, P(F_BEND), WAIT(2), P(F_STAND), WAIT(2));
		if (find_book(i, &t) && (t.tf >> 10 & 31) % 3 == 0)
			e_thing_set(&t, (uint16_t) (t.tf + (1u << 10)));
		return 0;
	case 3:
		pop(i, 0);
		return 0;
	}
	return 0;
}

/* Pay respects (DoWorkPayRespects): (with y, an item to leave, taken up first) walk to an item of
 * type x, (leave it there,) kneel a while, get up. */
static int pay_respects(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);

	switch (l->state) {
	case 0:
		if (l->y == -1)
			l->state++;
		else
			push(i, K_GRAB_ITEM, l->y, 1);
		return 0;
	case 1:
		push(i, K_GOTO_ITEM, l->x, -1);
		return 0;
	case 2:
		if (l->y == -1)
			l->state++;
		else
			push(i, K_DROP_ITEM, l->y, -1);
		return 0;
	case 3:
		if (d->result < 0) {
			pop(i, -1);
			return 0;
		}
		POST(i, P(F_STAND), WAIT(2), P(F_UP), WAIT(2), P(F_BEND), WAIT(2), P(F_KNEEL), WAIT(3),
			WAIT(40));
		return 0;
	case 4:
		POST(i, P(F_BEND), WAIT(2), P(F_UP), WAIT(2), P(F_STAND), WAIT(2));
		return 0;
	case 5:
		pop(i, 0);
		return 0;
	}
	return 0;
}

/* Tag (DoWorkTag): someone within 10 cells, walked up to; then maybe another. */
static int tag(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	int x, y, n = 0, pick;

	switch (l->state) {
	case 0:
		for (int k = 0; k < nnpc; k++) {
			place_of(k, &x, &y);
			n += far_of(npc[i].x, npc[i].y, x, y) <= 10;
		}
		if (n <= 1) {
			pop(i, -1);
			return 0;
		}
		pick = rnd(n);
		for (int k = 0; k < nnpc; k++) {
			place_of(k, &x, &y);
			if (far_of(npc[i].x, npc[i].y, x, y) <= 10 && pick-- == 0) {
				if (k == i)
					return 0;
				d->value = (int16_t) k;
				l->state++;
				return 0;
			}
		}
		return 0;
	case 1:
		if (!path_next_to(i, d->value, 30)) {
			pop(i, -1);
			return 0;
		}
		place_of(d->value, &x, &y);
		if (far_of(npc[i].x, npc[i].y, x, y) <= 1)
			l->state = 3;
		return 0;
	case 3:
		place_of(d->value, &x, &y);
		if (far_of(npc[i].x, npc[i].y, x, y) <= 2) {
			bark(i, 128, 5);            /* "Catch me if thou canst!", "Tag!  Thou art it!" */
			l->state = 4;
		} else
			l->state = 1;
		return 0;
	case 4:
		POST(i, P(F_STAND), WAIT(5));
		return show(i);
	case 5:
		if (chance(4))
			pop(i, 0);
		else
			l->state = 0;
		return 0;
	}
	return 0;
}

/* Fencing (DoWorkFencing): to the dummy, swinging at it (the sword's sounds, 2 and 4). */
static int fencing(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);

	switch (l->state) {
	case 0:
		push(i, K_READY_HAND, 602, 1);
		return 0;
	case 1:
		push(i, K_GOTO_ITEM, T_DUMMY, -1);
		return 0;
	case 2:
		if (d->result < 0)
			pop(i, -1);
		else
			l->state++;
		return 0;
	case 3:
		if (chance(2))
			POST(i, P(F_READY), WAIT(1), P(F_RAISE1), WAIT(1), SOUND(2), P(F_EXTEND1), WAIT(1),
				P(F_THRUST1), SOUND(4), WAIT(2), P(F_READY), WAIT(4));
		else
			POST(i, P(F_READY), WAIT(1), P(F_RAISE2), WAIT(1), SOUND(2), P(F_EXTEND2), WAIT(1),
				P(F_THRUST2), SOUND(4), WAIT(4), P(F_READY), WAIT(4));
		return 0;
	case 4:
		if (chance(3))
			pop(i, 0);
		else
			l->state = 3;
		return 0;
	}
	return 0;
}

/* Archery (DoWorkArchery): to the target, its arrows pulled out (its frame back, one a turn),
 * fourteen steps east, turned west, shooting till the arrows (1 to 3) are gone; each hit shows
 * on the target. (No bow drawn, no arrow flying.) */
static int archery(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;
	int frame;

	switch (l->state) {
	case 0:
		l->state++;
		return 0;
	case 1:
		push(i, K_GOTO_ITEM, T_TARGET, -1);
		return 0;
	case 2:
		if (d->result < 0)
			pop(i, -1);
		else
			l->state++;
		return 0;
	case 3:
		if (nearest(T_TARGET, npc[i].x, npc[i].y, 4, 0, 15, -1, &t) && (t.tf >> 10 & 31) != 0) {
			d->toggle = (t.tf >> 10 & 31) % 3 == 1;
			POST(i, P(F_READY), WAIT(2), P(F_RAISE1), WAIT(1), P(F_EXTEND1), WAIT(2), P(F_THRUST1),
				WAIT(2));
		} else
			l->state = 5;
		return show(i);
	case 4:
		if (nearest(T_TARGET, npc[i].x, npc[i].y, 4, 0, 15, -1, &t)) {
			frame = d->toggle ? 0 : (t.tf >> 10 & 31) - 1;
			e_thing_set(&t, (uint16_t) ((t.tf & ~(31u << 10)) | (uint16_t) frame << 10));
			l->state = frame == 0 ? 5 : 3;
		} else
			l->state = 5;
		return 0;
	case 5:
		push(i, K_READY_HAND, 597, 1);
		return 0;
	case 6:
		/* arrows: one to three, as the original makes them */
		frame = rnd(10);
		l->counter = (uint8_t) (frame < 2 ? 1 : frame < 5 ? 2 : 3);
		l->state++;
		return 0;
	case 7:
		d->value = 0;
		l->state++;
		return 0;
	case 8:
		if (d->value == 14) {
			l->state++;
			return 0;
		}
		if (step(i, 2)) {
			d->value++;
			return 1;
		}
		if (chance(5) || !npc_ahead(i, 2))
			pop(i, -1);
		else {
			bark(i, 93, 3);             /* "Step aside so that I can practice!" */
			uint8_t s[2] = { S_WAIT, 15 };
			post(i, s, 2);
			l->state = 8;
		}
		return 0;
	case 9:
		d->toggle = 0;
		POST(i, FACE(6), WAIT(5));
		return 0;
	case 10:
		if (!e_things(T_TARGET, (int16_t) (npc[i].x - 17), (int16_t) (npc[i].y + 1), npc[i].x,
			(int16_t) (npc[i].y + 1), &t, 1)) {
			pop(i, -1);
			return 0;
		}
		if (d->toggle) {
			/* the last arrow hit: one more in the target */
			d->toggle = 0;
			frame = t.tf >> 10 & 31;
			if (frame == 0)
				frame = rnd(8) * 3 + 1;
			else if (frame % 3 != 0)
				frame++;
			e_thing_set(&t, (uint16_t) ((t.tf & ~(31u << 10)) | (uint16_t) frame << 10));
		}
		if (l->counter == 0) {
			if (chance(3))
				pop(i, 0);
			else
				l->state = 0;
			return 0;
		}
		l->counter--;
		d->toggle = 1;
		POST(i, P(F_READY), WAIT(1), P(F_RAISE1), WAIT(1), P(F_EXTEND1), WAIT(1), P(F_READY),
			WAIT(2));
		l->state = 10;
		return 0;
	}
	return 0;
}

/* ---- carrying ---- */

/* The item NPC i carries that is spec (a type-frame), or -1. */
static int held_find(int i, int spec)
{
	for (int k = 0; k < 4; k++) {
		uint16_t tf = doing[i].held[k];
		if (tf != 0 && (tf & 0x3ff) == SPEC_TYPE(spec)
			&& (SPEC_FRAME(spec) < 0 || (tf >> 10 & 31) == SPEC_FRAME(spec)))
			return k;
	}
	return -1;
}

static int held_free(int i)
{
	for (int k = 0; k < 4; k++)
		if (doing[i].held[k] == 0)
			return 1;
	return 0;
}

/* A frame within type's (the original's ClampShapeFrame). */
static int clamp_frame(uint16_t type, int frame)
{
	int n = e_shape_frames(type);

	return frame >= n ? (n > 0 ? n - 1 : 0) : frame;
}

/* NPC i carries a new item (the original's CreateCarriedItem): its slot, or -1. */
static int held_add(int i, uint16_t tf)
{
	for (int k = 0; k < 4; k++)
		if (doing[i].held[k] == 0) {
			doing[i].held[k] = tf;
			return k;
		}
	return -1;
}

static uint16_t spec_tf(int spec)
{
	return (uint16_t) (SPEC_TYPE(spec) | (SPEC_FRAME(spec) < 0 ? 0 : SPEC_FRAME(spec)) << 10);
}

/* Where NPC i could put an item of type down beside it, the way it faces first (FindDropSpot):
 * 1, with the way and the level. */
static int drop_spot(int i, uint16_t type, int *way, int *z)
{
	for (int pass = 0; pass < 2; pass++)
		for (int h = 0; h < 3; h++)
			for (int k = 0; k < 4; k++) {
				int w = pass ? 1 + 2 * k : (doing[i].dir * 2 + 2 * k) & 7;
				if (e_can_stand(type, (int16_t) (npc[i].x + way_x[w]), (int16_t) (npc[i].y + way_y[w]),
					(uint8_t) (npc[i].z + h))) {
					*way = w;
					*z = npc[i].z + h;
					return 1;
				}
			}
	return 0;
}

/* Pick up an item x (a type-frame): walked to and taken; with y 1, one made when none is found
 * (PickUpItemSchedule). */
static int grab_item(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;

	switch (l->state) {
	case 0:
		if (held_find(i, l->x) >= 0)
			pop(i, 0);
		else
			push(i, K_GOTO_ITEM, l->x, -1);
		return 0;
	case 1:
		if (d->result < 0) {
			pop(i, l->y == 1 && held_add(i, spec_tf(l->x)) >= 0 ? 0 : -1);
			return 0;
		}
		if (nearest(SPEC_TYPE(l->x), npc[i].x, npc[i].y, 8, 0, 15, SPEC_FRAME(l->x), &t)
			&& t.z >= npc[i].z + 2) {
			l->counter = 1;
			POST(i, P(F_STAND), WAIT(1), P(F_READY), WAIT(2));
		} else {
			l->counter = 0;
			POST(i, P(F_STAND), WAIT(2), P(F_BEND), WAIT(2));
		}
		return 0;
	case 2:
		if (held_free(i)
			&& nearest(SPEC_TYPE(l->x), npc[i].x, npc[i].y, 8, 0, 15, SPEC_FRAME(l->x), &t)
			&& e_thing_remove(&t)) {
			held_add(i, t.tf);
			l->state++;
		} else
			pop(i, -1);
		return 0;
	case 3:
		if (l->counter)
			POST(i, P(F_READY), WAIT(1), P(F_STAND), WAIT(2));
		else
			POST(i, P(F_BEND), WAIT(2), P(F_STAND), WAIT(2));
		return 0;
	case 4:
		pop(i, 0);
		return 0;
	}
	return 0;
}

/* Put down the item x it carries: beside it (y -1), else on the item y, walked to
 * (PutDownItemSchedule; its sound, 18). */
static int drop_item(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;
	int way, z, k;

	switch (l->state) {
	case 0:
		l->counter = l->y != -1;
		if (l->y != -1) {
			l->state = 4;
			return 0;
		}
		k = held_find(i, l->x);
		if (k < 0 || !drop_spot(i, d->held[k], &way, &z)) {
			pop(i, -1);
			return 0;
		}
		face(i, way & 6);
		l->y = (int16_t) z;
		d->value = (int16_t) way;
		if (npc[i].z + 2 <= z)
			POST(i, P(F_STAND), WAIT(1), P(F_READY), WAIT(2));
		else
			POST(i, P(F_STAND), WAIT(2), P(F_BEND), WAIT(2));
		return show(i);
	case 1: {
		int x = npc[i].x + way_x[d->value & 7], y = npc[i].y + way_y[d->value & 7];
		k = held_find(i, l->x);
		if (k >= 0 && e_can_stand(d->held[k], (int16_t) x, (int16_t) y, (uint8_t) l->y)
			&& e_thing_add(d->held[k], (int16_t) x, (int16_t) y, (uint8_t) l->y, NULL)) {
			d->held[k] = 0;
			e_sound(18, (int16_t) x, (int16_t) y);
			l->state++;
		} else
			pop(i, -1);
		return 0;
	}
	case 2:
		if (l->counter && l->y >= npc[i].z + 2)
			POST(i, P(F_READY), WAIT(1), P(F_STAND), WAIT(2));
		else
			POST(i, P(F_BEND), WAIT(2), P(F_STAND), WAIT(2));
		return 0;
	case 3:
		pop(i, 0);
		return 0;
	case 4:
		push(i, K_GOTO_ITEM, l->y, l->x);
		cur(d)->state = 0x80;   /* its second try: the item carried fitting on top */
		return 0;
	case 5:
		if (d->result < 0 || !nearest(SPEC_TYPE(l->y), npc[i].x, npc[i].y, 8, 0, 15, SPEC_FRAME(l->y), &t)) {
			pop(i, -1);
			return 0;
		}
		l->y = (int16_t) (t.z + height_of(t.tf));
		d->value = (int16_t) (d->dir * 2);
		if (l->y >= npc[i].z + 2)
			POST(i, P(F_STAND), WAIT(2), P(F_READY), WAIT(1));
		else
			POST(i, P(F_STAND), WAIT(2), P(F_BEND), WAIT(2));
		l->state = 1;
		return 0;
	}
	return 0;
}

/* Carry an item x to an item y: taken up (made if none is found), put on it (CarryItemSchedule). */
static int move_item(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);

	switch (l->state) {
	case 0:
		l->state = held_find(i, l->x) >= 0 ? 1 : 5;
		return 0;
	case 1:
		push(i, K_DROP_ITEM, l->x, l->y);
		return 0;
	case 2:
		pop(i, d->result < 0 ? -1 : 0);
		return 0;
	case 5:
		push(i, K_GRAB_ITEM, l->x, 1);
		return 0;
	case 6:
		if (held_find(i, l->x) >= 0)
			l->state = 1;
		else
			pop(i, -1);
		return 0;
	}
	return 0;
}

/* ---- the items' scripts (the original's PostScriptToItem): frames, waits, sounds, a tick each
 * but the sounds ---- */

#define I_FRAME 1
#define I_WAIT 2
#define I_SOUND 3
#define I_NEXT 4                /* the next frame, round to the first */
#define I_NEXT_MAX 5            /* the next frame, but not past the last */
#define I_PREV_MIN 6            /* the frame before, but not before the first */
#define I_MARK 7
#define I_LOOP 8                /* back to the mark (or the start), n times */
#define THING_SCRIPTS 16

static struct ThingScript {
	struct EThing t;
	uint8_t on, pc, len, mark, loops;
	int16_t wait;
	uint8_t s[48];
} thing_scripts[THING_SCRIPTS];

/* A script for item t (one it had given way); nothing for none. */
static void post_thing(const struct EThing *t, const uint8_t *steps, int n, int found)
{
	struct ThingScript *o = NULL;

	if (!found)
		return;
	for (int k = 0; k < THING_SCRIPTS && o == NULL; k++)
		if (thing_scripts[k].on && thing_scripts[k].t.x == t->x && thing_scripts[k].t.y == t->y
			&& thing_scripts[k].t.z == t->z && (thing_scripts[k].t.tf & 0x3ff) == (t->tf & 0x3ff))
			o = &thing_scripts[k];
	for (int k = 0; k < THING_SCRIPTS && o == NULL; k++)
		if (!thing_scripts[k].on)
			o = &thing_scripts[k];
	if (o == NULL)
		return;
	o->t = *t;
	o->on = 1;
	o->pc = o->mark = o->loops = 0;
	o->wait = 0;
	o->len = (uint8_t) (n > (int) sizeof o->s ? (int) sizeof o->s : n);
	memcpy(o->s, steps, o->len);
}
#define THING(t, found, ...) do { static const uint8_t s_[] = { __VA_ARGS__ }; \
	post_thing(t, s_, sizeof s_, found); } while (0)

void kind_things(int blanks)
{
	for (int k = 0; k < THING_SCRIPTS; k++) {
		struct ThingScript *o = &thing_scripts[k];
		if (!o->on)
			continue;
		o->wait = (int16_t) (o->wait - blanks);
		while (o->on && o->wait <= 0) {
			if (o->pc >= o->len) {
				o->on = 0;
				break;
			}
			uint8_t op = o->s[o->pc], arg = o->s[o->pc + 1];
			int frame = o->t.tf >> 10 & 31, n = e_shape_frames(o->t.tf & 0x3ff), to = -1;
			o->pc += 2;
			switch (op) {
			case I_FRAME: to = arg; break;
			case I_NEXT: to = n > 0 ? (frame + 1) % n : 0; break;
			case I_NEXT_MAX: to = frame + 1 < n ? frame + 1 : frame; break;
			case I_PREV_MIN: to = frame > 0 ? frame - 1 : 0; break;
			case I_WAIT: o->wait = (int16_t) (o->wait + arg * TICK_BLANKS); break;
			case I_SOUND: e_sound(arg, o->t.x, o->t.y); break;
			case I_MARK: o->mark = o->pc; break;
			case I_LOOP:
				if (o->loops < arg) {
					o->loops++;
					o->pc = o->mark;
				}
				break;
			}
			if (to >= 0) {
				if (to != frame && !e_thing_set(&o->t, (uint16_t) ((o->t.tf & ~(31u << 10)) | (uint16_t) to << 10)))
					o->on = 0;      /* gone */
				o->wait = (int16_t) (o->wait + TICK_BLANKS);
			}
		}
	}
}

/* Fill a bucket (DoWorkFillBucket): the bucket taken (made if none), at the trough (x), or the
 * well (x), or (x -1) the trough unless it is empty; the bucket full (frame 1). */
static int fill_bucket(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;
	int k, found;

	switch (l->state) {
	case 0:
		if (held_find(i, 810) >= 0)
			l->state++;
		else
			push(i, K_GRAB_ITEM, 810, 1);
		return 0;
	case 1: {
		int type = l->x == 719 || l->x == 470 ? l->x : 719;
		if (l->x != 719 && l->x != 470 && (!nearest(719, npc[i].x, npc[i].y, 25, 0, 15, -1, &t)
			|| (t.tf >> 10 & 31) == 0))
			type = 470;
		if (d->result < 0)
			pop(i, -1);
		else
			push(i, K_GOTO_ITEM, type, -1);
		return 0;
	}
	case 2:
		if (d->result < 0) {
			k = held_find(i, 810);
			if (k >= 0)
				d->held[k] = 810 | 1 << 10;
			pop(i, 0);
		} else if (d->result == 719) {
			POST(i, P(F_STAND), WAIT(2), P(F_BEND), WAIT(2), P(F_BEND), WAIT(2), P(F_STAND), WAIT(2));
			found = nearest(719, npc[i].x, npc[i].y, 25, 0, 15, -1, &t);
			THING(&t, found, I_WAIT, 5, I_PREV_MIN, 0);
		} else {
			POST(i, P(F_READY), P(F_READY), P(F_THRUST1), P(F_THRUST1), P(F_RAISE1), P(F_RAISE1),
				P(F_EXTEND1), P(F_EXTEND1), P(F_RAISE1), P(F_RAISE1), P(F_THRUST1), P(F_THRUST1),
				P(F_READY), P(F_READY));
			found = nearest(740, npc[i].x, npc[i].y, 25, 0, 15, -1, &t);
			THING(&t, found, I_FRAME, 1, I_FRAME, 1, I_FRAME, 2, I_FRAME, 2, I_FRAME, 3, I_WAIT, 5,
				I_FRAME, 4, I_FRAME, 4, I_FRAME, 5, I_FRAME, 5, I_FRAME, 0);
		}
		return 0;
	case 3:
		k = held_find(i, 810);
		if (k >= 0)
			d->held[k] = 810 | 1 << 10;
		pop(i, 0);
		return 0;
	}
	return 0;
}

/* Fill the water trough (DoWorkFillWaterTrough): buckets from the well poured in till it is
 * full (frame 3). */
static int fill_trough(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;
	int k, found;

	switch (l->state) {
	case 0:
		if (nearest(719, npc[i].x, npc[i].y, 25, 0, 15, -1, &t) && (t.tf >> 10 & 31) == 3)
			pop(i, 0);
		else
			l->state++;
		return 0;
	case 1:
		push(i, K_FILL_BUCKET, 470, -1);
		return 0;
	case 2:
		if (d->result < 0)
			pop(i, -1);
		else
			push(i, K_GOTO_ITEM, 719, -1);
		return 0;
	case 3:
		if (d->result < 0) {
			pop(i, -1);
			return 0;
		}
		POST(i, P(F_STAND), WAIT(2), P(F_BEND), WAIT(2));
		found = nearest(719, npc[i].x, npc[i].y, 5, 0, 15, -1, &t);
		THING(&t, found, I_WAIT, 4, I_SOUND, 46, I_NEXT_MAX, 0);
		return 0;
	case 4:
		k = held_find(i, 810);
		if (k >= 0)
			d->held[k] = 810;
		POST(i, P(F_BEND), WAIT(2), P(F_STAND), WAIT(2));
		if (nearest(719, npc[i].x, npc[i].y, 5, 0, 15, -1, &t) && (t.tf >> 10 & 31) < 3)
			l->state = 1;
		return 0;
	case 5:
		push(i, K_DROP_ITEM, 810, -1);
		return 0;
	case 6:
		pop(i, 0);
		return 0;
	}
	return 0;
}

/* ---- looking round: the lights and the shutters ---- */

static int daytime(void)       /* the original's IsDaytime: from 5:00 till 21:00 */
{
	int hour = e_world_minutes() / 60;

	return hour > 4 && hour < 21;
}

/* Item t used, as its usecode does for shutters and lights: shutters opened or shut (their
 * sound, 2), a lamp post lit (its glow, 440, put 3 cells along; sound 106) or put out (46), a
 * light source lit or put out. 1 when it was one of these. */
static int use_item(struct EThing *t)
{
	uint16_t type = t->tf & 0x3ff, to;
	struct EThing glow;

	switch (type) {
	case 290: to = 372; break;
	case 291: to = 322; break;
	case 322: to = 291; break;
	case 372: to = 290; break;
	case 336: to = 338; break;
	case 338: to = 336; break;
	case 889: to = 526; break;
	case 526: to = 889; break;
	default: return 0;
	}
	if (!e_thing_set(t, (uint16_t) ((t->tf & ~0x3ffu) | to)))
		return 0;
	if (type == 889) {
		e_thing_add(440, (int16_t) (t->x + 3), (int16_t) (t->y + 3), t->z, NULL);
		e_sound(106, t->x, t->y);
	} else if (type == 526) {
		if (e_things(440, (int16_t) (t->x + 3), (int16_t) (t->y + 3), (int16_t) (t->x + 3),
			(int16_t) (t->y + 3), &glow, 1) && glow.z == t->z)
			e_thing_remove(&glow);
		e_sound(46, t->x, t->y);
	} else if (type < 336 || type > 338)
		e_sound(2, t->x, t->y);
	return 1;
}

/* Look round (DoWorkCheckArea): the lights or the shutters, whichever comes up, then the other
 * if that found nothing to do. */
static int check_area(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);

	switch (l->state) {
	case 0:
		l->state++;
		return 0;
	case 1:
		l->y = chance(2) ? K_CHECK_LIGHT : K_CHECK_SHUTTERS;
		push(i, l->y, l->x, -1);
		return 0;
	case 2:
		if (d->result < 0)
			push(i, l->y == K_CHECK_LIGHT ? K_CHECK_SHUTTERS : K_CHECK_LIGHT, l->x, -1);
		else
			pop(i, 0);
		return 0;
	case 3:
		pop(i, d->result < 0 ? -1 : 0);
		return 0;
	}
	return 0;
}

/* The nearer of what t0 and t1 found (have0, have1): 1 with it in t. */
static int nearer(int i, int have0, const struct EThing *t0, int have1, const struct EThing *t1,
	struct EThing *t)
{
	if (!have0 && !have1)
		return 0;
	*t = !have1 || (have0 && far_of(npc[i].x, npc[i].y, t0->x, t0->y)
		< far_of(npc[i].x, npc[i].y, t1->x, t1->y)) ? *t0 : *t1;
	return 1;
}

/* Check the lights (DoWorkCheckLight) or the shutters (DoWorkCheckShutters): to the nearest in the
 * wrong state for the hour (lit by day, dark by night; shut by day, open by night; a spent light
 * at any hour), used; with x 1, walked back after. */
static int check_lights(int i, int shutters)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing a, b, c, t;
	uint8_t under = d->level[0].kind;
	int done = 0;

	switch (l->state) {
	case 0: {
		if (planned || searched) {
			d->wait = 1;
			return 0;
		}
		searched = 1;
		if (l->x == 1) {
			l->x = npc[i].x;
			l->y = npc[i].y;
		}
		int x = npc[i].x, y = npc[i].y, day = daytime(), r;
		if (shutters) {
			r = under == K_PREACH ? 12 : under == K_SLEEP || under == K_WAITER ? 10 : 15;
			int ha = nearest(day ? 291 : 372, x, y, r, 0, 15, -1, &a);
			int hb = nearest(day ? 290 : 322, x, y, r, 0, 15, -1, &b);
			if (!nearer(i, ha, &a, hb, &b, &t)) {
				pop(i, -1);
				return 0;
			}
		} else {
			r = under == K_SLEEP || under == K_WAITER ? 8 : 12;
			int ha = nearest(day ? 526 : 889, x, y, r, 0, 15, -1, &a);        /* lamp posts */
			int hb = nearest(day ? 338 : 336, x, y, r, 0, 15, -1, &b);        /* light sources */
			int hc = nearest(997, x, y, r, 0, 15, -1, &c);                    /* spent */
			struct EThing ab;
			int hab = nearer(i, ha, &a, hb, &b, &ab);
			if (!nearer(i, hab, &ab, hc, &c, &t)) {
				pop(i, -1);
				return 0;
			}
			/* a lit light source in frame 7 or 9 is passed over for a lamp or a spent light */
			if ((t.tf & 0x3ff) == 338 && ((t.tf >> 10 & 31) == 7 || (t.tf >> 10 & 31) == 9)
				&& !nearer(i, ha, &a, hc, &c, &t)) {
				pop(i, -1);
				return 0;
			}
		}
		push(i, K_GOTO_ITEM, t.tf & 0x3ff, -1);
		return 0;
	}
	case 1:
		if (d->result < 0)
			pop(i, -1);
		else
			POST(i, P(F_STAND), WAIT(1), P(F_READY), WAIT(2));
		return 0;
	case 2: {
		int type = d->result, w = d->dir * 2;
		int x = npc[i].x + way_x[w], y = npc[i].y + way_y[w], found;
		if (shutters)
			found = nearest((uint16_t) type, npc[i].x, npc[i].y, 3, 0, 15, -1, &t);
		else
			found = e_things((uint16_t) type, (int16_t) x, (int16_t) y, (int16_t) x, (int16_t) y, &t, 1);
		if (found) {
			if (type == 997) {
				/* a spent light swapped for a fresh one */
				if (e_thing_remove(&t))
					e_thing_add((uint16_t) ((daytime() ? 336 : 338) | (t.tf & 0x7c00)), t.x, t.y, t.z, NULL);
			} else
				use_item(&t);
			done = 1;
			if (shutters)
				/* "These should be closed." ..., else "Ah, 'tis better!" ... */
				bark(i, type == 372 || type == 322 ? 113 : 116, 3);
			else if (chance(3))
				bark(i, 99, 3);         /* "Ah, 'tis better." */
			else if (chance(3))
				bark(i, type == 336 || type == 889 ? 102 : type == 997 ? 104 : 103, 1);
		}
		POST(i, P(F_STAND), WAIT(15));
		if (l->x == -1)
			pop(i, done ? 0 : -1);
		else if (start_path(i, l->x, l->y, 0, 100) == 0) {
			l->x = (int16_t) done;
			/* the walk back ends the state on */
		} else
			pop(i, -1);
		return 0;
	}
	case 4:
		pop(i, l->x ? 0 : -1);
		return 0;
	}
	return 0;
}

/* ---- the kinds of the schedules ---- */

/* Stand (RunStandSchedule). */
static int stand_kind(int i)
{
	struct Level *l = cur(&doing[i]);

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		doing[i].pose = F_STAND;
		l->state++;
		return show(i);
	}
	doing[i].wait = 60 * TICK_BLANKS;
	return 0;
}

/* Loiter (RunLoiterSchedule): walks to places within 15 cells of where it began (8 under combat
 * or a patrol), resting between. Pushed over another, it goes back to that now and then. */
static int loiter(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct ENpc *p = &npc[i];
	int outer = d->depth == 1, r;
	uint8_t under = d->level[0].kind;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		d->mark_x = p->x;
		d->mark_y = p->y;
		d->value = 0;
		l->state++;
		return 0;
	case 2:
		l->state++;
		return 0;
	case 3:
		if (outer && chance(3)) {
			d->value = 0;
			l->state = 2;
			push(i, K_CHECK_AREA, 1, -1);
		} else if (outer && chance(15))
			;                   /* its usecode: not yet */
		else if (d->value > 2)
			l->state = 5;
		else {
			int near = under == K_COMBAT || under == K_PATROL, w = near ? 8 : 15;
			r = start_path(i, d->mark_x + range(-w, w), d->mark_y + range(-w, w), p->z, near ? 15 : 30);
			if (r == 0)
				l->state++;
			else if (r == 2)
				d->value++;
			else
				d->wait = 1;
		}
		return 0;
	case 5:
		d->value = 0;
		{
			uint8_t s[2] = { S_WAIT, (uint8_t) (outer ? range(20, 40) : range(15, 25)) };
			post(i, s, 2);
		}
		d->pose = F_STAND;
		return show(i);
	case 6:
		if (outer)
			l->state = 2;
		else if (under == K_PATROL && l->x == 40 && far_of(p->x, p->y, me_x, me_y) <= 10)
			pop(i, 1);
		else if (under == K_COMBAT || chance(4))
			pop(i, 0);          /* back to where it was: not walked yet */
		else
			l->state = 2;
		return 0;
	case 7:
		pop(i, d->result);
		return 0;
	}
	return 0;
}

/* Wander (RunWanderSchedule): to places within 15 cells of where it is; after three walks not
 * found, a rest. */
static int wander(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct ENpc *p = &npc[i];
	int r;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		d->value = 0;
		l->state++;
		return 0;
	case 2:
		if (chance(3)) {
			d->value = 0;
			l->state = 1;
			push(i, K_CHECK_AREA, -1, -1);
		} else if (d->value > 2)
			l->state = 4;
		else if ((r = start_path(i, p->x + range(-15, 15), p->y + range(-15, 15), p->z, 45)) == 0)
			l->state++;
		else if (r == 2)
			d->value++;
		else
			d->wait = 1;
		return 0;
	case 4:
		{
			uint8_t s[2] = { S_WAIT, (uint8_t) range(20, 40) };
			post(i, s, 2);
		}
		d->pose = F_STAND;
		return show(i);
	case 5:
		l->state = 1;
		return 0;
	}
	return 0;
}

/* Tend shop (RunTendShopSchedule): walks within 8 cells of where it began, rests after a few walks
 * not found, and half the time goes to meet the avatar within 20 cells of there. */
static int shop(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct ENpc *p = &npc[i];
	int r;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		d->mark_x = p->x;
		d->mark_y = p->y;
		d->value = 0;
		l->state++;
		return 0;
	case 2:
		if (chance(3)) {
			d->value = 0;
			l->state = 1;
			push(i, K_CHECK_AREA, 1, -1);
		} else if (d->value > 3)
			l->state = 4;
		else if (chance(2)) {
			d->value = 0;
			l->state = 10;
		} else if ((r = start_path(i, d->mark_x + range(-8, 8), d->mark_y + range(-8, 8), 0, 15)) == 0)
			l->state++;
		else if (r == 2)
			d->value++;
		else
			d->wait = 1;
		return 0;
	case 4:
		d->value = 0;
		{
			uint8_t s[2] = { S_WAIT, (uint8_t) range(25, 50) };
			post(i, s, 2);
		}
		d->pose = F_STAND;
		return show(i);
	case 5:
		l->state = 2;
		return 0;
	case 10:
		if (far_of(p->x, p->y, me_x, me_y) <= 2) {
			uint8_t s[4] = { S_POSE, F_STAND, S_WAIT, (uint8_t) range(15, 25) };
			post(i, s, 4);
		} else if (far_of(me_x, me_y, d->mark_x, d->mark_y) < 20) {
			if (!path_next_to(i, avatar, 50))
				l->state = 2;
		} else
			l->state = 2;
		return 0;
	case 11:
		l->state = 2;
		return 0;
	}
	return 0;
}

/* ---- sleep (RunSleepSchedule) ---- */

/* The nearer of the two beds (futons for one wider than a cell) within 25 cells, and a spot to get
 * in from: walking there, 1; 0 no bed; -1 a bed but no spot; 2 to ask again. */
static int find_bed(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];
	struct EBody b;
	struct EThing a, c;
	int x, y, z, way, r;

	e_body(e_walk_frame(p->tf & 0x3ff, 2, 0), &b);
	int fits = b.fx == 0 && b.fy == 0;
	int have_a = nearest(fits ? T_BED : T_FUTON, p->x, p->y, 25, 0, 15, -1, &a);
	int have_c = nearest(fits ? T_BED_2 : T_FUTON_2, p->x, p->y, 25, 0, 15, -1, &c);
	if (!have_a && !have_c)
		return 0;
	if (!have_a || (have_c && far_of(p->x, p->y, c.x, c.y) < far_of(p->x, p->y, a.x, a.y)))
		a = c;
	if (!use_spot(i, &a, &x, &y, &z, &way, 0))
		return -1;
	if ((r = start_path(i, x, y, z, 100)) == 1)
		return 2;
	if (r == 2)
		return -1;
	d->use = a;
	d->spot_x = (int16_t) x;
	d->spot_y = (int16_t) y;
	d->spot_z = (uint8_t) z;
	d->face8 = (uint8_t) way;
	return 1;
}

/* At the spot: into the bed, lying along it, its cover (a bed of its type a level up nearby, an
 * odd frame) turned over the sleeper; 0 when the bed is gone or taken. */
static int into_bed(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];
	uint16_t bed = d->use.tf & 0x3ff;
	struct EThing in_cell[4], t;
	int fits = bed == T_BED || bed == T_BED_2;
	int x = d->use.x + (bed == T_BED && (d->use.tf >> 10 & 31) == 17), y = d->use.y, there = 0;
	uint8_t z = (uint8_t) (p->z + fits);

	for (int k = e_things(bed, d->use.x, d->use.y, d->use.x, d->use.y, in_cell, 4); k-- > 0; )
		there |= in_cell[k].tf == d->use.tf && in_cell[k].z == d->use.z;
	for (int s = 1; s < E_MOVERS && there; s++)
		if (slot_npc[s] >= 0 && slot_npc[s] != i && npc[slot_npc[s]].x == x
			&& npc[slot_npc[s]].y == y)
			there = 0;
	if (!there)
		return 0;
	if (fits && nearest(bed, p->x, p->y, 5, z, z, -1, &t) && (t.tf >> 10 & 1)
		&& e_thing_set(&t, (uint16_t) (t.tf + (1u << 10))))
		d->cover = t;
	p->x = (int16_t) x;
	p->y = (int16_t) y;
	p->z = z;
	d->dir = bed == T_BED || bed == T_FUTON ? 0 : 3;
	d->pose = F_LIE;
	show(i);
	return 1;
}

/* Woken (WakeUpNpc): the cover back, a step off the bed (south-west, else south, off a bed of
 * 1011; north-east, else east, off a 696), standing. */
static void wake_up(int i)
{
	struct Doing *d = &doing[i];
	struct ENpc *p = &npc[i];
	struct EThing t;

	if (d->cover.tf != 0) {
		e_thing_set(&d->cover, (uint16_t) (d->cover.tf - (1u << 10)));
		d->cover.tf = 0;
	}
	int w0 = -1, w1 = -1;
	if (e_things(T_BED, p->x, p->y, p->x, p->y, &t, 1)) {
		w0 = 5;
		w1 = 4;
	} else if (e_things(T_BED_2, p->x, p->y, p->x, p->y, &t, 1)) {
		w0 = 1;
		w1 = 2;
	}
	d->pose = F_STAND;
	if (w0 >= 0 && !step(i, w0))
		step(i, w1);
	d->pose = F_STAND;
	show(i);
}

/* ---- the avatar put in a chair or a bed by the usecode ---- */

static uint8_t avatar_moved, avatar_posed;
static int avatar_kind = -1;    /* its work before (what GetWorkType says meanwhile: sitting, asleep) */

static void avatar_work(int kind)
{
	struct Doing *d = &doing[avatar];

	if (avatar_kind < 0)
		avatar_kind = d->depth ? d->level[0].kind : K_STAND;
	set_kind(avatar, kind);
}

int kind_avatar_sit(const struct EThing *chair)
{
	int i = avatar, x, y, z, way;

	if (i < 0 || !use_spot(i, chair, &x, &y, &z, &way, 0))
		return 0;
	npc[i].x = (int16_t) x;
	npc[i].y = (int16_t) y;
	npc[i].z = (uint8_t) z;
	face(i, way);
	doing[i].pose = F_SIT;
	show(i);
	avatar_work(K_MAJOR_SIT);
	avatar_moved = avatar_posed = 1;
	return 1;
}

int kind_avatar_bed(const struct EThing *bed)
{
	int i = avatar, x, y, z, way;

	if (i < 0 || !use_spot(i, bed, &x, &y, &z, &way, 0))
		return 0;
	int16_t ox = npc[i].x, oy = npc[i].y;
	uint8_t oz = npc[i].z;
	npc[i].x = (int16_t) x;
	npc[i].y = (int16_t) y;
	npc[i].z = (uint8_t) z;
	doing[i].use = *bed;
	if (!into_bed(i)) {
		npc[i].x = ox;
		npc[i].y = oy;
		npc[i].z = oz;
		return 0;
	}
	avatar_work(K_SLEEP);
	avatar_moved = avatar_posed = 1;
	return 1;
}

void kind_avatar_stand(void)
{
	int i = avatar;

	if (i < 0 || !avatar_posed)
		return;
	if (doing[i].pose == F_LIE)
		wake_up(i);
	doing[i].pose = F_STAND;
	show(i);
	if (avatar_kind >= 0)
		set_kind(i, avatar_kind);
	avatar_kind = -1;
	avatar_posed = 0;
	avatar_moved = 1;
}

int kind_avatar_posed(void)
{
	return avatar_posed;
}

int kind_avatar_moved(void)
{
	int m = avatar_moved;

	avatar_moved = 0;
	return m;
}

static int sleep_kind(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	int r;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		push(i, K_CHECK_AREA, 1, -1);
		return 0;
	case 2:
		l->state = d->result < 0 ? 3 : 1;
		return 0;
	case 3:
		if (planned || searched) {
			d->wait = 1;
			return 0;
		}
		searched = 1;
		r = find_bed(i);
		if (r == 2)
			d->wait = 1;
		else if (r == 1)
			l->state++;
		else if (r == 0)
			l->state = 10;
		else {
			set_kind(i, K_LOITER);
			cur(d)->state = 1;
		}
		return 0;
	case 5:
		face(i, d->face8);
		l->state++;
		d->wait = TICK_BLANKS;
		return show(i);
	case 6:
		if (d->result < 0 || !into_bed(i)) {
			set_kind(i, K_LOITER);
			cur(d)->state = 1;
			return 0;
		}
		l->state++;
		return 1;
	case 7:
		/* asleep: now and then, the brighter the more often, woken by the avatar near (not
		 * Penumbra, 150): it barks, follows the avatar a while, then goes back to bed */
		if (rnd(750) < npc[i].intelligence && npc[i].number != 150
			&& far_of(npc[i].x, npc[i].y, me_x, me_y) <= 15
			&& e_clear_line(npc[i].x, npc[i].y, npc[i].z, me_x, me_y, npc[avatar].z)) {
			int number = npc[i].number;
			wake_up(i);
			set_kind(i, K_SLEEP);
			if (number < 256)
				cur(d)->state = 255;    /* after the hound, from the start: back to bed */
			push(i, K_HOUND, -1, -1);
			cur(d)->state = 1;
			bark(i, 149, 6);
			return 1;
		}
		return 0;
	case 10:
		d->spot_x = npc[i].x;
		d->spot_y = npc[i].y;
		d->spot_z = npc[i].z;
		POST(i, P(F_LIE));
		return 0;
	}
	d->wait = 600 * TICK_BLANKS;
	return 0;
}

/* ---- sitting, eating ---- */

/* Sit (RunMajorSitSchedule, not in the party). */
static int major_sit(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		push(i, K_SIT, -1, -1);
		return 0;
	case 2:
		if (d->result < 0) {
			set_kind(i, K_LOITER);
			cur(d)->state = 1;
		} else
			l->state++;
		return 0;
	}
	d->wait = 600 * TICK_BLANKS;
	return 0;
}

/* The food a cell (or, far, two) ahead of a sitter eaten: 1 when there was some. */
static int eat_ahead(int i, int far)
{
	struct ENpc *p = &npc[i];
	struct EThing t;
	int w = doing[i].dir * 2;
	int x0 = p->x + way_x[w], y0 = p->y + way_y[w], x1 = p->x + way_x[w] * far, y1 = p->y + way_y[w] * far;

	if (!e_things(T_FOOD, (int16_t) (x0 < x1 ? x0 : x1), (int16_t) (y0 < y1 ? y0 : y1),
		(int16_t) (x0 > x1 ? x0 : x1), (int16_t) (y0 > y1 ? y0 : y1), &t, 1))
		return 0;
	return e_thing_remove(&t);
}

/* Eat at the inn (RunEatAtInnSchedule): sits, and now and then eats the food ahead (or calls for
 * service: a bark). */
static int eat_inn(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		push(i, K_SIT, -1, -1);
		return 0;
	case 2:
		if (d->result < 0) {
			set_kind(i, K_LOITER);
			cur(d)->state = 1;
		} else
			push(i, K_PAUSE, range(40, 100), -1);
		return 0;
	case 3:
		if (chance(2)) {
			if (eat_ahead(i, 2))
				bark(i, 37, 3);         /* "Mmmm, tasty!", "Burp!", "Mmmm..." */
		} else
			bark(i, 32, 5);             /* "More food!", "Service!", ... */
		l->state = 2;
		return 0;
	}
	return 0;
}

/* Eat (RunEatSchedule): (food put on a plate: not yet) sits at the plate, waits, eats. */
static int eat(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		push(i, K_MOVE_ITEM, T_FOOD, T_PLATE);
		return 0;
	case 2:
	case 4:
		if (d->result < 0) {
			set_kind(i, K_LOITER);
			cur(d)->state = 1;
		} else
			l->state++;
		return 0;
	case 3:
		push(i, K_SIT, T_PLATE, -1);
		return 0;
	case 5:
		{
			uint8_t s[2] = { S_WAIT, (uint8_t) range(20, 50) };
			post(i, s, 2);
		}
		return 0;
	case 6:
		eat_ahead(i, 1);
		l->state++;
		return 0;
	}
	d->wait = 600 * TICK_BLANKS;
	return 0;
}

/* Desk work (RunDeskWorkSchedule): sits at the nearer desk a while, (takes a new desk item to a
 * table: not yet,) looks round. */
static int desk(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing a, c;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		l->state = (uint8_t) ((rnd(3) + 1) * 10);
		return 0;
	case 10: {
		if (planned || searched) {
			d->wait = 1;
			return 0;
		}
		searched = 1;
		int have_a = nearest(T_DESK, npc[i].x, npc[i].y, 25, 0, 15, -1, &a);
		int have_c = nearest(T_DESK_2, npc[i].x, npc[i].y, 25, 0, 15, -1, &c);
		push(i, K_SIT, have_a && (!have_c || far_of(npc[i].x, npc[i].y, a.x, a.y)
			< far_of(npc[i].x, npc[i].y, c.x, c.y)) ? T_DESK : T_DESK_2, -1);
		return 0;
	}
	case 11:
		if (d->result < 0)
			l->state = 1;
		else {
			uint8_t s[2] = { S_WAIT, (uint8_t) range(60, 90) };
			post(i, s, 2);
		}
		return 0;
	case 12:
	case 21:
	case 31:
		l->state = 1;
		return 0;
	case 20: {
		/* a new desk item, taken to one of the desks and tables in reach */
		static const uint16_t places[4] = { 407, 633, 1000, 283 };
		static const uint8_t frames[13] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 15 };
		int found[4], any = 0, pick;
		if (planned || searched) {
			d->wait = 1;
			return 0;
		}
		searched = 1;
		for (int k = 0; k < 4; k++)
			any |= found[k] = nearest(places[k], npc[i].x, npc[i].y, 25, 0, 15, -1, &a);
		if (!any) {
			l->state = 1;
			return 0;
		}
		do
			pick = rnd(4);
		while (!found[pick]);
		push(i, K_MOVE_ITEM, TF(675, frames[rnd(13)]), places[pick]);
		return 0;
	}
	case 30:
		push(i, K_CHECK_AREA, 1, -1);
		return 0;
	}
	return 0;
}

/* ---- the kinds that walk and show ---- */

/* Pace (RunPaceSchedule): back and forth (horizontal: west first; vertical: north), turning about
 * where the way is blocked, waiting for an NPC in the way. */
static int pace(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	int h;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		d->value = 0;
		l->x = l->kind == K_HOR_PACE ? 6 : 0;
		l->state++;
		return 0;
	case 2:
		if (d->value == 0) {
			if (step(i, l->x))
				return 1;
			if (npc_ahead(i, l->x)) {
				d->pose = F_STAND;
				bark(i, 0, 3);          /* "Step aside!", "Stand away!", "Move on!" */
				d->value++;
				return show(i);
			}
			l->state++;
		} else if (d->value > 15)
			d->value = 0;
		else
			d->value++;
		return 0;
	case 3: {
		h = l->x;
		uint8_t s[] = { S_POSE, F_STAND, S_WAIT, 2, S_FACE, (uint8_t) ((h + 6) & 7), S_WAIT, 2,
			S_FACE, (uint8_t) ((h + 4) & 7), S_WAIT, 2 };
		post(i, s, sizeof s);
		return 0;
	}
	case 4:
		if (d->level[0].kind != K_HOR_PACE && d->level[0].kind != K_VER_PACE && chance(4))
			pop(i, 0);          /* back where it was (not walked yet) */
		else {
			l->x = (int16_t) ((l->x + 4) & 7);
			l->state = 2;
		}
		return 0;
	case 5:
		pop(i, d->result);
		return 0;
	}
	return 0;
}

/* Hound (DoWorkHound): keeps within 5 cells of the avatar, now and then walking near it. */
static int hound(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct ENpc *p = &npc[i];

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		l->counter = 0;
		if (far_of(p->x, p->y, me_x, me_y) > 5) {
			if (!path_next_to(i, avatar, 60)) {
				l->counter = 1;
				l->state = 3;
			}
		} else if (chance(10) && start_path(i, me_x + range(-5, 5), me_y + range(-5, 5), p->z, 30) == 0)
			l->state++;
		return 0;
	case 3: {
		uint8_t s[2] = { S_WAIT, (uint8_t) (l->counter ? range(10, 20) : range(3, 7)) };
		post(i, s, 2);
		return 0;
	}
	case 4:
		l->state = 1;
		return 0;
	}
	return 0;
}

/* Talk (RunTalkSchedule): walks up to the avatar and stands before it. (The conversation it
 * then starts is not done.) */
static int talk(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		if (!path_next_to(i, avatar, 75)) {
			push(i, K_LOITER, -1, -1);
			cur(d)->state = 1;
		} else if (far_of(npc[i].x, npc[i].y, me_x, me_y) <= 3)
			l->state++;
		else if (chance(20))
			bark(i, 20, 3);             /* "Stop!", "I would have words with thee." */
		return 0;
	case 2:
		POST(i, P(F_STAND));
		return 0;
	}
	d->wait = 60 * TICK_BLANKS;
	return 0;
}

/* Shy (RunShySchedule): the avatar within 10 cells, it steps to a spot within 5 that is further. */
static int shy(int i)
{
	struct Level *l = cur(&doing[i]);
	struct ENpc *p = &npc[i];

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1: {
		int far = far_of(p->x, p->y, me_x, me_y);
		if (far <= 10) {
			int x = p->x + range(-5, 5), y = p->y + range(-5, 5);
			if (far_of(me_x, me_y, x, y) > far + 2 && start_path(i, x, y, p->z, 100) == 0)
				l->state++;
		}
		return 0;
	}
	case 3:
		l->state = 1;
		return 0;
	}
	return 0;
}

/* Dance (RunDanceSchedule): poses and turns on the spot, now and then a step or two. */
static int dance(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	int w = d->dir * 2;
	uint8_t t2 = (uint8_t) ((w + 2) & 7), t4 = (uint8_t) ((w + 4) & 7), t6 = (uint8_t) ((w + 6) & 7);

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		d->mark_x = npc[i].x;
		d->mark_y = npc[i].y;
		l->state++;
		return 0;
	case 2:
		POST(i, P(F_READY), P(F_READY), P(F_RAISE1), P(F_RAISE1), P(F_EXTEND1), P(F_EXTEND1),
			P(F_THRUST1), P(F_THRUST1), P(F_READY), P(F_READY));
		l->state = 8;
		return 0;
	case 3:
	case 4:
	case 5: {
		uint8_t s[] = { S_POSE, l->state == 3 ? F_OUT : F_UP, S_POSE, l->state == 3 ? F_OUT : F_UP,
			S_FACE, t2, S_WAIT, 2, S_FACE, t4, S_WAIT, 2, S_FACE, t6, S_WAIT, 2, S_FACE, (uint8_t) w,
			S_WAIT, 2 };
		if (l->state == 5)
			post(i, s + 4, sizeof s - 4);
		else
			post(i, s, sizeof s);
		l->state = 8;
		return 0;
	}
	case 6: {
		uint8_t s[] = { S_POSE, F_UP, S_POSE, F_UP, S_POSE, F_OUT, S_POSE, F_OUT, S_LOOP,
			(uint8_t) range(1, 4) };
		post(i, s, sizeof s);
		l->state = 8;
		return 0;
	}
	case 7:
		l->state = start_path(i, d->mark_x + range(-3, 3), d->mark_y + range(-3, 3), 0, 10) == 0 ? 8 : 9;
		return 0;
	case 8:
	case 9:
		l->state = (uint8_t) range(2, 7);
		return 0;
	}
	return 0;
}

/* Preach (RunPreachSchedule): at the podium, exhorting the congregation; visiting one of them;
 * praying at the Fellowship's icon. (The words are barks: not yet.) */
static int preach(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	int k;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		d->toggle = 1;
		l->state++;
		return 0;
	case 2:
		if (chance(3)) {
			l->state = 1;
			push(i, K_CHECK_AREA, 1, -1);
			return 0;
		}
		l->state = d->toggle ? 10 : (uint8_t) ((rnd(3) + 1) * 10);
		d->toggle = !d->toggle;
		return 0;
	case 10:
		push(i, K_GOTO_ITEM, T_PODIUM, -1);
		return 0;
	case 11:
		if (d->result < 0) {
			set_kind(i, K_LOITER);
			cur(d)->state = 1;
		} else
			l->state++;
		return 0;
	case 12:
		POST(i, WAIT(20));
		return 0;
	case 13:
		bark(i, 3, 5);                  /* "Strive for unity!", "Trust thy brother!" */
		POST(i, WAIT(20));
		if ((k = npc_in_front(i, 0)) >= 0)
			bark(k, 12, 4);             /* "I am worthy!", "I believe!", "Yes!" */
		return 0;
	case 14:
	case 23:
	case 31:
		l->state = 2;
		return 0;
	case 20:
		k = npc_in_front(i, 1);
		if (k >= 0) {
			d->value = (int16_t) k;
			l->state++;
		} else
			l->state = 2;
		return 0;
	case 21: {
		int x, y;
		place_of(d->value, &x, &y);
		if (!path_next_to(i, d->value, 100))
			l->state = 1;
		else if (far_of(npc[i].x, npc[i].y, x, y) <= 1)
			l->state++;
		return 0;
	}
	case 22:
		bark(i, 8, 4);                  /* "Say it with me, brother!" */
		POST(i, P(F_STAND), WAIT(30));
		return 0;
	case 30:
		push(i, K_PAY_RESPECTS, T_ICON, -1);
		return 0;
	}
	return 0;
}

/* Kid games (RunKidGamesSchedule): tag, over and over. */
static int kids(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		l->state = 10;
		return 0;
	case 2:
		if (d->result < 0) {
			push(i, K_LOITER, -1, -1);
			cur(d)->state = 1;
		} else
			l->state = 1;
		return 0;
	case 3:
		l->state = 1;
		return 0;
	case 10:
		push(i, K_TAG, -1, -1);
		return 0;
	case 11:
		l->state = 2;
		return 0;
	}
	return 0;
}

/* Duel (RunDuelSchedule): archery or fencing practice, at a target or a dummy within 25 cells. */
static int duel(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;
	int x = npc[i].x, y = npc[i].y;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		if (planned || searched) {
			d->wait = 1;
			return 0;
		}
		searched = 1;
		if (chance(2) && e_things(T_TARGET, (int16_t) (x - 25), (int16_t) (y - 25), (int16_t) (x + 25),
			(int16_t) (y + 25), &t, 1))
			push(i, K_ARCHERY, -1, -1);
		else if (e_things(T_DUMMY, (int16_t) (x - 25), (int16_t) (y - 25), (int16_t) (x + 25),
			(int16_t) (y + 25), &t, 1))
			push(i, K_FENCING, -1, -1);
		else
			d->wait = 30 * TICK_BLANKS;
		return 0;
	case 2:
		l->state = 1;
		return 0;
	}
	return 0;
}

/* Patrol (RunPatrolSchedule): from path marker to path marker (their frames number them, 0 to 31),
 * back and forth along them; each marker's quality says what to do there. */
static void patrol_next(struct Level *l)
{
	if (l->y) {
		if (l->x < 31)
			l->x++;
		else
			l->y = 0;
	} else {
		if (l->x > 0)
			l->x--;
		else
			l->y = 1;
	}
}

static int patrol(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;
	int r;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		if (planned || searched) {
			d->wait = 1;
			return 0;
		}
		searched = 1;
		l->state = nearest(T_PATH, npc[i].x, npc[i].y, 25, 0, 15, -1, &t) ? 2 : 40;
		return 0;
	case 2:
		l->x = 0;
		l->y = 0;
		l->counter = 0;
		l->state++;
		return 0;
	case 3:
		if (l->counter & 0x40) {
			/* act at this marker again */
			d->result = 0;
			l->state = 5;
			return 0;
		}
		if (planned || searched) {
			d->wait = 1;
			return 0;
		}
		{
			int16_t was_x = l->x, was_y = l->y;
			patrol_next(l);
			searched = 1;
			if (!nearest(T_PATH, npc[i].x, npc[i].y, 25, 0, 15, l->x, &t)) {
				l->y = !l->y;
				return 0;
			}
			r = start_path(i, t.x, t.y, t.z, 100);
			if (r == 1) {
				/* again next step, to the same marker */
				l->x = was_x;
				l->y = was_y;
				d->wait = 1;
				return 0;
			}
		}
		if (r == 2) {
			l->y = !l->y;
			return 0;
		}
		r = e_thing_quality(&t);
		l->counter = (uint8_t) (r < 0 ? 0 : r);
		l->state++;
		return 0;
	case 5:
		if (d->result < 0) {
			l->y = !l->y;
			l->state = 3;
			return 0;
		}
		l->state = 3;
		switch (l->counter & 0x1f) {
		case 1:
			l->x = 0;
			l->y = 0;
			break;
		case 2:
			l->state = 2;
			push(i, K_PAUSE, 15, -1);
			break;
		case 3:
			l->state = 2;
			push(i, K_PAUSE, range(45, 90), -1);
			cur(d)->state = 255;        /* the pause after the sitting: 255 + 1 wraps to 0 */
			push(i, K_SIT, -1, -1);
			break;
		case 4:
			l->state = 2;
			{
				static const uint8_t flowers[5] = { 1, 2, 3, 6, 7 };
				push(i, K_PAY_RESPECTS, T_TOMBSTONE, TF(999, flowers[rnd(5)]));
			}
			break;
		case 5:
			POST(i, P(F_STAND), WAIT(2), P(F_BEND), WAIT(2), P(F_KNEEL), WAIT(20), P(F_BEND), WAIT(2),
				P(F_STAND), WAIT(2));
			l->state = 3;
			break;
		case 6:
			l->state = 2;
			push(i, K_LOITER, -1, -1);
			cur(d)->state = 1;
			break;
		case 7:
		case 8: {
			int w = d->dir * 2;
			uint8_t s[] = { S_POSE, F_STAND, S_WAIT, 2, S_FACE,
				(uint8_t) ((w + ((l->counter & 0x1f) == 7 ? 6 : 2)) & 7), S_WAIT, 2, S_FACE,
				(uint8_t) ((w + 4) & 7), S_WAIT, 2 };
			post(i, s, sizeof s);
			l->state = 3;
			break;
		}
		case 9:
		case 10:
			l->state = 2;
			push(i, (l->counter & 0x1f) == 9 ? K_HOR_PACE : K_VER_PACE, -1, -1);
			cur(d)->state = 1;
			break;
		case 11:
			if (chance(2))
				l->y = !l->y;
			break;
		case 12:
			if (chance(2))
				patrol_next(l);
			break;
		case 13:
			l->state = 20;
			break;
		case 14:
			l->state = 2;
			push(i, K_CHECK_AREA, 1, -1);
			break;
		case 16:
			POST(i, P(F_STAND), WAIT(2), P(F_BEND), WAIT(2), P(F_STAND), WAIT(2));
			l->state = 3;
			break;
		case 17:
			POST(i, P(F_BEND), WAIT(2), P(F_STAND), WAIT(2));
			l->state = 3;
			break;
		case 18:
			l->state = 30;
			break;
		case 19:
			/* the patrollers waiting at a marker go on */
			for (int k = 0; k < nnpc; k++)
				if (doing[k].depth && cur(&doing[k])->kind == K_PATROL && cur(&doing[k])->state == 30)
					cur(&doing[k])->state++;
			break;
		case 22:
			POST(i, P(F_READY), P(F_RAISE1), P(F_EXTEND1), P(F_THRUST1), P(F_READY), P(F_STAND));
			l->state = 3;
			break;
		case 23:
			POST(i, P(F_READY), P(F_RAISE2), P(F_EXTEND2), P(F_THRUST2), P(F_READY), P(F_STAND));
			l->state = 3;
			break;
		case 24:
			l->state = 2;
			push(i, K_READ, -1, -1);
			break;
		case 25:
			if (chance(2)) {
				l->x = 0;
				l->y = 0;
			}
			break;
		}
		return 0;
	case 20:
		push(i, K_READY_HAND, 623, 1);
		return 0;
	case 21: {
		/* the hammer (its sound, 45) a few times */
		uint8_t s[] = { S_POSE, F_READY, S_WAIT, 2, S_POSE, F_RAISE1, S_WAIT, 2, S_POSE, F_OUT,
			S_SOUND, 45, S_WAIT, 2, S_POSE, F_READY, S_WAIT, 2, S_LOOP, (uint8_t) range(0, 2) };
		post(i, s, sizeof s);
		return 0;
	}
	case 22:
	case 31:
		l->state = 3;
		return 0;
	case 30:
		d->wait = 10 * TICK_BLANKS;     /* till another patroller's marker says go on */
		return 0;
	case 40:
		push(i, K_LOITER, 40, -1);
		cur(d)->state = 1;
		return 0;
	case 41:
		l->state = 40;
		return 0;
	}
	return 0;
}

/* ---- the kinds that carry and make things ---- */

/* A diner (eating at the inn, sitting) within 25 cells of waiter i with no serving spec ahead,
 * room ahead for one, and a free cell to one side to serve from (FindUnservedDiner): -1 for none. */
static int unserved_diner(int i, int spec)
{
	uint16_t type = SPEC_TYPE(spec), wtf = e_walk_frame(npc[i].tf & 0x3ff, 2, 0);
	struct EThing t;

	for (int k = 0; k < nnpc; k++) {
		struct Doing *e = &doing[k];
		if (k == i || k == avatar || e->depth == 0 || e->level[0].kind != K_EAT_INN || e->pose != F_SIT
			|| far_of(npc[i].x, npc[i].y, npc[k].x, npc[k].y) > 25)
			continue;
		int w = e->dir * 2, ax = npc[k].x + way_x[w], ay = npc[k].y + way_y[w];
		if (e_things(type, (int16_t) ax, (int16_t) ay, (int16_t) ax, (int16_t) ay, &t, 1)
			&& (SPEC_FRAME(spec) < 0 || (t.tf >> 10 & 31) == SPEC_FRAME(spec)))
			continue;
		if (!e_can_stand(type, (int16_t) ax, (int16_t) ay, (uint8_t) (npc[k].z + (type == T_PLATE ? 2 : 3))))
			continue;
		for (int side = 6; side <= 10; side += 4) {
			int sw = (w + side) & 7;
			if (e_can_stand(wtf, (int16_t) (npc[k].x + way_x[sw]), (int16_t) (npc[k].y + way_y[sw]), npc[i].z))
				return k;
		}
	}
	return -1;
}

/* A walk to the cell beside x, y to the left or right of one facing way w (8: any side), as the
 * original's WalkBesideItem: what start_path gives, 0 also when beside it already (with the state
 * moved past the walk), 2 when no cell is free. */
static int walk_beside(int i, int x, int y, int w)
{
	struct Doing *d = &doing[i];
	uint16_t tf = e_walk_frame(npc[i].tf & 0x3ff, 2, 0);
	int tx, ty, r;

	if (far_of(npc[i].x, npc[i].y, x, y) == 1 && (npc[i].x == x || npc[i].y == y)) {
		cur(d)->state++;        /* no walk: as if it had ended */
		d->result = 0;
		return 0;
	}
	if (w == 8) {
		int k;
		for (k = 2; k < 10; k += 2)
			if (e_can_stand(tf, (int16_t) (x + way_x[k & 7]), (int16_t) (y + way_y[k & 7]), npc[i].z))
				break;
		if (k == 10)
			return 2;
		tx = x + way_x[k & 7];
		ty = y + way_y[k & 7];
		w = (k + 4) & 7;
	} else if (e_can_stand(tf, (int16_t) (x + way_x[(w + 6) & 7]), (int16_t) (y + way_y[(w + 6) & 7]), npc[i].z)) {
		d->value = 0;
		tx = x + way_x[(w + 6) & 7];
		ty = y + way_y[(w + 6) & 7];
	} else if (e_can_stand(tf, (int16_t) (x + way_x[(w + 2) & 7]), (int16_t) (y + way_y[(w + 2) & 7]), npc[i].z)) {
		d->value = 1;
		tx = x + way_x[(w + 2) & 7];
		ty = y + way_y[(w + 2) & 7];
	} else
		return 2;
	r = start_path(i, tx, ty, 0, 100);
	if (r == 0)
		d->face8 = (uint8_t) w;
	return r;
}

/* A serving set on the table ahead of the diner beside the waiter (SetDownServing): a plate on the
 * table top, food on the plate. */
static void set_down_serving(int i, int spec)
{
	static const uint8_t plates[2] = { 4, 5 };
	static const uint8_t foods[17] = { 0, 2, 3, 6, 9, 11, 12, 15, 16, 17, 19, 24, 25, 27, 29, 30, 31 };
	uint16_t type = SPEC_TYPE(spec);
	int frame = type == T_PLATE ? plates[rnd(2)] : type == T_FOOD ? foods[rnd(17)] : SPEC_FRAME(spec);
	int w = (doing[i].dir * 2 + (doing[i].value ? 7 : 1)) & 7;
	int x = npc[i].x + way_x[w], y = npc[i].y + way_y[w], z = npc[i].z + (type == T_PLATE ? 0 : 1) + 2;
	struct EThing t;

	if (e_things(type, (int16_t) x, (int16_t) y, (int16_t) x, (int16_t) y, &t, 1))
		return;
	if (e_can_stand(type, (int16_t) x, (int16_t) y, (uint8_t) z))
		e_thing_add((uint16_t) (type | clamp_frame(type, frame < 0 ? 0 : frame) << 10), (int16_t) x,
			(int16_t) y, (uint8_t) z, NULL);
}

/* An item spec within 25 cells with no one beside it but the waiter and a side free to reach
 * (FindItemToClear): 1, into t. */
static int item_to_clear(int i, int spec, struct EThing *t)
{
	static struct EThing found[48];
	uint16_t tf = e_walk_frame(npc[i].tf & 0x3ff, 2, 0);
	int n = e_things(SPEC_TYPE(spec), (int16_t) (npc[i].x - 25), (int16_t) (npc[i].y - 25),
		(int16_t) (npc[i].x + 25), (int16_t) (npc[i].y + 25), found, 48);

	for (int k = 0; k < n; k++) {
		int busy = 0, w;
		if (SPEC_FRAME(spec) >= 0 && (found[k].tf >> 10 & 31) != SPEC_FRAME(spec))
			continue;
		for (w = 0; w < 8 && !busy; w += 2) {
			int x = found[k].x + way_x[w], y = found[k].y + way_y[w];
			busy = me_x == x && me_y == y;
			for (int s = 1; s < E_MOVERS && !busy; s++)
				busy = slot_npc[s] >= 0 && slot_npc[s] != i && npc[slot_npc[s]].x == x && npc[slot_npc[s]].y == y;
		}
		if (busy)
			continue;
		for (w = 0; w < 8; w += 2)
			if (e_can_stand(tf, (int16_t) (found[k].x + way_x[w]), (int16_t) (found[k].y + way_y[w]), npc[i].z)) {
				*t = found[k];
				return 1;
			}
	}
	return 0;
}

/* The item spec in the cell NPC i faces taken away (RemoveItemAhead). */
static void remove_ahead(int i, int spec)
{
	struct EThing t[4];
	int w = doing[i].dir * 2, x = npc[i].x + way_x[w], y = npc[i].y + way_y[w];
	int n = e_things(SPEC_TYPE(spec), (int16_t) x, (int16_t) y, (int16_t) x, (int16_t) y, t, 4);

	for (int k = 0; k < n; k++)
		if (SPEC_FRAME(spec) < 0 || (t[k].tf >> 10 & 31) == SPEC_FRAME(spec)) {
			e_thing_remove(&t[k]);
			return;
		}
}

/* Waiter (RunWaiterSchedule): plates and then food served to diners at the inn, tableware set
 * out, the cauldron stirred, plates and food cleared away. */
static int waiter(int i)
{
	static const uint16_t types[5] = { 944, 388, 616, 681, 628 };   /* pot, utensils, bottle, jar, cup */
	static const uint16_t tables[2] = { 1003, 1018 };
	static const uint16_t places[3] = { 1003, 1018, 872 };          /* two tables and a stove */
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;
	int k, r;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		if (chance(3)) {
			l->state = 0;
			push(i, K_CHECK_AREA, 1, -1);
		} else {
			l->state = chance(2) ? 10 : (uint8_t) ((rnd(3) + 2) * 10);
			l->x = 0;
		}
		return 0;
	case 10:
		if (planned || searched) {
			d->wait = 1;
			return 0;
		}
		searched = 1;
		if (l->x == 0)
			l->x = T_PLATE;
		k = unserved_diner(i, l->x);
		if (k >= 0) {
			r = walk_beside(i, npc[k].x, npc[k].y, doing[k].dir * 2);
			if (r == 0 && l->state == 10)
				l->state++;
			else if (r == 1)
				d->wait = 1;
		} else if (l->x == T_PLATE)
			l->x = T_FOOD;
		else
			l->state = 1;
		return 0;
	case 11:
		l->state++;
		return 0;
	case 12:
		if (d->result < 0)
			l->state = 1;
		else {
			face(i, d->face8);
			POST(i, WAIT(0));
			return show(i);
		}
		return 0;
	case 13:
		if (chance(3)) {
			bark(i, 27, 5);             /* "Can I help thee?", "What wilt thou have?" */
			POST(i, WAIT(15), P(F_READY), WAIT(2));
		}
		else
			POST(i, P(F_READY), WAIT(2));
		return 0;
	case 14:
		set_down_serving(i, l->x);
		POST(i, P(F_READY), WAIT(2), P(F_STAND), WAIT(2));
		l->state = 10;
		return 0;
	case 20:
		l->y = (int16_t) types[rnd(5)];
		push(i, K_MOVE_ITEM, l->y, l->y == 944 ? places[rnd(3)] : tables[rnd(2)]);
		return 0;
	case 21:
	case 32:
		l->state = 1;
		return 0;
	case 30:
		push(i, K_GOTO_ITEM, 995, -1);
		return 0;
	case 31:
		if (d->result < 0)
			l->state = 1;
		else {
			uint8_t s[4] = { S_FACE, d->face8, S_WAIT, (uint8_t) range(10, 20) };
			post(i, s, 4);
		}
		return 0;
	case 40:
		if (planned || searched) {
			d->wait = 1;
			return 0;
		}
		searched = 1;
		if (l->x == 0)
			l->x = T_FOOD;
		if (item_to_clear(i, l->x, &t) && (r = walk_beside(i, t.x, t.y, 8)) != 2) {
			if (r == 1) {
				d->wait = 1;
				return 0;
			}
			l->counter = t.z;
			if (l->state == 40)
				l->state++;
		} else if (l->x == T_FOOD)
			l->x = T_PLATE;
		else
			l->state = 1;
		return 0;
	case 41:
		l->state++;
		return 0;
	case 42:
		face(i, d->face8);
		if (l->counter >= 2)
			POST(i, P(F_STAND), WAIT(1), P(F_READY), WAIT(2));
		else
			POST(i, P(F_STAND), WAIT(2), P(F_BEND), WAIT(2));
		return show(i);
	case 43:
		/* a plate goes with the food on it */
		if (l->x == T_PLATE)
			remove_ahead(i, T_FOOD);
		remove_ahead(i, l->x);
		if (l->counter >= 2)
			POST(i, P(F_READY), WAIT(1), P(F_STAND), WAIT(2));
		else
			POST(i, P(F_BEND), WAIT(2), P(F_STAND), WAIT(2));
		return 0;
	case 44:
		l->state = 40;
		return 0;
	}
	return 0;
}

/* Farm (RunFarmSchedule): working the crops (a knife or scythe), each now and then cut. */
static int farm(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		push(i, K_READY_HAND, chance(2) ? 615 : 618, 1);
		return 0;
	case 2:
		d->value = 0;
		l->state++;
		return 0;
	case 3:
		push(i, K_GOTO_ITEM, TF(423, rnd(8) * 4 + rnd(3)), -1);
		return 0;
	case 4:
		if (d->result < 0) {
			if (chance(8))
				bark(i, 96, 3);         /* "Whew!  'Tis hot!", "We need rain..." */
			l->state = 3;
		} else if (chance(2))
			POST(i, P(F_READY), WAIT(1), P(F_RAISE1), WAIT(1), P(F_EXTEND1), WAIT(1), P(F_THRUST1), WAIT(2),
				P(F_READY));
		else
			POST(i, P(F_READY), WAIT(1), P(F_RAISE2), WAIT(1), P(F_EXTEND2), WAIT(1), P(F_THRUST2), WAIT(2),
				P(F_READY));
		return 0;
	case 5:
		if (chance(4)) {
			/* crops come in fours of frames: these to the last of theirs */
			int w = d->dir * 2, x = npc[i].x + way_x[w], y = npc[i].y + way_y[w];
			if (e_things(423, (int16_t) x, (int16_t) y, (int16_t) x, (int16_t) y, &t, 1))
				e_thing_set(&t, (uint16_t) (t.tf | 3u << 10));
			l->state = 2;
		} else {
			if (d->value >= 5) {
				bark(i, 63, 3);         /* "Blast!", "Ouch - cut myself!" */
				d->value = 0;
			} else
				d->value++;
			l->state = 4;
		}
		return 0;
	}
	return 0;
}

/* Mine (RunMinerSchedule): a pick at lead or iron ore; the rock now and then cracks a frame
 * further, or an uncracked one gives way to a gold nugget or a gem. */
static int miner(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		push(i, K_READY_HAND, 624, 1);
		return 0;
	case 2:
		l->x = 0;
		l->state++;
		return 0;
	case 3:
		l->y = chance(2) ? 915 : 916;
		push(i, K_GOTO_ITEM, TF(l->y, 0), -1);
		return 0;
	case 4:
		if (d->result < 0) {
			if (chance(5)) {
				set_kind(i, K_LOITER);
				cur(d)->state = 1;
			} else
				l->state = 3;
		} else if (chance(2))
			POST(i, P(F_READY), WAIT(1), P(F_RAISE1), WAIT(1), P(F_EXTEND1), WAIT(1), P(F_THRUST1), SOUND(4),
				WAIT(1), P(F_READY));
		else
			POST(i, P(F_READY), WAIT(1), P(F_RAISE2), WAIT(1), P(F_EXTEND2), WAIT(1), P(F_THRUST2), SOUND(4),
				WAIT(1), P(F_READY));
		return 0;
	case 5:
		l->state = 4;
		if (chance(4)) {
			int w = d->dir * 2, x = npc[i].x + way_x[w] * 3, y = npc[i].y + way_y[w] * 3;
			if (!e_things((uint16_t) l->y, (int16_t) x, (int16_t) y, (int16_t) x, (int16_t) y, &t, 1)) {
				l->state = 2;
				return 0;
			}
			int frame = t.tf >> 10 & 31;
			if (frame == 0 && chance(10)) {
				uint16_t found = chance(2) ? 645 : 760;
				if (e_thing_remove(&t))
					e_thing_add(found, t.x, t.y, t.z, NULL);
				if (chance(2))
					bark(i, 69, 3);     /* "Eureka!", "I am rich!" */
				l->state = 2;
				return 0;
			}
			frame = clamp_frame((uint16_t) l->y, frame + 1);
			e_thing_set(&t, (uint16_t) ((t.tf & ~(31u << 10)) | (uint16_t) frame << 10));
			if (frame == 3) {
				l->state = 2;
				return 0;
			}
			l->x = 0;
		} else if (l->x >= 5) {
			bark(i, 66, 3);             /* "Still no gold!", "Whew!" */
			l->x = 0;
		} else
			l->x++;
		return 0;
	}
	return 0;
}

/* Blacksmith (RunBlacksmithSchedule): a sword blank heated in the firepit (the bellows worked),
 * hammered on the anvil, quenched in the trough, the tools put back. */
static int smith(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t, pit;
	int found, k;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		if (chance(3)) {
			l->state = 0;
			push(i, K_CHECK_AREA, 1, -1);
		} else
			push(i, K_MOVE_ITEM, TF(668, 0), 739);
		return 0;
	case 2:
		push(i, K_GOTO_ITEM, 431, -1);
		return 0;
	case 3:
		if (d->result < 0) {
			l->state = 1;
			return 0;
		}
		/* the bellows worked while the firepit and the blank heat up */
		POST(i, P(F_STAND), WAIT(2), P(F_UP), WAIT(2), P(F_BEND), WAIT(2), P(F_UP), WAIT(2), P(F_STAND),
			WAIT(2), S_LOOP, 7);
		found = nearest(431, npc[i].x, npc[i].y, 5, 0, 15, -1, &t);
		THING(&t, found, I_FRAME, 0, I_WAIT, 2, I_FRAME, 1, I_WAIT, 2, I_SOUND, 47, I_FRAME, 2, I_WAIT, 2,
			I_FRAME, 1, I_WAIT, 2, I_FRAME, 0, I_WAIT, 2, I_LOOP, 7);
		found = nearest(739, npc[i].x, npc[i].y, 10, 0, 15, -1, &pit);
		THING(&pit, found, I_WAIT, 34, I_NEXT_MAX, 0, I_WAIT, 29, I_NEXT_MAX, 0, I_WAIT, 29, I_NEXT_MAX, 0,
			I_WAIT, 45, I_PREV_MIN, 0, I_WAIT, 60, I_PREV_MIN, 0, I_WAIT, 105, I_PREV_MIN, 0);
		found = found ? nearest(668, pit.x, pit.y, 10, 0, 15, -1, &t)
			: nearest(668, npc[i].x, npc[i].y, 10, 0, 15, -1, &t);
		THING(&t, found, I_WAIT, 34, I_NEXT_MAX, 0, I_MARK, 0, I_WAIT, 14, I_NEXT_MAX, 0, I_LOOP, 3);
		return show(i);
	case 4:
		push(i, K_READY_HAND, 994, 1);
		return 0;
	case 5:
		push(i, K_MOVE_ITEM, 668, 991);
		return 0;
	case 6:
		if (d->result < 0)
			l->state = 1;
		else
			push(i, K_READY_HAND, 623, 1);
		return 0;
	case 7:
		push(i, K_GOTO_ITEM, 991, -1);
		return 0;
	case 8:
		POST(i, P(F_READY), WAIT(2), P(F_RAISE1), WAIT(2), P(F_OUT), SOUND(45), WAIT(2), P(F_READY), WAIT(2),
			S_LOOP, 2);
		found = nearest(668, npc[i].x, npc[i].y, 10, 0, 15, -1, &t);
		THING(&t, found, I_WAIT, 8, I_PREV_MIN, 0, I_LOOP, 2);
		return 0;
	case 9:
		if (nearest(719, npc[i].x, npc[i].y, 25, 0, 15, -1, &t) && (t.tf >> 10 & 31) == 0)
			push(i, K_FILL_TROUGH, -1, -1);
		else
			l->state++;
		return 0;
	case 10:
		push(i, K_GRAB_ITEM, 668, -1);
		return 0;
	case 11:
		if (d->result < 0)
			l->state = 1;
		else
			push(i, K_GOTO_ITEM, 719, -1);
		return 0;
	case 12:
		if (d->result < 0) {
			l->state = 1;
			return 0;
		}
		POST(i, P(F_STAND), WAIT(2), P(F_UP), WAIT(2), P(F_BEND), WAIT(2), P(F_UP), WAIT(2), P(F_STAND),
			WAIT(2), S_LOOP, 2);
		found = nearest(719, npc[i].x, npc[i].y, 5, 0, 15, -1, &t);
		THING(&t, found, I_WAIT, 7, I_SOUND, 46, I_WAIT, 15, I_SOUND, 46, I_WAIT, 15, I_SOUND, 46,
			I_PREV_MIN, 0);
		return 0;
	case 13:
		k = held_find(i, 668);
		if (k >= 0)
			d->held[k] = 668;   /* cooled: frame 0 */
		l->state++;
		return 0;
	case 14:
		push(i, K_MOVE_ITEM, 623, 1003);
		return 0;
	case 15:
		push(i, K_MOVE_ITEM, 994, 1003);
		return 0;
	case 16:
		l->state = 1;
		return 0;
	}
	return 0;
}

/* Bake (RunBakeSchedule): kitchen things set out, a wait at the hearth, the bread there taken to a
 * table, dough made from flour and kneaded and put in the hearth, the dough in it baked. */
static int bake(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t, hearth;
	int k;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		if (!chance(4))
			l->state = (uint8_t) ((rnd(6) + 1) * 10);
		return 0;
	case 10:
		push(i, K_MOVE_ITEM, TF(863, rnd(e_shape_frames(863) > 0 ? e_shape_frames(863) : 1)),
			chance(2) ? 1003 : 1018);
		return 0;
	case 11:
	case 22:
	case 61:
		l->state = 1;
		return 0;
	case 20:
	case 31:
		push(i, K_GOTO_ITEM, 831, -1);
		return 0;
	case 21: {
		uint8_t s[2] = { S_WAIT, (uint8_t) range(12, 20) };
		post(i, s, 2);
		return 0;
	}
	case 30:
	case 50:
		if (!nearest(831, npc[i].x, npc[i].y, 25, 0, 15, -1, &hearth)) {
			set_kind(i, K_LOITER);
			cur(d)->state = 1;
			return 0;
		}
		k = e_things(l->state == 30 ? T_FOOD : 658, (int16_t) (hearth.x - 1), hearth.y, (int16_t) (hearth.x - 1),
			hearth.y, &t, 1);
		if (l->state == 30) {
			l->state = k ? 31 : 1;
			return 0;
		}
		if (k && e_thing_remove(&t)) {
			static const uint8_t baked[6] = { 0, 2, 3, 4, 5, 6 };
			e_thing_add((uint16_t) (T_FOOD | baked[rnd(6)] << 10), t.x, t.y, t.z, NULL);
		}
		l->state = 1;
		return 0;
	case 32:
		if (d->result < 0)
			l->state = 1;
		else
			push(i, K_MOVE_ITEM, T_FOOD, 633);
		return 0;
	case 33:
		if (d->result < 0)
			while ((k = held_find(i, T_FOOD)) >= 0)
				d->held[k] = 0;
		l->state = 1;
		return 0;
	case 40:
		push(i, K_GOTO_ITEM, TF(863, 13), -1);
		return 0;
	case 41:
		POST(i, P(F_STAND), WAIT(2), P(F_BEND), WAIT(2));
		return 0;
	case 42:
		POST(i, P(F_BEND), WAIT(2), P(F_STAND), WAIT(2));
		held_add(i, 863);
		return 0;
	case 43:
		push(i, K_DROP_ITEM, 863, chance(2) ? 1003 : 1018);
		return 0;
	case 44:
		push(i, K_GRAB_ITEM, TF(863, 0), -1);
		return 0;
	case 45:
		if ((k = held_find(i, TF(863, 0))) >= 0)
			d->held[k] = 0;
		held_add(i, 658);
		l->state++;
		return 0;
	case 46:
		push(i, K_DROP_ITEM, TF(658, 0), chance(2) ? 1003 : 1018);
		return 0;
	case 47: {
		int w = d->dir * 2, x = npc[i].x + way_x[w], y = npc[i].y + way_y[w];
		POST(i, P(F_STAND), WAIT(8), P(F_READY), WAIT(5), P(F_STAND), WAIT(4), P(F_READY), WAIT(5),
			P(F_STAND), WAIT(3));
		k = e_things(658, (int16_t) x, (int16_t) y, (int16_t) x, (int16_t) y, &t, 1);
		THING(&t, k, I_WAIT, 10, I_NEXT, 0, I_WAIT, 10, I_NEXT, 0);
		return 0;
	}
	case 48:
		push(i, K_MOVE_ITEM, TF(658, 2), 831);
		return 0;
	case 49:
		if (d->result < 0 && (k = held_find(i, 658)) >= 0)
			d->held[k] = 0;
		l->state = 1;
		return 0;
	case 60:
		push(i, K_CHECK_AREA, 1, -1);
		return 0;
	}
	return 0;
}

/* Sew (RunSewSchedule): wool spun into thread at the wheel, the thread woven into cloth at the
 * loom, the cloth cut into clothes with shears. */
static int sew(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;
	int k, found;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		if (chance(3)) {
			l->state = 0;
			push(i, K_CHECK_AREA, 1, -1);
		} else
			push(i, K_GRAB_ITEM, 653, -1);
		return 0;
	case 2:
		push(i, K_DROP_ITEM, 653, -1);
		return 0;
	case 3:
		push(i, K_SIT, 651, -1);
		return 0;
	case 4:
	case 10:
	case 14:
	case 16:
		if (d->result < 0) {
			set_kind(i, K_LOITER);
			cur(d)->state = 1;
			return 0;
		}
		if (l->state == 4)
			l->state++;
		else if (l->state == 10) {
			found = nearest(261, npc[i].x, npc[i].y, 5, 0, 15, -1, &t);
			THING(&t, found, I_SOUND, 102, I_NEXT, 0, I_NEXT, 0, I_NEXT, 0, I_NEXT, 0, I_NEXT, 0, I_NEXT, 0,
				I_NEXT, 0, I_NEXT, 0, I_LOOP, 6);
			POST(i, WAIT(52));
		} else if (l->state == 14)
			push(i, K_READY_HAND, 698, 1);
		else
			POST(i, P(F_RAISE1), P(F_RAISE1), P(F_EXTEND1), P(F_EXTEND1), P(F_THRUST1), P(F_THRUST1),
				P(F_RAISE1), P(F_RAISE1), P(F_EXTEND1), P(F_EXTEND1), P(F_THRUST1), P(F_THRUST1));
		return 0;
	case 5:
		POST(i, WAIT(40));
		found = nearest(651, npc[i].x, npc[i].y, 5, 0, 15, -1, &t);
		THING(&t, found, I_SOUND, 97, I_NEXT, 0, I_LOOP, 39);
		return 0;
	case 6:
		held_add(i, 654);
		l->state++;
		return 0;
	case 7:
		push(i, K_DROP_ITEM, 654, -1);
		return 0;
	case 8:
		if (d->result < 0)
			l->state++;
		else
			push(i, K_GRAB_ITEM, 654, -1);
		return 0;
	case 9:
		push(i, K_GOTO_ITEM, 261, -1);
		return 0;
	case 11:
		held_add(i, (uint16_t) (851 | rnd(5) << 10));
		if ((k = held_find(i, 654)) >= 0)
			d->held[k] = 0;
		l->state++;
		return 0;
	case 12:
		push(i, K_DROP_ITEM, 851, -1);
		return 0;
	case 13:
		push(i, K_MOVE_ITEM, 851, 971);
		return 0;
	case 15:
		push(i, K_GOTO_ITEM, 851, -1);
		return 0;
	case 17: {
		int w = d->dir * 2, x = npc[i].x + way_x[w], y = npc[i].y + way_y[w];
		if (e_things(851, (int16_t) x, (int16_t) y, (int16_t) x, (int16_t) y, &t, 1) && e_thing_remove(&t)) {
			e_thing_add(738, t.x, t.y, t.z, NULL);
			held_add(i, (uint16_t) (851 | (rnd(5) + 5) << 10));
			l->state++;
		} else {
			set_kind(i, K_LOITER);
			cur(d)->state = 1;
		}
		return 0;
	}
	case 18:
		push(i, K_DROP_ITEM, 851, -1);
		return 0;
	case 19:
		push(i, K_GRAB_ITEM, 738, -1);
		return 0;
	case 20:
		push(i, K_MOVE_ITEM, 738, chance(2) ? 971 : 890);
		return 0;
	case 21:
		if (d->result < 0)
			while ((k = held_find(i, 738)) >= 0)
				d->held[k] = 0;
		push(i, K_GRAB_ITEM, 851, -1);
		return 0;
	case 22:
		if ((k = held_find(i, 851)) >= 0)
			d->held[k] = 0;
		l->state++;
		return 0;
	case 23:
		push(i, K_DROP_ITEM, 698, 890);
		return 0;
	case 24:
		l->state = 1;
		return 0;
	}
	return 0;
}

/* Lab work (RunLabSchedule): reading, then a potion taken to the cauldron, stirred, and put on a
 * table. */
static int lab(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t;
	int found;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		push(i, K_READ, -1, -1);
		return 0;
	case 2:
		if (chance(3))
			push(i, K_CHECK_AREA, 1, -1);
		else
			l->state++;
		return 0;
	case 3:
		push(i, K_GRAB_ITEM, TF(340, rnd(8)), 1);
		return 0;
	case 4:
		if (d->result < 0)
			l->state = 1;
		else
			push(i, K_GOTO_ITEM, 995, -1);
		return 0;
	case 5:
		if (d->result < 0) {
			l->state = 1;
			return 0;
		}
		POST(i, P(F_STAND), WAIT(2), P(F_BEND), WAIT(2));
		if (chance(2)) {
			found = nearest(995, npc[i].x, npc[i].y, 1, 0, 15, -1, &t);
			uint8_t s[] = { I_WAIT, 6, I_FRAME, (uint8_t) range(1, 4), I_WAIT, (uint8_t) range(40, 130),
				I_FRAME, 0 };
			post_thing(&t, s, sizeof s, found);
		}
		return 0;
	case 6:
		POST(i, P(F_BEND), WAIT(2), P(F_STAND), WAIT(2));
		return 0;
	case 7:
		push(i, K_DROP_ITEM, 340, rnd(2) ? 1018 : 1003);
		return 0;
	case 8:
		l->state = chance(4) ? 1 : 2;
		return 0;
	}
	return 0;
}

/* Thief (RunThiefSchedule): follows the avatar about, and now and then sidles up to it. (Picking
 * its pack, and the greeting, are not done.) */
static int thief(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		if (far_of(npc[i].x, npc[i].y, me_x, me_y) > 8) {
			if (!path_next_to(i, avatar, 100))
				l->state = 3;
		} else
			l->state = chance(5) ? 2 : 3;
		return 0;
	case 2:
		if (!path_next_to(i, avatar, 100))
			l->state = 1;
		else if (far_of(npc[i].x, npc[i].y, me_x, me_y) <= 2) {
			bark(i, 16, 4);             /* "Nice weather today.", "Good day." */
			POST(i, WAIT(15));  /* (the pack picked: not yet) */
		}
		return 0;
	case 3:
		d->value = 0;
		l->state++;
		return 0;
	case 4: {
		int r = start_path(i, me_x + range(-8, 8), me_y + range(-8, 8), npc[i].z, 100);
		if (r == 0)
			l->state++;
		else if (r == 2 && d->value++ > 3)
			l->state = 1;
		return 0;
	}
	case 6: {
		uint8_t s[2] = { S_WAIT, (uint8_t) range(5, 10) };
		post(i, s, 2);
		l->state = 1;
		return 0;
	}
	}
	return 0;
}

/* Graze (RunGrazeSchedule): wanders within 7 cells of where it began, standing, raising and
 * lowering its head between. */
static int graze(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	int r;

	switch (l->state) {
	case 0:
		push(i, K_GOTO_PLACE, -1, -1);
		return 0;
	case 1:
		d->mark_x = npc[i].x;
		d->mark_y = npc[i].y;
		d->value = 0;
		l->state++;
		return 0;
	case 2:
		if (d->value > 2)
			l->state = 4;
		else if ((r = start_path(i, d->mark_x + range(-7, 7), d->mark_y + range(-7, 7), npc[i].z, 25)) == 0)
			l->state++;
		else if (r == 2)
			d->value++;
		return 0;
	case 4: {
		d->value = 0;
		uint8_t s[] = { S_POSE, F_STAND, S_WAIT, (uint8_t) range(2, 5), S_MARK, 0, S_POSE, F_UP, S_WAIT,
			(uint8_t) range(2, 5), S_POSE, F_OUT, S_WAIT, (uint8_t) range(2, 5), S_LOOP, (uint8_t) range(2, 5) };
		post(i, s, sizeof s);
		return 0;
	}
	case 5:
		l->state = 2;
		return 0;
	}
	return 0;
}

/* ---- running them ---- */

int e_world_trace = -1;

static void trace(int i, const char *what)
{
	struct Doing *d = &doing[i];
	static uint8_t last_kind, last_state, last_depth;
	char text[160];
	int n = 0;

	if (npc[i].number != e_world_trace || (d->depth == last_depth && cur(d)->kind == last_kind
		&& cur(d)->state == last_state && what == NULL))
		return;
	last_kind = cur(d)->kind;
	last_state = cur(d)->state;
	last_depth = d->depth;
	n = snprintf(text, sizeof text, "npc %d at %d,%d,%d:", npc[i].number, npc[i].x, npc[i].y, npc[i].z);
	for (int k = 0; k < d->depth && n < 120; k++)
		n += snprintf(text + n, sizeof text - (size_t) n, " %d/%d", d->level[k].kind, d->level[k].state);
	snprintf(text + n, sizeof text - (size_t) n, " result %d%s%s\n", d->result, what ? " " : "",
		what ? what : "");
	eb_log(text);
}

int kind_tick(int i)
{
	struct Doing *d = &doing[i];

	if (d->depth)
		trace(i, NULL);
	if (d->pc < d->script_len)
		return run_script(i);
	d->script_len = 0;
	d->wait = TICK_BLANKS;
	if (d->depth == 0)
		set_kind(i, K_STAND);
	switch (cur(d)->kind) {
	case K_GOTO_PLACE: return goto_place(i);
	case K_GOTO_ITEM: return goto_item(i);
	case K_SIT: return sit(i);
	case K_PAUSE: return pause_kind(i);
	case K_READ: return read_kind(i);
	case K_PAY_RESPECTS: return pay_respects(i);
	case K_TAG: return tag(i);
	case K_FENCING: return fencing(i);
	case K_ARCHERY: return archery(i);
	case K_GRAB_ITEM: return grab_item(i);
	case K_DROP_ITEM: return drop_item(i);
	case K_MOVE_ITEM: return move_item(i);
	case K_FILL_BUCKET: return fill_bucket(i);
	case K_FILL_TROUGH: return fill_trough(i);
	/* not yet: these say they are done (READY_HAND: no weapon in hand, drawn; GOTO_REM_L: where
	 * it was is where it is) */
	case K_READY_HAND:
	case K_GOTO_REM_L:
		pop(i, 0);
		return 0;
	case K_CHECK_AREA: return check_area(i);
	case K_CHECK_LIGHT: return check_lights(i, 0);
	case K_CHECK_SHUTTERS: return check_lights(i, 1);
	case K_STAND: return stand_kind(i);
	case K_LOITER: return loiter(i);
	case K_WANDER: return wander(i);
	case K_SHOP: return shop(i);
	case K_SLEEP: return sleep_kind(i);
	case K_MAJOR_SIT: return major_sit(i);
	case K_EAT_INN: return eat_inn(i);
	case K_EAT: return eat(i);
	case K_DESK: return desk(i);
	case K_HOR_PACE:
	case K_VER_PACE: return pace(i);
	case K_HOUND: return hound(i);
	case K_TALK: return talk(i);
	case K_SHY: return shy(i);
	case K_DANCE: return dance(i);
	case K_PREACH: return preach(i);
	case K_KIDS: return kids(i);
	case K_DUEL: return duel(i);
	case K_PATROL: return patrol(i);
	case K_WAITER: return waiter(i);
	case K_FARM: return farm(i);
	case K_MINER: return miner(i);
	case K_SMITH: return smith(i);
	case K_BAKE: return bake(i);
	case K_SEW: return sew(i);
	case K_LAB: return lab(i);
	case K_THIEF: return thief(i);
	case K_GRAZE: return graze(i);
	default:
		/* the kinds not done yet loiter */
		set_kind(i, K_LOITER);
		cur(d)->state = 1;
		return 0;
	}
}

void kind_walked(int i, int ok)
{
	struct Doing *d = &doing[i];

	d->result = (int16_t) (ok ? 0 : -1);
	trace(i, ok ? "walked" : "walk failed");
	if (cur(d)->kind == K_GOTO_PLACE)
		pop(i, d->result);
	else
		cur(d)->state++;
	d->wait = 1;
}

void kind_lost(int i)
{
	set_kind(i, K_WANDER);
	cur(&doing[i])->state = 1;
}

/* Out of bed (its cover back, back where it got in) or up from a chair. */
static void leave(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];

	if (d->cover.tf != 0) {
		e_thing_set(&d->cover, (uint16_t) (d->cover.tf - (1u << 10)));
		d->cover.tf = 0;
	}
	if (d->pose == F_LIE && (p->x != d->spot_x || p->y != d->spot_y || p->z != d->spot_z)
		&& (!in_near_window(d->spot_x, d->spot_y)
			|| e_can_stand(e_walk_frame(p->tf & 0x3ff, 2, 0), d->spot_x, d->spot_y, d->spot_z))) {
		p->x = d->spot_x;
		p->y = d->spot_y;
		p->z = d->spot_z;
	}
	d->script_len = 0;
	if (d->pose != F_STAND) {
		d->pose = F_STAND;
		show(i);
		int s = in_slot(i);
		if (s)
			e_mover_set(s, p->tf, p->x, p->y, p->z);
	}
}

void kind_period(int i, int kind)
{
	leave(i);
	set_kind(i, kind);
}

void kind_init(int i)
{
	struct Doing *d = &doing[i];
	int kind = npc[i].activity, pose = npc[i].tf >> 10 & 15;

	if (npc[i].number == 0)
		avatar = i;
	set_kind(i, kind);
	d->pose = (uint8_t) pose;
	d->spot_x = npc[i].x;
	d->spot_y = npc[i].y;
	d->spot_z = npc[i].z;
	/* in bed or sitting already, as the game was left */
	if (kind == K_SLEEP && pose == F_LIE)
		cur(d)->state = 7;
	else if (pose == F_SIT && (kind == K_MAJOR_SIT || kind == K_EAT_INN || kind == K_EAT
		|| kind == K_DESK))
		cur(d)->state = kind == K_MAJOR_SIT ? 3 : kind == K_EAT_INN ? 2 : kind == K_EAT ? 5 : 11;
	else
		d->pose = F_STAND;
}
