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
 * Not yet: barks (no text), usecode, held items and missiles (no weapons drawn, so archery and
 * fencing are the poses and sounds alone, and the arrows hit), items carried, made or put down
 * (food on a plate, a desk's items, a plant on a grave), looking round the area (CHECK_AREA
 * finds nothing), combat, waking to chase the avatar; and the kinds that carry and make things
 * (waiter, farmer, miner, smith, baker, seamstress, alchemist, thief, grazing).
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
#define S_LOOP 5                /* from the start again, n times */
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

	d->result = (int8_t) (result < 0 ? -1 : 0);
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
				d->pc = 0;
			}
			break;
		}
	}
	d->wait = 1;
	return 0;
}

/* Where NPC k is (the avatar's from the world step). */
static int avatar = -1;

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

/* Where the original stands to use a type (Operate::findUseSpot's table, for the types the
 * schedules here use): from the item's hot spot, and the way (0-7) to face. */
static const struct UseSpot {
	uint16_t type;
	int8_t n, spot[4][3];
} use_spots[] = {
	{ T_BED, 3, { { -1, 1, 0 }, { 1, -1, 6 }, { -3, -1, 2 } } },
	{ T_BED_2, 3, { { 1, -1, 6 }, { -1, -3, 4 }, { -1, 1, 0 } } },
	{ T_FUTON, 3, { { -1, 3, 0 }, { 3, -1, 6 }, { -3, 0, 2 } } },
	{ T_FUTON_2, 3, { { 3, 0, 6 }, { 0, 3, 4 }, { 0, -3, 0 } } },
	{ T_DUMMY, 1, { { -2, 3, 0 } } },
	{ T_TARGET, 1, { { 3, -1, 6 } } },
	{ T_ICON, 2, { { 4, 0, 6 }, { 3, 0, 6 } } },
	{ T_TOMBSTONE, 1, { { 0, -3, 0 } } },
};

/* A spot NPC i could stand on to use t from: 1, with it and the way to face. */
static int use_spot(int i, const struct EThing *t, int *x, int *y, int *z, int *way)
{
	uint16_t type = t->tf & 0x3ff, tf = e_walk_frame(npc[i].tf & 0x3ff, 2, 0);
	int frame = t->tf >> 10 & 31;

	if (type == T_CHAIR) {
		*way = (frame & 3) * 2;
		*x = t->x + way_x[*way];
		*y = t->y + way_y[*way];
		*z = t->z;
		return in_near_window(*x, *y) && e_can_stand(tf, (int16_t) *x, (int16_t) *y, t->z);
	}
	if (type == T_PODIUM) {
		if (frame != 0 && frame != 2)
			return 0;
		*x = t->x - 1;
		*y = t->y - (frame == 2);
		*z = t->z;
		*way = frame ? 4 : 2;
		return in_near_window(*x, *y) && e_can_stand(tf, (int16_t) *x, (int16_t) *y, t->z);
	}
	for (unsigned k = 0; k < sizeof use_spots / sizeof use_spots[0]; k++) {
		if (use_spots[k].type != type)
			continue;
		for (int j = 0; j < use_spots[k].n; j++) {
			*x = t->x + use_spots[k].spot[j][0];
			*y = t->y + use_spots[k].spot[j][1];
			*z = t->z;
			*way = use_spots[k].spot[j][2];
			if (in_near_window(*x, *y) && e_can_stand(tf, (int16_t) *x, (int16_t) *y, t->z))
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

/* Walk to the use spot of the nearest item of type x (near one of type y, unless -1) within 25
 * cells, and face it (WalkToItemSchedule). */
static int goto_item(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);
	struct EThing t, a;
	int x, y, z, way, r;

	switch (l->state) {
	case 0:
		if (planned || searched) {
			d->wait = 1;
			return 0;
		}
		searched = 1;
		if (l->y < 0 ? !nearest((uint16_t) l->x, npc[i].x, npc[i].y, 25, 0, 15, -1, &t)
			: !nearest((uint16_t) l->y, npc[i].x, npc[i].y, 25, 0, 15, -1, &a)
				|| !nearest((uint16_t) l->x, a.x, a.y, 25, 0, 15, -1, &t)) {
			pop(i, -1);
			return 0;
		}
		if (!use_spot(i, &t, &x, &y, &z, &way) || (r = start_path(i, x, y, z, 100)) == 2) {
			pop(i, -1);
			return 0;
		}
		if (r == 1) {
			d->wait = 1;
			return 0;
		}
		d->face8 = (uint8_t) way;
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
		pop(i, 0);
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
				if (!use_spot(i, &t, &x, &y, &z, &way))
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

/* Pay respects (DoWorkPayRespects): walk to an item of type x, kneel a while, get up. (What the
 * original puts down there, a plant on a grave, is not.) */
static int pay_respects(int i)
{
	struct Doing *d = &doing[i];
	struct Level *l = cur(d);

	switch (l->state) {
	case 0:
		l->state = 1;
		return 0;
	case 1:
		push(i, K_GOTO_ITEM, l->x, -1);
		return 0;
	case 2:
		l->state = 3;
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
		l->state = far_of(npc[i].x, npc[i].y, x, y) <= 2 ? 4 : 1;
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
	if (!use_spot(i, &a, &x, &y, &z, &way))
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
		/* asleep (waking to chase the avatar: not yet) */
		d->wait = 600 * TICK_BLANKS;
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
		if (chance(2))
			eat_ahead(i, 2);
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
	case 20:
		push(i, K_MOVE_ITEM, 675, T_DESK);
		return 0;
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
	case 13:
		POST(i, WAIT(20));
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
			push(i, K_PAY_RESPECTS, T_TOMBSTONE, -1);
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
	/* not yet: these say they are done (READY_HAND: no held items; MOVE_ITEM: no items made;
	 * GOTO_REM_L: where it was is where it is; CHECK_AREA: nothing found) */
	case K_READY_HAND:
	case K_MOVE_ITEM:
	case K_GOTO_REM_L:
		pop(i, 0);
		return 0;
	case K_CHECK_AREA:
		pop(i, -1);
		return 0;
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

	d->result = (int8_t) (ok ? 0 : -1);
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
