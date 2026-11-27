/* The engine test's world (engine.h): a clock, and the NPCs on their schedules.
 *
 * SCHEDULE.DAT gives each NPC (by its number) up to eight entries, one for each three hours of
 * the day it starts something new: the kind of thing (a schedule type) and the place. As the
 * original, an NPC goes on with what it is doing until the next of its entries comes round, then
 * takes up that one's. Here that is only where it goes and how it moves there: those near the
 * avatar (in the mover slots) walk to the place on a route planned as the original plans one
 * (route.c), up and down stairs; those far off are put there. Walking a route is the original's
 * too: a step at the next waypoint (straight, else along one axis), planned again after a few
 * steps that could not be made, given up after half as many steps again as it is long. A shut
 * door in an NPC's way it opens, and shuts again once 2 cells further from it than it came. A
 * place beyond the chunks held near the view is walked at straight, sliding along walls. At the
 * place, the roaming kinds (loiter, wander, pace, patrol, graze, ...) walk to places nearby in
 * turn, and the rest stand. What the kinds do there (sleep in a bed, work a forge, sell, eat) is
 * the game's, still to come.
 */
#include <string.h>

#include "engine.h"

#define MOST_NPCS 400
#define MOST_ENTRIES 1024
#define STEP_BLANKS 10          /* an NPC walks a cell every 10 blanks: 6 a second */
#define ROAM 4                  /* the roaming kinds keep within 4 cells of their place */
#define ROUTES 15               /* routes walked at once (as the original's slots) */
#define ROUTE_PLACE 150         /* steps tried planning one to a schedule's place (the original's) */
#define ROUTE_ROAM 45           /* ... to a place near it, roaming (the original's for wandering) */

static struct ENpc *npc;
static int nnpc;

/* SCHEDULE.DAT: a count, the first entry of each NPC (and the end), the entries: the period (low
 * 3 bits) and kind (high 5), x, y in a region, the region */
static uint16_t sched_count, sched_first[257];
static uint8_t sched[MOST_ENTRIES][4];

/* each NPC: its kind of doing, where it is going, which way it faces, its steps, and blanks to
 * wait before it moves again */
static struct Doing {
	uint8_t kind, dir;
	uint16_t steps;
	int16_t place_x, place_y;   /* its schedule's place (roaming round it) */
	int16_t goal_x, goal_y;     /* where it is walking now */
	int16_t wait;
	int8_t door;                /* a door it opened and is to shut behind it, or -1 */
	uint8_t nearest;            /* the nearest it has been to that door */
	int8_t route;               /* the route it walks, or -1 */
	uint8_t no_route;           /* none was found to its goal: walked at straight */
} doing[MOST_NPCS];

/* the routes being walked: whose, the waypoints, the next of them, steps not made in a row, steps
 * taken and its length */
static struct Route {
	int16_t npc;
	int16_t goal_x, goal_y;
	uint8_t n, next, failures;
	uint16_t steps, length;
	struct EWay way[E_ROUTE_WAYS];
} routes[ROUTES];

static int in_near_window(int x, int y);
static int16_t me_x, me_y;          /* the avatar */
static int planned;              /* a route planned this step: the rest wait for the next */
static int16_t slot_npc[E_MOVERS];   /* the NPC in each mover slot (1 on), or -1 */
static int16_t picked_x, picked_y;
static uint32_t start_blanks, last_blanks, last_pick;
static int start_minutes, minutes, period = -1;

static uint32_t rng = 0x5eed;

static int rnd(int n)
{
	rng = rng * 1103515245u + 12345u;
	return (int) ((rng >> 16) % (uint32_t) n);
}

static int roams(uint8_t kind)
{
	/* horizontal and vertical pace, hound, loiter, wander, graze, shy, kid games, patrol */
	return kind == 1 || kind == 2 || kind == 9 || kind == 11 || kind == 12 || kind == 17
		|| kind == 20 || kind == 25 || kind == 29;
}

static void load(void)
{
	static uint8_t raw[2 + 257 * 2 + MOST_ENTRIES * 4];
	int n = e_read_file("STATIC", "SCHEDULE.DAT", raw, sizeof raw);

	sched_count = 0;
	if (n < 2)
		return;
	uint16_t count = (uint16_t) (raw[0] | raw[1] << 8);
	if (count > 256 || n < 2 + (count + 1) * 2)
		return;
	for (int i = 0; i <= count; i++)
		sched_first[i] = (uint16_t) (raw[2 + i * 2] | raw[3 + i * 2] << 8);
	int entries = (n - 2 - (count + 1) * 2) / 4;
	if (entries > MOST_ENTRIES)
		entries = MOST_ENTRIES;
	sched_first[count] = (uint16_t) entries;
	memcpy(sched, raw + 2 + (count + 1) * 2, (size_t) entries * 4);
	sched_count = count;
}

/* Whether NPC number has an entry starting at exactly period p. */
static const uint8_t *entry_at(uint16_t number, int p)
{
	if (number >= sched_count)
		return NULL;
	for (int k = sched_first[number]; k < sched_first[number + 1] && k < MOST_ENTRIES; k++)
		if ((sched[k][0] & 7) == p)
			return sched[k];
	return NULL;
}

static int in_slot(int i)
{
	for (int s = 1; s < E_MOVERS; s++)
		if (slot_npc[s] == i)
			return s;
	return 0;
}

void e_world_init(struct ENpc *npcs, int n, int start, uint32_t blanks)
{
	npc = npcs;
	nnpc = n < MOST_NPCS ? n : MOST_NPCS;
	load();
	start_minutes = minutes = start;
	period = start / 180 % 8;
	start_blanks = last_blanks = blanks;
	last_pick = start_blanks;
	picked_x = picked_y = -1000;
	for (int s = 0; s < E_MOVERS; s++)
		slot_npc[s] = -1;
	/* each goes on with what it is doing, where it is */
	for (int i = 0; i < nnpc; i++) {
		struct Doing *d = &doing[i];
		d->kind = npc[i].activity;
		d->dir = 2;
		d->steps = 0;
		d->place_x = d->goal_x = npc[i].x;
		d->place_y = d->goal_y = npc[i].y;
		d->wait = (int16_t) rnd(120);
		d->door = -1;
		d->route = -1;
		d->no_route = 0;
	}
	for (int r = 0; r < ROUTES; r++)
		routes[r].npc = -1;
}

int e_world_minutes(void)
{
	return minutes % (24 * 60);
}

/* Shuts the door an NPC opened, if it is still open. */
static void shut_behind(struct Doing *d)
{
	if (d->door >= 0 && !e_door_shut(d->door))
		e_door_toggle(d->door);
	d->door = -1;
}

static void drop_route(struct Doing *d)
{
	if (d->route >= 0)
		routes[d->route].npc = -1;
	d->route = -1;
}

/* The cell nearest x, y within the chunks held near the view (one in from their edge) that a
 * shape could stand on at z: 1 and x, y set; 0 when none is within 6 cells of that. */
static int near_stand(uint16_t tf, int16_t *x, int16_t *y, uint8_t z)
{
	int16_t x0, y0, x1, y1;

	e_near_window(&x0, &y0, &x1, &y1);
	int cx = *x < x0 + 1 ? x0 + 1 : *x > x1 - 1 ? x1 - 1 : *x;
	int cy = *y < y0 + 1 ? y0 + 1 : *y > y1 - 1 ? y1 - 1 : *y;
	for (int r = 0; r <= 6; r++)
		for (int dy = -r; dy <= r; dy++)
			for (int dx = -r; dx <= r; dx++) {
				int nx = cx + dx, ny = cy + dy;
				if ((dx != -r && dx != r && dy != -r && dy != r) || nx <= x0 || nx >= x1
					|| ny <= y0 || ny >= y1 || !e_can_stand(tf, (int16_t) nx, (int16_t) ny, z))
					continue;
				*x = (int16_t) nx;
				*y = (int16_t) ny;
				return 1;
			}
	return 0;
}

/* A route for NPC i to its goal from where it is (or, the goal beyond the chunks held, towards
 * it): 1 when there is one. */
static int plan(int i, uint16_t tf)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];
	int r = d->route;

	if (r < 0)
		for (r = 0; r < ROUTES && routes[r].npc >= 0; r++)
			;
	if (r >= ROUTES)
		return 0;
	struct Route *o = &routes[r];
	/* to its schedule's place at level 0, as the original; roaming about it, at its own */
	int place = d->goal_x == d->place_x && d->goal_y == d->place_y;
	int16_t tx = d->goal_x, ty = d->goal_y;
	uint8_t tz = place ? 0 : p->z;
	/* beyond the chunks held near the view: as far as the cell within them nearest it that it
	 * could stand on (planned on from there) */
	if (!in_near_window(tx, ty) && (!near_stand(tf, &tx, &ty, tz) || (tx == p->x && ty == p->y)))
		return 0;
	planned = 1;
	int n = e_route_plan(tf, p->x, p->y, p->z, tx, ty, tz, place ? ROUTE_PLACE : ROUTE_ROAM, o->way);
	if (n == 0) {
		drop_route(d);
		return 0;
	}
	o->npc = (int16_t) i;
	o->goal_x = d->goal_x;
	o->goal_y = d->goal_y;
	o->n = (uint8_t) n;
	o->next = 0;
	o->failures = 0;
	o->steps = 0;
	o->length = 0;
	for (int k = 1; k < n; k++) {
		int dx = o->way[k].x - o->way[k - 1].x, dy = o->way[k].y - o->way[k - 1].y;
		dx = dx < 0 ? -dx : dx;
		dy = dy < 0 ? -dy : dy;
		o->length = (uint16_t) (o->length + (dx > dy ? dx : dy));
	}
	d->route = (int8_t) r;
	return 1;
}

/* As the original's routes: a door it opened shut once it is 2 cells further than the nearest it
 * has come. */
static void mind_door(int i)
{
	struct Doing *d = &doing[i];
	int16_t x, y;

	if (d->door < 0)
		return;
	e_door_place(d->door, &x, &y);
	int dx = npc[i].x - x, dy = npc[i].y - y;
	int far = dx < 0 ? -dx : dx, fy = dy < 0 ? -dy : dy;
	if (fy > far)
		far = fy;
	if (far < d->nearest)
		d->nearest = (uint8_t) (far > 255 ? 255 : far);
	else if (far > d->nearest + 2)
		shut_behind(d);
}

/* The NPCs nearest x, y (not the avatar) in the mover slots, from those within 48 by 40 cells. */
static void pick(int16_t x, int16_t y)
{
	static int32_t far[E_MOVERS];
	static int16_t near[E_MOVERS];
	int nnear = 0;

	for (int i = 0; i < nnpc; i++) {
		int dx = npc[i].x - x, dy = npc[i].y - y;
		if (npc[i].number == 0 || dx < -48 || dx > 48 || dy < -40 || dy > 40)
			continue;
		int32_t dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
		int at = nnear < E_MOVERS - 1 ? nnear++ : E_MOVERS - 2;
		if (at == E_MOVERS - 2 && nnear == E_MOVERS - 1 && dist >= far[at])
			continue;
		while (at > 0 && far[at - 1] > dist) {
			far[at] = far[at - 1];
			near[at] = near[at - 1];
			at--;
		}
		far[at] = dist;
		near[at] = (int16_t) i;
	}
	/* those leaving the slots shut the doors they opened, as a route given up */
	for (int s = 1; s < E_MOVERS; s++) {
		int was = slot_npc[s], kept = 0;
		for (int k = 0; k < nnear && was >= 0; k++)
			kept |= near[k] == was;
		if (was >= 0 && !kept) {
			shut_behind(&doing[was]);
			drop_route(&doing[was]);
		}
	}
	for (int s = 1; s < E_MOVERS; s++) {
		slot_npc[s] = s - 1 < nnear ? near[s - 1] : -1;
		if (slot_npc[s] < 0)
			e_mover_set(s, 0, 0, 0, 0);
		else {
			const struct ENpc *p = &npc[slot_npc[s]];
			e_mover_set(s, p->tf, p->x, p->y, p->z);
		}
	}
	picked_x = x;
	picked_y = y;
}

/* A new period: each NPC with an entry for it takes it up; those not near are put there. */
static void new_period(int p)
{
	for (int i = 0; i < nnpc; i++) {
		const uint8_t *e = entry_at(npc[i].number, p);
		if (e == NULL || npc[i].number == 0)
			continue;
		struct Doing *d = &doing[i];
		d->kind = e[0] >> 3;
		d->place_x = d->goal_x = (int16_t) (e[3] % 12 * 256 + e[1]);
		d->place_y = d->goal_y = (int16_t) (e[3] / 12 * 256 + e[2]);
		d->wait = (int16_t) rnd(60);
		d->no_route = 0;
		if (!in_slot(i)) {
			npc[i].x = d->place_x;
			npc[i].y = d->place_y;
			npc[i].z = 0;
		}
	}
}

/* The facing (0-3: north, east, south, west) for a step way 0-7: slantwise ones turned on. */
static uint8_t facing(int way)
{
	return (uint8_t) (((way + 1) & 7) >> 1);
}

/* NPC i's step on its route, as the original walks one. 1 when its look or place changed. */
static int walk_route(int i)
{
	static const int8_t way_x[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
	static const int8_t way_y[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];
	uint16_t type = p->tf & 0x3ff, tf = e_walk_frame(type, 2, 0);
	struct Route *o = &routes[d->route];
	int door;

	/* steps not made: planned again from here; gone on too long: given up (walked at straight) */
	if (o->failures > 3) {
		if (planned) {
			d->wait = 1;
			return 0;
		}
		if (!plan(i, tf)) {
			d->no_route = 1;
			return 0;
		}
	}
	o = &routes[d->route];
	if (o->length > 10 && ++o->steps > o->length + o->length / 2) {
		drop_route(d);
		d->no_route = 1;
		return 0;
	}
	if (o->next < o->n - 1 && p->x == o->way[o->next].x && p->y == o->way[o->next].y
		&& p->z == o->way[o->next].z)
		o->next++;
	if (p->x == o->way[o->n - 1].x && p->y == o->way[o->n - 1].y && p->z == o->way[o->n - 1].z) {
		/* the end of a route towards a goal beyond the chunks held: on from here */
		drop_route(d);
		d->wait = 1;
		return 0;
	}
	const struct EWay *w = &o->way[o->next];
	int way = e_route_step(tf, &p->x, &p->y, &p->z, w->x, w->y, w->z, E_ROUTE_STOP, &door);
	if (way >= 0 && door) {
		/* a shut door: opened (held, shut behind it); an open one's leaf: round it */
		int k = e_door_at(tf, (int16_t) (p->x + way_x[way]), (int16_t) (p->y + way_y[way]), p->z);
		if (k >= 0 && e_door_shut(k)) {
			shut_behind(d);
			if (e_door_toggle(k)) {
				d->door = (int8_t) k;
				d->nearest = 255;
			}
			d->dir = facing(way);
			d->wait = STEP_BLANKS;
			return 0;
		}
		way = e_route_step(tf, &p->x, &p->y, &p->z, w->x, w->y, w->z, E_ROUTE_AVOID, &door);
	}
	if (way < 0) {
		o->failures++;
		d->wait = STEP_BLANKS;
		return 0;
	}
	d->dir = facing(way);
	d->steps++;
	p->tf = e_walk_frame(type, d->dir, d->steps);
	d->wait = STEP_BLANKS;
	return 1;
}

static int in_near_window(int x, int y)
{
	int16_t x0, y0, x1, y1;

	e_near_window(&x0, &y0, &x1, &y1);
	return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

/* One NPC near the avatar: a step towards its goal, or (there) a new goal or standing. 1 when
 * its look or place changed. */
static int act(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];
	int dx = (d->goal_x > p->x) - (d->goal_x < p->x), dy = (d->goal_y > p->y) - (d->goal_y < p->y);
	uint16_t type = p->tf & 0x3ff;

	if (dx == 0 && dy == 0) {
		drop_route(d);
		/* there: the roaming kinds pick a place near theirs, the rest stand */
		if (roams(d->kind)) {
			int rx = d->kind == 2 ? 0 : rnd(2 * ROAM + 1) - ROAM;
			int ry = d->kind == 1 ? 0 : rnd(2 * ROAM + 1) - ROAM;
			d->goal_x = (int16_t) (d->place_x + rx);
			d->goal_y = (int16_t) (d->place_y + ry);
			d->no_route = 0;
			d->wait = (int16_t) (60 + rnd(180));
		} else
			d->wait = 240;
		uint16_t standing = e_walk_frame(type, d->dir, 0);
		if (p->tf == standing)
			return 0;
		p->tf = standing;
		return 1;
	}
	/* a route to it, when it is near enough to plan one */
	if (d->route >= 0 && (routes[d->route].goal_x != d->goal_x || routes[d->route].goal_y != d->goal_y))
		drop_route(d);
	if (d->route < 0 && !d->no_route && !in_near_window(d->goal_x, d->goal_y)
		&& (p->x < me_x - 24 || p->x > me_x + 24 || p->y < me_y - 16 || p->y > me_y + 16)) {
		/* off the screen, going beyond the chunks held: put there, as those far off */
		p->x = d->goal_x;
		p->y = d->goal_y;
		p->z = 0;
		return 1;
	}
	if (d->route < 0 && !d->no_route) {
		if (planned) {
			d->wait = 1;           /* one route planned a step (they cost): its turn next */
			return 0;
		}
		if (!plan(i, e_walk_frame(type, 2, 0)))
			d->no_route = 1;
	}
	if (d->route >= 0)
		return walk_route(i);
	/* else straight at it: the longer way first, as one walks, up or down stairs; a shut door in the way opened (it
	 * steps through next time); along a wall when the other way is free; else give up */
	int ax = dx != 0 && (dy == 0 || (d->goal_x - p->x) * dx > (d->goal_y - p->y) * dy);
	int sx = ax ? dx : 0, sy = ax ? 0 : dy;
	uint8_t dir = (uint8_t) (sy < 0 ? 0 : sx > 0 ? 1 : sy > 0 ? 2 : 3);
	uint16_t tf = e_walk_frame(type, dir, 0);
	int step = e_step(tf, &p->x, &p->y, &p->z, sx, sy);
	if (step == E_STEP_DOOR && d->door < 0) {
		int door = e_door_at(tf, (int16_t) (p->x + sx), (int16_t) (p->y + sy), p->z);
		if (door >= 0 && e_door_shut(door) && e_door_toggle(door)) {
			d->door = (int8_t) door;
			d->nearest = 255;
			d->dir = dir;
			d->wait = STEP_BLANKS;
			return 0;
		}
	}
	if (step != E_STEP_MADE) {
		sx = ax ? 0 : dx;
		sy = ax ? dy : 0;
		dir = (uint8_t) (sy < 0 ? 0 : sx > 0 ? 1 : sy > 0 ? 2 : 3);
		if (!(sx || sy) || e_step(e_walk_frame(type, dir, 0), &p->x, &p->y, &p->z, sx, sy)
			!= E_STEP_MADE) {
			d->goal_x = p->x;          /* stuck: it stays */
			d->goal_y = p->y;
			d->wait = STEP_BLANKS;
			return 0;
		}
	}
	d->dir = dir;
	d->steps++;
	p->tf = e_walk_frame(type, dir, d->steps);
	d->wait = STEP_BLANKS;
	return 1;
}

void e_world_step(uint32_t blanks, int16_t x, int16_t y)
{
	int16_t dt = (int16_t) (blanks - last_blanks > 60 ? 60 : blanks - last_blanks);

	last_blanks = blanks;
	me_x = x;
	me_y = y;
	minutes = start_minutes + (int) ((blanks - start_blanks) / E_WORLD_MINUTE);
	int p = minutes / 180 % 8;
	if (p != period) {
		period = p;
		new_period(p);
	}
	/* the NPCs near the avatar picked again when it has gone 8 cells, or every 2 seconds */
	if ((x > picked_x ? x - picked_x : picked_x - x) + (y > picked_y ? y - picked_y : picked_y - y)
		>= 8 || blanks - last_pick >= 120) {
		pick(x, y);
		last_pick = blanks;
	}
	planned = 0;
	for (int s = 1; s < E_MOVERS; s++) {
		int i = slot_npc[s];
		if (i < 0)
			continue;
		doing[i].wait = (int16_t) (doing[i].wait - dt);
		if (doing[i].wait > 0)
			continue;
		e_mover_self(s);
		int changed = act(i);
		e_mover_self(-1);
		mind_door(i);
		if (changed)
			e_mover_set(s, npc[i].tf, npc[i].x, npc[i].y, npc[i].z);
	}
}
