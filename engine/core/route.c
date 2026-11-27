/* Routes (engine.h): an NPC's way to a place, planned as the original plans one.
 *
 * The original walks a straight line at the goal, a step at a time, the way that is most
 * straight, else along the longer axis, else the shorter. Where that is blocked it sends two
 * walkers along the obstacle from there, one keeping it on the left and one on the right, a step
 * each in turn, and takes the first that comes back nearer the goal than it has been and can step
 * straight at it (or reaches it). Waypoints are kept where a wall walker turns and where the
 * level changes. When the goal is higher up, it plans from the goal back (easier down stairs than
 * up), then turns the list round. Last it straightens the route: from each waypoint, the furthest
 * one a straight walk reaches, those between dropped; with a waypoint either side of each door
 * that walk goes through. While planning, doors are walked through as if open: the walk opens
 * them.
 *
 * Here routes are planned within the chunks the engine holds near the view (as the original's
 * collision grid covers only the world near the avatar).
 */
#include <string.h>

#include "engine.h"

/* the eight ways, from north clockwise */
static const int8_t way_x[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int8_t way_y[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

static int sign(int v) { return (v > 0) - (v < 0); }
static int mag(int v) { return v < 0 ? -v : v; }

int e_way_of(int dx, int dy)
{
	static const int8_t by_sign[3][3] = { { 7, 0, 1 }, { 6, -1, 2 }, { 5, 4, 3 } };

	return by_sign[sign(dy) + 1][sign(dx) + 1];
}

/* A walker: a shape where it is, where it is going, and how its last step went. */
struct Walker {
	const struct EBody *body;
	int16_t x, y;
	uint8_t z;
	int16_t tx, ty;
	uint8_t tz;
	uint8_t mode;               /* E_ROUTE_THROUGH, E_ROUTE_AVOID, E_ROUTE_STOP */
	uint8_t climbed, door;      /* the last step's level changed; it was into a door */
};

static int16_t win_x0, win_y0, win_x1, win_y1;

/* A step way d: 1 when taken (or, stopping at doors, when a door is there: not moved). */
static int try_step(struct Walker *w, int d)
{
	int16_t x = w->x, y = w->y;
	uint8_t z = w->z;
	int nx = x + way_x[d], ny = y + way_y[d];

	w->climbed = w->door = 0;
	if (nx < win_x0 || nx > win_x1 || ny < win_y0 || ny > win_y1)
		return 0;
	int r = e_step_body(w->body, &x, &y, &z, way_x[d], way_y[d], w->mode != E_ROUTE_STOP);
	if (r == E_STEP_BLOCKED)
		return 0;
	if (r == E_STEP_DOOR) {
		w->door = 1;
		if (w->mode == E_ROUTE_AVOID)
			return 0;
		if (w->mode == E_ROUTE_STOP)
			return 1;
	}
	w->climbed = z != w->z;
	w->x = x;
	w->y = y;
	w->z = z;
	return 1;
}

/* A step towards the goal: straight at it, else (going slantwise) along the longer axis, else the
 * shorter. The way taken, or -1 (none; or at the goal's cell). */
static int choose_step(struct Walker *w)
{
	int dx = w->tx - w->x, dy = w->ty - w->y;
	int d = e_way_of(dx, dy);

	if (d < 0)
		return -1;
	if (try_step(w, d))
		return d;
	if (dx != 0 && dy != 0) {
		int first = mag(dx) >= mag(dy) ? e_way_of(dx, 0) : e_way_of(0, dy);
		int second = mag(dx) >= mag(dy) ? e_way_of(0, dy) : e_way_of(dx, 0);
		if (try_step(w, first))
			return first;
		if (try_step(w, second))
			return second;
	}
	return -1;
}

static int at_goal(const struct Walker *w)
{
	return w->x == w->tx && w->y == w->ty && w->z == w->tz;
}

static int goal_distance(const struct Walker *w)
{
	int dx = mag(w->tx - w->x), dy = mag(w->ty - w->y);

	return dx > dy ? dx : dy;
}

/* A list of waypoints being made. */
struct List {
	struct EWay *way;
	int n, most;
};

static int add(struct List *l, int16_t x, int16_t y, uint8_t z)
{
	if (l->n >= l->most)
		return 0;
	l->way[l->n].x = x;
	l->way[l->n].y = y;
	l->way[l->n].z = z;
	l->n++;
	return 1;
}

static int add_at(struct List *l, const struct Walker *w)
{
	return add(l, w->x, w->y, w->z);
}

/* From w, blocked: two walkers along the obstacle, heading way at first, one turning right
 * round it (keeping it on its left) and one left, until one is nearer the goal than it has been
 * and can step straight at it, or reaches it. Its turns go on the list, and w is put where it
 * left the obstacle. 0 when neither gets anywhere. */
static int follow_wall(struct Walker *w, struct List *out, int way, int *count, int limit)
{
	static struct EWay trail_way[2][E_ROUTE_WAYS];
	struct List trail[2] = { { trail_way[0], 0, E_ROUTE_WAYS }, { trail_way[1], 0, E_ROUTE_WAYS } };
	struct Walker end[2] = { *w, *w };
	int dir[2] = { way, way }, turn[2] = { 0, 0 }, nearest[2];

	nearest[0] = nearest[1] = w->z == w->tz ? goal_distance(w) : 9999;
	if (!add_at(out, w))
		return 0;
	for (;;) {
		if (*count > limit)
			return 0;
		for (int i = 0; i < 2; i++) {
			struct Walker *e = &end[i];
			int d = -1;
			/* the first way free, sweeping from a turn towards the wall round the other way */
			for (int j = 0; j < 8 && d < 0; j++) {
				int t = i == 0 ? (dir[i] - turn[i] + j) & 7 : (dir[i] + turn[i] - j) & 7;
				if (try_step(e, t))
					d = t;
			}
			if (d < 0)
				return 0;
			(*count)++;
			if (d != dir[i] || e->climbed) {
				if (!add_at(&trail[i], e))
					return 0;
				dir[i] = d;
			}
			turn[i] = (dir[i] & 1) + 1;
			struct Walker back = *e;
			int left = at_goal(e);
			if (!left && e->z == e->tz && goal_distance(e) <= nearest[i]) {
				nearest[i] = goal_distance(e);
				left = try_step(e, e_way_of(e->tx - e->x, e->ty - e->y)) && !e->climbed;
				if (!left)
					*e = back;
			}
			if (left) {
				/* off the wall: its trail (and the step off) onto the list, w from there */
				if (!add_at(&trail[i], e))
					return 0;
				for (int k = 0; k < trail[i].n; k++)
					if (!add(out, trail[i].way[k].x, trail[i].way[k].y, trail[i].way[k].z))
						return 0;
				w->x = back.x;
				w->y = back.y;
				w->z = back.z;
				return 1;
			}
			/* the two met going different ways: round the obstacle, unless they only cross */
			if (end[0].x == end[1].x && end[0].y == end[1].y && end[0].z == end[1].z
				&& dir[0] != dir[1]) {
				int other = (dir[1 - i] + 4) & 7, crossed = 0;
				for (int j = 0; j < 8 && !crossed; j++) {
					int t = i == 0 ? (dir[i] - turn[i] + j) & 7 : (dir[i] + turn[i] - j) & 7;
					if (t == other)
						return 0;
					back = *e;
					if (try_step(e, t)) {
						*e = back;
						crossed = 1;
					}
				}
				if (!crossed)
					return 0;
			}
		}
	}
}

/* Drops the waypoints a straight walk can go past, and puts one either side of each door such
 * a walk goes through. */
static int straighten(const struct EBody *body, struct List *l)
{
	static struct EWay kept_way[E_ROUTE_WAYS], extra_way[8];
	struct List kept = { kept_way, 0, E_ROUTE_WAYS };
	int i = 0;

	while (i < l->n - 1) {
		if (!add(&kept, l->way[i].x, l->way[i].y, l->way[i].z))
			return 0;
		int next = i + 1;
		struct List extra = { extra_way, 0, 8 };
		for (int j = l->n - 1; j > i + 1; j--) {
			struct Walker w = { body, l->way[i].x, l->way[i].y, l->way[i].z,
				l->way[j].x, l->way[j].y, l->way[j].z, E_ROUTE_THROUGH, 0, 0 };
			int in_door = 0, ok = 1;
			int16_t px = w.x, py = w.y;
			uint8_t pz = w.z;
			extra.n = 0;
			while (!at_goal(&w)) {
				if (choose_step(&w) < 0) {
					ok = 0;
					break;
				}
				if (w.door && !in_door) {
					/* before the door, unless that is hardly away from the start */
					if ((mag(px - l->way[i].x) > 3 || mag(py - l->way[i].y) > 3
						|| pz != l->way[i].z) && !add(&extra, px, py, pz)) {
						ok = 0;
						break;
					}
				} else if (!w.door && in_door && !add_at(&extra, &w)) {
					ok = 0;
					break;
				}
				in_door = w.door;
				px = w.x;
				py = w.y;
				pz = w.z;
			}
			if (ok) {
				for (int k = 0; k < extra.n; k++)
					if (!add(&kept, extra.way[k].x, extra.way[k].y, extra.way[k].z))
						return 0;
				next = j;
				break;
			}
		}
		i = next;
	}
	if (!add(&kept, l->way[l->n - 1].x, l->way[l->n - 1].y, l->way[l->n - 1].z))
		return 0;
	memcpy(l->way, kept.way, (size_t) kept.n * sizeof *kept.way);
	l->n = kept.n;
	return 1;
}

static int plan(uint16_t tf, int16_t x, int16_t y, uint8_t z, int16_t tx, int16_t ty, uint8_t tz,
	int limit, struct EWay *way)
{
	struct List l = { way, 0, E_ROUTE_WAYS };
	int count = 0, detour = 0, tries = 0, last = 0, saved = 0;

	e_near_window(&win_x0, &win_y0, &win_x1, &win_y1);
	if (tx < win_x0 || tx > win_x1 || ty < win_y0 || ty > win_y1 || !e_can_stand(tf, tx, ty, tz))
		return 0;
	/* up stairs: planned from the top down, turned round after */
	int reversed = tz > z;
	struct EBody body;
	e_body(tf, &body);
	struct Walker w = { &body, x, y, z, tx, ty, tz, E_ROUTE_THROUGH, 0, 0 };
	if (reversed) {
		w.x = tx; w.y = ty; w.z = tz;
		w.tx = x; w.ty = y; w.tz = z;
	}
	int16_t gx = w.tx, gy = w.ty;
	uint8_t gz = w.tz;
	if (!add_at(&l, &w))
		return 0;
	while (!at_goal(&w)) {
		if (count > limit)
			return 0;
		if (detour) {
			/* on another level than the goal and stuck: on the way it was going, then along */
			if (tries++ > 15)
				return 0;
			if (try_step(&w, saved))
				continue;
			if (!add_at(&l, &w))
				return 0;
			detour = 0;
			tries = 0;
			last = saved;
			if (!follow_wall(&w, &l, saved, &count, limit))
				return 0;
		}
		int prev = last;
		last = choose_step(&w);
		if (last < 0 && w.z != w.tz) {
			detour = 1;
			saved = prev;
			last = prev;
			continue;
		}
		if (last >= 0) {
			count++;
			if (w.climbed && !add_at(&l, &w))
				return 0;
		} else {
			int d = e_way_of(w.tx - w.x, w.ty - w.y);
			last = d;
			if (!follow_wall(&w, &l, d, &count, limit))
				return 0;
		}
	}
	if (!add(&l, gx, gy, gz))
		return 0;
	if (reversed)
		for (int i = 0, j = l.n - 1; i < j; i++, j--) {
			struct EWay t = l.way[i];
			l.way[i] = l.way[j];
			l.way[j] = t;
		}
	if (!straighten(&body, &l))
		return 0;
	e_stats.routes_planned++;
	e_stats.route_steps += (uint32_t) count;
	return l.n;
}

int e_route_plan(uint16_t tf, int16_t x, int16_t y, uint8_t z, int16_t tx, int16_t ty, uint8_t tz,
	int limit, struct EWay *way)
{
	uint32_t t0 = eb_clock();
	int n = plan(tf, x, y, z, tx, ty, tz, limit, way);
	uint32_t t = eb_clock() - t0;
	if (n == 0)
		e_stats.routes_failed++;

	e_stats.route_time += t;
	if (t > e_stats.route_worst)
		e_stats.route_worst = t;
	return n;
}

int e_route_step(uint16_t tf, int16_t *x, int16_t *y, uint8_t *z, int16_t tx, int16_t ty, uint8_t tz,
	int mode, int *door)
{
	struct EBody body;
	e_body(tf, &body);
	struct Walker w = { &body, *x, *y, *z, tx, ty, tz, (uint8_t) mode, 0, 0 };

	e_near_window(&win_x0, &win_y0, &win_x1, &win_y1);
	int d = choose_step(&w);
	*door = w.door;
	*x = w.x;
	*y = w.y;
	*z = w.z;
	return d;
}
