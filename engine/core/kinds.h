/* What world.c (the clock, the periods, walking) and kinds.c (what each kind of schedule does)
 * share. Not part of the engine's interface (engine.h). */
#ifndef KINDS_H
#define KINDS_H

#include "engine.h"

#define MOST_NPCS 400
#define STEP_BLANKS 10          /* an NPC walks a cell every 10 blanks: 6 a second */
#define TICK_BLANKS 10          /* the original's schedule ticks (its waits), a step's time */

/* the kinds (schedule types, the original's work types) */
#define K_COMBAT 0
#define K_HOR_PACE 1
#define K_VER_PACE 2
#define K_TALK 3
#define K_DANCE 4
#define K_EAT 5
#define K_FARM 6
#define K_SHOP 7
#define K_MINER 8
#define K_HOUND 9
#define K_STAND 10
#define K_LOITER 11
#define K_WANDER 12
#define K_SMITH 13
#define K_SLEEP 14
#define K_WAIT 15
#define K_MAJOR_SIT 16
#define K_GRAZE 17
#define K_BAKE 18
#define K_SEW 19
#define K_SHY 20
#define K_LAB 21
#define K_THIEF 22
#define K_WAITER 23
#define K_KIDS 25
#define K_EAT_INN 26
#define K_DUEL 27
#define K_PREACH 28
#define K_PATROL 29
#define K_DESK 30
#define K_GOTO_ITEM 32
#define K_SIT 35
#define K_READ 37
#define K_PAY_RESPECTS 38
#define K_ARCHERY 40
#define K_FENCING 41
#define K_READY_HAND 42
#define K_PAUSE 43
#define K_MOVE_ITEM 44
#define K_TAG 46
#define K_GOTO_PLACE 49
#define K_GOTO_REM_L 50
#define K_CHECK_AREA 52
#define K_GRAB_ITEM 33
#define K_DROP_ITEM 34
#define K_FILL_BUCKET 36
#define K_FILL_TROUGH 39
#define K_CHECK_LIGHT 45
#define K_CHECK_SHUTTERS 51

/* a level of what an NPC does: a kind pushed over another runs, then the other goes on (as the
 * original's schedule stack): its state, its two arguments, a counter */
struct Level {
	uint8_t kind, state;
	int16_t x, y;
	uint8_t counter;
};
#define LEVELS 4

#define W_NONE 0
#define W_PATH 1                /* a route to a goal near (start_path) */
#define W_PLACE 2               /* to its schedule's place, near or far */

struct Doing {
	struct Level level[LEVELS];
	uint8_t depth;              /* levels in use; level[depth - 1] runs */
	int16_t result;             /* how the last walk or level pushed ended: < 0 blocked (a walk
	                             * to an item: its type) */
	uint8_t dir;                /* facing 0-3 (north, east, south, west) */
	uint8_t pose;               /* the frame shown (0-15: 0 standing, 10 sitting, ...) */
	uint8_t face8;              /* the way (0-7) to face at the end of a walk */
	uint8_t walking;            /* W_ */
	uint8_t toggle;
	int16_t value;              /* the original's scheduleValue */
	int16_t mark_x, mark_y;     /* where something began (the original's sVr) */
	int16_t place_x, place_y;   /* its schedule's place */
	int16_t goal_x, goal_y;     /* where it is walking */
	uint8_t goal_z, limit;
	uint16_t steps;
	int16_t wait;               /* blanks before it does the next thing */
	int8_t door;                /* a door it opened and is to shut behind it, or -1 */
	uint8_t nearest;            /* the nearest it has been to that door */
	int8_t route;               /* the route it walks, or -1 */
	uint8_t no_route, retry, replans;
	uint16_t held[4];           /* the items it carries (type-frame words; 0: none) */
	struct EThing use;          /* the bed it sleeps in */
	struct EThing cover;        /* the bed's cover it turned over it (tf 0: none) */
	int16_t spot_x, spot_y;     /* where it got into bed */
	uint8_t spot_z;
	uint8_t pc, script_len, loops, mark;
	uint8_t script[30];         /* a script of poses and waits (kinds.c's S_ ops) */
};

extern struct Doing doing[MOST_NPCS];
extern struct ENpc *npc;
extern int nnpc;
extern int16_t me_x, me_y;      /* the avatar */
extern int planned;             /* a route planned this step: the rest wait for the next */
extern int searched;            /* an item looked for this step (a bed, a chair): likewise */
extern int16_t slot_npc[E_MOVERS];

int rnd(int n);
int far_of(int x0, int y0, int x1, int y1);
int in_near_window(int x, int y);
int in_slot(int i);

/* world.c: walking. A route to x, y, z planned in at most limit steps and walked: 0 when it is
 * (the level's state goes on when it ends: d->result 0 there, -1 not), 1 when none could be
 * planned this step (ask again), 2 when there is none. */
int start_path(int i, int x, int y, int z, int limit);
/* To its schedule's place (planned and walked as world.c does it; put there when far). */
void start_place(int i);

/* kinds.c */
int kind_tick(int i);           /* the next thing it does: 1 when its look or place changed */
void kind_walked(int i, int ok);        /* a walk has ended */
void kind_lost(int i);          /* no way to its place: it wanders */
void kind_period(int i, int kind);      /* its next entry has come round */
void kind_init(int i);          /* as the game was left */
void kind_things(int blanks);   /* the items' scripts (a bellows, a loom), blanks on */
int avatar_index(void);         /* the avatar's NPC index, or -1 */

/* usecode.c */
void uc_world_init(void);       /* the party: the avatar alone */

#endif
