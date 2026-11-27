/* The engine test's world (engine.h): a clock, and the NPCs on their schedules.
 *
 * SCHEDULE.DAT gives each NPC (by its number) up to eight entries, one for each three hours of
 * the day it starts something new: the kind of thing (a schedule type) and the place. As the
 * original, an NPC goes on with what it is doing until the next of its entries comes round, then
 * takes up that one's. Those near the avatar (in the mover slots) walk to the place on a route
 * planned as the original plans one (route.c), up and down stairs; those far off are put there.
 * Walking a route is the original's too: a step at the next waypoint (straight, else along one
 * axis), planned again after a few steps that could not be made, given up after half as many
 * steps again as it is long. A shut door in an NPC's way it opens, and shuts again once 2 cells
 * further from it than it came. A place beyond the chunks held near the view is walked at
 * straight, sliding along walls.
 *
 * At the place, each kind does what the original's schedule of that kind does (its states, its
 * ranges, its waits, in ticks of TICK_BLANKS):
 *   sleep: the nearest bed within 25 cells (a futon for one wider than a cell), from one of the
 *     spots its type is used from; in it, lying along it, its cover turned over; none: lies down
 *     where it is. Up again at its next entry, back where it got in, the cover back.
 *   sit, eat at the inn: a chair (else a seat) within 12 cells, picked at random; walks in front
 *     of it, faces its way, bends and sits. At the inn now and then eats the food a cell or two
 *     ahead. eat: a chair by the nearest plate; eats the food ahead once, after a while.
 *     desk work: a chair by the nearer desk.
 *   tend shop: walks about within 8 cells of where it got to, rests now and then, and goes to
 *     meet the avatar when it is within 20 cells of there.
 *   loiter: walks about within 15 cells of where it got to (patrol 8), resting between walks.
 *   wander: walks to places within 15 cells of where it is, resting after walks not found.
 *   the other roaming kinds (pace, hound, graze, ...) walk about within 4 cells; the rest stand.
 * Where the original says something (barks), works usecode, checks the area round it, puts food
 * on a plate or wakes to chase the avatar, this does not yet.
 */
#include <string.h>

#include "engine.h"

#define MOST_NPCS 400
#define MOST_ENTRIES 1024
#define STEP_BLANKS 10          /* an NPC walks a cell every 10 blanks: 6 a second */
#define TICK_BLANKS 10          /* the original's schedule waits, a tick a step */
#define ROAM 4                  /* the other roaming kinds keep within 4 cells of their place */
#define ROUTES 15               /* routes walked at once (as the original's slots) */
#define ROUTE_PLACE 150         /* steps tried planning one to a schedule's place (the original's) */
#define ROUTE_USE 100           /* ... to the spot to use a bed or chair from */
#define ROUTE_ROAM 45           /* ... to a place near it, wandering */

/* the kinds (schedule types) that do something here */
#define K_EAT 5
#define K_SHOP 7
#define K_STAND 10
#define K_LOITER 11
#define K_WANDER 12
#define K_SLEEP 14
#define K_SIT 16
#define K_EAT_INN 26
#define K_PATROL 29
#define K_DESK 30

/* the things they use */
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

/* where an NPC is in what it does */
#define S_GOING 0               /* walking to its schedule's place */
#define S_ROAMING 1             /* there: walking about it, or standing */
#define S_TO_USE 2              /* walking to the spot to use its bed or chair from */
#define S_USING 3               /* in bed, or sitting (or sitting down) */

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
	uint8_t stage;              /* S_ */
	uint8_t fails;              /* walks about not found, in a row */
	uint8_t walked;             /* it has just walked somewhere about its place */
	uint8_t pose;               /* sitting down: the frames yet to show (2 bend, 1 sit) */
	uint16_t steps;
	int16_t place_x, place_y;   /* its schedule's place */
	int16_t home_x, home_y;     /* where it walks about (where it got to) */
	int16_t goal_x, goal_y;     /* where it is walking now */
	uint8_t goal_z;
	uint8_t limit;              /* steps tried planning a route there */
	int16_t wait;
	int8_t door;                /* a door it opened and is to shut behind it, or -1 */
	uint8_t nearest;            /* the nearest it has been to that door */
	int8_t route;               /* the route it walks, or -1 */
	uint8_t no_route;           /* none was found to its goal: walked at straight */
	uint8_t retry;              /* to its place: routes not found, part way there tried next */
	uint8_t replans;            /* routes planned again with no step made since */
	struct EThing use;          /* the bed or chair it uses */
	struct EThing cover;        /* the bed's cover it turned over it (tf 0: none) */
	int16_t spot_x, spot_y;     /* where it uses it from */
	uint8_t spot_z, spot_dir;
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
	/* horizontal and vertical pace, hound, graze, shy, kid games */
	return kind == 1 || kind == 2 || kind == 9 || kind == 17 || kind == 20 || kind == 25;
}

static int far_of(int x0, int y0, int x1, int y1)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;

	return dx > dy ? dx : dy;
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
		memset(d, 0, sizeof *d);
		d->kind = npc[i].activity;
		d->dir = 2;
		d->place_x = d->goal_x = npc[i].x;
		d->place_y = d->goal_y = npc[i].y;
		d->limit = ROUTE_PLACE;
		d->wait = (int16_t) rnd(120);
		d->door = -1;
		d->route = -1;
		/* in bed or sitting already, as the game was left */
		int pose = npc[i].tf >> 10 & 15;
		if ((d->kind == K_SLEEP && pose == E_POSE_LIE) || (pose == E_POSE_SIT
			&& (d->kind == K_SIT || d->kind == K_EAT_INN || d->kind == K_EAT || d->kind == K_DESK))) {
			d->stage = S_USING;
			d->spot_x = npc[i].x;
			d->spot_y = npc[i].y;
			d->spot_z = npc[i].z;
		}
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
 * it): 1 when there is one, 0 when there is none, -1 when no route is free to plan it in. */
static int plan(int i, uint16_t tf)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];
	int r = d->route;

	if (r < 0)
		for (r = 0; r < ROUTES && routes[r].npc >= 0; r++)
			;
	if (r >= ROUTES)
		return -1;              /* none free: tried again later */
	struct Route *o = &routes[r];
	int16_t tx = d->goal_x, ty = d->goal_y;
	uint8_t tz = d->goal_z;
	/* as the original's walk to its place: none found there, part way (14, 10, 6 cells along
	 * each axis) */
	if (d->retry > 0) {
		int step = 18 - 4 * d->retry;
		tx = (int16_t) (p->x + (tx > p->x + step ? step : tx < p->x - step ? -step : tx - p->x));
		ty = (int16_t) (p->y + (ty > p->y + step ? step : ty < p->y - step ? -step : ty - p->y));
	}
	/* beyond the chunks held near the view: as far as the cell within them nearest it that it
	 * could stand on (planned on from there) */
	if (!in_near_window(tx, ty) && (!near_stand(tf, &tx, &ty, tz) || (tx == p->x && ty == p->y)))
		return 0;
	planned = 1;
	int n = e_route_plan(tf, p->x, p->y, p->z, tx, ty, tz, d->limit, o->way);
	if (n == 0) {
		drop_route(d);
		return 0;
	}
	d->retry = 0;
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

/* ---- what the kinds do at their place ---- */

static const int8_t dir_x[4] = { 0, 1, 0, -1 }, dir_y[4] = { -1, 0, 1, 0 };

/* NPC i shown as tf: 1 when that is a change. */
static int show(int i, uint16_t tf)
{
	if (npc[i].tf == tf)
		return 0;
	npc[i].tf = tf;
	return 1;
}

static int stand(int i)
{
	return show(i, e_pose_frame(npc[i].tf & 0x3ff, doing[i].dir, E_POSE_STAND));
}

/* Out of bed or up from a chair (its next entry come): the bed's cover back, and back where it
 * got in. */
static void leave(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];

	if (d->cover.tf != 0) {
		e_thing_set(&d->cover, (uint16_t) (d->cover.tf - (1u << 10)));
		d->cover.tf = 0;
	}
	if (d->stage == S_USING) {
		if (d->kind == K_SLEEP && (p->x != d->spot_x || p->y != d->spot_y || p->z != d->spot_z)
			&& (!in_near_window(d->spot_x, d->spot_y)
				|| e_can_stand(e_pose_frame(p->tf & 0x3ff, 2, 0), d->spot_x, d->spot_y, d->spot_z))) {
			p->x = d->spot_x;
			p->y = d->spot_y;
			p->z = d->spot_z;
		}
		stand(i);
		int s = in_slot(i);
		if (s)
			e_mover_set(s, p->tf, p->x, p->y, p->z);
	}
	d->stage = S_GOING;
	d->pose = 0;
}

/* NPC i to walk to x, y, z, planning in at most limit steps: 1; 0 (a walk not found, counted)
 * when it could not stand there. */
static int walk_to(int i, int x, int y, uint8_t z, int limit)
{
	struct Doing *d = &doing[i];

	if (!in_near_window(x, y) || (x == npc[i].x && y == npc[i].y)
		|| !e_can_stand(e_pose_frame(npc[i].tf & 0x3ff, 2, 0), (int16_t) x, (int16_t) y, z)) {
		d->fails++;
		d->wait = TICK_BLANKS;
		return 0;
	}
	d->goal_x = (int16_t) x;
	d->goal_y = (int16_t) y;
	d->goal_z = z;
	d->limit = (uint8_t) limit;
	d->no_route = 0;
	d->walked = 1;
	d->wait = 1;
	return 1;
}

static void become_loiter(int i)
{
	struct Doing *d = &doing[i];

	d->kind = K_LOITER;
	d->stage = S_ROAMING;
	d->home_x = npc[i].x;
	d->home_y = npc[i].y;
	d->fails = 0;
	d->walked = 0;
}

/* The nearest of type within r cells of x, y (and of level z0 to z1; of two as near, the lower)
 * into t: 1; 0 for none. */
static int nearest(uint16_t type, int x, int y, int r, int z0, int z1, struct EThing *t)
{
	static struct EThing found[64];
	int n = e_things(type, (int16_t) (x - r), (int16_t) (y - r), (int16_t) (x + r), (int16_t) (y + r),
		found, 64), best = -1, best_far = 0;

	for (int k = 0; k < n; k++) {
		int far = far_of(x, y, found[k].x, found[k].y) * 16 + found[k].z;
		if (found[k].z < z0 || found[k].z > z1 || (best >= 0 && far >= best_far))
			continue;
		best = k;
		best_far = far;
	}
	if (best < 0)
		return 0;
	*t = found[best];
	return 1;
}

/* The spots a bed (or futon) is got into from, as the original's: x, y from its hot spot and the
 * way (0-7) to face. */
static const int8_t bed_spots[4][3][3] = {
	{ { -1, 1, 0 }, { 1, -1, 6 }, { -3, -1, 2 } },  /* T_BED */
	{ { 1, -1, 6 }, { -1, -3, 4 }, { -1, 1, 0 } },  /* T_BED_2 */
	{ { -1, 3, 0 }, { 3, -1, 6 }, { -3, 0, 2 } },   /* T_FUTON */
	{ { 3, 0, 6 }, { 0, 3, 4 }, { 0, -3, 0 } },     /* T_FUTON_2 */
};

/* Sleep: the nearer of the two kinds of bed (futons for one wider than a cell) within 25 cells,
 * and a spot beside it to get in from: walking there. 0: no bed (it lies down where it is); -1:
 * no spot (it loiters). */
static int find_bed(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];
	struct EBody b;
	struct EThing a, c;
	uint16_t tf = e_pose_frame(p->tf & 0x3ff, 2, 0);

	e_body(tf, &b);
	int fits = b.fx == 0 && b.fy == 0;
	int ka = fits ? 0 : 2;
	int have_a = nearest(fits ? T_BED : T_FUTON, p->x, p->y, 25, 0, 15, &a);
	int have_c = nearest(fits ? T_BED_2 : T_FUTON_2, p->x, p->y, 25, 0, 15, &c);
	if (!have_a && !have_c)
		return 0;
	if (!have_a || (have_c && far_of(p->x, p->y, c.x, c.y) < far_of(p->x, p->y, a.x, a.y))) {
		a = c;
		ka++;
	}
	for (int k = 0; k < 3; k++) {
		int x = a.x + bed_spots[ka][k][0], y = a.y + bed_spots[ka][k][1];
		if (!in_near_window(x, y) || !e_can_stand(tf, (int16_t) x, (int16_t) y, a.z))
			continue;
		d->use = a;
		d->spot_x = (int16_t) x;
		d->spot_y = (int16_t) y;
		d->spot_z = a.z;
		d->spot_dir = (uint8_t) (bed_spots[ka][k][2] >> 1);
		walk_to(i, x, y, a.z, ROUTE_USE);
		d->stage = S_TO_USE;
		d->walked = 0;
		return 1;
	}
	return -1;
}

/* A chair or seat t: the cell in front of it (it faces as its frame's low 2 bits say) as NPC i's
 * spot, if it could stand there: walking there, 1. */
static int try_seat(int i, const struct EThing *t)
{
	struct Doing *d = &doing[i];
	int face = t->tf >> 10 & 3, x = t->x + dir_x[face], y = t->y + dir_y[face];

	if (!in_near_window(x, y) || !e_can_stand(e_pose_frame(npc[i].tf & 0x3ff, 2, 0), (int16_t) x,
		(int16_t) y, t->z))
		return 0;
	d->use = *t;
	d->spot_x = (int16_t) x;
	d->spot_y = (int16_t) y;
	d->spot_z = t->z;
	d->spot_dir = (uint8_t) face;
	walk_to(i, x, y, t->z, ROUTE_USE);
	d->stage = S_TO_USE;
	d->walked = 0;
	return 1;
}

/* Sit: by an item of type near (the nearest within 25 cells) the nearest chair within 12 of it,
 * else seat; or, anchor 0, one of the chairs (else seats) within 12 cells picked at random, else
 * the first that will do. 0: none (it loiters). */
static int find_seat(int i, uint16_t anchor)
{
	static struct EThing found[48];
	struct ENpc *p = &npc[i];
	struct EThing a, t;

	for (int pass = 0; pass < 2; pass++) {
		uint16_t type = pass == 0 ? T_CHAIR : T_SEAT;
		if (anchor != 0) {
			if (nearest(anchor, p->x, p->y, 25, 0, 15, &a) && nearest(type, a.x, a.y, 12, 0, 15, &t)
				&& try_seat(i, &t))
				return 1;
			continue;
		}
		int n = e_things(type, (int16_t) (p->x - 12), (int16_t) (p->y - 12), (int16_t) (p->x + 12),
			(int16_t) (p->y + 12), found, 48);
		if (n > 0 && try_seat(i, &found[rnd(n)]))
			return 1;
		for (int k = 0; k < n; k++)
			if (try_seat(i, &found[k]))
				return 1;
	}
	return 0;
}

/* At its schedule's place: what its kind does first. */
static int arrive(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];
	struct EThing a, c;
	int r;

	d->home_x = p->x;
	d->home_y = p->y;
	d->fails = 0;
	d->walked = 0;
	d->stage = S_ROAMING;
	switch (d->kind) {
	case K_SLEEP:
		r = find_bed(i);
		if (r == 0) {
			/* no bed: it lies down where it is */
			d->stage = S_USING;
			d->spot_x = p->x;
			d->spot_y = p->y;
			d->spot_z = p->z;
			d->wait = 600 * TICK_BLANKS;
			return show(i, e_pose_frame(p->tf & 0x3ff, d->dir, E_POSE_LIE));
		}
		if (r < 0)
			become_loiter(i);
		break;
	case K_SIT:
	case K_EAT_INN:
		if (!find_seat(i, 0))
			become_loiter(i);
		break;
	case K_EAT:
		if (!find_seat(i, T_PLATE))
			become_loiter(i);
		break;
	case K_DESK: {
		/* the nearer of the two desks */
		int have_a = nearest(T_DESK, p->x, p->y, 25, 0, 15, &a);
		int have_c = nearest(T_DESK_2, p->x, p->y, 25, 0, 15, &c);
		uint16_t desk = have_a && (!have_c || far_of(p->x, p->y, a.x, a.y)
			< far_of(p->x, p->y, c.x, c.y)) ? T_DESK : T_DESK_2;
		if (!(have_a || have_c) || !find_seat(i, desk))
			become_loiter(i);
		break;
	}
	}
	if (d->stage == S_TO_USE)
		return stand(i);
	return -1;                  /* roaming: on to that */
}

/* At the spot to use its bed or chair from: into bed, or sitting down; else it loiters. */
static int use_it(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];
	uint16_t type = p->tf & 0x3ff, bed = d->use.tf & 0x3ff;
	struct EThing t;

	if (p->x != d->spot_x || p->y != d->spot_y) {
		become_loiter(i);
		return -1;
	}
	d->dir = d->spot_dir;
	if (d->kind != K_SLEEP) {
		/* faces its way, stands, bends, sits: 2 ticks each */
		d->stage = S_USING;
		d->pose = 2;
		d->wait = 2 * TICK_BLANKS;
		return stand(i);
	}
	/* the bed still there, and no one in it */
	int fits = bed == T_BED || bed == T_BED_2;
	int x = d->use.x + (bed == T_BED && (d->use.tf >> 10 & 31) == 17), y = d->use.y;
	uint8_t z = (uint8_t) (p->z + fits);
	struct EThing in_cell[4];
	int there = 0;
	for (int k = e_things(bed, d->use.x, d->use.y, d->use.x, d->use.y, in_cell, 4); k-- > 0; )
		there |= in_cell[k].tf == d->use.tf && in_cell[k].z == d->use.z;
	for (int s = 1; s < E_MOVERS && there; s++)
		if (slot_npc[s] >= 0 && slot_npc[s] != i && npc[slot_npc[s]].x == x
			&& npc[slot_npc[s]].y == y)
			there = 0;
	if (!there) {
		become_loiter(i);
		return -1;
	}
	/* the cover over it: a bed of its type a level up nearby, an odd frame turned to the next */
	if (fits && nearest(bed, p->x, p->y, 5, z, z, &t) && (t.tf >> 10 & 1)
		&& e_thing_set(&t, (uint16_t) (t.tf + (1u << 10))))
		d->cover = t;
	p->x = (int16_t) x;
	p->y = (int16_t) y;
	p->z = z;
	/* lying along it */
	d->dir = bed == T_BED || bed == T_FUTON ? 0 : 3;
	d->stage = S_USING;
	d->goal_x = p->x;
	d->goal_y = p->y;
	d->wait = 600 * TICK_BLANKS;
	show(i, e_pose_frame(type, d->dir, E_POSE_LIE));
	return 1;
}

/* The food a cell (or, far, two) ahead of a sitter eaten: 1 when there was some. */
static int eat_ahead(int i, int far)
{
	struct ENpc *p = &npc[i];
	struct EThing t;
	int dx = dir_x[doing[i].dir], dy = dir_y[doing[i].dir];
	int x0 = p->x + dx, y0 = p->y + dy, x1 = p->x + dx * far, y1 = p->y + dy * far;

	if (!e_things(T_FOOD, (int16_t) (x0 < x1 ? x0 : x1), (int16_t) (y0 < y1 ? y0 : y1),
		(int16_t) (x0 > x1 ? x0 : x1), (int16_t) (y0 > y1 ? y0 : y1), &t, 1))
		return 0;
	return e_thing_remove(&t);
}

/* In bed or in a chair. */
static int using(int i)
{
	struct Doing *d = &doing[i];
	uint16_t type = npc[i].tf & 0x3ff;

	if (d->pose == 2) {
		d->pose = 1;
		d->wait = 2 * TICK_BLANKS;
		return show(i, e_pose_frame(type, d->dir, E_POSE_BEND));
	}
	if (d->pose == 1) {
		d->pose = 0;
		d->wait = (int16_t) ((d->kind == K_EAT_INN ? 40 + rnd(61) : d->kind == K_EAT ? 20 + rnd(31)
			: 600) * TICK_BLANKS);
		d->fails = 0;
		return show(i, e_pose_frame(type, d->dir, E_POSE_SIT));
	}
	if (d->kind == K_EAT_INN) {
		/* now and then eats what is ahead of it */
		if (rnd(2) == 0)
			eat_ahead(i, 2);
		d->wait = (int16_t) ((40 + rnd(61)) * TICK_BLANKS);
	} else if (d->kind == K_EAT && d->fails == 0) {
		/* eats its meal once */
		eat_ahead(i, 1);
		d->fails = 1;
		d->wait = 600 * TICK_BLANKS;
	} else
		d->wait = 600 * TICK_BLANKS;
	return 0;
}

/* About its place: the next walk, or a rest. */
static int roam(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];

	switch (d->kind) {
	case K_SHOP:
		/* as the original's: walks within 8 cells of its place, rests after a few walks not
		 * found, and half the time goes to meet the avatar within 20 cells of there */
		if (d->fails > 3) {
			d->fails = 0;
			d->wait = (int16_t) ((25 + rnd(26)) * TICK_BLANKS);
			return stand(i);
		}
		if (rnd(2) == 0) {
			if (far_of(p->x, p->y, me_x, me_y) <= 2) {
				d->wait = (int16_t) ((15 + rnd(11)) * TICK_BLANKS);
				return stand(i);
			}
			if (far_of(me_x, me_y, d->home_x, d->home_y) < 20) {
				/* beside the avatar, on its side */
				int x = me_x + (p->x > me_x) - (p->x < me_x), y = me_y + (p->y > me_y) - (p->y < me_y);
				walk_to(i, x, y, p->z, 50);
				d->fails = 0;
				return stand(i);
			}
			d->wait = TICK_BLANKS;
			return stand(i);
		}
		walk_to(i, d->home_x + rnd(17) - 8, d->home_y + rnd(17) - 8, p->z, 15);
		return stand(i);
	case K_LOITER:
	case K_PATROL:
		/* within 15 cells of where it got to (8 patrolling), resting between walks */
		if (d->walked || d->fails > 2) {
			d->walked = 0;
			d->fails = 0;
			d->wait = (int16_t) ((20 + rnd(21)) * TICK_BLANKS);
			return stand(i);
		}
		if (d->kind == K_PATROL)
			walk_to(i, d->home_x + rnd(17) - 8, d->home_y + rnd(17) - 8, p->z, 15);
		else
			walk_to(i, d->home_x + rnd(31) - 15, d->home_y + rnd(31) - 15, p->z, 30);
		return stand(i);
	case K_WANDER:
		/* to places within 15 cells of where it is, resting after a few not found */
		if (d->fails > 2) {
			d->fails = 0;
			d->wait = (int16_t) ((20 + rnd(21)) * TICK_BLANKS);
			return stand(i);
		}
		walk_to(i, p->x + rnd(31) - 15, p->y + rnd(31) - 15, p->z, ROUTE_ROAM);
		return stand(i);
	}
	if (roams(d->kind)) {
		int rx = d->kind == 2 ? 0 : rnd(2 * ROAM + 1) - ROAM;
		int ry = d->kind == 1 ? 0 : rnd(2 * ROAM + 1) - ROAM;
		if (walk_to(i, d->place_x + rx, d->place_y + ry, p->z, ROUTE_ROAM))
			d->wait = (int16_t) (60 + rnd(180));
		return stand(i);
	}
	d->wait = 240;
	return stand(i);
}

/* No way to its place found (nor part way): as the original, put there when the avatar is far
 * from both, there when within 5 cells of it, else it wanders. */
static int lost(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];

	d->wait = TICK_BLANKS;
	if (far_of(me_x, me_y, d->place_x, d->place_y) >= 24 && far_of(me_x, me_y, p->x, p->y) >= 24) {
		p->x = d->goal_x = d->place_x;
		p->y = d->goal_y = d->place_y;
		p->z = 0;
		return 1;
	}
	d->goal_x = p->x;
	d->goal_y = p->y;
	if (far_of(p->x, p->y, d->place_x, d->place_y) > 5) {
		d->kind = K_WANDER;
		d->stage = S_ROAMING;
		d->fails = 0;
	}
	return 0;
}

/* At its goal (or stuck): what comes next. 1 when its look or place changed. */
static int there(int i)
{
	int r;

	switch (doing[i].stage) {
	case S_GOING:
		/* looking for a bed or chair costs as a route does: one a step */
		if (doing[i].kind == K_SLEEP || doing[i].kind == K_SIT || doing[i].kind == K_EAT_INN
			|| doing[i].kind == K_EAT || doing[i].kind == K_DESK) {
			if (planned) {
				doing[i].wait = 1;
				return 0;
			}
			planned = 1;
		}
		if ((r = arrive(i)) >= 0)
			return r;
		break;
	case S_TO_USE:
		if ((r = use_it(i)) >= 0)
			return r;
		break;
	case S_USING:
		return using(i);
	}
	return roam(i);
}

/* A new period: each NPC with an entry for it takes it up; those not near are put there. */
static void new_period(int p)
{
	for (int i = 0; i < nnpc; i++) {
		const uint8_t *e = entry_at(npc[i].number, p);
		if (e == NULL || npc[i].number == 0)
			continue;
		struct Doing *d = &doing[i];
		leave(i);
		d->kind = e[0] >> 3;
		d->stage = S_GOING;
		d->place_x = d->goal_x = (int16_t) (e[3] % 12 * 256 + e[1]);
		d->place_y = d->goal_y = (int16_t) (e[3] / 12 * 256 + e[2]);
		d->goal_z = 0;          /* as the original: its place at level 0 */
		d->limit = ROUTE_PLACE;
		d->wait = (int16_t) rnd(60);
		d->no_route = 0;
		d->retry = 0;
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

static int lost(int i);
static void become_loiter(int i);
static int stand(int i);

/* No route to its goal (or one that cannot be walked): a walk about its place not taken, a bed or
 * chair given up (it loiters), part way to its place tried next (a route a step) and then lost;
 * else walked at straight. */
static int no_way(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];

	drop_route(d);
	d->replans = 0;
	switch (d->stage) {
	case S_ROAMING:
		d->fails++;
		d->walked = 0;
		d->goal_x = p->x;
		d->goal_y = p->y;
		d->wait = TICK_BLANKS;
		return 0;
	case S_TO_USE:
		become_loiter(i);
		d->goal_x = p->x;
		d->goal_y = p->y;
		d->wait = TICK_BLANKS;
		return stand(i);
	case S_GOING:
		if (++d->retry < 4) {
			d->wait = 1;
			return 0;
		}
		d->retry = 0;
		return lost(i);
	}
	d->no_route = 1;
	return 0;
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
		if (++d->replans > 2 || plan(i, tf) <= 0)
			return no_way(i);
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
			if (!e_door_toggle(k))
				return no_way(i);      /* locked */
			d->door = (int8_t) k;
			d->nearest = 255;
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
	d->replans = 0;
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
		return there(i);
	}
	/* a route to it, when it is near enough to plan one */
	if (d->route >= 0 && (routes[d->route].goal_x != d->goal_x || routes[d->route].goal_y != d->goal_y))
		drop_route(d);
	if (d->route < 0 && !d->no_route && d->stage == S_GOING && !in_near_window(d->goal_x, d->goal_y)
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
		int r = plan(i, e_walk_frame(type, 2, 0));
		if (r < 0) {
			d->wait = TICK_BLANKS;
			return 0;
		}
		if (r == 0)
			return no_way(i);
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
			if (d->stage == S_GOING)
				return lost(i);
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
