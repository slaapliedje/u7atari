/* The engine test's world (engine.h): a clock, the NPCs' schedules, and their walking.
 *
 * SCHEDULE.DAT gives each NPC (by its number) up to eight entries, one for each three hours of
 * the day it starts something new: the kind of thing (a schedule type) and the place. As the
 * original, an NPC goes on with what it is doing until the next of its entries comes round, then
 * takes up that one's. What each kind does is kinds.c's; here is the clock, the NPCs near the
 * avatar (in the mover slots, the only ones that act), and their walking.
 *
 * Walking to the place: those near walk on a route planned as the original plans one (route.c),
 * up and down stairs; those far off are put there. No route found, part way is tried (14, 10,
 * then 6 cells along), and then, as the original, the NPC is put there if the avatar is far
 * from both, counts as there within 5 cells, or else wanders. A place beyond the chunks held
 * near the view is walked at straight, sliding along walls. A walk to a goal near (the kinds'
 * start_path) is planned at once, or not at all.
 *
 * Walking a route is the original's: a step at the next waypoint (straight, else along one
 * axis), planned again after a few steps that could not be made (three times with no step
 * between, or a locked door in the way, and there is no way there), given up after half as many
 * steps again as it is long. A shut door in an NPC's way it opens, and shuts again once 2 cells
 * further from it than it came.
 */
#include <string.h>

#include "kinds.h"

#define MOST_ENTRIES 1024
#define ROUTES 15               /* routes walked at once (as the original's slots) */
#define ROUTE_PLACE 150         /* steps tried planning one to a schedule's place (the original's) */

struct ENpc *npc;
int nnpc;
struct Doing doing[MOST_NPCS];
int16_t me_x, me_y;
int planned;
int searched;
int16_t slot_npc[E_MOVERS];     /* the NPC in each mover slot (1 on), or -1 */

/* SCHEDULE.DAT: a count, the first entry of each NPC (and the end), the entries: the period (low
 * 3 bits) and kind (high 5), x, y in a region, the region */
static uint16_t sched_count, sched_first[257];
static uint8_t sched[MOST_ENTRIES][4];

/* the routes being walked: whose, the waypoints, the next of them, steps not made in a row, steps
 * taken and its length */
static struct Route {
	int16_t npc;
	int16_t goal_x, goal_y;
	uint8_t n, next, failures;
	uint16_t steps, length;
	struct EWay way[E_ROUTE_WAYS];
} routes[ROUTES];

static int16_t picked_x, picked_y;
static uint32_t start_blanks, last_blanks, last_pick;
static int start_minutes, minutes, period = -1;

static uint32_t rng = 0x5eed;

int rnd(int n)
{
	rng = rng * 1103515245u + 12345u;
	return (int) ((rng >> 16) % (uint32_t) n);
}

int far_of(int x0, int y0, int x1, int y1)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;

	return dx > dy ? dx : dy;
}

int in_near_window(int x, int y)
{
	int16_t x0, y0, x1, y1;

	e_near_window(&x0, &y0, &x1, &y1);
	return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

int in_slot(int i)
{
	for (int s = 1; s < E_MOVERS; s++)
		if (slot_npc[s] == i)
			return s;
	return 0;
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
	for (int r = 0; r < ROUTES; r++)
		routes[r].npc = -1;
	/* each goes on with what it is doing, where it is (its place) */
	for (int i = 0; i < nnpc; i++) {
		struct Doing *d = &doing[i];
		memset(d, 0, sizeof *d);
		d->dir = 2;
		d->place_x = d->goal_x = npc[i].x;
		d->place_y = d->goal_y = npc[i].y;
		d->wait = (int16_t) rnd(120);
		d->door = -1;
		d->route = -1;
		kind_init(i);
	}
	uc_world_init();
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

int start_path(int i, int x, int y, int z, int limit)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];

	drop_route(d);
	d->goal_x = (int16_t) x;
	d->goal_y = (int16_t) y;
	d->goal_z = (uint8_t) z;
	d->limit = (uint8_t) (limit > 255 ? 255 : limit);
	d->no_route = 0;
	d->replans = 0;
	d->retry = 0;
	if (x == p->x && y == p->y) {
		d->walking = W_PATH;    /* there already: the walk ends at once */
		return 0;
	}
	if (!in_near_window(x, y) || !e_can_stand(e_walk_frame(p->tf & 0x3ff, 2, 0), (int16_t) x,
		(int16_t) y, (uint8_t) z))
		return 2;
	if (planned)
		return 1;               /* a route a step (they cost): ask again */
	int r = plan(i, e_walk_frame(p->tf & 0x3ff, 2, 0));
	if (r < 0)
		return 1;
	if (r == 0)
		return 2;
	d->walking = W_PATH;
	return 0;
}

void start_place(int i)
{
	struct Doing *d = &doing[i];

	drop_route(d);
	d->goal_x = d->place_x;
	d->goal_y = d->place_y;
	d->goal_z = 0;              /* as the original: its place at level 0 */
	d->limit = ROUTE_PLACE;
	d->no_route = 0;
	d->replans = 0;
	d->retry = 0;
	d->walking = W_PLACE;
}

/* The walk has ended (there, or not). */
static void end_walk(int i, int ok)
{
	struct Doing *d = &doing[i];

	drop_route(d);
	d->walking = W_NONE;
	d->goal_x = npc[i].x;
	d->goal_y = npc[i].y;
	kind_walked(i, ok);
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

	/* the party first, wherever it is */
	for (int j = 1; uc_party(j) >= 0 && nnear < E_MOVERS - 1; j++) {
		far[nnear] = -1;
		near[nnear++] = (int16_t) uc_party(j);
	}
	for (int i = 0; i < nnpc; i++) {
		int dx = npc[i].x - x, dy = npc[i].y - y;
		if (npc[i].number == 0 || (npc[i].status & 0x8800) || dx < -48 || dx > 48 || dy < -40 || dy > 40)
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
		if (e == NULL || npc[i].number == 0 || (npc[i].status & 0x0800))
			continue;
		struct Doing *d = &doing[i];
		drop_route(d);
		d->walking = W_NONE;
		d->place_x = d->goal_x = (int16_t) (e[3] % 12 * 256 + e[1]);
		d->place_y = d->goal_y = (int16_t) (e[3] / 12 * 256 + e[2]);
		d->wait = (int16_t) rnd(60);
		kind_period(i, e[0] >> 3);
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

/* No way to its place found (nor part way): as the original, put there when the avatar is far
 * from both, there when within 5 cells of it, else it wanders. */
static int lost(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];

	d->wait = TICK_BLANKS;
	if (far_of(me_x, me_y, d->place_x, d->place_y) >= 24 && far_of(me_x, me_y, p->x, p->y) >= 24) {
		p->x = d->place_x;
		p->y = d->place_y;
		p->z = 0;
		end_walk(i, 1);
		return 1;
	}
	if (far_of(p->x, p->y, d->place_x, d->place_y) <= 5) {
		end_walk(i, 1);
		return 0;
	}
	drop_route(d);
	d->walking = W_NONE;
	kind_lost(i);
	return 0;
}

/* No route to its goal (or one that cannot be walked): to its place, part way tried next (a
 * route a step) and then lost; a walk near, ended (not there). */
static int no_way(int i)
{
	struct Doing *d = &doing[i];

	drop_route(d);
	d->replans = 0;
	d->wait = TICK_BLANKS;
	if (d->walking == W_PLACE) {
		if (++d->retry < 4) {
			d->wait = 1;
			return 0;
		}
		d->retry = 0;
		return lost(i);
	}
	end_walk(i, 0);
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
	d->pose = 0;
	p->tf = e_walk_frame(type, d->dir, d->steps);
	d->wait = STEP_BLANKS;
	return 1;
}

/* A step of NPC i's walk. 1 when its look or place changed. */
static int walk(int i)
{
	struct ENpc *p = &npc[i];
	struct Doing *d = &doing[i];
	int dx = (d->goal_x > p->x) - (d->goal_x < p->x), dy = (d->goal_y > p->y) - (d->goal_y < p->y);
	uint16_t type = p->tf & 0x3ff;

	if (dx == 0 && dy == 0) {
		end_walk(i, 1);
		return 0;
	}
	if (d->route >= 0 && (routes[d->route].goal_x != d->goal_x || routes[d->route].goal_y != d->goal_y))
		drop_route(d);
	if (d->route < 0 && !d->no_route && d->walking == W_PLACE && !in_near_window(d->goal_x, d->goal_y)
		&& (p->x < me_x - 24 || p->x > me_x + 24 || p->y < me_y - 16 || p->y > me_y + 16)) {
		/* off the screen, going beyond the chunks held: put there, as those far off */
		p->x = d->goal_x;
		p->y = d->goal_y;
		p->z = 0;
		end_walk(i, 1);
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
	/* else straight at it: the longer way first, as one walks, up or down stairs; a shut door in
	 * the way opened (it steps through next time); along a wall when the other way is free; else
	 * it is stuck */
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
			if (d->walking == W_PLACE)
				return lost(i);
			end_walk(i, 0);
			return 0;
		}
	}
	d->dir = dir;
	d->steps++;
	d->pose = 0;
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
	searched = 0;
	kind_things(dt);
	e_barks_age(dt);
	for (int s = 1; s < E_MOVERS; s++) {
		int i = slot_npc[s];
		if (i < 0)
			continue;
		doing[i].wait = (int16_t) (doing[i].wait - dt);
		if (doing[i].wait > 0)
			continue;
		e_mover_self(s);
		int changed = doing[i].walking ? walk(i) : kind_tick(i);
		if (doing[i].walking && doing[i].lost && doing[i].wait > STEP_BLANKS / 3)
			doing[i].wait = STEP_BLANKS / 3;    /* (lost party: back at a run, as the original's) */
		e_mover_self(-1);
		mind_door(i);
		if (changed)
			e_mover_set(s, npc[i].tf, npc[i].x, npc[i].y, npc[i].z);
	}
}

int e_world_npc_near(int16_t x, int16_t y, int dx, int dy)
{
	int best = -1, best_far = 3;

	for (int s = 1; s < E_MOVERS; s++) {
		int i = slot_npc[s];
		if (i < 0 || npc[i].number == 0)
			continue;
		/* ahead (1 or 2 cells) first, then the nearest within 2 */
		int ahead = (npc[i].x == x + dx && npc[i].y == y + dy)
			|| (npc[i].x == x + 2 * dx && npc[i].y == y + 2 * dy);
		int far = ahead ? 0 : far_of(x, y, npc[i].x, npc[i].y);
		if (far < best_far) {
			best_far = far;
			best = npc[i].number;
		}
	}
	return best;
}

int e_world_mover_npc(int s)
{
	return s == 0 ? 0 : s > 0 && s < E_MOVERS && slot_npc[s] >= 0 ? npc[slot_npc[s]].number : -1;
}

void e_world_advance(int m)
{
	start_minutes += m;
	minutes += m;
}

void e_world_avatar_place(int16_t x, int16_t y, uint8_t z, int dir)
{
	int i = avatar_index();

	if (i >= 0 && !kind_avatar_posed()) {
		npc[i].x = x;
		npc[i].y = y;
		npc[i].z = z;
		doing[i].dir = (uint8_t) dir;
	}
}

int e_world_avatar_pose(int16_t *x, int16_t *y, uint8_t *z, uint16_t *tf)
{
	int i = avatar_index();

	if (i < 0 || !kind_avatar_moved())
		return 0;
	*x = npc[i].x;
	*y = npc[i].y;
	*z = npc[i].z;
	*tf = npc[i].tf;
	return 1;
}

void e_world_avatar_stand(void)
{
	kind_avatar_stand();
}

void e_world_party_step(int moved)
{
	kind_party_step(moved);
}

void stop_walk(int i)
{
	shut_behind(&doing[i]);
	drop_route(&doing[i]);
	doing[i].walking = W_NONE;
}

void world_gone(int i)
{
	int s = in_slot(i);

	if (s) {
		slot_npc[s] = -1;
		e_mover_set(s, 0, 0, 0, 0);
	}
	if (npc[i].status & 0x0800)
		uc_party_leave(i);
}
