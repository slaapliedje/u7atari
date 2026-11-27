/* What NPCs carry (engine.h, e_held_): the items of NPC.DAT's contents, kept as a table, each
 * in an NPC or in another (a bag, a backpack), or loose (made, or taken up, and not yet put
 * anywhere: the original's item being dragged).
 *
 * The rules are the original's: a counted item (type class 3) keeps its count in its quality
 * byte's low 7 bits (the top: okay to take); the classes with a quality (2, 6, 7, 11, 12, 13)
 * keep it whole; the rest are one, without. What a locked chest (522) or sealed box (798) holds
 * is not counted. An NPC carries at most twice its strength in stones (weights in tenths, a
 * stack its count times its type's, a tenth of that for coins and reagents, from WGTVOL.DAT);
 * a container takes what its bulk leaves room for (the bulks of what it holds, without nesting).
 *
 * Also in the world's containers (E_HELD_WORLD), read in when first opened (engine.c).
 *
 * Not the original's: an item given to an NPC goes in its first container with room, else on
 * the NPC (the original fills its hands and slots first, as READY.DAT says); on the NPC itself
 * at most HELD_ON items. Nothing shows where (no paperdoll yet), so only that is changed.
 */
#include <string.h>

#include "engine.h"

#define HELD_FREE (-32768)
#define HELD_ON 12              /* items an NPC holds outside its containers */
#define LOCKED_CHEST 522
#define SEALED_BOX 798
#define GOLD_COIN 644
#define REAGENT 842

static struct Held {
	uint16_t tf;
	uint8_t quality;
	int16_t in;                 /* a held item's index, E_HELD_ON(n), E_HELD_LOOSE or free */
} held[E_HELD_MOST];
static int nheld;               /* the table's use: none above it */
static uint8_t wgtvol[2048];    /* each type's weight and bulk */
static uint8_t wgtvol_ok;

static int cls(int i)
{
	return e_shape_class(held[i].tf & 0x3ff);
}

static int has_quality(int c)
{
	return c == 2 || c == 6 || c == 7 || c == 11 || c == 12 || c == 13;
}

void e_held_clear(void)
{
	for (int i = 0; i < E_HELD_MOST; i++)
		held[i].in = HELD_FREE;
	nheld = 0;
	if (!wgtvol_ok)
		wgtvol_ok = e_read_file("STATIC", "WGTVOL.DAT", wgtvol, sizeof wgtvol) == (int32_t) sizeof wgtvol;
}

int e_held_add(uint16_t tf, uint8_t quality, int in)
{
	for (int i = 0; i < E_HELD_MOST; i++)
		if (held[i].in == HELD_FREE) {
			held[i].tf = tf;
			held[i].quality = quality;
			held[i].in = (int16_t) in;
			if (i >= nheld)
				nheld = i + 1;
			return i;
		}
	return -1;
}

int e_held_ok(int i)
{
	return i >= 0 && i < nheld && held[i].in != HELD_FREE;
}

int e_held_in(int i)
{
	return e_held_ok(i) ? held[i].in : HELD_FREE;
}

uint16_t e_held_tf(int i)
{
	return e_held_ok(i) ? held[i].tf : 0;
}

void e_held_set_tf(int i, uint16_t tf)
{
	if (e_held_ok(i))
		held[i].tf = tf;
}

int e_held_quality(int i)
{
	return e_held_ok(i) && has_quality(cls(i)) ? held[i].quality : 0;
}

int e_held_set_quality(int i, int q)
{
	if (!e_held_ok(i) || !has_quality(cls(i)))
		return 0;
	held[i].quality = (uint8_t) q;
	return 1;
}

int e_held_quantity(int i)
{
	return !e_held_ok(i) ? 0 : cls(i) == 3 ? held[i].quality & 0x7f : 1;
}

int e_held_frame_counts(int type)
{
	switch (type) {
	case 417: case 554: case 556: case 558: case 560: case 568: case 722: case 723: case 948:
		return 24;              /* arrows, bolts: from frame 24 */
	case 581: case 627: case 644:
		return 0;               /* ammunition, lockpicks, gold */
	}
	return -1;
}

int e_held_set_quantity(int i, int n)
{
	if (!e_held_ok(i) || cls(i) != 3)
		return 0;
	held[i].quality = (uint8_t) ((held[i].quality & 0x80) | (n & 0x7f));
	/* (as the original's Item_setQuantity: some show how many by their frame) */
	int f = e_held_frame_counts(held[i].tf & 0x3ff);
	if (f >= 0 && n > 0) {
		f += n <= 6 ? n - 1 : n <= 12 ? 6 : 7;
		held[i].tf = (uint16_t) ((held[i].tf & ~(31u << 10)) | (uint16_t) f << 10);
	}
	return 1;
}

int e_held_owner(int i)
{
	int in = e_held_root(i);

	return E_HELD_IS_NPC(in) ? E_HELD_ON(0) - in : -1;
}

int e_held_root(int i)
{
	for (int n = 0; e_held_ok(i) && n < E_HELD_MOST; n++) {
		int in = held[i].in;
		if (in < 0)
			return in;
		i = in;
	}
	return HELD_FREE;
}

/* Whether i is (deep) in "in" and counted: no locked chest or sealed box between. */
static int counted_in(int i, int in)
{
	for (int n = 0; n < E_HELD_MOST; n++) {
		int up = held[i].in;
		if (up == in)
			return 1;
		if (up < 0 || !e_held_ok(up))
			return 0;
		int type = held[up].tf & 0x3ff;
		if (type == LOCKED_CHEST || type == SEALED_BOX)
			return 0;
		i = up;
	}
	return 0;
}

static int matches(int i, int type, int quality, int frame)
{
	return (type < 0 || (held[i].tf & 0x3ff) == type)
		&& (quality < 0 || e_held_quality(i) == quality)
		&& (frame < 0 || (held[i].tf >> 10 & 31) == frame);
}

int e_held_find(int in, int type, int quality, int frame, int after)
{
	for (int i = after + 1; i < nheld; i++)
		if (held[i].in != HELD_FREE && matches(i, type, quality, frame) && counted_in(i, in))
			return i;
	return -1;
}

int e_held_count(int in, int type, int quality, int frame)
{
	int n = 0;

	for (int i = e_held_find(in, type, quality, frame, -1); i >= 0;
		i = e_held_find(in, type, quality, frame, i))
		n += e_held_quantity(i);
	return n;
}

static int weight_of(int i)
{
	int type = held[i].tf & 0x3ff, w = wgtvol[type * 2];

	if (cls(i) == 3) {
		w *= e_held_quantity(i);
		if (type == GOLD_COIN || type == REAGENT)
			w /= 10;
		if (w == 0)
			w = 1;
	}
	return w;
}

/* What all in "in" weighs (deep). */
static int weight_in(int in)
{
	int w = 0;

	for (int i = 0; i < nheld; i++)
		if (held[i].in != HELD_FREE && i != in) {
			for (int up = held[i].in, n = 0; n < E_HELD_MOST; up = held[up].in, n++) {
				if (up == in) {
					w += weight_of(i);
					break;
				}
				if (up < 0 || !e_held_ok(up))
					break;
			}
		}
	return w;
}

static int bulk_in(int in)
{
	int b = 0;

	for (int i = 0; i < nheld; i++)
		if (held[i].in == in)
			b += wgtvol[(held[i].tf & 0x3ff) * 2 + 1];
	return b;
}

/* Whether container "in" (of type) has room for i's bulk. */
static int holds_bulk(int type, int in, int i)
{
	int room = wgtvol[type * 2 + 1];

	return room == 0 || wgtvol[(held[i].tf & 0x3ff) * 2 + 1] + bulk_in(in) < room;
}

int e_held_place(int i, int in, int strength)
{
	if (!e_held_ok(i) || i == in)
		return 2;
	int owner = E_HELD_IS_NPC(in) ? E_HELD_ON(0) - in : in >= 0 ? e_held_owner(in) : -1;
	if (E_HELD_IS_WORLD(in)) {
		/* into the world's container: not locked, with room */
		struct EThing t;
		if (!e_ref_thing(E_HELD_WORLD(0) - in + 1, &t))
			return 3;
		int type = t.tf & 0x3ff;
		if (type == LOCKED_CHEST || type == SEALED_BOX)
			return 3;
		if (!holds_bulk(type, in, i))
			return 4;
	} else if (in > E_HELD_LOOSE) {
		/* into a container: not one it holds, not locked, with room */
		if (!e_held_ok(in))
			return 3;
		for (int up = in, n = 0; up >= 0 && n < E_HELD_MOST; up = held[up].in, n++)
			if (up == i)
				return 2;
		int type = held[in].tf & 0x3ff;
		if (type == LOCKED_CHEST || type == SEALED_BOX)
			return 3;
		if (!holds_bulk(type, in, i))
			return 4;
	} else if (E_HELD_IS_NPC(in)) {
		/* to an NPC: its first container with room, else on it */
		int bag = -1, on = 0;
		for (int k = 0; k < nheld; k++)
			if (held[k].in == in) {
				int type = held[k].tf & 0x3ff;
				on++;
				if (bag < 0 && e_shape_class(type) == 6 && type != LOCKED_CHEST
					&& type != SEALED_BOX && holds_bulk(type, k, i))
					bag = k;
			}
		if (bag < 0 && on >= HELD_ON)
			return 3;
		if (bag >= 0)
			in = bag;
	}
	if (owner >= 0 && in != E_HELD_LOOSE && e_held_owner(i) != owner) {
		int adding = weight_of(i) + weight_in(i);
		if (adding + weight_in(E_HELD_ON(owner)) > strength * 2 * 10)
			return 1;
	}
	held[i].in = (int16_t) in;
	return 0;
}

void e_held_remove(int i)
{
	if (!e_held_ok(i))
		return;
	/* what it holds goes with it */
	for (int k = 0; k < nheld; k++)
		if (held[k].in == i)
			e_held_remove(k);
	held[i].in = HELD_FREE;
	while (nheld > 0 && held[nheld - 1].in == HELD_FREE)
		nheld--;
}

int e_held_used(void)
{
	int n = 0;

	for (int i = 0; i < nheld; i++)
		n += held[i].in != HELD_FREE;
	return n;
}

int e_held_section(int k, struct ESection *s)
{
	if (k != 0)
		return 0;
	memcpy(s->tag, "HELD", 4);
	s->size = sizeof held[0];
	s->count = (uint32_t) nheld;
	s->most = E_HELD_MOST;
	s->data = held;
	return 1;
}

void e_held_loaded(void)
{
	nheld = (int) e_save_count("HELD");
	for (int i = nheld; i < E_HELD_MOST; i++)
		held[i].in = HELD_FREE;
}
