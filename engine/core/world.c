/* The engine test's world (engine.h): a clock, and the NPCs on their schedules.
 *
 * SCHEDULE.DAT gives each NPC (by its number) up to eight entries, one for each three hours of
 * the day it starts something new: the kind of thing (a schedule type) and the place. As the
 * original, an NPC goes on with what it is doing until the next of its entries comes round, then
 * takes up that one's. Here that is only where it goes and how it moves there: those near the
 * avatar (in the mover slots) walk to the place, a cell at a time, along walls when one way is
 * free; those far off are put there. At the place, the roaming kinds (loiter, wander, pace,
 * patrol, graze, ...) walk to places nearby in turn, and the rest stand. What the kinds do there
 * (sleep in a bed, work a forge, sell, eat) is the game's, still to come.
 */
#include <string.h>

#include "engine.h"

#define MOST_NPCS 400
#define MOST_ENTRIES 1024
#define STEP_BLANKS 10          /* an NPC walks a cell every 10 blanks: 6 a second */
#define ROAM 4                  /* the roaming kinds keep within 4 cells of their place */

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
} doing[MOST_NPCS];

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
	}
}

int e_world_minutes(void)
{
	return minutes % (24 * 60);
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
		if (!in_slot(i)) {
			npc[i].x = d->place_x;
			npc[i].y = d->place_y;
			npc[i].z = 0;
		}
	}
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
		/* there: the roaming kinds pick a place near theirs, the rest stand */
		if (roams(d->kind)) {
			int rx = d->kind == 2 ? 0 : rnd(2 * ROAM + 1) - ROAM;
			int ry = d->kind == 1 ? 0 : rnd(2 * ROAM + 1) - ROAM;
			d->goal_x = (int16_t) (d->place_x + rx);
			d->goal_y = (int16_t) (d->place_y + ry);
			d->wait = (int16_t) (60 + rnd(180));
		} else
			d->wait = 240;
		uint16_t standing = e_walk_frame(type, d->dir, 0);
		if (p->tf == standing)
			return 0;
		p->tf = standing;
		return 1;
	}
	/* the longer way first, as one walks; along a wall when one way is free; else give up */
	int ax = dx != 0 && (dy == 0 || (d->goal_x - p->x) * dx > (d->goal_y - p->y) * dy);
	int sx = ax ? dx : 0, sy = ax ? 0 : dy;
	uint8_t dir = (uint8_t) (sy < 0 ? 0 : sx > 0 ? 1 : sy > 0 ? 2 : 3);
	uint16_t tf = e_walk_frame(type, dir, 0);
	if (e_blocked(tf, (int16_t) (p->x + sx), (int16_t) (p->y + sy), p->z)) {
		sx = ax ? 0 : dx;
		sy = ax ? dy : 0;
		if ((sx || sy)
			&& !e_blocked(tf, (int16_t) (p->x + sx), (int16_t) (p->y + sy), p->z))
			dir = (uint8_t) (sy < 0 ? 0 : sx > 0 ? 1 : sy > 0 ? 2 : 3);
		else {
			d->goal_x = p->x;          /* stuck: it stays */
			d->goal_y = p->y;
			d->wait = STEP_BLANKS;
			return 0;
		}
	}
	p->x = (int16_t) (p->x + sx);
	p->y = (int16_t) (p->y + sy);
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
	for (int s = 1; s < E_MOVERS; s++) {
		int i = slot_npc[s];
		if (i < 0)
			continue;
		doing[i].wait = (int16_t) (doing[i].wait - dt);
		if (doing[i].wait > 0)
			continue;
		if (act(i))
			e_mover_set(s, npc[i].tf, npc[i].x, npc[i].y, npc[i].z);
	}
}
