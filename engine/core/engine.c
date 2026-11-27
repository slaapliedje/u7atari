/* The engine test's core: the world view from the original's files (engine.h, engine/README.md).
 *
 * What is done per frame is kept small. The static layer is a ring of 4 by 3 chunk areas, 512 by
 * 384 pixels: each area holds a chunk's terrain and every static object over it, drawn once, in
 * an order worked out among those objects alone. The areas the view covers are drawn when it
 * reaches them (at the start); the next ones ahead of it a little each frame, before it does.
 * Each frame copies the ring under the view (one to four blits), and draws the movers with,
 * clipped to each, the statics in front of it.
 *
 * World: 3072 by 3072 cells of 8 by 8 pixels; 16 by 16 cells a chunk, 16 by 16 chunks a region,
 * 12 by 12 regions. An object stands with its hot spot at the bottom right of its footprint, cell
 * (x, y), and is drawn lifted 4 pixels up and left for each level of z.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "engine.h"

#define TILE_TYPES 150          /* types below are flat 8 by 8 tiles */
#define REGION_SLOTS 4         /* the chunks in use touch at most 2 by 2 */
#define REGION_ITEMS 4096
#ifndef CHUNK_SLOTS
#define CHUNK_SLOTS 36         /* the ring's 4 by 3 areas and those around them, 6 by 5 */
#endif
#define CHUNK_OBJECTS 96       /* cells with an object shape */
#define CHUNK_STATICS 224      /* its objects, all told */
#define AREA_OBJECTS 512       /* objects over one area */
#define AREA_EDGES 4096
/* How far a shape reaches from its hot spot: 63 pixels left or up (frame headers), more 4 a level
 * up to 15 lifted; 8 right or down. So only the chunk's own objects and those of the 8 around it
 * can draw over a chunk's area. */
#define REACH_BEFORE 123
#define REACH_AFTER 8
#define CHUNK_BINS 8
#define CHUNK_BIN_ENTRIES 1024

struct EStats e_stats;

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
static inline int16_t rds16(const uint8_t *p) { return (int16_t) rd16(p); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

/* ---- memory: a bump store; the shape cache takes what is left ---- */

static uint8_t *mem_next, *mem_end;

static void *take(uint32_t n)
{
	uint8_t *p = mem_next;

	n = (n + 7) & ~(uint32_t) 7;
	if ((uint32_t) (mem_end - mem_next) < n)
		return NULL;
	mem_next += n;
	return p;
}

/* ---- files ---- */

static char prefix_path[200];

static int16_t open_file(const char *dir, const char *name)
{
	char path[240];

	snprintf(path, sizeof path, "%s%s/%s", prefix_path, dir, name);
	return plat_file_open(path, PLAT_FILE_READ);
}

static int read_at(int16_t f, uint32_t at, void *buf, uint32_t n)
{
	return plat_file_seek(f, (int32_t) at, PLAT_SEEK_SET) == (int32_t) at
		&& plat_file_read(f, buf, (int32_t) n) == (int32_t) n;
}

/* A flex file: an 80-byte title, its magic and count, then (offset, size) per entry at 0x80. */
struct Flex {
	int16_t f;
	uint16_t count;
	uint32_t *table;            /* offset, size, in host order */
};

static int flex_open(struct Flex *x, const char *dir, const char *name)
{
	uint8_t head[8], *raw;

	if ((x->f = open_file(dir, name)) < 0 || !read_at(x->f, 0x50, head, 8))
		return -1;
	x->count = (uint16_t) rd32(head + 4);
	if ((x->table = take(x->count * 8u)) == NULL)
		return -1;
	raw = (uint8_t *) x->table;
	if (!read_at(x->f, 0x80, raw, x->count * 8u))
		return -1;
	for (uint16_t i = 0; i < x->count * 2u; i++)
		x->table[i] = rd32(raw + i * 4);
	return 0;
}

/* ---- the shapes' facts (TFA.DAT, type.h's bits) ---- */

struct Info {
	uint8_t height, fx, fy, cls, flags;
};
#define INFO_ANIMATED 1
#define INFO_TRANSLUCENT 2
#define INFO_SOLID 4
#define INFO_DOOR 8
#define CLASS_NONE 0

static struct Info info[1024];
static uint8_t ceiling = 15;    /* statics and movers above it are not drawn (e_follow) */
static uint8_t palette[768];
static uint16_t *map;           /* chunk number of each of the 192 by 192 chunks */
static struct Flex shapes;
static int16_t chunk_file;
static int16_t chunk_data = -1;  /* the dataset's chunks (engine.h), or -1 */

/* ---- the shape cache: a ring, the oldest shapes giving way; the movers' kept below it ---- */

#define SHAPE_QUEUE 1024
static const uint8_t *shape_at[1024] __attribute__((aligned(8)));  /* the GPU reads it */
static uint8_t *pool, *pool_next, *pool_pinned, *pool_end;
static struct Resident {
	uint16_t type;
	uint8_t *start;
} queue[SHAPE_QUEUE];
static uint16_t queue_head, queue_count;

static uint16_t evictions;      /* counts shapes given way */

static void evict_oldest(void)
{
	evictions++;
	shape_at[queue[queue_head].type] = NULL;
	queue_head = (uint16_t) ((queue_head + 1) % SHAPE_QUEUE);
	queue_count--;
}

/* ---- converting a shape's frames to the engine's runs (engine.h) ---- */

static uint8_t converted;           /* shapes is the dataset's: object shapes made already */
static uint8_t *raw_shape;          /* an object shape as read, before it is converted */
static uint32_t raw_shape_size;
/* A shape, read in if need be: a flat's frames as they are, an object's converted; NULL when it
 * has none or cannot fit. On a backend that draws as the CPU goes on, reading in waits for the
 * drawing first (its plat_file_read): a shape given way to may still be being drawn from. */
/* Room at pool_next for size bytes: the oldest shapes in the way give way. */
static void make_room(uint32_t size)
{
	if ((uint32_t) (pool_end - pool_next) < size) {
		/* past the end: the oldest there give way, and on from the start */
		while (queue_count > 0 && queue[queue_head].start >= pool_next)
			evict_oldest();
		pool_next = pool_pinned;
	}
	while (queue_count > 0 && queue[queue_head].start >= pool_next
		&& queue[queue_head].start < pool_next + size)
		evict_oldest();
	if (queue_count == SHAPE_QUEUE)
		evict_oldest();
}

static const uint8_t *load_shape(uint16_t type);

static const uint8_t *shape(uint16_t type)
{

	if (type >= shapes.count || shape_at[type] != NULL)
		return type < shapes.count ? shape_at[type] : NULL;
	uint32_t t0 = eb_clock();
	const uint8_t *got = load_shape(type);
	e_stats.load_time += eb_clock() - t0;
	return got;
}

static const uint8_t *load_shape(uint16_t type)
{
	uint32_t at, raw, size;

	at = shapes.table[type * 2];
	raw = shapes.table[type * 2 + 1];
	if (raw == 0)
		return NULL;
	if (type >= TILE_TYPES && converted) {
		/* the dataset's: read as it is */
		size = (raw + 7) & ~(uint32_t) 7;
		if (size > (uint32_t) (pool_end - pool_pinned))
			return NULL;
		make_room(size);
		if (!read_at(shapes.f, at, pool_next, raw)
			|| (e_little_endian() && !e_swap_shape(pool_next, raw, 1))
			|| *(const uint32_t *) pool_next != raw)
			return NULL;
	} else if (type >= TILE_TYPES) {
		if (raw > raw_shape_size || raw < 8 || !read_at(shapes.f, at, raw_shape, raw))
			return NULL;
		uint32_t first = rd32(raw_shape + 4);
		if (rd32(raw_shape) != raw || first < 8 || first > raw || (first & 3))
			return NULL;
		/* made in one go where it goes, room made for more than any real shape needs (1.94
		 * times the original's at most); else measured first */
		uint32_t bound = (raw * 2 + 2 * E_CONVERT_SLACK + 7) & ~(uint32_t) 7;
		size = 0;
		if (bound <= (uint32_t) (pool_end - pool_pinned)) {
			make_room(bound);
			size = eb_convert(raw_shape, raw, pool_next, bound);
		}
		if (size == 0) {
			if ((size = eb_convert(raw_shape, raw, NULL, 0)) == 0)
				return NULL;
			size = (size + 7) & ~(uint32_t) 7;
			if (size > (uint32_t) (pool_end - pool_pinned))
				return NULL;
			make_room(size);
			eb_convert(raw_shape, raw, pool_next, size + E_CONVERT_SLACK);
		}
		size = (size + 7) & ~(uint32_t) 7;
	} else {
		size = (raw + 7) & ~(uint32_t) 7;
		if (size > (uint32_t) (pool_end - pool_pinned))
			return NULL;
		make_room(size);
		if (!read_at(shapes.f, at, pool_next, raw))
			return NULL;
	}
	shape_at[type] = pool_next;
	queue[(queue_head + queue_count) % SHAPE_QUEUE].type = type;
	queue[(queue_head + queue_count) % SHAPE_QUEUE].start = pool_next;
	queue_count++;
	pool_next += size;
	e_stats.shape_loads++;
	e_stats.shape_bytes = (uint32_t) (pool_end - pool_pinned);
	return shape_at[type];
}

/* An object shape's frame: its box (int16 right, left, above, below), its first run after it. */
static const int16_t *frame_of(uint16_t type, uint16_t frame)
{
	const uint32_t *s = (const uint32_t *) shape(type);

	if (s == NULL || type < TILE_TYPES)
		return NULL;
	if ((frame + 1) * 4u >= s[1])
		return NULL;
	return (const int16_t *) ((const uint8_t *) s + s[1 + frame]);
}

/* ---- regions: the fixed (U7IFIX) and movable (U7IREG) items of a region, per chunk ---- */

struct Item {
	uint16_t tf;                /* type, frame << 10, mirrored 0x8000 */
	uint8_t x, y;               /* cell in the region */
	uint8_t z;
	uint8_t quality;            /* a movable one's (a path marker's number of what to do) */
};

struct Region {
	int16_t id;
	uint16_t age;
	uint16_t start[257];
	struct Item *items;
};

static struct Region regions[REGION_SLOTS];
static uint16_t age_now;
static uint8_t *scratch;        /* a file's worth of bytes */
#define SCRATCH 32768

static void add_ifix(struct Region *r, uint16_t *n, const uint8_t *e, int count, int chunk)
{
	int bx = (chunk & 15) << 4, by = (chunk >> 4) << 4;

	for (int i = 0; i < count && *n < REGION_ITEMS; i++, e += 4) {
		struct Item *it = &r->items[(*n)++];
		it->x = (uint8_t) (bx + (e[0] >> 4));
		it->y = (uint8_t) (by + (e[0] & 15));
		it->z = e[1] & 15;
		it->tf = rd16(e + 2);
		it->quality = 0;
	}
}

static void doors_into(struct Region *r);

static struct Region *region(int id)
{
	struct Region *r = NULL, *old = &regions[0];
	char name[16];
	int16_t f;
	int32_t len;
	uint16_t n = 0;

	for (int i = 0; i < REGION_SLOTS; i++) {
		if (regions[i].id == id) {
			regions[i].age = ++age_now;
			return &regions[i];
		}
		if (regions[i].age < old->age)
			old = &regions[i];
	}
	r = old;
	r->id = (int16_t) id;
	r->age = ++age_now;
	uint32_t t_region = eb_clock();

	/* fixed items: a flex of 256 entries, one a chunk, of 4-byte records; and the movable ones:
	 * 256 trees, one a chunk, of records by length (6, 12, 18; 0 or 1 ends a level), whose
	 * containers' contents follow them a level down (not drawn) */
	uint16_t fixedStart[257];
	uint8_t *fixed = NULL;
	snprintf(name, sizeof name, "U7IFIX%02X", id);
	if (chunk_data < 0 && (f = open_file("STATIC", name)) >= 0) {
		len = plat_file_length(f);
		if (len > 0x80 + 256 * 8 && len <= SCRATCH && read_at(f, 0, scratch, (uint32_t) len))
			fixed = scratch;
		plat_file_close(f);
	}
	struct Item *items = r->items;
	uint16_t ireg = 0;
	static struct Item movable[REGION_ITEMS], merged[REGION_ITEMS];
	snprintf(name, sizeof name, "U7IREG%02X", id);
	uint16_t movableStart[257];
	memset(movableStart, 0, sizeof movableStart);
	if (fixed != NULL) {
		for (int c = 0; c < 256; c++) {
			fixedStart[c] = n;
			uint32_t at = rd32(fixed + 0x80 + c * 8), size = rd32(fixed + 0x84 + c * 8);
			if (size && at + size <= (uint32_t) len)
				add_ifix(r, &n, fixed + at, (int) (size / 4), c);
		}
		fixedStart[256] = n;
	} else {
		for (int c = 0; c <= 256; c++)
			fixedStart[c] = 0;
	}
	/* the fixed ones are now items[0..n); read the movable ones beside, then merge per chunk */
	/* the dataset's (a new game's, with its NPC.DAT) or the save's */
	if ((f = open_file("ENGINE", name)) >= 0 || (f = open_file("GAMEDAT", name)) >= 0) {
		len = plat_file_length(f);
		if (len > 0 && len <= SCRATCH && read_at(f, 0, scratch, (uint32_t) len)) {
			/* one run of records for the whole region, each in the chunk its cell is in; a
			 * container's contents follow it, ended by a 0 or 1 */
			static uint8_t in_chunk[REGION_ITEMS];
			int32_t i = 0;
			int depth = 0;
			while (i < len) {
				uint8_t l = scratch[i++];
				if (l == 0 || l == 1) {
					if (depth > 0)
						depth--;
					continue;
				}
				const uint8_t *d = scratch + i;
				i += l;
				if (i > len)
					break;
				if (depth == 0 && ireg < REGION_ITEMS) {
					struct Item *it = &movable[ireg];
					it->x = d[0];
					it->y = d[1];
					it->tf = rd16(d + 2);
					it->z = (l == 6 ? d[4] : d[9]) >> 4;
					it->quality = l == 6 ? d[5] : 0;
					in_chunk[ireg++] = (uint8_t) ((d[1] >> 4) * 16 + (d[0] >> 4));
				}
				/* contents follow only a type whose class has them (the original's
				 * CLASS_CONTENTS: containers, 6, and bodies, 12 and 13), not an egg's data */
				uint8_t cls = info[rd16(d + 2) & 0x3ff].cls;
				if (l >= 12 && rd16(d + 4) != 0 && (cls == 6 || cls == 12 || cls == 13))
					depth++;
			}
			/* by chunk, as they came within each (merged, below, lent for it) */
			struct Item *sorted = merged;
			for (uint16_t k = 0; k < ireg; k++)
				movableStart[in_chunk[k] + 1]++;
			for (int c = 0; c < 256; c++)
				movableStart[c + 1] = (uint16_t) (movableStart[c + 1] + movableStart[c]);
			for (uint16_t k = 0; k < ireg; k++)
				sorted[movableStart[in_chunk[k]]++] = movable[k];
			for (int c = 256; c > 0; c--)
				movableStart[c] = movableStart[c - 1];
			movableStart[0] = 0;
			memcpy(movable, sorted, ireg * sizeof *movable);
		}
		plat_file_close(f);
	}
	movableStart[256] = ireg;
	/* items: per chunk, its fixed ones then its movable ones */
	uint16_t m = 0;
	for (int c = 0; c < 256; c++) {
		r->start[c] = m;
		for (uint16_t k = fixedStart[c]; k < fixedStart[c + 1] && m < REGION_ITEMS; k++)
			merged[m++] = items[k];
		for (uint16_t k = movableStart[c]; k < movableStart[c + 1] && m < REGION_ITEMS; k++)
			merged[m++] = movable[k];
	}
	r->start[256] = m;
	memcpy(items, merged, m * sizeof *items);
	doors_into(r);
	e_stats.region_time += eb_clock() - t_region;
	return r;
}

/* ---- chunks: their cell words, and their static objects ---- */

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* A static object: its type-frame word, the cell of its hot spot, its level, and its picture's
 * rectangle in world pixels (inclusive). */
struct Static {
	uint16_t tf;
	int16_t x, y;
	uint8_t z, flat;
	int16_t r[4];               /* x0, y0, x1, y1 */
	uint8_t fx, fy, height;     /* its footprint: cells west and north of x, y; levels up */
	uint8_t solid;              /* solid, not a door: its levels make a ceiling (e_follow) */
};

/* A static's footprint, from its shape's facts (turned if it is). */
static void set_footprint(struct Static *s)
{
	const struct Info *in = &info[s->tf & 0x3ff];

	s->fx = s->tf & 0x8000 ? in->fy : in->fx;
	s->fy = s->tf & 0x8000 ? in->fx : in->fy;
	s->height = in->height;
	s->flat = in->height == 0;
	/* doors (the original keeps them apart) do not make a ceiling */
	s->solid = (in->flags & (INFO_SOLID | INFO_DOOR)) == INFO_SOLID;
}

struct Chunk {
	int16_t cx, cy;
	uint16_t age;
	uint8_t nobj, built;
	uint16_t nstatic;
	uint16_t nfixed;            /* of them, those in the bins (the rest looked through) */
	int16_t reach[4];           /* the box round all their pictures (x0, y0, x1, y1) */
	uint32_t flats[5];          /* the flat types its cells use, a bit each */
	uint8_t cells[512];
	uint8_t obj[CHUNK_OBJECTS]; /* cells with a shape of class none: objects at z 0 */
	struct Static *statics;     /* its objects: those cells', its fixed items, its movable ones */
	/* its statics by where their pictures lie: 8 by 8 bins of 32 pixels from 120 pixels left of
	 * and above the chunk, as far as its objects reach (REACH_BEFORE, REACH_AFTER) */
	uint16_t bin_start[CHUNK_BINS * CHUNK_BINS + 1];
	uint8_t *bin_list;
	/* collision, as the original's grid: per cell the levels solid statics take, those doors
	 * take, and whether the ground is solid (water, rock); made when first asked for */
	uint16_t *solid_at, *door_at;
	uint16_t ground[16];        /* a row each, a bit a cell (x & 15) */
	uint16_t *mover_at;         /* the levels solid movers take, per cell (stamped, below) */
	uint8_t grid_ok;
};

static struct Chunk *chunks;
static uint8_t *chunk_slot;     /* per chunk of the world, its slot + 1, or 0 */
static uint8_t movers_dirty = 1; /* a mover has moved: their levels to be stamped again */
static uint16_t chunk_gen[CHUNK_SLOTS]; /* per slot, counts the chunks given it: statics moved */

static int chunk_record(struct Chunk *k);

/* The chunk's cells, read in if need be, the least lately used giving way. */
static struct Chunk *chunk(int cx, int cy)
{
	struct Chunk *k, *old = &chunks[0];
	uint8_t slot = chunk_slot[cy * 192 + cx];

	if (slot != 0) {
		k = &chunks[slot - 1];
		k->age = ++age_now;
		return k;
	}
	for (int i = 1; i < CHUNK_SLOTS; i++)
		if (chunks[i].age < old->age)
			old = &chunks[i];
	k = old;
	if (k->cx >= 0)
		chunk_slot[k->cy * 192 + k->cx] = 0;
	chunk_gen[k - chunks]++;
	chunk_slot[cy * 192 + cx] = (uint8_t) (k - chunks + 1);
	k->cx = (int16_t) cx;
	k->cy = (int16_t) cy;
	k->age = ++age_now;
	k->nobj = 0;
	k->built = 0;
	k->grid_ok = 0;
	memset(k->mover_at, 0, 256 * 2);
	movers_dirty = 1;
	k->nstatic = 0;
	memset(k->flats, 0, sizeof k->flats);
	uint32_t t_cells = eb_clock();
	if (!read_at(chunk_file, map[cy * 192 + cx] * 512u, k->cells, 512))
		memset(k->cells, 0, 512);
	e_stats.cells_time += eb_clock() - t_cells;
	if (chunk_data >= 0) {
		/* the dataset's record: what the cells hold, and the fixed statics, binned */
		if (!chunk_record(k)) {
			k->nstatic = k->nfixed = 0;
			memset(k->bin_start, 0, sizeof k->bin_start);
		}
		return k;
	}
	for (int i = 0; i < 256; i++) {
		uint16_t type = rd16(k->cells + i * 2) & 0x3ff;
		if (type < TILE_TYPES)
			k->flats[type >> 5] |= 1UL << (type & 31);
		else if (info[type].cls == CLASS_NONE && k->nobj < CHUNK_OBJECTS)
			k->obj[k->nobj++] = (uint8_t) i;
	}
	return k;
}

/* The picture's rectangle of an object at a cell and level, from its frame's header; 0 when the
 * shape or frame is missing. */
static int picture_rect(uint16_t tf, int x, int y, int z, int16_t r[4])
{
	const int16_t *f = frame_of(tf & 0x3ff, (tf >> 10) & 31);
	int16_t right, left, above, below, px, py;

	if (f == NULL)
		return 0;
	right = f[0];
	left = f[1];
	above = f[2];
	below = f[3];
	if (tf & 0x8000) {
		int16_t t = left; left = above; above = t;
		t = right; right = below; below = t;
	}
	px = (int16_t) (x * 8 + 7 - z * 4);
	py = (int16_t) (y * 8 + 7 - z * 4);
	r[0] = (int16_t) (px - left);
	r[1] = (int16_t) (py - above);
	r[2] = (int16_t) (px + right);
	r[3] = (int16_t) (py + below);
	return 1;
}

/* A shape's box over all its frames, from the hot spot: right, left, above, below (as a frame's
 * header has them, unflipped); worked out once a shape. */
static int16_t (*shape_box)[4];
static uint8_t *shape_boxed;

static const int16_t *box_of(uint16_t type)
{
	if (!shape_boxed[type]) {
		const uint32_t *s = (const uint32_t *) shape(type);
		int16_t *b = shape_box[type];
		b[0] = b[1] = b[2] = b[3] = 0;
		if (s != NULL && type >= TILE_TYPES) {
			uint32_t frames = (s[1] - 4) / 4;
			for (uint32_t f = 0; f < frames; f++) {
				const int16_t *h = (const int16_t *) ((const uint8_t *) s + s[1 + f]);
				for (int i = 0; i < 4; i++)
					if (h[i] > b[i])
						b[i] = h[i];
			}
		}
		shape_boxed[type] = 1;
	}
	return shape_box[type];
}

/* The chunk with its statics listed, in the chunk's own order. */
/* ---- the dataset's chunks (engine.h): a chunk's fixed statics, binned ---- */

static inline uint16_t be16(const uint8_t *p) { return (uint16_t) (p[0] << 8 | p[1]); }
static inline void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t) (v >> 8);
	p[1] = (uint8_t) v;
}

/* Into k: its record's statics and bins. 0 when it cannot be read. */
static int chunk_record(struct Chunk *k)
{
	uint8_t at[8];
	uint32_t idx = (uint32_t) k->cy * 192 + (uint32_t) k->cx, from, size;

	if (!read_at(chunk_data, E_CHUNKS_TABLE + idx * 4, at, 8))
		return 0;
	from = (uint32_t) be16(at) << 16 | be16(at + 2);
	size = ((uint32_t) be16(at + 4) << 16 | be16(at + 6)) - from;
	if (size < E_CHUNK_HEAD || size > SCRATCH || !read_at(chunk_data, from, scratch, size))
		return 0;
	uint16_t n = be16(scratch), nl = be16(scratch + 2);
	const uint8_t *p = scratch + E_CHUNK_HEAD;
	if (n > CHUNK_STATICS || nl > CHUNK_BIN_ENTRIES
		|| size < E_CHUNK_HEAD + n * (uint32_t) E_CHUNK_STATIC + nl)
		return 0;
	for (int i = 0; i < 5; i++)
		k->flats[i] = (uint32_t) be16(scratch + 4 + i * 4) << 16 | be16(scratch + 6 + i * 4);
	for (int b = 0; b <= CHUNK_BINS * CHUNK_BINS; b++)
		k->bin_start[b] = be16(scratch + 24 + b * 2);
	if (!e_little_endian())
		memcpy(k->statics, p, n * (uint32_t) E_CHUNK_STATIC);
	else
		for (uint16_t i = 0; i < n; i++, p += E_CHUNK_STATIC) {
			struct Static *s = &k->statics[i];
			s->tf = be16(p);
			s->x = (int16_t) be16(p + 2);
			s->y = (int16_t) be16(p + 4);
			s->z = p[6];
			s->flat = p[7];
			for (int j = 0; j < 4; j++)
				s->r[j] = (int16_t) be16(p + 8 + j * 2);
			s->fx = p[16];
			s->fy = p[17];
			s->height = p[18];
			s->solid = p[19];
		}
	memcpy(k->bin_list, scratch + E_CHUNK_HEAD + n * (uint32_t) E_CHUNK_STATIC, nl);
	k->nstatic = k->nfixed = n;
	return 1;
}

static struct Chunk *chunk_statics_now(int cx, int cy);

uint32_t e_chunk_record(int cx, int cy, uint8_t *out)
{
	const struct Chunk *k = chunk_statics_now(cx, cy);
	uint16_t nl = k->bin_start[CHUNK_BINS * CHUNK_BINS];
	uint8_t *p = out + E_CHUNK_HEAD;

	if (nl > CHUNK_BIN_ENTRIES)
		nl = CHUNK_BIN_ENTRIES;
	put_be16(out, k->nstatic);
	put_be16(out + 2, nl);
	for (int i = 0; i < 5; i++) {
		put_be16(out + 4 + i * 4, (uint16_t) (k->flats[i] >> 16));
		put_be16(out + 6 + i * 4, (uint16_t) k->flats[i]);
	}
	for (int b = 0; b <= CHUNK_BINS * CHUNK_BINS; b++)
		put_be16(out + 24 + b * 2, k->bin_start[b]);
	for (uint16_t i = 0; i < k->nstatic; i++, p += E_CHUNK_STATIC) {
		const struct Static *s = &k->statics[i];
		put_be16(p, s->tf);
		put_be16(p + 2, (uint16_t) s->x);
		put_be16(p + 4, (uint16_t) s->y);
		p[6] = s->z;
		p[7] = s->flat;
		for (int j = 0; j < 4; j++)
			put_be16(p + 8 + j * 2, (uint16_t) s->r[j]);
		p[16] = s->fx;
		p[17] = s->fy;
		p[18] = s->height;
		p[19] = s->solid;
	}
	memcpy(p, k->bin_list, nl);
	return (uint32_t) (p - out) + nl;
}

static struct Chunk *chunk_statics(int cx, int cy)
{
	uint8_t slot = chunk_slot[cy * 192 + cx];

	if (slot != 0 && chunks[slot - 1].built) {
		chunks[slot - 1].age = ++age_now;
		return &chunks[slot - 1];
	}
	uint32_t t0 = eb_clock();
	struct Chunk *k = chunk_statics_now(cx, cy);
	e_stats.chunk_time += eb_clock() - t0;
	return k;
}

/* The box round all of a chunk's statics' pictures (empty: x0 > x1). */
static void chunk_reach(struct Chunk *k)
{
	int16_t x0 = 32767, y0 = 32767, x1 = -32768, y1 = -32768;

	for (uint16_t i = 0; i < k->nstatic; i++) {
		const int16_t *r = k->statics[i].r;
		if (r[0] < x0) x0 = r[0];
		if (r[1] < y0) y0 = r[1];
		if (r[2] > x1) x1 = r[2];
		if (r[3] > y1) y1 = r[3];
	}
	k->reach[0] = x0;
	k->reach[1] = y0;
	k->reach[2] = x1;
	k->reach[3] = y1;
}

static struct Chunk *chunk_statics_now(int cx, int cy)
{
	struct Chunk *k = chunk(cx, cy);

	if (k->built)
		return k;
	struct Region *r = region((cy >> 4) * 12 + (cx >> 4));
	int c = (cy & 15) * 16 + (cx & 15);
	int rx = (cx >> 4) << 8, ry = (cy >> 4) << 8;
	int n = k->nobj + r->start[c + 1] - r->start[c];

	/* with the dataset, the fixed ones are in (chunk()), and the region gave the movable ones
	 * alone; the chunk's own objects are among the fixed */
	k->nstatic = chunk_data >= 0 ? k->nfixed : 0;
	for (int i = 0; i < n && k->nstatic < CHUNK_STATICS; i++) {
		struct Static *s = &k->statics[k->nstatic];
		if (i < k->nobj) {
			int cell = k->obj[i];
			s->tf = rd16(k->cells + cell * 2);
			s->x = (int16_t) (cx * 16 + (cell & 15));
			s->y = (int16_t) (cy * 16 + (cell >> 4));
			s->z = 0;
		} else {
			struct Item *it = &r->items[r->start[c] + i - k->nobj];
			s->tf = it->tf;
			s->x = (int16_t) (rx + it->x);
			s->y = (int16_t) (ry + it->y);
			s->z = it->z;
		}
		if ((s->tf & 0x3ff) < TILE_TYPES || !picture_rect(s->tf, s->x, s->y, s->z, s->r))
			continue;
		/* the markers the original blanks (HideMarkerShapes: eggs, path markers, ...): kept, to
		 * be found, but drawn nowhere */
		uint16_t type = s->tf & 0x3ff;
		if (type == 275 || type == 200 || type == 961 || type == 607) {
			s->r[0] = s->r[1] = 32767;
			s->r[2] = s->r[3] = -32768;
		}
		set_footprint(s);
		k->nstatic++;
	}
	if (chunk_data >= 0) {
		chunk_reach(k);
		k->built = 1;
		return k;
	}
	k->nfixed = k->nstatic;
	/* the bins: counts, starts, the lists, the starts back */
	int16_t ox = (int16_t) (cx * 128 - 120), oy = (int16_t) (cy * 128 - 120);
	memset(k->bin_start, 0, sizeof k->bin_start);
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1) {
			uint16_t sum = 0;
			for (int b = 0; b < CHUNK_BINS * CHUNK_BINS; b++) {
				uint16_t c = k->bin_start[b];
				k->bin_start[b] = sum;
				sum = (uint16_t) (sum + c);
			}
			k->bin_start[CHUNK_BINS * CHUNK_BINS] = sum;
		}
		for (uint16_t i = 0; i < k->nstatic; i++) {
			const int16_t *r = k->statics[i].r;
			int bx0 = clampi((r[0] - ox) >> 5, 0, CHUNK_BINS - 1);
			int bx1 = clampi((r[2] - ox) >> 5, 0, CHUNK_BINS - 1);
			int by0 = clampi((r[1] - oy) >> 5, 0, CHUNK_BINS - 1);
			int by1 = clampi((r[3] - oy) >> 5, 0, CHUNK_BINS - 1);
			for (int by = by0; by <= by1; by++)
				for (int bx = bx0; bx <= bx1; bx++) {
					uint16_t *b = &k->bin_start[by * CHUNK_BINS + bx];
					if (pass == 0)
						(*b)++;
					else if (*b < CHUNK_BIN_ENTRIES)
						k->bin_list[(*b)++] = (uint8_t) i;
				}
		}
	}
	for (int b = CHUNK_BINS * CHUNK_BINS; b > 0; b--)
		k->bin_start[b] = k->bin_start[b - 1];
	k->bin_start[0] = 0;
	chunk_reach(k);
	k->built = 1;
	return k;
}

/* The chunk's statics whose pictures overlap q (x0, y0, x1, y1), each once, standing or flat as
 * asked, by their place in the chunk's list; how many, up to max. */
static int chunk_find(const struct Chunk *k, const int16_t q[4], int flat, uint8_t *out, int max)
{
	int16_t ox = (int16_t) (k->cx * 128 - 120), oy = (int16_t) (k->cy * 128 - 120);
	int bx0 = clampi((q[0] - ox) >> 5, 0, CHUNK_BINS - 1), bx1 = clampi((q[2] - ox) >> 5, 0, CHUNK_BINS - 1);
	int by0 = clampi((q[1] - oy) >> 5, 0, CHUNK_BINS - 1), by1 = clampi((q[3] - oy) >> 5, 0, CHUNK_BINS - 1);
	int n = 0;

	if (q[2] < ox || q[3] < oy || q[0] >= ox + CHUNK_BINS * 32 || q[1] >= oy + CHUNK_BINS * 32
		|| q[2] < k->reach[0] || q[3] < k->reach[1] || q[0] > k->reach[2] || q[1] > k->reach[3])
		return 0;
	if ((bx1 - bx0 + 1) * (by1 - by0 + 1) > 4) {
		/* a large query (an area): the list itself, in order, is quicker than many bins */
		for (uint16_t i = 0; i < k->nstatic && n < max; i++) {
			const int16_t *r = k->statics[i].r;
			if (k->statics[i].flat == flat && r[0] <= q[2] && q[0] <= r[2] && r[1] <= q[3]
				&& q[1] <= r[3])
				out[n++] = (uint8_t) i;
		}
		return n;
	}
	for (int by = by0; by <= by1; by++)
		for (int bx = bx0; bx <= bx1; bx++) {
			int b = by * CHUNK_BINS + bx;
			for (uint16_t p = k->bin_start[b]; p < k->bin_start[b + 1]; p++) {
				uint8_t i = k->bin_list[p];
				const int16_t *r = k->statics[i].r;
				if (k->statics[i].flat != flat || r[0] > q[2] || q[0] > r[2] || r[1] > q[3]
					|| q[1] > r[3])
					continue;
				/* only in the bin holding the overlap's top-left corner */
				int x = r[0] > q[0] ? r[0] : q[0], y = r[1] > q[1] ? r[1] : q[1];
				if (clampi((x - ox) >> 5, 0, CHUNK_BINS - 1) != bx
					|| clampi((y - oy) >> 5, 0, CHUNK_BINS - 1) != by)
					continue;
				int at = n < max ? n++ : max - 1;
				while (at > 0 && out[at - 1] > i) {
					out[at] = out[at - 1];
					at--;
				}
				out[at] = i;
			}
		}
	/* those not binned (the movable ones, with the dataset's chunks), after: later in the list */
	for (uint16_t i = k->nfixed; i < k->nstatic && n < max; i++) {
		const int16_t *r = k->statics[i].r;
		if (k->statics[i].flat == flat && r[0] <= q[2] && q[0] <= r[2] && r[1] <= q[3]
			&& q[1] <= r[3])
			out[n++] = (uint8_t) i;
	}
	return n;
}

/* ---- order: among the objects over one area, or a mover and those near it ---- */

struct Local {
	const struct Static *s;
	int16_t r[4];               /* picture: x0, y0, x1, y1 */
	int16_t cx0, cx1, cy0, cy1; /* footprint, cells, inclusive */
	uint8_t z0, z1;             /* levels: bottom, top (bottom + height) */
	uint8_t mark;
	uint16_t pred;              /* first of its "drawn before" list */
};

static void make_local(struct Local *l, const struct Static *s)
{
	l->s = s;
	memcpy(l->r, s->r, sizeof l->r);
	l->cx1 = s->x;
	l->cx0 = (int16_t) (s->x - s->fx);
	l->cy1 = s->y;
	l->cy0 = (int16_t) (s->y - s->fy);
	l->z0 = s->z;
	l->z1 = (uint8_t) (s->z + s->height);
}

static inline int overlaps(const int16_t *a, int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
	return a[0] <= x1 && x0 <= a[2] && a[1] <= y1 && y0 <= a[3];
}

/* Draws a static into dst, world pixel (ox, oy) at dst's (0, 0), clipped. */
static void draw_static(struct ESurface *dst, const int16_t clip[4], const struct Static *s,
	int16_t ox, int16_t oy)
{
	const int16_t *f = frame_of(s->tf & 0x3ff, (s->tf >> 10) & 31);

	if (f == NULL)
		return;
	eb_shape(dst, clip, (const uint32_t *) (f + 4), (int16_t) (s->x * 8 + 7 - s->z * 4 - ox),
		(int16_t) (s->y * 8 + 7 - s->z * 4 - oy), (s->tf & 0x8000) != 0);
}

/* ---- the ring of areas ---- */

#ifndef RING_AX
#define RING_AX 4
#define RING_AY 3
#endif
#define RING_W (RING_AX * 128)
#define RING_H (RING_AY * 128)
static struct ESurface ring;
static int16_t ring_ax[RING_AX * RING_AY], ring_ay[RING_AX * RING_AY];
static uint8_t ring_top[RING_AX * RING_AY];   /* the highest level among an area's statics */
static uint8_t ring_stale[RING_AX * RING_AY]; /* drawn for another ceiling: to be drawn again */
/* read by the GPU in longs: 4-byte aligned (the 68000's compiler gives 2) */
static struct EFlat flat_table[E_FLAT_TYPES] __attribute__((aligned(8)));

static struct Local *loc;
static uint16_t nloc;
static uint32_t *loc_pairs;
static uint16_t *loc_preds, *loc_order, nlocorder;

static void visit(uint16_t i)
{
	/* depth first, what lies behind first; a cycle is cut where it closes */
	if (loc[i].mark)
		return;
	loc[i].mark = 1;
	for (uint16_t k = loc[i].pred; k < loc[i + 1].pred; k++)
		visit(loc_preds[k]);
	loc_order[nlocorder++] = i;
}

/* Draws area (ax, ay) into its place in the ring: the chunk's terrain, then every static over
 * it, the flat ones first in the chunks' order, then the standing ones in order. */
/* An area object's picture and footprint, for comparing (order_of()'s, inline in draw_area). */
static struct Edge {
	int16_t x0, x1, y0, y1;
	int16_t cx0, cx1, cy0, cy1;
	uint8_t z0, z1;
	uint16_t pad;
} edges[AREA_OBJECTS];

static void draw_area(int ax, int ay)
{
	int slot = (ay % RING_AY) * RING_AX + ax % RING_AX;
	int16_t rx = (int16_t) (ax % RING_AX * 128), ry = (int16_t) (ay % RING_AY * 128);
	int16_t wx = (int16_t) (ax * 128), wy = (int16_t) (ay * 128);
	int16_t ox = (int16_t) (wx - rx), oy = (int16_t) (wy - ry);
	const int16_t clip[4] = { rx, ry, (int16_t) (rx + 127), (int16_t) (ry + 127) };
	struct Chunk *k = chunk(ax, ay);
	struct ETileChunk tc;
	uint32_t npairs = 0;

	e_stats.rebuilds++;
	for (int t = 0; t < E_FLAT_TYPES; t++)
		if (k->flats[t >> 5] >> (t & 31) & 1) {
			const uint8_t *p = shape((uint16_t) t);
			flat_table[t].pixels = p;
			flat_table[t].frames = p != NULL ? shapes.table[t * 2 + 1] / 64 : 0;
		}
	tc.cells = k->cells;
	tc.to = ring.pixels + ry * RING_W + rx;
	tc.across = tc.down = 16;
	eb_tiles(&ring, flat_table, &tc, 1);

	nloc = 0;
	uint8_t top = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int cy = ay - 1; cy <= ay + 1; cy++)
			for (int cx = ax - 1; cx <= ax + 1; cx++) {
				if (cx < 0 || cy < 0 || cx >= 192 || cy >= 192)
					continue;
				struct Chunk *n = chunk_statics(cx, cy);
				const int16_t area[4] = { wx, wy, (int16_t) (wx + 127), (int16_t) (wy + 127) };
				uint8_t found[CHUNK_STATICS];
				int count = chunk_find(n, area, pass == 0, found, CHUNK_STATICS);
				for (int i = 0; i < count; i++) {
					const struct Static *s = &n->statics[found[i]];
					if (s->z > top)
						top = s->z;
					if (s->z > ceiling)
						continue;
					if (pass == 0)
						draw_static(&ring, clip, s, ox, oy);
					else if (nloc < AREA_OBJECTS)
						make_local(&loc[nloc++], s);
				}
			}
	e_stats.statics = nloc;

	uint32_t t_order = eb_clock();
	/* each overlapping pair once; then by object its list, and the walk */
	/* most of an area's pictures overlap, so nearly every pair is compared: the pictures' edges
	 * and the footprints side by side, and order_of() as it is, inline, without the calls */
	for (uint16_t i = 0; i < nloc; i++) {
		const struct Local *l = &loc[i];
		struct Edge *e = &edges[i];
		e->x0 = l->r[0];
		e->y0 = l->r[1];
		e->x1 = l->r[2];
		e->y1 = l->r[3];
		e->cx0 = l->cx0;
		e->cx1 = l->cx1;
		e->cy0 = l->cy0;
		e->cy1 = l->cy1;
		e->z0 = l->z0;
		e->z1 = l->z1;
	}
	for (uint16_t i = 0; i < nloc; i++) {
		const struct Edge *a = &edges[i], *b = a;
		for (uint16_t j = (uint16_t) (i + 1); j < nloc; j++) {
			b++;
			if (b->x0 > a->x1 || b->x1 < a->x0 || b->y0 > a->y1 || b->y1 < a->y0)
				continue;
			int xc = a->cx1 < b->cx0 ? -1 : a->cx0 > b->cx1 ? 1 : 0;
			int yc = a->cy1 < b->cy0 ? -1 : a->cy0 > b->cy1 ? 1 : 0;
			int zc = a->z1 <= b->z0 ? -1 : a->z0 >= b->z1 ? 1 : 0;
			int r;
			if (xc == 0 && yc == 0)
				r = zc;
			else if (zc != 0 && (xc == 0 || xc == zc) && (yc == 0 || yc == zc))
				r = zc;
			else if (xc == 0)
				r = yc;
			else if (yc == 0)
				r = xc;
			else
				r = xc == yc ? xc : 0;
			if (r != 0 && npairs < AREA_EDGES)
				loc_pairs[npairs++] = r < 0 ? (uint32_t) j << 16 | i : (uint32_t) i << 16 | j;
		}
	}
	e_stats.edges = npairs;
	e_stats.pair_tests += (uint32_t) nloc * (nloc - 1u) / 2;
	for (uint16_t i = 0; i <= nloc; i++)
		loc[i].pred = 0;
	for (uint32_t p = 0; p < npairs; p++)
		loc[loc_pairs[p] >> 16].pred++;
	uint16_t sum = 0;
	for (uint16_t i = 0; i <= nloc; i++) {
		uint16_t c = loc[i].pred;
		loc[i].pred = sum;
		sum = (uint16_t) (sum + c);
	}
	for (uint32_t p = 0; p < npairs; p++)
		loc_preds[loc[loc_pairs[p] >> 16].pred++] = (uint16_t) loc_pairs[p];
	for (uint16_t i = nloc; i > 0; i--)
		loc[i].pred = loc[i - 1].pred;
	loc[0].pred = 0;
	for (uint16_t i = 0; i < nloc; i++)
		loc[i].mark = 0;
	nlocorder = 0;
	for (uint16_t i = 0; i < nloc; i++)
		visit(i);
	e_stats.order_time += eb_clock() - t_order;
	for (uint16_t i = 0; i < nlocorder; i++)
		draw_static(&ring, clip, loc[loc_order[i]].s, ox, oy);
	ring_ax[slot] = (int16_t) ax;
	ring_ay[slot] = (int16_t) ay;
	ring_top[slot] = top;
	ring_stale[slot] = 0;
}

static int in_ring(int ax, int ay)
{
	int slot = (ay % RING_AY) * RING_AX + ax % RING_AX;

	return ring_ax[slot] == ax && ring_ay[slot] == ay;
}

/* ---- movers ---- */

#define MOVER_FRONTS 32
#ifndef MOVER_MARGIN
#define MOVER_CANDS 160        /* statics kept near a mover */
#define MOVER_MARGIN 24        /* pixels round its box they are kept for */
#endif
#define MOVER_SLOTS 12         /* chunks they come from */
static struct Mover {
	uint16_t tf;
	int16_t x, y;
	uint8_t z, ok;
	struct Static s;
	/* the statics in front of it, in drawing order, worked out for it as it is (s); valid, or
	 * not yet */
	uint8_t cached, nfront;
	const struct Static *fronts[MOVER_FRONTS] __attribute__((aligned(4)));  /* a GPU reads them */
	/* the standing statics over its box and MOVER_MARGIN round it (near), from the chunks in
	 * slots (each still given the chunk it was: gens); kept while it moves within that box */
	int16_t near_box[4];
	uint8_t near_ok, nnear, nslot;
	uint16_t evictions;         /* the count when its fronts' shapes were last all read in */
	uint8_t slots[MOVER_SLOTS];
	uint16_t gens[MOVER_SLOTS];
	const struct Static **near;
} movers[E_MOVERS] __attribute__((aligned(8)));
static uint8_t nmovers;

/* ---- the ceiling over a mover, as the original's (UpdateCeiling): of the levels the solid
 * statics (not doors) take over its row of cells, those above it; the lowest less one, or 15 ---- */

static int listener;             /* the mover sounds are heard from (the one followed) */

int e_follow(int i)
{
	const struct Mover *v = &movers[i];

	listener = i;
	const struct Info *in = &info[v->tf & 0x3ff];
	int w = v->tf & 0x8000 ? in->fy : in->fx;
	uint16_t taken = 0, own = (uint16_t) (((1u << (in->height ? in->height : 1)) - 1) << v->z);
	int top = 15;

	/* a static covers the cells up to its footprint west and north of it: up to 7 */
	for (int cy = v->y >> 4; cy <= (v->y + 7) >> 4; cy++)
		for (int cx = (v->x - w) >> 4; cx <= (v->x + 7) >> 4; cx++) {
			if (cx < 0 || cy < 0 || cx >= 192 || cy >= 192)
				continue;
			const struct Chunk *k = chunk_statics(cx, cy);
			for (uint16_t j = 0; j < k->nstatic; j++) {
				const struct Static *s = &k->statics[j];
				if (!s->solid || s->height == 0 || v->y < s->y - s->fy || v->y > s->y
					|| v->x - w > s->x || v->x < s->x - s->fx)
					continue;
				taken |= (uint16_t) (((1u << s->height) - 1) << s->z);
			}
		}
	/* the levels above it alone (the original's mask: its own and those under cleared); the
	 * lowest of them less one */
	taken &= (uint16_t) ~(own | (own - 1));
	if (taken != 0)
		for (top = -1; !(taken & 1); taken = (uint16_t) (taken >> 1))
			top++;
	if (top != ceiling) {
		/* the areas with statics between the two drawn again (e_draw: one a frame, nearest the
		 * middle of the view first, so there is no pause), and the fronts found again */
		int low = top < ceiling ? top : ceiling;
		ceiling = (uint8_t) top;
		for (int a = 0; a < RING_AX * RING_AY; a++)
			if (ring_top[a] > low)
				ring_stale[a] = 1;
		for (uint8_t m = 0; m < nmovers; m++)
			movers[m].cached = 0;
	}
	return ceiling;
}

/* Chunk cx, cy's collision grid, made if need be: from its statics and those of the chunks east
 * and south of it, whose footprints (west and north of their hot spots) reach into it. */
static struct Chunk *grid(int cx, int cy)
{
	struct Chunk *k = chunk_statics(cx, cy);

	if (k->grid_ok)
		return k;
	memset(k->solid_at, 0, 256 * 2);
	memset(k->door_at, 0, 256 * 2);
	memset(k->ground, 0, sizeof k->ground);
	for (int i = 0; i < 256; i++) {
		uint16_t type = rd16(k->cells + i * 2) & 0x3ff;
		if (type < TILE_TYPES && (info[type].flags & INFO_SOLID))
			k->ground[i >> 4] |= (uint16_t) (1u << (i & 15));
	}
	int x0 = cx * 16, y0 = cy * 16;
	for (int n = 0; n < 4; n++) {
		int ncx = cx + (n & 1), ncy = cy + (n >> 1);
		if (ncx >= 192 || ncy >= 192)
			continue;
		const struct Chunk *o = n == 0 ? k : chunk_statics(ncx, ncy);
		for (uint16_t j = 0; j < o->nstatic; j++) {
			const struct Static *st = &o->statics[j];
			const struct Info *in = &info[st->tf & 0x3ff];
			if (!(in->flags & INFO_SOLID) || st->height == 0 || st->x - st->fx > x0 + 15
				|| st->y - st->fy > y0 + 15 || st->x < x0 || st->y < y0)
				continue;
			uint16_t levels = (uint16_t) (((1u << st->height) - 1) << st->z);
			uint16_t *to = in->flags & INFO_DOOR ? k->door_at : k->solid_at;
			for (int y = clampi(st->y - st->fy, y0, y0 + 15); y <= st->y && y <= y0 + 15; y++)
				for (int x = clampi(st->x - st->fx, x0, x0 + 15); x <= st->x && x <= x0 + 15; x++)
					to[(y - y0) * 16 + x - x0] |= levels;
		}
	}
	k->grid_ok = 1;
	return k;
}

/* The solid movers in the collision grid, as the original adds NPCs to it: each one's levels over
 * its footprint, in the chunks held, stamped again (all) when one has moved. The cells stamped
 * are kept to be cleared. A walker is left out of its own steps (e_mover_self), as the original
 * takes it out of the grid for them. */
#define MOVER_STAMPS 512
static struct {
	uint8_t slot, cell;
} stamps[MOVER_STAMPS];
static uint16_t nstamps;
static int self_mover = -1;
static int16_t self_box[4];
static uint16_t self_levels;

static uint16_t body_of(uint16_t tf, uint8_t z, int *fx, int *fy);

static void restamp(void)
{
	for (uint16_t i = 0; i < nstamps; i++)
		chunks[stamps[i].slot].mover_at[stamps[i].cell] = 0;
	nstamps = 0;
	for (uint8_t m = 0; m < nmovers; m++) {
		const struct Mover *v = &movers[m];
		int fx, fy;
		if (v->tf == 0 || !(info[v->tf & 0x3ff].flags & INFO_SOLID))
			continue;
		uint16_t levels = body_of(v->tf, v->z, &fx, &fy);
		for (int y = v->y - fy; y <= v->y; y++)
			for (int x = v->x - fx; x <= v->x; x++) {
				uint8_t slot = x >= 0 && y >= 0 && x < 192 * 16 && y < 192 * 16
					? chunk_slot[(y >> 4) * 192 + (x >> 4)] : 0;
				if (slot == 0 || nstamps == MOVER_STAMPS)
					continue;
				uint8_t c = (uint8_t) ((y & 15) * 16 + (x & 15));
				chunks[slot - 1].mover_at[c] |= levels;
				stamps[nstamps].slot = (uint8_t) (slot - 1);
				stamps[nstamps++].cell = c;
			}
	}
	movers_dirty = 0;
}

static void self_set(void)
{
	int fx, fy;
	const struct Mover *v = &movers[self_mover];

	self_levels = 0;
	if (v->tf == 0 || !(info[v->tf & 0x3ff].flags & INFO_SOLID))
		return;
	self_levels = body_of(v->tf, v->z, &fx, &fy);
	self_box[0] = (int16_t) (v->x - fx);
	self_box[1] = (int16_t) (v->y - fy);
	self_box[2] = v->x;
	self_box[3] = v->y;
}

void e_mover_self(int i)
{
	self_mover = i;
	self_levels = 0;
	if (i >= 0 && i < nmovers)
		self_set();
}

/* The levels the movers take at cell c (x, y) of chunk k, but the walker's own. */
static inline uint16_t movers_at(const struct Chunk *k, int c, int x, int y)
{
	uint16_t v = k->mover_at[c];

	if (v && self_levels && x >= self_box[0] && x <= self_box[2] && y >= self_box[1]
		&& y <= self_box[3])
		v &= (uint16_t) ~self_levels;
	return v;
}

/* Levels (a bit each) the solid statics take over the cells x0..x1, y0..y1 (and the solid
 * movers, with_movers), and apart (as the original's collision grid keeps them) those the doors
 * take; and whether any of the cells' ground is solid. */
static inline __attribute__((always_inline)) uint16_t cells_taken(int x0, int y0, int x1, int y1,
	uint16_t *doors, int *ground, int with_movers)
{
	static struct Chunk *last;
	uint16_t taken = 0, door = 0;
	int hard = 0;

	if (with_movers && movers_dirty)
		restamp();
	/* most often all in one chunk, the one last asked about (routes ask a great deal) */
	if (x0 >> 4 == x1 >> 4 && y0 >> 4 == y1 >> 4 && x0 >= 0 && y0 >= 0) {
		const struct Chunk *k = last;
		if (k == NULL || k->cx != x0 >> 4 || k->cy != y0 >> 4 || !k->grid_ok) {
			if (x0 >> 4 >= 192 || y0 >> 4 >= 192)
				goto none;
			k = last = grid(x0 >> 4, y0 >> 4);
		}
		unsigned row = (unsigned) (y0 & 15) * 16;
		uint16_t bits = (uint16_t) (((2u << (x1 & 15)) - 1) & ~((1u << (x0 & 15)) - 1));
		for (int y = y0; y <= y1; y++, row += 16) {
			const uint16_t *so = k->solid_at + row, *dr = k->door_at + row;
			for (int x = x0 & 15; x <= (x1 & 15); x++) {
				taken |= so[x];
				door |= dr[x];
				if (with_movers)
					taken |= movers_at(k, (int) row + x, (x0 & ~15) + x, y);
			}
			hard |= k->ground[row >> 4] & bits;
		}
		goto done;
	}
	for (int cy = y0 >> 4; cy <= y1 >> 4; cy++)
		for (int cx = x0 >> 4; cx <= x1 >> 4; cx++) {
			if (cx < 0 || cy < 0 || cx >= 192 || cy >= 192)
				continue;
			const struct Chunk *k = grid(cx, cy);
			for (int y = y0 > cy * 16 ? y0 : cy * 16; y <= y1 && y < cy * 16 + 16; y++)
				for (int x = x0 > cx * 16 ? x0 : cx * 16; x <= x1 && x < cx * 16 + 16; x++) {
					int c = (y & 15) * 16 + (x & 15);
					taken |= k->solid_at[c];
					door |= k->door_at[c];
					hard |= k->ground[c >> 4] >> (c & 15) & 1;
					if (with_movers)
						taken |= movers_at(k, c, x, y);
				}
		}
	goto done;
none:
	taken = door = 0;
done:
	if (doors != NULL)
		*doors = door;
	if (ground != NULL)
		*ground = hard;
	return taken;
}

static uint16_t levels_taken(int x0, int y0, int x1, int y1, uint16_t *doors)
{
	return cells_taken(x0, y0, x1, y1, doors, NULL, 0);
}

/* A shape's footprint (cells west and north of its hot spot) and the levels it takes at z. */
static uint16_t body_of(uint16_t tf, uint8_t z, int *fx, int *fy)
{
	const struct Info *in = &info[tf & 0x3ff];

	*fx = tf & 0x8000 ? in->fy : in->fx;
	*fy = tf & 0x8000 ? in->fx : in->fy;
	return (uint16_t) (((1u << (in->height ? in->height : 1)) - 1) << z);
}

int e_clear_line(int16_t x0, int16_t y0, uint8_t z0, int16_t x1, int16_t y1, uint8_t z1)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
	int sx = x1 > x0 ? 1 : -1, sy = y1 > y0 ? 1 : -1, n = dx > dy ? dx : dy, err = dx - dy;
	int x = x0, y = y0;
	uint16_t doors;

	for (int k = 1; k < n; k++) {
		int e2 = 2 * err;
		if (e2 > -dy) {
			err -= dy;
			x += sx;
		}
		if (e2 < dx) {
			err += dx;
			y += sy;
		}
		/* at chest height, two levels over the ground between them */
		int z = z0 + (z1 - z0) * k / n + 2;
		if (z > 15)
			z = 15;
		if (levels_taken(x, y, x, y, &doors) >> z & 1)
			return 0;
	}
	return 1;
}

int e_blocked(uint16_t tf, int16_t x, int16_t y, uint8_t z)
{
	int fx, fy;
	uint16_t body = body_of(tf, z, &fx, &fy), door;

	return ((levels_taken(x - fx, y - fy, x, y, &door) | door) & body) != 0;
}

/* As the original's CanWalkAt, for a body taking levels body0 << z over cells whose solid
 * levels are taken (ground: theirs is solid): nothing solid where it would stand, and something
 * under it (the ground at 0, else a solid static's top over any of the cells). */
static int walk_ok(uint16_t body0, uint16_t taken, int ground, int z)
{
	if (taken & (uint16_t) (body0 << z))
		return 0;
	return z == 0 ? !ground : (taken >> (z - 1)) & 1;
}

/* Whether shape tf can stand at x, y, z (walk_ok); with a door there, *door. */
static int can_walk(uint16_t tf, int x, int y, int z, int *door)
{
	int fx, fy, ground;
	uint16_t body0 = body_of(tf, 0, &fx, &fy), doors;
	uint16_t taken = cells_taken(x - fx, y - fy, x, y, &doors, &ground, 1);

	*door = (doors & (uint16_t) (body0 << z)) != 0;
	return walk_ok(body0, taken, ground, z);
}

void e_body(uint16_t tf, struct EBody *b)
{
	int fx, fy;

	b->levels = body_of(tf, 0, &fx, &fy);
	b->fx = (int8_t) fx;
	b->fy = (int8_t) fy;
}

int e_step_body(const struct EBody *b, int16_t *x, int16_t *y, uint8_t *z, int dx, int dy, int through)
{
	int nx = *x + dx, ny = *y + dy, nz = *z, ground;
	uint16_t body0 = b->levels, doors;
	uint16_t taken = cells_taken(nx - b->fx, ny - b->fy, nx, ny, &doors, &ground, 1);

	/* as CheckMove for a walker: its level, else one up, else one down */
	if (!walk_ok(body0, taken, ground, nz)) {
		if (nz < 15 && walk_ok(body0, taken, ground, nz + 1))
			nz++;
		else if (nz > 0 && walk_ok(body0, taken, ground, nz - 1))
			nz--;
		else
			return E_STEP_BLOCKED;
	}
	int door = (doors & (uint16_t) (body0 << nz)) != 0;
	/* a door there: the avatar's step (and a route's) stops at it; a route planned goes on */
	if (door && !through)
		return E_STEP_DOOR;
	*x = (int16_t) nx;
	*y = (int16_t) ny;
	*z = (uint8_t) nz;
	return door ? E_STEP_DOOR : E_STEP_MADE;
}

int e_step(uint16_t tf, int16_t *x, int16_t *y, uint8_t *z, int dx, int dy)
{
	struct EBody b;

	e_body(tf, &b);
	return e_step_body(&b, x, y, z, dx, dy, 0);
}

int e_can_stand(uint16_t tf, int16_t x, int16_t y, uint8_t z)
{
	int door;

	return can_walk(tf, x, y, z, &door) && !door;
}

int e_walk(uint16_t tf, int16_t *x, int16_t *y, uint8_t *z, int dx, int dy, int open_doors)
{
	/* the eight ways, from north clockwise */
	static const int8_t way_x[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
	static const int8_t way_y[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
	int way = 0, first;

	if (dx == 0 && dy == 0)
		return E_STEP_BLOCKED;
	while (way_x[way] != dx || way_y[way] != dy)
		way++;
	/* as the original's party step: that way, else an eighth one way, else the other */
	if ((first = e_step(tf, x, y, z, dx, dy)) == E_STEP_MADE)
		return E_STEP_MADE;
	for (int turn = 1; turn <= 7; turn += 6) {
		int w = (way + turn) & 7;
		if (e_step(tf, x, y, z, way_x[w], way_y[w]) == E_STEP_MADE)
			return E_STEP_MADE;
	}
	if (first == E_STEP_DOOR && open_doors) {
		int d = e_door_at(tf, (int16_t) (*x + dx), (int16_t) (*y + dy), *z);
		if (d >= 0 && e_door_shut(d) && e_door_toggle(d))
			return E_STEP_DOOR;
	}
	return E_STEP_BLOCKED;
}

int e_door_ahead(uint16_t tf, int16_t x, int16_t y, uint8_t z, int dx, int dy)
{
	/* the cell ahead, those either side of it, the one past it */
	const int8_t try_x[4] = { (int8_t) dx, (int8_t) (dx - dy), (int8_t) (dx + dy), (int8_t) (2 * dx) };
	const int8_t try_y[4] = { (int8_t) dy, (int8_t) (dy + dx), (int8_t) (dy - dx), (int8_t) (2 * dy) };

	for (int i = 0; i < 4; i++) {
		int d = e_door_at(tf, (int16_t) (x + try_x[i]), (int16_t) (y + try_y[i]), z);
		if (d >= 0)
			return d;
	}
	return -1;
}

/* ---- doors, and the other movable items the game changes (a bed's cover, food eaten): each
 * one touched is kept here (where it began, where and what it is now, the chunk whose list has
 * it, whether it is gone), and put back so when its region is read again ---- */

#define MOST_DOORS 512

static struct Door {
	int16_t x0, y0;             /* where it began, as its region's file has it */
	uint16_t tf0;
	int16_t x, y;               /* now */
	uint16_t tf;
	uint8_t z;
	uint8_t gone;               /* taken out of the world */
	uint8_t added;              /* put into the world (not in its region's file) */
	int16_t lcx, lcy;           /* the chunk whose list holds it */
} doors[MOST_DOORS];
static int ndoors;

/* In region r, the item of chunk list c (within the region) that is tf at x, y (the region's
 * cells), z; or -1. */
static int find_item(const struct Region *r, int c, uint16_t tf, int x, int y, uint8_t z)
{
	for (int i = r->start[c]; i < r->start[c + 1]; i++) {
		const struct Item *it = &r->items[i];
		if (it->tf == tf && it->x == (uint8_t) x && it->y == (uint8_t) y && it->z == z)
			return i;
	}
	return -1;
}

/* Item i of region r from chunk list a to the end of chunk list b. */
static void move_item(struct Region *r, int i, int a, int b)
{
	struct Item it = r->items[i];

	if (a == b)
		return;
	memmove(&r->items[i], &r->items[i + 1], (size_t) (r->start[256] - i - 1) * sizeof it);
	for (int c = a + 1; c <= 256; c++)
		r->start[c]--;
	int at = r->start[b + 1];
	memmove(&r->items[at + 1], &r->items[at], (size_t) (r->start[256] - at) * sizeof it);
	r->items[at] = it;
	for (int c = b + 1; c <= 256; c++)
		r->start[c]++;
}

/* Item it put at the end of chunk list c of region r: 0 when the region is full. */
static int insert_item(struct Region *r, int c, const struct Item *it)
{
	if (r->start[256] >= REGION_ITEMS)
		return 0;
	int at = r->start[c + 1];
	memmove(&r->items[at + 1], &r->items[at], (size_t) (r->start[256] - at) * sizeof *it);
	r->items[at] = *it;
	for (int k = c + 1; k <= 256; k++)
		r->start[k]++;
	return 1;
}

/* Item i of region r, in chunk list c, taken out. */
static void remove_item(struct Region *r, int i, int c)
{
	memmove(&r->items[i], &r->items[i + 1], (size_t) (r->start[256] - i - 1) * sizeof r->items[0]);
	for (int k = c + 1; k <= 256; k++)
		r->start[k]--;
}

static int region_of(int x, int y) { return (y >> 8) * 12 + (x >> 8); }
static int list_of(int x, int y) { return (y >> 4 & 15) * 16 + (x >> 4 & 15); }

/* A region just read: its doors as the game has left them. */
static void doors_into(struct Region *r)
{
	for (int d = 0; d < ndoors; d++) {
		const struct Door *o = &doors[d];
		if (o->added) {
			/* one put there: put back, where it is now */
			if (!o->gone && region_of(o->x, o->y) == r->id) {
				struct Item it = { o->tf, (uint8_t) o->x, (uint8_t) o->y, o->z, 0 };
				insert_item(r, list_of(o->x, o->y), &it);
			}
			continue;
		}
		if (region_of(o->x0, o->y0) != r->id
			|| (o->x == o->x0 && o->y == o->y0 && o->tf == o->tf0 && !o->gone))
			continue;
		int c = list_of(o->x0, o->y0), i = find_item(r, c, o->tf0, o->x0, o->y0, o->z);
		if (i < 0)
			continue;
		if (o->gone) {
			remove_item(r, i, c);
			continue;
		}
		r->items[i].tf = o->tf;
		r->items[i].x = (uint8_t) o->x;
		r->items[i].y = (uint8_t) o->y;
		if (region_of(o->lcx * 16, o->lcy * 16) == r->id)
			move_item(r, i, c, (o->lcy & 15) * 16 + (o->lcx & 15));
	}
}

/* A free entry of the table: a new one, or (full) one as it began given up (looked at, not
 * changed); -1 when none is. */
static int new_kept(void)
{
	if (ndoors < MOST_DOORS)
		return ndoors++;
	for (int d = 0; d < ndoors; d++)
		if (doors[d].x == doors[d].x0 && doors[d].y == doors[d].y0 && doors[d].tf == doors[d].tf0
			&& !doors[d].gone && !doors[d].added)
			return d;
	return -1;
}

/* The handle of the door that is static s of chunk cx, cy: kept, or made; -1 if none is left. */
static int door_of(const struct Static *s, int cx, int cy)
{
	for (int d = 0; d < ndoors; d++)
		if (doors[d].x == s->x && doors[d].y == s->y && doors[d].tf == s->tf
			&& doors[d].z == s->z && !doors[d].gone)
			return d;
	int d = new_kept();
	if (d < 0)
		return -1;
	struct Door *o = &doors[d];
	o->x0 = o->x = s->x;
	o->y0 = o->y = s->y;
	o->tf0 = o->tf = s->tf;
	o->z = s->z;
	o->gone = 0;
	o->added = 0;
	o->lcx = (int16_t) cx;
	o->lcy = (int16_t) cy;
	return d;
}

/* The door a body (levels taken, cells west and north) at x, y would be in, and its chunk; NULL
 * for none. */
static const struct Static *door_in(uint16_t body, int fx, int fy, int x, int y, int *ocx, int *ocy)
{
	int x0 = x - fx, y0 = y - fy;

	for (int cy = y0 >> 4; cy <= (y + 7) >> 4; cy++)
		for (int cx = x0 >> 4; cx <= (x + 7) >> 4; cx++) {
			if (cx < 0 || cy < 0 || cx >= 192 || cy >= 192)
				continue;
			const struct Chunk *k = chunk_statics(cx, cy);
			for (uint16_t j = 0; j < k->nstatic; j++) {
				const struct Static *s = &k->statics[j];
				if (!(info[s->tf & 0x3ff].flags & INFO_DOOR) || y0 > s->y || y < s->y - s->fy
					|| x0 > s->x || x < s->x - s->fx
					|| !((((1u << (s->height ? s->height : 1)) - 1) << s->z) & body))
					continue;
				*ocx = cx;
				*ocy = cy;
				return s;
			}
		}
	return NULL;
}

int e_door_at(uint16_t tf, int16_t x, int16_t y, uint8_t z)
{
	int fx, fy, cx, cy;
	uint16_t body = body_of(tf, z, &fx, &fy);
	const struct Static *s = door_in(body, fx, fy, x, y, &cx, &cy);

	return s != NULL ? door_of(s, cx, cy) : -1;
}

static int shut_at(uint16_t tf, int x, int y, uint8_t z);

int e_open_door_at(const struct EBody *b, int16_t x, int16_t y, uint8_t z)
{
	int cx, cy;
	const struct Static *s = door_in((uint16_t) (b->levels << z), b->fx, b->fy, x, y, &cx, &cy);

	return s != NULL && !shut_at(s->tf, s->x, s->y, s->z);
}

/* The levels taken at cell x, y, solid or by a door. */
static uint16_t taken_at(int x, int y)
{
	uint16_t door = 0;

	return (uint16_t) (levels_taken(x, y, x, y, &door) | door);
}

/* Whether a door tf at x, y, z is shut: in a wall, something past each end of it, along its
 * length (a wall, or the other leaf of a double door). */
static int shut_at(uint16_t tf, int x, int y, uint8_t z)
{
	const struct Info *in = &info[tf & 0x3ff];
	int fx = tf & 0x8000 ? in->fy : in->fx, fy = tf & 0x8000 ? in->fx : in->fy;
	uint16_t levels = (uint16_t) (((1u << (in->height ? in->height : 1)) - 1) << z);

	if (fx >= fy)
		return (taken_at(x - fx - 1, y) & levels) && (taken_at(x + 1, y) & levels);
	return (taken_at(x, y - fy - 1) & levels) && (taken_at(x, y + 1) & levels);
}

int e_door_shut(int door)
{
	const struct Door *o = &doors[door];

	return shut_at(o->tf, o->x, o->y, o->z);
}

void e_door_place(int door, int16_t *x, int16_t *y)
{
	*x = doors[door].x;
	*y = doors[door].y;
}

/* The ring's areas under a picture, to be drawn again; and the chunk's statics made again. */
static void static_changed(uint16_t tf, int x, int y, int z, int cx, int cy)
{
	int16_t r[4];
	uint8_t slot = chunk_slot[cy * 192 + cx];

	if (slot != 0) {
		chunks[slot - 1].built = 0;
		chunk_gen[slot - 1]++;
	}
	/* its grid, and those west and north its footprint may reach into */
	for (int n = 0; n < 4; n++) {
		int gx = cx - (n & 1), gy = cy - (n >> 1);
		uint8_t g = gx >= 0 && gy >= 0 ? chunk_slot[gy * 192 + gx] : 0;
		if (g != 0)
			chunks[g - 1].grid_ok = 0;
	}
	if (picture_rect(tf, x, y, z, r))
		for (int ay = r[1] >> 7; ay <= r[3] >> 7; ay++)
			for (int ax = r[0] >> 7; ax <= r[2] >> 7; ax++)
				if (ax >= 0 && ay >= 0 && ax < 192 && ay < 192 && in_ring(ax, ay))
					ring_stale[(ay % RING_AY) * RING_AX + ax % RING_AX] = 1;
	for (uint8_t m = 0; m < nmovers; m++)
		movers[m].cached = 0;
}

/* Kept item d made tf at x, y (in its region), or taken out (gone): its region's item too. 0 when
 * it is not there, or x, y is past its region's edge (left as it is). */
static int keep_set(int d, uint16_t tf, int x, int y, int gone)
{
	struct Door *o = &doors[d];
	struct Region *r = region(region_of(o->x, o->y));
	int c = list_of(o->lcx * 16, o->lcy * 16), i = -1;

	if (o->gone)
		return 0;
	if (region_of(o->lcx * 16, o->lcy * 16) == r->id)
		i = find_item(r, c, o->tf, o->x, o->y, o->z);
	if (i < 0 || region_of(x, y) != r->id)
		return 0;
	static_changed(o->tf, o->x, o->y, o->z, o->lcx, o->lcy);
	if (gone) {
		remove_item(r, i, c);
		o->gone = 1;
		return 1;
	}
	r->items[i].tf = tf;
	r->items[i].x = (uint8_t) x;
	r->items[i].y = (uint8_t) y;
	move_item(r, i, c, list_of(x, y));
	o->tf = tf;
	o->x = (int16_t) x;
	o->y = (int16_t) y;
	o->lcx = (int16_t) (x >> 4);
	o->lcy = (int16_t) (y >> 4);
	static_changed(tf, x, y, o->z, o->lcx, o->lcy);
	return 1;
}

/* One leaf opened or shut, as its usecode does it; 0 if it can't be. */
static int door_turn(int door)
{
	struct Door *o = &doors[door];
	int state = o->tf >> 10 & 3, mx = 0, my = 0;
	uint16_t type = o->tf & 0x3ff, to;

	/* as the original's toggleDoor (and the doors' usecode): 2 and 3 are locked */
	if (state > 1)
		return 0;
	switch (type) {
	case 270: to = 376; break;
	case 376: to = 270; break;
	case 225: to = 392; break;
	case 392: to = 225; break;
	case 432: case 246:
		to = type == 432 ? 433 : 250;
		if (state == 1)
			my = 3;
		else
			mx = -3;
		break;
	case 433: case 250:
		to = type == 433 ? 432 : 246;
		if (state == 1)
			mx = 3;
		else
			my = -3;
		break;
	default:
		return 0;
	}
	uint16_t tf = (uint16_t) ((o->tf & ~(3u << 10)) | (uint16_t) (state ^ 1) << 10);
	tf = (uint16_t) ((tf & ~0x3ffu) | to);
	if (!keep_set(door, tf, o->x + mx, o->y + my, 0))
		return 0;
	e_stats.doors_turned++;
	return 1;
}

/* The other leaf of a double door: as the doors' usecode looks for it, the partner type (270 and
 * 432, 376 and 433, 225 and 246, 392 and 250) within 7 cells of the leaf just turned, as open or
 * shut as that one was, on its row or its column (by type and state); -1 if there is none. */
static int partner(uint16_t type, int state, int x, int y, uint8_t z)
{
	uint16_t want;
	int row;                    /* 1: the same y; 0: the same x */

	switch (type) {
	case 270: want = 432; break;
	case 432: want = 270; break;
	case 225: want = 246; break;
	case 246: want = 225; break;
	case 376: want = 433; break;
	case 433: want = 376; break;
	case 392: want = 250; break;
	case 250: want = 392; break;
	default: return -1;
	}
	row = (type == 270 || type == 432 || type == 225 || type == 246) ? state == 0 : state == 1;
	for (int cy = (y - 7) >> 4; cy <= (y + 7) >> 4; cy++)
		for (int cx = (x - 7) >> 4; cx <= (x + 7) >> 4; cx++) {
			if (cx < 0 || cy < 0 || cx >= 192 || cy >= 192)
				continue;
			const struct Chunk *k = chunk_statics(cx, cy);
			for (uint16_t j = 0; j < k->nstatic; j++) {
				const struct Static *s = &k->statics[j];
				if ((s->tf & 0x3ff) == want && (s->tf >> 10 & 3) == state && s->z == z
					&& s->x >= x - 7 && s->x <= x + 7 && s->y >= y - 7 && s->y <= y + 7
					&& (row ? s->y == y : s->x == x))
					return door_of(s, cx, cy);
			}
		}
	return -1;
}

int e_door_toggle(int door)
{
	uint16_t type = doors[door].tf & 0x3ff;
	int state = doors[door].tf >> 10 & 3;

	if (!door_turn(door))
		return 0;
	int other = partner(type, state, doors[door].x, doors[door].y, doors[door].z);
	if (other >= 0 && other != door)
		door_turn(other);
	/* as the original's toggleDoor: Black Gate's door opening (30) or shutting (31) */
	e_sound(state == 0 ? 30 : 31, doors[door].x, doors[door].y);
	return 1;
}

int e_things(uint16_t type, int16_t x0, int16_t y0, int16_t x1, int16_t y1, struct EThing *out,
	int most)
{
	int16_t wx0, wy0, wx1, wy1;
	int n = 0;

	e_near_window(&wx0, &wy0, &wx1, &wy1);
	x0 = x0 < wx0 ? wx0 : x0 < 0 ? 0 : x0;
	y0 = y0 < wy0 ? wy0 : y0 < 0 ? 0 : y0;
	x1 = x1 > wx1 ? wx1 : x1 > E_WORLD_CELLS - 1 ? E_WORLD_CELLS - 1 : x1;
	y1 = y1 > wy1 ? wy1 : y1 > E_WORLD_CELLS - 1 ? E_WORLD_CELLS - 1 : y1;
	for (int cy = y0 >> 4; cy <= y1 >> 4 && y0 <= y1; cy++)
		for (int cx = x0 >> 4; cx <= x1 >> 4 && x0 <= x1; cx++) {
			const struct Chunk *k = chunk_statics(cx, cy);
			for (uint16_t j = 0; j < k->nstatic && n < most; j++) {
				const struct Static *s = &k->statics[j];
				if ((s->tf & 0x3ff) != type || s->x < x0 || s->x > x1 || s->y < y0 || s->y > y1)
					continue;
				out[n].tf = s->tf;
				out[n].x = s->x;
				out[n].y = s->y;
				out[n].z = s->z;
				n++;
			}
		}
	return n;
}

int e_thing_ahead(int16_t x, int16_t y, uint8_t z, int dx, int dy, int (*wanted)(uint16_t tf),
	struct EThing *out)
{
	/* the cell ahead, those either side of it, the one past it; the highest wanted item there
	 * from the shape's feet to a little over its head */
	const int8_t try_x[4] = { (int8_t) dx, (int8_t) (dx - dy), (int8_t) (dx + dy), (int8_t) (2 * dx) };
	const int8_t try_y[4] = { (int8_t) dy, (int8_t) (dy + dx), (int8_t) (dy - dx), (int8_t) (2 * dy) };
	int16_t wx0, wy0, wx1, wy1;

	e_near_window(&wx0, &wy0, &wx1, &wy1);
	for (int i = 0; i < 4; i++) {
		int tx = x + try_x[i], ty = y + try_y[i], found = 0;
		if (tx < wx0 || ty < wy0 || tx > wx1 || ty > wy1)
			continue;
		for (int cy = ty >> 4; cy <= (ty + 7) >> 4 && cy < 192; cy++)
			for (int cx = tx >> 4; cx <= (tx + 7) >> 4 && cx < 192; cx++) {
				const struct Chunk *k = chunk_statics(cx, cy);
				for (uint16_t j = 0; j < k->nstatic; j++) {
					const struct Static *st = &k->statics[j];
					if (tx > st->x || tx < st->x - st->fx || ty > st->y || ty < st->y - st->fy
						|| st->z > z + 6 || st->z + st->height < z || !wanted(st->tf))
						continue;
					if (found && st->z + st->height <= out->z)
						continue;
					out->tf = st->tf;
					out->x = st->x;
					out->y = st->y;
					out->z = st->z;
					found = 1;
				}
			}
		if (found)
			return 1;
	}
	return 0;
}

/* The kept handle of thing t, a movable item (as e_things gave it) still there; or -1. */
static int thing_handle(const struct EThing *t)
{
	if (t->x < 0 || t->y < 0 || t->x >= E_WORLD_CELLS || t->y >= E_WORLD_CELLS)
		return -1;
	/* one kept already (its region read in as need be, its chunk not) */
	for (int d = 0; d < ndoors; d++)
		if (doors[d].x == t->x && doors[d].y == t->y && doors[d].tf == t->tf
			&& doors[d].z == t->z && !doors[d].gone)
			return d;
	int cx = t->x >> 4, cy = t->y >> 4;
	const struct Chunk *k = chunk_statics(cx, cy);
	for (uint16_t j = 0; j < k->nstatic; j++) {
		const struct Static *s = &k->statics[j];
		if (s->tf == t->tf && s->x == t->x && s->y == t->y && s->z == t->z)
			return door_of(s, cx, cy);
	}
	return -1;
}

int e_thing_set(struct EThing *t, uint16_t tf)
{
	int d = thing_handle(t);

	if (d < 0 || !keep_set(d, tf, t->x, t->y, 0))
		return 0;
	t->tf = tf;
	return 1;
}

int e_thing_add(uint16_t tf, int16_t x, int16_t y, uint8_t z, struct EThing *out)
{
	if (x < 0 || y < 0 || x >= E_WORLD_CELLS || y >= E_WORLD_CELLS || (tf & 0x3ff) < TILE_TYPES)
		return 0;
	struct Region *r = region(region_of(x, y));
	struct Item it = { tf, (uint8_t) x, (uint8_t) y, z, 0 };
	int d = new_kept();
	if (d < 0 || !insert_item(r, list_of(x, y), &it)) {
		if (d == ndoors - 1)
			ndoors--;
		return 0;
	}
	struct Door *o = &doors[d];
	o->x0 = o->x = x;
	o->y0 = o->y = y;
	o->tf0 = o->tf = tf;
	o->z = z;
	o->gone = 0;
	o->added = 1;
	o->lcx = (int16_t) (x >> 4);
	o->lcy = (int16_t) (y >> 4);
	static_changed(tf, x, y, z, o->lcx, o->lcy);
	if (out != NULL) {
		out->tf = tf;
		out->x = x;
		out->y = y;
		out->z = z;
	}
	return 1;
}

int e_shape_frames(uint16_t type)
{
	const uint32_t *s = (const uint32_t *) shape(type);

	return s == NULL || type < TILE_TYPES ? 0 : (int) (s[1] / 4 - 1);
}

int e_inside(void)
{
	return ceiling != 15;
}

int e_shape_class(int type)
{
	return type >= 0 && type < 1024 ? info[type].cls : 0;
}

int e_thing_ref(const struct EThing *t)
{
	int d = thing_handle(t);

	return d < 0 ? 0 : d + 1;
}

int e_ref_thing(int ref, struct EThing *t)
{
	if (ref < 1 || ref > ndoors || doors[ref - 1].gone)
		return 0;
	const struct Door *o = &doors[ref - 1];
	t->tf = o->tf;
	t->x = o->x;
	t->y = o->y;
	t->z = o->z;
	return 1;
}

int e_thing_quality(const struct EThing *t)
{
	if (t->x < 0 || t->y < 0 || t->x >= E_WORLD_CELLS || t->y >= E_WORLD_CELLS)
		return -1;
	struct Region *r = region(region_of(t->x, t->y));
	int i = find_item(r, list_of(t->x, t->y), t->tf, t->x & 255, t->y & 255, t->z);
	return i < 0 ? -1 : r->items[i].quality;
}

int e_thing_remove(const struct EThing *t)
{
	int d = thing_handle(t);

	return d >= 0 && keep_set(d, t->tf, t->x, t->y, 1);
}

/* ---- sounds: the original's AdLib effects as the dataset has them rendered (ENGINE/SFX.FLX),
 * each read into a cache the first time it is played, the oldest giving way ---- */

#define SOUND_CACHE (64 * 1024)
static struct Flex sounds;
static uint8_t sounds_ok;
static uint8_t *sound_cache;
static uint32_t sound_next;
static uint32_t sound_at[256];  /* each sound's place in the cache + 1, or 0 */

static void open_sounds(void)
{
	uint8_t title[64];

	if (flex_open(&sounds, "ENGINE", "SFX.FLX") < 0)
		return;
	if (!read_at(sounds.f, 0, title, sizeof title)
		|| memcmp(title, E_SOUNDS_TITLE, sizeof E_SOUNDS_TITLE - 1) != 0
		|| (sound_cache = take(SOUND_CACHE)) == NULL) {
		plat_file_close(sounds.f);
		return;
	}
	sounds_ok = 1;
}

void e_sound(int number, int16_t x, int16_t y)
{
	if (!sounds_ok || number < 0 || number >= sounds.count || number >= 256)
		return;
	/* as the original's PlaySoundAtItem with the AdLib: fainter by 2 a cell across and down
	 * from the avatar, panned by 2 a cell across */
	const struct Mover *v = &movers[listener];
	int dx = x - v->x, dy = y - v->y;
	int volume = 127 - 2 * ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
	int pan = clampi(64 + 2 * dx, 0, 127);
	uint32_t at = sounds.table[number * 2], size = sounds.table[number * 2 + 1] & ~1u;
	if (volume < 0 || size == 0 || size > SOUND_CACHE)
		return;
	if (sound_at[number] == 0) {
		if (sound_next + size > SOUND_CACHE)
			sound_next = 0;
		for (int k = 0; k < sounds.count && k < 256; k++)
			if (sound_at[k] != 0 && sound_at[k] - 1 < sound_next + size
				&& sound_at[k] - 1 + sounds.table[k * 2 + 1] > sound_next)
				sound_at[k] = 0;
		if (!read_at(sounds.f, at, sound_cache + sound_next, size))
			return;
		sound_at[number] = sound_next + 1;
		sound_next = (sound_next + size + 7) & ~7u;
	}
	eb_sound(sound_cache + sound_at[number] - 1, size / 2, volume, pan);
}

/* ---- text (STATIC/TEXT.FLX) and barks: a line over an NPC's head a while, in the original's
 * yellow font (FONTS.VGA's first: its frames are the letters by their codes) ---- */

static void note_drawn(const struct ESurface *frame, const int16_t clip[4]);
static uint8_t ring_shown;      /* (below) */
struct Bark;
static int bark_box(const struct Bark *b, int16_t box[4]);
static struct Flex text_flex;
static uint8_t text_ok;
static uint8_t *font;           /* font 0, in the engine's form */
#define FONT_SPACING (-1)       /* the original's for font 0: between letters */
#define FONT_LEADING (-1)
#define BARKS 8
#define BARK_TEXT 72

static struct Bark {
	int8_t mover;               /* whose (a mover slot), or -1: none */
	int16_t life;               /* blanks left */
	char text[BARK_TEXT];
} barks[BARKS];

static void open_text(void)
{
	static struct Flex fonts;
	uint32_t made;

	/* (the files shut: a GameDrive keeps few open; the text's is opened as a line is wanted) */
	text_ok = flex_open(&text_flex, "STATIC", "TEXT.FLX") == 0;
	if (text_flex.f >= 0)
		plat_file_close(text_flex.f);
	if (flex_open(&fonts, "STATIC", "FONTS.VGA") < 0 || fonts.count == 0) {
		if (fonts.f >= 0)
			plat_file_close(fonts.f);
		return;
	}
	uint32_t at = fonts.table[0], size = fonts.table[1];
	int ok = size != 0 && size <= SCRATCH && read_at(fonts.f, at, scratch, size);
	plat_file_close(fonts.f);
	if (!ok || (made = e_convert_shape(scratch, size, NULL, 0)) == 0
		|| (font = take(made + E_CONVERT_SLACK)) == NULL)
		return;
	if (e_convert_shape(scratch, size, font, made + E_CONVERT_SLACK) == 0)
		font = NULL;
}

/* Letter c's frame (its box, then its runs), or NULL. */
static const int16_t *letter(uint8_t c)
{
	const uint32_t *f = (const uint32_t *) font;

	if (f == NULL || (c + 1) * 4u >= f[1])
		return NULL;
	return (const int16_t *) (font + f[1 + c]);
}

const char *e_text(int section, int n)
{
	/* Black Gate's file: eight sections of 1024, 256, 256, ... entries */
	static const uint16_t start[9] = { 0, 1024, 1280, 1536, 1792, 2048, 2304, 2560, 2560 };
	static char line[BARK_TEXT];
	uint32_t k;

	line[0] = 0;
	if (!text_ok || section < 0 || section > 7 || n < 0 || n >= start[section + 1] - start[section])
		return line;
	k = start[section] + (uint32_t) n;
	if (k >= text_flex.count)
		return line;
	uint32_t at = text_flex.table[k * 2], size = text_flex.table[k * 2 + 1];
	if (size > sizeof line - 1)
		size = sizeof line - 1;
	int16_t f = size ? open_file("STATIC", "TEXT.FLX") : -1;
	if (f < 0 || !read_at(f, at, line, size))
		size = 0;
	if (f >= 0)
		plat_file_close(f);
	line[size] = 0;
	return line;
}

void e_bark(int mover, const char *text, int blanks)
{
	struct Bark *b = NULL;

	if (mover < 0 || mover >= nmovers || text == NULL || text[0] == 0)
		return;
	/* its own last one gives way, else a free one, else the oldest */
	for (int k = 0; k < BARKS && b == NULL; k++)
		if (barks[k].life > 0 && barks[k].mover == mover)
			b = &barks[k];
	for (int k = 0; k < BARKS && b == NULL; k++)
		if (barks[k].life <= 0)
			b = &barks[k];
	if (b == NULL) {
		b = &barks[0];
		for (int k = 1; k < BARKS; k++)
			if (barks[k].life < b->life)
				b = &barks[k];
	}
	b->mover = (int8_t) mover;
	b->life = (int16_t) blanks;
	int n = 0;
	for (; text[n] && n < BARK_TEXT - 1; n++)
		b->text[n] = text[n] == '@' ? '"' : text[n];   /* as the original: '@' a double quote */
	b->text[n] = 0;
	/* as the original's: the others it would overlap given up */
	int16_t mine[4], other[4];
	if (font != NULL && bark_box(b, mine))
		for (int k = 0; k < BARKS; k++)
			if (&barks[k] != b && bark_box(&barks[k], other) && other[0] <= mine[2]
				&& mine[0] <= other[2] && other[1] <= mine[3] && mine[1] <= other[3])
				barks[k].life = 0;
}

void e_barks_age(int blanks)
{
	for (int k = 0; k < BARKS; k++)
		if (barks[k].life > 0)
			barks[k].life = (int16_t) (barks[k].life - blanks);
}

/* Where bark b would be drawn, in world pixels (x0, y0, x1, y1): 0 when nowhere. */
static int bark_box(const struct Bark *b, int16_t box[4])
{
	int16_t r[4];
	int height = 0, width = 0;

	if (b->life <= 0 || b->mover >= nmovers || movers[b->mover].tf == 0)
		return 0;
	const struct Mover *v = &movers[b->mover];
	if (!picture_rect(v->tf, v->x, v->y, v->z, r))
		return 0;
	for (const char *c = b->text; *c; c++) {
		const int16_t *f = letter((uint8_t) *c);
		if (f != NULL) {
			if (f[2] + f[3] + 1 > height)
				height = f[2] + f[3] + 1;
			width += f[0] + f[1] + 1 + 2 * FONT_SPACING;
		}
	}
	box[0] = r[0];
	box[1] = (int16_t) (r[1] - 2 - height);
	box[2] = (int16_t) (r[0] + width);
	box[3] = (int16_t) (r[1] - 2);
	return 1;
}

/* The barks of movers in view drawn into frame: above the speaker's picture, from its left. */
static void draw_barks(struct ESurface *frame, int16_t vx, int16_t vy)
{
	static const int16_t whole[4] = { 0, 0, E_VIEW_W - 1, E_VIEW_H - 1 };
	int16_t r[4];

	if (font == NULL)
		return;
	for (int k = 0; k < BARKS; k++) {
		const struct Bark *b = &barks[k];
		if (b->life <= 0 || b->mover >= nmovers || movers[b->mover].tf == 0)
			continue;
		const struct Mover *v = &movers[b->mover];
		if (v->z > ceiling || !picture_rect(v->tf, v->x, v->y, v->z, r))
			continue;
		int x = r[0] - vx, base = r[1] - vy - 2, height = 0, width = 0;
		for (const char *c = b->text; *c; c++) {
			const int16_t *f = letter((uint8_t) *c);
			if (f != NULL) {
				if (f[2] + f[3] + 1 > height)
					height = f[2] + f[3] + 1;
				width += f[0] + f[1] + 1 + 2 * FONT_SPACING;
			}
		}
		if (x + width < 0 || x >= E_VIEW_W || base < 0 || base - height >= E_VIEW_H)
			continue;
		/* kept within the view across */
		if (x + width > E_VIEW_W)
			x = E_VIEW_W - width;
		if (x < 0)
			x = 0;
		int16_t box[4] = { (int16_t) x, (int16_t) (base - height), (int16_t) (x + width), (int16_t) base };
		for (const char *c = b->text; *c; c++) {
			const int16_t *f = letter((uint8_t) *c);
			if (f == NULL)
				continue;
			eb_shape(frame, whole, (const uint32_t *) (f + 4), (int16_t) (x + f[1]), (int16_t) base, 0);
			x += f[0] + f[1] + 1 + 2 * FONT_SPACING;   /* the printer's: spacing twice */
		}
		if (ring_shown)
			note_drawn(frame, box);
	}
}

/* ---- the conversation's pictures: faces (FACES.VGA), the pointer (POINTERS.SHP), text ---- */

#define FACE_SLOTS 3
#define FACE_ROOM (14 * 1024)
static struct {
	int16_t face;
	uint16_t age;
	uint8_t *s;
} faces[FACE_SLOTS];
static uint16_t face_age;
static uint8_t *pointer_shape;

/* A converted shape's frame, its box first, or NULL. */
static const int16_t *shape_frame(const uint8_t *s, int frame)
{
	const uint32_t *f = (const uint32_t *) s;

	if (s == NULL || frame < 0 || (frame + 1) * 4u >= f[1])
		return NULL;
	return (const int16_t *) (s + f[1 + frame]);
}

/* Whether every frame of a raw shape lies within the converter's offsets of its hot spot (a span
 * starts within -128 to 127 of it): then the backend's converter (the Jaguar's GPU, which does
 * not look) can take it. */
static int fits_offsets(const uint8_t *raw, uint32_t size)
{
	if (size < 8 || rd32(raw) != size)
		return 0;
	uint32_t first = rd32(raw + 4);
	if (first < 8 || first > size || (first & 3))
		return 0;
	for (uint32_t f = 0; f < (first - 4) / 4; f++) {
		uint32_t p = rd32(raw + 4 + f * 4);
		if (p + 8 > size || rds16(raw + p) > 127 || rds16(raw + p + 2) > 128 || rds16(raw + p + 4) > 128
			|| rds16(raw + p + 6) > 127)
			return 0;
	}
	return 1;
}

/* Shape bytes (raw, size) converted into room bytes at out (by the backend's converter): 1 when
 * they fit. */
static int convert_into(const uint8_t *raw, uint32_t size, uint8_t *out, uint32_t room)
{
	if (!fits_offsets(raw, size))
		return 0;
	uint32_t made = eb_convert(raw, size, NULL, 0);

	return made != 0 && made + E_CONVERT_SLACK <= room && eb_convert(raw, size, out, room) != 0;
}

/* What converting a shape makes (by the backend's converter), or 0 when it does not fit. */
static uint32_t convert_size(const uint8_t *raw, uint32_t size)
{
	return fits_offsets(raw, size) ? eb_convert(raw, size, NULL, 0) : 0;
}

static uint8_t *read_room;      /* the faces' slots, one block: a book's room while it is read */

static void open_faces(void)
{
	/* one block: a book's, scroll's or sign's room while it is read (read_room) */
	uint8_t *block = take(FACE_SLOTS * FACE_ROOM);
	for (int k = 0; k < FACE_SLOTS; k++) {
		faces[k].face = -1;
		faces[k].s = block != NULL ? block + k * FACE_ROOM : NULL;
	}
	read_room = block;
	int16_t f = open_file("STATIC", "POINTERS.SHP");
	if (f < 0)
		return;
	int32_t len = plat_file_length(f);
	if (len > 0 && len <= SCRATCH && read_at(f, 0, scratch, (uint32_t) len)) {
		uint32_t made = e_convert_shape(scratch, (uint32_t) len, NULL, 0);
		if (made != 0 && (pointer_shape = take(made + E_CONVERT_SLACK)) != NULL
			&& !convert_into(scratch, (uint32_t) len, pointer_shape, made + E_CONVERT_SLACK))
			pointer_shape = NULL;
	}
	plat_file_close(f);
}

/* Face n (FACES.VGA's entry), read in if need be (the oldest giving way). */
static const uint8_t *face_shape(int n)
{
	int k, old = 0;
	uint8_t head[8];

	for (k = 0; k < FACE_SLOTS; k++) {
		if (faces[k].face == n && faces[k].s != NULL) {
			faces[k].age = ++face_age;
			return faces[k].s;
		}
		if (faces[k].age < faces[old].age)
			old = k;
	}
	if (faces[old].s == NULL)
		return NULL;
	int16_t f = open_file("STATIC", "FACES.VGA");
	if (f < 0)
		return NULL;
	int ok = read_at(f, 0x80 + 8 * (uint32_t) n, head, 8);
	uint32_t at = rd32(head), size = rd32(head + 4);
	ok = ok && n >= 0 && n < 1024 && size > 0 && size <= SCRATCH && read_at(f, at, scratch, size);
	plat_file_close(f);
	faces[old].face = -1;
	if (!ok || !convert_into(scratch, size, faces[old].s, FACE_ROOM))
		return NULL;
	faces[old].face = (int16_t) n;
	faces[old].age = ++face_age;
	return faces[old].s;
}

void e_draw_face(struct ESurface *frame, int face, int fr, int16_t x, int16_t y, int16_t *w, int16_t *h)
{
	static const int16_t whole[4] = { 0, 0, E_VIEW_W - 1, E_VIEW_H - 1 };
	const int16_t *f = shape_frame(face_shape(face), fr);

	if (w != NULL)
		*w = f != NULL ? (int16_t) (f[0] + f[1] + 1) : 0;
	if (h != NULL)
		*h = f != NULL ? (int16_t) (f[2] + f[3] + 1) : 0;
	if (f != NULL && frame != NULL)
		eb_shape(frame, whole, (const uint32_t *) (f + 4), x, y, 0);
}

void e_draw_pointer(struct ESurface *frame, int fr, int16_t x, int16_t y)
{
	static const int16_t whole[4] = { 0, 0, E_VIEW_W - 1, E_VIEW_H - 1 };
	const int16_t *f = shape_frame(pointer_shape, fr);

	if (f != NULL)
		eb_shape(frame, whole, (const uint32_t *) (f + 4), x, y, 0);
	else {
		static int told;
		if (!told++)
			eb_log(pointer_shape == NULL ? "no pointer shape\n" : "no pointer frame\n");
	}
}

/* ---- gumps (GUMPS.VGA), read in as wanted: the slider's a few kept; a book's, scroll's or
 * sign's in the faces' room while it is read (no face is shown then), with its font. Font 2 (the
 * slider's numbers). A gump wider or taller than the converter's offsets reach from its hot
 * spot (a span must start within -128 to 127 of it) is cut in bands up to 256 wide, each
 * centred on its own, drawn where its part of the gump is. ---- */

#define GUMP_ROOM (8 * 1024)
#define GUMPS_KEPT 8
#define GUMP_PARTS 2
struct Gump {
	int16_t n, w, h;
	uint8_t parts;
	uint32_t at[GUMP_PARTS];    /* each part, converted, in its room */
	int16_t dx[GUMP_PARTS], dy[GUMP_PARTS];     /* where its hot spot is from the gump's */
};
static uint8_t *gump_room, *font2;
static uint32_t gump_used;
static struct Gump gumps_kept[GUMPS_KEPT];
static int ngumps_kept;
#define READ_ROOM (FACE_SLOTS * FACE_ROOM)
static struct Gump reading;     /* the gump being read (parts 0: none) */
static uint8_t *reading_font;
static int reading_font_n = -1;
static uint8_t span_px[512];

static inline void wr32(uint8_t *p, uint32_t v)
{
	wr16(p, (uint16_t) v);
	wr16(p + 2, (uint16_t) (v >> 16));
}

static void open_gumps(void)
{
	static struct Flex fonts;
	uint32_t made;

	gump_room = take(GUMP_ROOM);
	if (flex_open(&fonts, "STATIC", "FONTS.VGA") < 0 || fonts.count < 3) {
		if (fonts.f >= 0)
			plat_file_close(fonts.f);
		return;
	}
	uint32_t at = fonts.table[2 * 2], size = fonts.table[2 * 2 + 1];
	int ok = size != 0 && size <= SCRATCH && read_at(fonts.f, at, scratch, size);
	plat_file_close(fonts.f);
	if (ok && (made = e_convert_shape(scratch, size, NULL, 0)) != 0
		&& (font2 = take(made + E_CONVERT_SLACK)) != NULL
		&& !convert_into(scratch, size, font2, made + E_CONVERT_SLACK))
		font2 = NULL;
}

/* Frame 0 of raw shape s, the part of it from along lo to hi, its hot spot moved to cx, cy: a raw
 * one-frame shape at out (run-encoded again: repeats of 3 or more filled): its size, or 0. */
static uint32_t cut_raw(const uint8_t *s, uint32_t size, int lo, int hi, int cx, int cy, uint8_t *out,
	uint32_t room)
{
	uint32_t p = rd32(s + 4), o = 16;

	if (p + 8 > size || room < 20)
		return 0;
	int right = rds16(s + p), left = rds16(s + p + 2), up = rds16(s + p + 4), down = rds16(s + p + 6);
	int r = right < hi - 1 ? right : hi - 1, l = -left > lo ? -left : lo;
	wr32(out + 4, 8);
	wr16(out + 8, (uint16_t) (r - cx));
	wr16(out + 10, (uint16_t) (cx - l));
	wr16(out + 12, (uint16_t) (up + cy));
	wr16(out + 14, (uint16_t) (down - cy));
	for (p += 8;;) {
		if (p + 2 > size)
			return 0;
		uint16_t head = rd16(s + p);
		if (head == 0)
			break;
		if (p + 6 > size)
			return 0;
		int len = head >> 1, along = rds16(s + p + 2), across = rds16(s + p + 4);
		p += 6;
		if (len > (int) sizeof span_px)
			return 0;
		if (!(head & 1)) {
			if (p + (uint32_t) len > size)
				return 0;
			memcpy(span_px, s + p, (size_t) len);
			p += (uint32_t) len;
		} else
			for (int pos = 0; pos < len;) {
				if (p >= size)
					return 0;
				int c = s[p] >> 1, fill = s[p] & 1;
				p++;
				if (pos + c > len || p + (fill ? 1u : (uint32_t) c) > size)
					return 0;
				if (fill)
					memset(span_px + pos, s[p++], (size_t) c);
				else {
					memcpy(span_px + pos, s + p, (size_t) c);
					p += (uint32_t) c;
				}
				pos += c;
			}
		int a = along > lo ? along : lo, e = along + len < hi ? along + len : hi;
		if (a >= e)
			continue;
		if (o + 6 > room)
			return 0;
		uint32_t h = o;
		o += 6;
		wr16(out + h, (uint16_t) ((e - a) << 1 | 1));
		wr16(out + h + 2, (uint16_t) (a - cx));
		wr16(out + h + 4, (uint16_t) (across - cy));
		const uint8_t *px = span_px - along;
		for (int i = a; i < e;) {
			int rep = 1;
			while (i + rep < e && rep < 127 && px[i + rep] == px[i])
				rep++;
			if (o + 2 + 127 > room)
				return 0;
			if (rep >= 3) {
				out[o++] = (uint8_t) (rep << 1 | 1);
				out[o++] = px[i];
				i += rep;
				continue;
			}
			uint32_t start = o++;
			int lit = 0;
			while (i < e && lit < 127) {
				if (lit > 0 && i + 2 < e && px[i] == px[i + 1] && px[i] == px[i + 2])
					break;
				out[o++] = px[i++];
				lit++;
			}
			out[start] = (uint8_t) (lit << 1);
		}
	}
	if (o + 2 > room)
		return 0;
	wr16(out + o, 0);
	o += 2;
	wr32(out, o);
	return o;
}

/* Gump n read and converted into room bytes at out (cut in bands if need be): 1 and g. */
static int load_gump(int n, uint8_t *out, uint32_t room, struct Gump *g)
{
	uint8_t head[8], *raw = scratch, *cut = scratch + SCRATCH / 2;
	uint32_t used = 0;

	g->parts = 0;
	int16_t f = open_file("STATIC", "GUMPS.VGA");
	if (f < 0)
		return 0;
	int ok = read_at(f, 0x80 + 8 * (uint32_t) n, head, 8);
	uint32_t at = rd32(head), size = rd32(head + 4);
	ok = ok && size > 16 && size <= SCRATCH / 2 && read_at(f, at, raw, size);
	plat_file_close(f);
	if (!ok)
		return 0;
	uint32_t p = rd32(raw + 4);
	if (p + 8 > size)
		return 0;
	int right = rds16(raw + p), left = rds16(raw + p + 2), up = rds16(raw + p + 4), down = rds16(raw + p + 6);
	g->n = (int16_t) n;
	g->w = (int16_t) (right + left + 1);
	g->h = (int16_t) (up + down + 1);
	uint32_t made = convert_size(raw, size);
	if (made != 0) {
		if (made + E_CONVERT_SLACK > room || !convert_into(raw, size, out, made + E_CONVERT_SLACK))
			return 0;
		g->parts = 1;
		g->at[0] = 0;
		g->dx[0] = g->dy[0] = 0;
		return 1;
	}
	/* in bands, each centred */
	int cy = (down - up) / 2;
	for (int lo = -left; lo <= right && g->parts < GUMP_PARTS; lo += 256) {
		int hi = lo + 256 > right + 1 ? right + 1 : lo + 256, cx = lo + 128, k = g->parts;
		uint32_t c = cut_raw(raw, size, lo, hi, cx, cy, cut, SCRATCH / 2);
		made = c ? convert_size(cut, c) : 0;
		if (made == 0 || used + made + E_CONVERT_SLACK > room
			|| !convert_into(cut, c, out + used, made + E_CONVERT_SLACK))
			return 0;
		g->at[k] = used;
		g->dx[k] = (int16_t) cx;
		g->dy[k] = (int16_t) cy;
		g->parts++;
		used = (used + made + E_CONVERT_SLACK + 7) & ~7u;
	}
	return g->parts > 0;
}

static void draw_gump(struct ESurface *frame, const uint8_t *room, const struct Gump *g, int16_t x, int16_t y)
{
	static const int16_t whole[4] = { 0, 0, E_VIEW_W - 1, E_VIEW_H - 1 };

	for (int k = 0; k < g->parts; k++) {
		const int16_t *f = shape_frame(room + g->at[k], 0);
		if (f != NULL)
			eb_shape(frame, whole, (const uint32_t *) (f + 4), (int16_t) (x + g->dx[k]),
				(int16_t) (y + g->dy[k]), 0);
	}
}

static const struct Gump *gump_shape(int n)
{
	for (int k = 0; k < ngumps_kept; k++)
		if (gumps_kept[k].n == n)
			return &gumps_kept[k];
	if (gump_room == NULL)
		return NULL;
	for (int tries = 0; tries < 2; tries++) {
		if (ngumps_kept == GUMPS_KEPT)
			ngumps_kept = 0, gump_used = 0;     /* full: all given way */
		struct Gump *g = &gumps_kept[ngumps_kept];
		if (load_gump(n, gump_room + gump_used, GUMP_ROOM - gump_used, g)) {
			uint32_t base = gump_used, end = 0;
			for (int k = 0; k < g->parts; k++) {
				g->at[k] += base;
				uint32_t sz = ((const uint32_t *) (gump_room + g->at[k]))[0];
				if (g->at[k] + sz > end)
					end = g->at[k] + sz;
			}
			gump_used = (end + E_CONVERT_SLACK + 7) & ~7u;
			if (gump_used > GUMP_ROOM)
				gump_used = GUMP_ROOM;
			ngumps_kept++;
			return g;
		}
		if (gump_used == 0)
			return NULL;
		ngumps_kept = 0, gump_used = 0;         /* no room left: all given way, once more */
	}
	return NULL;
}

void e_draw_gump(struct ESurface *frame, int n, int fr, int16_t x, int16_t y, int16_t *w, int16_t *h)
{
	const struct Gump *g = gump_shape(n);

	(void) fr;
	if (w != NULL)
		*w = g != NULL ? g->w : 0;
	if (h != NULL)
		*h = g != NULL ? g->h : 0;
	if (g != NULL && frame != NULL)
		draw_gump(frame, gump_room, g, x, y);
}

int e_reading_open(int gump, int fontn)
{
	static struct Flex fonts;
	uint32_t made;

	for (int k = 0; k < FACE_SLOTS; k++)
		faces[k].face = -1;     /* (their room taken) */
	reading.parts = 0;
	reading_font = NULL;
	reading_font_n = -1;
	if (read_room == NULL || !load_gump(gump, read_room, READ_ROOM, &reading))
		return 0;
	uint32_t used = 0;
	for (int k = 0; k < reading.parts; k++) {
		uint32_t end = reading.at[k] + ((const uint32_t *) (read_room + reading.at[k]))[0];
		if (end > used)
			used = end;
	}
	used = (used + E_CONVERT_SLACK + 7) & ~7u;
	if (flex_open(&fonts, "STATIC", "FONTS.VGA") < 0 || fonts.count <= (uint32_t) fontn) {
		if (fonts.f >= 0)
			plat_file_close(fonts.f);
		return 0;
	}
	uint32_t at = fonts.table[fontn * 2], size = fonts.table[fontn * 2 + 1];
	int ok = size != 0 && size <= SCRATCH && read_at(fonts.f, at, scratch, size);
	plat_file_close(fonts.f);
	if (!ok || (made = convert_size(scratch, size)) == 0 || used + made + E_CONVERT_SLACK > READ_ROOM
		|| !convert_into(scratch, size, read_room + used, made + E_CONVERT_SLACK))
		return 0;
	reading_font = read_room + used;
	reading_font_n = fontn;
	return 1;
}

void e_reading_close(void)
{
	reading.parts = 0;
	reading_font = NULL;
	reading_font_n = -1;
	e_text_font(0);
}

void e_draw_reading(struct ESurface *frame, int16_t x, int16_t y)
{
	draw_gump(frame, read_room, &reading, x, y);
}

void e_draw_small(struct ESurface *frame, int16_t x, int16_t y, const char *s)
{
	static const int16_t whole[4] = { 0, 0, E_VIEW_W - 1, E_VIEW_H - 1 };

	/* font 2 as the slider sets it: no spacing, so a letter steps its width */
	for (; *s; s++) {
		const int16_t *f = shape_frame(font2, (uint8_t) *s);
		if (f == NULL)
			continue;
		eb_shape(frame, whole, (const uint32_t *) (f + 4), (int16_t) (x + f[1]), y, 0);
		x = (int16_t) (x + f[0] + f[1] + 1);
	}
}

/* The text font (e_text_font): font 0, or the one read in for reading; the original's spacing
 * and leading for each (its FontLetterSpacing, FontLineSpacing). */
static const int8_t letter_spacing[8] = { -1, 1, 0, 1, 0, 0, 0, -1 };
static const int8_t line_spacing[8] = { -1, 0, -1, 0, 1, 1, 1, -1 };
static const uint8_t *text_font;
static int text_n;

int e_text_font(int n)
{
	if (n == reading_font_n && reading_font != NULL) {
		text_font = reading_font;
		text_n = n;
		return 1;
	}
	text_font = font;
	text_n = 0;
	return n == 0;
}

static const int16_t *text_letter(uint8_t c)
{
	if (text_font == NULL)
		text_font = font;
	return shape_frame(text_font, c);
}

int e_letter_width(uint8_t c)
{
	const int16_t *f = text_letter(c);

	/* as the original's: a letter's frame width, and the font's spacing */
	return f != NULL ? f[0] + f[1] + 1 + letter_spacing[text_n & 7] : 0;
}

int e_text_width(const char *s)
{
	int w = 0;

	for (; *s; s++)
		w += e_letter_width((uint8_t) *s) + letter_spacing[text_n & 7];   /* as its textWidth: twice */
	return w;
}

int e_line_height(void)
{
	static int height[8];

	if (height[text_n & 7] == 0) {
		/* the font's height over all its letters, and its leading */
		int h = 0;
		for (int c = 0; c < 256; c++) {
			const int16_t *f = text_letter((uint8_t) c);
			if (f != NULL && f[2] + f[3] + 1 > h)
				h = f[2] + f[3] + 1;
		}
		if (h == 0)
			return 0;
		height[text_n & 7] = h + line_spacing[text_n & 7];
	}
	return height[text_n & 7];
}

void e_draw_text(struct ESurface *frame, int16_t x, int16_t y, const char *s, int n)
{
	static const int16_t whole[4] = { 0, 0, E_VIEW_W - 1, E_VIEW_H - 1 };

	for (int k = 0; k < n && s[k]; k++) {
		const int16_t *f = text_letter((uint8_t) s[k]);
		if (f == NULL)
			continue;
		eb_shape(frame, whole, (const uint32_t *) (f + 4), (int16_t) (x + f[1]), y, 0);
		x = (int16_t) (x + e_letter_width((uint8_t) s[k]) + letter_spacing[text_n & 7]);   /* its printChar */
	}
}

int16_t e_open_static(const char *name)
{
	return open_file("STATIC", name);
}

int32_t e_read_file(const char *dir, const char *name, uint8_t *to, uint32_t most)
{
	int16_t f = open_file(dir, name);
	int32_t n;

	if (f < 0)
		return -1;
	n = plat_file_read(f, to, (int32_t) most);
	plat_file_close(f);
	return n;
}

/* ---- the NPCs of NPC.DAT (engine.h) ---- */

static int16_t npc_file;
static uint32_t npc_at, npc_have, npc_used;   /* the file read through scratch */

static int npc_byte(void)
{
	if (npc_used == npc_have) {
		int32_t n = plat_file_read(npc_file, scratch, SCRATCH);
		if (n <= 0)
			return -1;
		npc_at += npc_have;
		npc_have = (uint32_t) n;
		npc_used = 0;
	}
	return scratch[npc_used++];
}

static int npc_bytes(uint8_t *to, int n)
{
	for (int i = 0; i < n; i++) {
		int b = npc_byte();
		if (b < 0)
			return 0;
		to[i] = (uint8_t) b;
	}
	return 1;
}

int e_npcs(struct ENpc *out, int max)
{
	uint8_t rec[117];
	int n = 0;

	if ((npc_file = open_file("ENGINE", "NPC.DAT")) < 0
		&& (npc_file = open_file("GAMEDAT", "NPC.DAT")) < 0)
		return 0;
	npc_at = npc_have = npc_used = 0;
	e_held_clear();
	if (!npc_bytes(rec, 4)) {
		plat_file_close(npc_file);
		return 0;
	}
	int count = rds16(rec) + rds16(rec + 2);
	for (int i = 0; i < count && npc_bytes(rec, 117); i++) {
		/* x, y in its region, type-frame; the extension: contents, region, .., z << 4; then the
		 * NPC's own: .., its item (0: not on the map), status */
		uint16_t tf = rd16(rec + 2), ref = rd16(rec + 14), status = rd16(rec + 16);
		if (ref != 0 && (tf & 0x3ff) != 0 && rd16(rec + 4) != 0) {
			/* what it carries, as a region's movable items: levels ended by 0 or 1; each kept
			 * (e_held_), in the NPC or the container it is in */
			int depth = 0, in[8];
			in[0] = E_HELD_ON(i);
			for (;;) {
				int l = npc_byte();
				uint8_t item[18];
				if (l < 0)
					goto done;
				if (l == 0 || l == 1) {
					if (depth-- == 0)
						break;
					continue;
				}
				if ((l != 6 && l != 12 && l != 18) || !npc_bytes(item, l))
					goto done;
				/* its quality: a plain one's sixth byte, a container's eighth */
				int k = e_held_add(rd16(item + 2), l == 6 ? item[5] : l == 12 ? item[7] : 0,
					in[depth < 8 ? depth : 7]);
				if (l >= 12 && rd16(item + 4) != 0) {
					depth++;
					if (depth < 8)
						in[depth] = k >= 0 ? k : in[depth - 1];
				}
			}
		}
		if (ref == 0 || (tf & 0x3ff) == 0 || (status & 0x8000) || n >= max)
			continue;
		uint8_t region = rec[6];
		out[n].tf = tf;
		out[n].x = (int16_t) (region % 12 * 256 + rec[0]);
		out[n].y = (int16_t) (region / 12 * 256 + rec[1]);
		out[n].z = rec[9] >> 4;
		out[n].activity = rec[22];      /* its work type: what it is doing */
		/* its buffer, from byte 12: status, strength, dexterity, intelligence, combat, work
		 * type, attack mode, type, hit points, magic, mana, face, ..., its name at 89 */
		out[n].status = (uint16_t) (rec[16] | rec[17] << 8);
		out[n].strength = rec[18];
		out[n].dexterity = rec[19];
		out[n].intelligence = rec[20];
		out[n].combat = rec[21];
		out[n].hit_points = rec[26];
		out[n].magic = rec[27];
		out[n].mana = rec[28];
		out[n].face = rec[29];
		memcpy(out[n].name, rec + 101, 16);
		out[n].name[15] = 0;
		out[n].number = (uint16_t) i;
		n++;
	}
done:
	plat_file_close(npc_file);
	return n;
}

int e_mover_add(uint16_t tf, int16_t x, int16_t y, uint8_t z)
{
	if (nmovers >= E_MOVERS)
		return -1;
	/* before the first frame: its shape is kept below the ring of shapes, never given way */
	if (shape(tf & 0x3ff) != NULL && queue_count > 0) {
		pool_pinned = pool_next;
		queue_head = queue_count = 0;
	}
	e_mover_set(nmovers, tf, x, y, z);
	return nmovers++;
}

void e_mover_set(int i, uint16_t tf, int16_t x, int16_t y, uint8_t z)
{
	if (movers[i].tf != tf || movers[i].x != x || movers[i].y != y || movers[i].z != z)
		movers_dirty = 1;
	movers[i].tf = tf;
	movers[i].x = x;
	movers[i].y = y;
	movers[i].z = z;
	if (i == self_mover)
		self_set();
}

/* Draws the movers into frame, behind ones first, each followed by the statics in front of it
 * (and those in front of them), clipped to it. */
/* -1: a is drawn before b; 1: after; 0: either. By the ranges of their footprints and levels:
 * what lies south or east, or above, is in front. (Inline in draw_area too, from struct Edge.) */
static inline int order_of(const struct Static *a, const struct Static *b)
{
	int ax0 = a->x - a->fx, ay0 = a->y - a->fy, bx0 = b->x - b->fx, by0 = b->y - b->fy;
	int az1 = a->z + a->height, bz1 = b->z + b->height;
	int xc = a->x < bx0 ? -1 : ax0 > b->x ? 1 : 0;
	int yc = a->y < by0 ? -1 : ay0 > b->y ? 1 : 0;
	int zc = az1 <= b->z ? -1 : a->z >= bz1 ? 1 : 0;

	/* overlapping every way: as the original's sort, the higher in front, at one level the
	 * shorter (a bed's cover over its sleeper) */
	if (xc == 0 && yc == 0)
		return zc != 0 ? zc : a->z != b->z ? (a->z > b->z ? 1 : -1)
			: a->height != b->height ? (a->height < b->height ? 1 : -1) : 0;
	if (zc != 0 && (xc == 0 || xc == zc) && (yc == 0 || yc == zc))
		return zc;
	if (xc == 0)
		return yc;
	if (yc == 0)
		return xc;
	return xc == yc ? xc : 0;
}

/* The chunks a mover's statics came from (near, and its fronts) still hold them. */
static int near_valid(const struct Mover *v)
{
	for (uint8_t i = 0; i < v->nslot; i++)
		if (chunk_gen[v->slots[i]] != v->gens[i])
			return 0;
	return 1;
}

/* The standing statics over a mover's box and MOVER_MARGIN round it, kept; near_ok 0 when there
 * are too many to keep (or too many chunks). */
static void find_near(struct Mover *v, const int16_t box[4])
{
	int16_t q[4] = {
		(int16_t) (box[0] - MOVER_MARGIN), (int16_t) (box[1] - MOVER_MARGIN),
		(int16_t) (box[2] + MOVER_MARGIN), (int16_t) (box[3] + MOVER_MARGIN),
	};

	e_stats.near_finds++;
	v->near_ok = 0;
	v->nnear = v->nslot = 0;
	memcpy(v->near_box, q, sizeof q);
	for (int cy = (q[1] - REACH_AFTER) >> 7; cy <= (q[3] + REACH_BEFORE) >> 7; cy++)
		for (int cx = (q[0] - REACH_AFTER) >> 7; cx <= (q[2] + REACH_BEFORE) >> 7; cx++) {
			if (cx < 0 || cy < 0 || cx >= 192 || cy >= 192)
				continue;
			struct Chunk *k = chunk_statics(cx, cy);
			uint8_t found[CHUNK_STATICS];
			int count = chunk_find(k, q, 0, found, CHUNK_STATICS);
			if (v->nslot == MOVER_SLOTS || v->nnear + count > MOVER_CANDS)
				return;
			v->slots[v->nslot] = (uint8_t) (k - chunks);
			v->gens[v->nslot++] = chunk_gen[k - chunks];
			for (int j = 0; j < count; j++)
				v->near[v->nnear++] = &k->statics[found[j]];
		}
	/* a chunk read in for a later one may have taken the slot of an earlier one */
	v->near_ok = (uint8_t) near_valid(v);
}

static void note_drawn(const struct ESurface *frame, const int16_t clip[4]);
static uint8_t ring_shown;      /* the last eb_view showed the ring itself */

/* A mover's fronts worked out anew (it moved, or a chunk they came from gave its slot up). */
static void find_fronts(struct Mover *v)
{
		uint32_t f0 = eb_clock();
		e_stats.front_misses++;
		const int16_t *b = box_of(v->tf & 0x3ff);
		int16_t px = (int16_t) (v->x * 8 + 7 - v->z * 4), py = (int16_t) (v->y * 8 + 7 - v->z * 4);
		int16_t box[4];
		if (v->tf & 0x8000) {
			box[0] = (int16_t) (px - b[2]); box[1] = (int16_t) (py - b[1]);
			box[2] = (int16_t) (px + b[3]); box[3] = (int16_t) (py + b[0]);
		} else {
			box[0] = (int16_t) (px - b[1]); box[1] = (int16_t) (py - b[2]);
			box[2] = (int16_t) (px + b[0]); box[3] = (int16_t) (py + b[3]);
		}
		const int16_t *q = box;
		const struct Static *cand[128];
		uint8_t ncand = 0;
		if (!v->near_ok || !near_valid(v) || box[0] < v->near_box[0] || box[1] < v->near_box[1]
			|| box[2] > v->near_box[2] || box[3] > v->near_box[3])
			find_near(v, box);
		if (v->near_ok) {
			/* those over the box itself, in the order the chunks would give them */
			for (uint8_t j = 0; j < v->nnear && ncand < 128; j++) {
				const int16_t *r = v->near[j]->r;
				if (r[0] <= q[2] && q[0] <= r[2] && r[1] <= q[3] && q[1] <= r[3]
					&& v->near[j]->z <= ceiling)
					cand[ncand++] = v->near[j];
			}
		} else {
			/* too many near it to keep: the statics over it, from the chunks whose objects
			 * can reach it */
			v->nslot = 0;
			for (int cy = (q[1] - REACH_AFTER) >> 7; cy <= (q[3] + REACH_BEFORE) >> 7; cy++)
				for (int cx = (q[0] - REACH_AFTER) >> 7; cx <= (q[2] + REACH_BEFORE) >> 7; cx++) {
					if (cx < 0 || cy < 0 || cx >= 192 || cy >= 192)
						continue;
					struct Chunk *k = chunk_statics(cx, cy);
					uint8_t found[CHUNK_STATICS];
					int count = chunk_find(k, q, 0, found, CHUNK_STATICS);
					if (v->nslot < MOVER_SLOTS) {
						v->slots[v->nslot] = (uint8_t) (k - chunks);
						v->gens[v->nslot++] = chunk_gen[k - chunks];
					}
					for (int j = 0; j < count && ncand < 128; j++)
						if (k->statics[found[j]].z <= ceiling)
							cand[ncand++] = &k->statics[found[j]];
				}
		}
		/* in front of it, or of one found in front of it: each new one is tried against those
		 * left; then they are drawn each after those it is in front of */
		const struct Static *front[64];
		uint8_t nfront = 0, draw[64];
		e_stats.front_cands += ncand;
		for (int8_t against = -1; against < (int8_t) nfront; against++) {
			const struct Static *by = against < 0 ? &v->s : front[against];
			e_stats.front_compares += ncand;
			for (uint8_t j = 0; j < ncand && nfront < 64; j++)
				if (cand[j] != NULL && order_of(cand[j], by) > 0) {
					front[nfront++] = cand[j];
					cand[j] = NULL;
				}
		}
		for (uint8_t f = 0; f < nfront; f++) {
			uint8_t at = f;
			while (at > 0 && order_of(front[f], front[draw[at - 1]]) < 0) {
				draw[at] = draw[at - 1];
				at--;
			}
			draw[at] = f;
		}
		v->nfront = nfront < MOVER_FRONTS ? nfront : MOVER_FRONTS;
		for (uint8_t f = 0; f < v->nfront; f++)
			v->fronts[f] = front[draw[f]];
		v->cached = 1;
		e_stats.front_time += eb_clock() - f0;
}

/* Each mover: where it is, and its fronts as they are there, their shapes read in (for a
 * backend that draws them from the shape table, eb_movers). Its picture: v->s.r, v->ok. */
static void ready_movers(int16_t vx, int16_t vy)
{
	for (uint8_t m = 0; m < nmovers; m++) {
		struct Mover *v = &movers[m];
		/* what is in front of it depends on where it stands and its shape, not on which frame it
		 * shows: the statics over the box of all its frames are found once it stands somewhere */
		int reshaped = ((v->s.tf ^ v->tf) & 0x83ff) != 0;
		if (v->s.x != v->x || v->s.y != v->y || v->s.z != v->z || reshaped)
			v->cached = 0;
		v->s.tf = v->tf;
		v->s.x = v->x;
		v->s.y = v->y;
		v->s.z = v->z;
		if (reshaped)
			set_footprint(&v->s);
		v->ok = v->z <= ceiling && picture_rect(v->tf, v->x, v->y, v->z, v->s.r);
		if (!v->ok || v->s.r[2] < vx || v->s.r[0] >= vx + E_VIEW_W || v->s.r[3] < vy
			|| v->s.r[1] >= vy + E_VIEW_H) {
			v->ok = 0;
			continue;
		}
		if (v->cached && !near_valid(v))
			v->cached = 0;
		if (!v->cached) {
			find_fronts(v);
			v->evictions = (uint16_t) (evictions - 1);
		}
	}
	/* the fronts' shapes, in; reading one in may put out another's, so until none goes */
	for (int round = 0; round < 4; round++) {
		uint16_t before = evictions;
		for (uint8_t m = 0; m < nmovers; m++) {
			struct Mover *v = &movers[m];
			if (!v->ok || v->evictions == evictions)
				continue;
			shape(v->tf & 0x3ff);
			for (uint8_t f = 0; f < v->nfront; f++)
				shape(v->fronts[f]->tf & 0x3ff);
			v->evictions = before;
		}
		if (evictions == before)
			break;
	}
}

static void draw_movers(struct ESurface *frame, int16_t vx, int16_t vy)
{
	uint8_t sorted[E_MOVERS], n = 0;

	ready_movers(vx, vy);
	if (!ring_shown) {
		/* the backend's own way, from the shape table */
		static struct EMoverDraw list[E_MOVERS] __attribute__((aligned(8)));
		for (uint8_t m = 0; m < nmovers; m++) {
			list[m].s = movers[m].ok ? &movers[m].s : NULL;
			list[m].nfront = movers[m].ok ? movers[m].nfront : 0;
			list[m].fronts = (const void *const *) movers[m].fronts;
		}
		if (eb_movers(frame, list, nmovers, vx, vy, (const void *const *) shape_at))
			return;
	}
	for (uint8_t m = 0; m < nmovers; m++) {
		struct Mover *v = &movers[m];
		if (!v->ok)
			continue;
		uint8_t k = n++;
		while (k > 0 && order_of(&v->s, &movers[sorted[k - 1]].s) < 0) {
			sorted[k] = sorted[k - 1];
			k--;
		}
		sorted[k] = m;
	}
	for (uint8_t i = 0; i < n; i++) {
		struct Mover *v = &movers[sorted[i]];
		const int16_t *r = v->s.r;
		int16_t clip[4] = {
			(int16_t) (r[0] - vx), (int16_t) (r[1] - vy), (int16_t) (r[2] - vx), (int16_t) (r[3] - vy),
		};
		static const int16_t whole[4] = { 0, 0, E_VIEW_W - 1, E_VIEW_H - 1 };

		draw_static(frame, whole, &v->s, vx, vy);
		e_stats.drawn++;
		if (clip[0] < 0) clip[0] = 0;
		if (clip[1] < 0) clip[1] = 0;
		if (clip[2] > E_VIEW_W - 1) clip[2] = E_VIEW_W - 1;
		if (clip[3] > E_VIEW_H - 1) clip[3] = E_VIEW_H - 1;
		note_drawn(frame, clip);
		for (uint8_t f = 0; f < v->nfront; f++) {
			draw_static(frame, clip, v->fronts[f], vx, vy);
			e_stats.drawn++;
		}
	}
}

/* ---- the frame ---- */

int e_init(const char *prefix, uint8_t *memory, uint32_t size)
{
	uint8_t *tfa;
	struct Flex pal;
	int16_t f;

	snprintf(prefix_path, sizeof prefix_path, "%s", prefix);
	mem_next = memory;
	mem_end = memory + size;
	memset(&e_stats, 0, sizeof e_stats);
	if ((scratch = take(SCRATCH)) == NULL)
		return -1;

	if ((f = open_file("STATIC", "TFA.DAT")) < 0 || !read_at(f, 0, scratch, 3072))
		return -2;
	plat_file_close(f);
	tfa = scratch;
	for (int i = 0; i < 1024; i++) {
		const uint8_t *t = tfa + i * 3;
		info[i].height = t[0] >> 5;
		info[i].cls = t[1] & 15;
		info[i].fx = t[2] & 7;
		info[i].fy = (t[2] >> 3) & 7;
		info[i].flags = (uint8_t) ((t[0] & 4 ? INFO_ANIMATED : 0) | (t[2] & 0x80 ? INFO_TRANSLUCENT : 0)
			| (t[0] & 8 ? INFO_SOLID : 0) | (t[1] & 0x20 ? INFO_DOOR : 0));
	}

	if (flex_open(&pal, "STATIC", "PALETTES.FLX") < 0 || !read_at(pal.f, pal.table[0], palette, 768))
		return -3;
	memcpy(palette + E_BLACK * 3, palette, 3);
	plat_file_close(pal.f);

	if ((map = take(192 * 192 * 2)) == NULL || (f = open_file("STATIC", "U7MAP")) < 0)
		return -4;
	for (int r = 0; r < 144; r++) {
		if (!read_at(f, r * 512u, scratch, 512))
			return -4;
		int rx = r % 12, ry = r / 12;
		for (int c = 0; c < 256; c++)
			map[(ry * 16 + c / 16) * 192 + rx * 16 + c % 16] = rd16(scratch + c * 2);
	}
	plat_file_close(f);

	if ((chunk_file = open_file("STATIC", "U7CHUNKS")) < 0)
		return -5;
	chunk_data = open_file("ENGINE", "CHUNKS.DAT");
	if (chunk_data >= 0) {
		char title[sizeof E_CHUNKS_TITLE];
		if (!read_at(chunk_data, 0, title, sizeof title - 1)
			|| memcmp(title, E_CHUNKS_TITLE, sizeof title - 1) != 0) {
			plat_file_close(chunk_data);
			chunk_data = -1;
		}
	}
	/* the dataset's shapes if there are any (engine.h), else the original's, converted as read */
	converted = 0;
	if (flex_open(&shapes, "ENGINE", "SHAPES.FLX") == 0) {
		char title[sizeof E_SHAPES_TITLE];
		if (read_at(shapes.f, 0, title, sizeof title - 1)
			&& memcmp(title, E_SHAPES_TITLE, sizeof title - 1) == 0)
			converted = 1;
		else
			plat_file_close(shapes.f);
	}
	if (!converted) {
		if (flex_open(&shapes, "STATIC", "SHAPES.VGA") < 0)
			return -6;
		/* room to read the largest object shape in before converting it */
		raw_shape_size = 0;
		for (uint16_t t = TILE_TYPES; t < shapes.count; t++)
			if (shapes.table[t * 2 + 1] > raw_shape_size)
				raw_shape_size = shapes.table[t * 2 + 1];
		if ((raw_shape = take(raw_shape_size)) == NULL)
			return -7;
	}

	for (int i = 0; i < REGION_SLOTS; i++) {
		regions[i].id = -1;
		if ((regions[i].items = take(REGION_ITEMS * sizeof(struct Item))) == NULL)
			return -7;
	}
	if ((shape_box = take(1024 * sizeof *shape_box)) == NULL || (shape_boxed = take(1024)) == NULL)
		return -7;
	memset(shape_boxed, 0, 1024);
	if ((chunks = take(CHUNK_SLOTS * sizeof *chunks)) == NULL
		|| (chunk_slot = take(192 * 192)) == NULL)
		return -7;
	memset(chunk_slot, 0, 192 * 192);
	for (int i = 0; i < CHUNK_SLOTS; i++) {
		chunks[i].cx = chunks[i].cy = -1000;
		if ((chunks[i].statics = take(CHUNK_STATICS * sizeof(struct Static))) == NULL
			|| (chunks[i].bin_list = take(CHUNK_BIN_ENTRIES)) == NULL
			|| (chunks[i].solid_at = take(256 * 2)) == NULL
			|| (chunks[i].door_at = take(256 * 2)) == NULL
			|| (chunks[i].mover_at = take(256 * 2)) == NULL)
			return -7;
	}
	ring.pitch = ring.w = RING_W;
	ring.h = RING_H;
	if ((ring.pixels = take(RING_W * RING_H)) == NULL || (ring.rows = take(RING_H * 4)) == NULL)
		return -7;
	e_surface_rows(&ring);
	for (int i = 0; i < RING_AX * RING_AY; i++) {
		ring_ax[i] = ring_ay[i] = -1;
		ring_stale[i] = 0;
	}
	if ((loc = take((AREA_OBJECTS + 1) * sizeof *loc)) == NULL
		|| (loc_pairs = take(AREA_EDGES * 4)) == NULL || (loc_preds = take(AREA_EDGES * 2)) == NULL
		|| (loc_order = take(AREA_OBJECTS * 2)) == NULL)
		return -7;
	for (int i = 0; i < E_MOVERS; i++)
		if ((movers[i].near = take(MOVER_CANDS * sizeof *movers[i].near)) == NULL)
			return -7;
	open_sounds();
	open_text();
	open_faces();
	open_gumps();
	{
		/* the usecode's rooms, when there is usecode to run */
		int16_t f = open_file("STATIC", "USECODE");
		if (f >= 0) {
			plat_file_close(f);
			void *c = take(E_UC_CODE), *h = take(E_UC_HEAP);
			if (c != NULL && h != NULL)
				e_uc_memory(c, h);
		}
	}
	pool = pool_next = pool_pinned = mem_next;
	pool_end = mem_end;
	if (pool_end - pool < 256 * 1024)
		return -8;
	return 0;
}

void e_surface_rows(struct ESurface *s)
{
	for (uint16_t i = 0; i < s->h; i++) {
		uint32_t a = (uint32_t) i * s->pitch;
		s->rows[i * 4] = (uint8_t) a;
		s->rows[i * 4 + 1] = (uint8_t) (a >> 8);
		s->rows[i * 4 + 2] = (uint8_t) (a >> 16);
		s->rows[i * 4 + 3] = (uint8_t) (a >> 24);
	}
}

const uint32_t *e_frame_runs(uint16_t type, uint16_t frame)
{
	const int16_t *f = frame_of(type, frame);

	return f != NULL ? (const uint32_t *) (f + 4) : NULL;
}

const uint8_t *e_palette(void)
{
	return palette;
}

static int16_t view_x, view_y, last_x, last_y;
static int8_t heading_x, heading_y; /* the way the view last moved, per axis */

void e_set_view(int16_t x, int16_t y)
{
	view_x = x;
	view_y = y;
}

void e_near_window(int16_t *x0, int16_t *y0, int16_t *x1, int16_t *y1)
{
	/* the chunks under the view and one round them: those the chunk slots keep */
	*x0 = (int16_t) (((view_x >> 4) - 1) * 16);
	*y0 = (int16_t) (((view_y >> 4) - 1) * 16);
	*x1 = (int16_t) ((((view_x + E_VIEW_W / 8 - 1) >> 4) + 2) * 16 - 1);
	*y1 = (int16_t) ((((view_y + E_VIEW_H / 8 - 1) >> 4) + 2) * 16 - 1);
}

/* The next area ahead of the view not yet in the ring, if the ring has room for it beside what
 * the view covers: 1 and its place when there is one. */
void e_copy_view(struct ESurface *frame, const struct ESurface *r, int16_t tx, int16_t ty)
{
	int16_t w = (int16_t) (RING_W - tx < E_VIEW_W ? RING_W - tx : E_VIEW_W);
	int16_t h = (int16_t) (RING_H - ty < E_VIEW_H ? RING_H - ty : E_VIEW_H);

	eb_copy(frame, 0, 0, r, tx, ty, w, h);
	if (w < E_VIEW_W)
		eb_copy(frame, w, 0, r, 0, ty, (int16_t) (E_VIEW_W - w), h);
	if (h < E_VIEW_H) {
		eb_copy(frame, 0, h, r, tx, 0, w, (int16_t) (E_VIEW_H - h));
		if (w < E_VIEW_W)
			eb_copy(frame, w, h, r, 0, 0, (int16_t) (E_VIEW_W - w), (int16_t) (E_VIEW_H - h));
	}
}

/* Where the movers were drawn in each frame (a layer, shown over the ring), to be cleared before
 * it is drawn in again; a few frames' worth (the backend's buffers). */
#define DRAWN_FRAMES 4
static struct Drawn {
	const uint8_t *pixels;
	uint8_t n;
	int16_t r[E_MOVERS][4];
} drawn[DRAWN_FRAMES];

static struct Drawn *drawn_of(const struct ESurface *frame)
{
	struct Drawn *free_one = NULL;

	for (int i = 0; i < DRAWN_FRAMES; i++) {
		if (drawn[i].pixels == frame->pixels)
			return &drawn[i];
		if (drawn[i].pixels == NULL && free_one == NULL)
			free_one = &drawn[i];
	}
	if (free_one != NULL)
		free_one->pixels = frame->pixels;
	return free_one;
}

static void clear_drawn(struct ESurface *frame)
{
	struct Drawn *d = drawn_of(frame);

	if (d == NULL)
		return;
	for (uint8_t i = 0; i < d->n; i++) {
		int16_t x0 = (int16_t) (d->r[i][0] & ~7), x1 = (int16_t) ((d->r[i][2] + 8) & ~7);
		eb_clear(frame, x0, d->r[i][1], (int16_t) (x1 - x0), (int16_t) (d->r[i][3] - d->r[i][1] + 1));
	}
	d->n = 0;
}

/* A mover's picture drawn in frame, clipped to it: to be cleared next time. */
static void note_drawn(const struct ESurface *frame, const int16_t clip[4])
{
	struct Drawn *d = drawn_of(frame);

	if (d != NULL && d->n < E_MOVERS)
		memcpy(d->r[d->n++], clip, sizeof d->r[0]);
}

/* The next area ahead of the view, the way it last moved, that the ring has room for. When the
 * backend shows the ring itself (eb_view gave 0), the area left behind is only given up a little
 * way past (RING_TRAIL pixels): it may still be on the screen for a frame or two. */
#define RING_TRAIL 16
static int next_ahead(int16_t vx, int16_t vy, int ax0, int ay0, int ax1, int ay1, int *ax, int *ay)
{
	int trail = ring_shown ? RING_TRAIL : 0;
	int tx0 = heading_x > 0 ? (vx - trail) >> 7 : ax0;
	int tx1 = heading_x < 0 ? (vx + E_VIEW_W - 1 + trail) >> 7 : ax1;
	int ty0 = heading_y > 0 ? (vy - trail) >> 7 : ay0;
	int ty1 = heading_y < 0 ? (vy + E_VIEW_H - 1 + trail) >> 7 : ay1;

	if (heading_x != 0 && tx1 - tx0 + 1 < RING_AX) {
		int col = heading_x > 0 ? ax1 + 1 : ax0 - 1;
		for (int y = ay0; y <= ay1; y++)
			if (col >= 0 && col < 192 && !in_ring(col, y)) {
				*ax = col;
				*ay = y;
				return 1;
			}
	}
	if (heading_y != 0 && ty1 - ty0 + 1 < RING_AY) {
		int row = heading_y > 0 ? ay1 + 1 : ay0 - 1;
		for (int x = ax0; x <= ax1; x++)
			if (row >= 0 && row < 192 && !in_ring(x, row)) {
				*ax = x;
				*ay = row;
				return 1;
			}
	}
	return 0;
}

void e_draw(struct ESurface *frame)
{
	int16_t vx = (int16_t) (view_x * 8), vy = (int16_t) (view_y * 8);
	int ax0 = vx >> 7, ay0 = vy >> 7, ax1 = (vx + E_VIEW_W - 1) >> 7, ay1 = (vy + E_VIEW_H - 1) >> 7;
	uint32_t t0 = eb_clock(), t1;
	int drew = 0, ax, ay;

	if (view_x != last_x)
		heading_x = view_x > last_x ? 1 : -1;
	if (view_y != last_y)
		heading_y = view_y > last_y ? 1 : -1;
	last_x = view_x;
	last_y = view_y;

	/* the areas under the view, now; else one ahead of it */
	for (ay = ay0; ay <= ay1; ay++)
		for (ax = ax0; ax <= ax1; ax++)
			if (ax < 192 && ay < 192 && !in_ring(ax, ay)) {
				draw_area(ax, ay);
				drew = 1;
			}
	/* areas drawn for another ceiling: out of view, given up; in view, the one nearest the middle
	 * drawn again */
	int best = -1, best_d = 0;
	for (int a = 0; a < RING_AX * RING_AY; a++) {
		if (!ring_stale[a])
			continue;
		if (ring_ax[a] < ax0 || ring_ax[a] > ax1 || ring_ay[a] < ay0 || ring_ay[a] > ay1) {
			ring_ax[a] = ring_ay[a] = -1;
			ring_stale[a] = 0;
			continue;
		}
		int dx = ring_ax[a] * 128 + 64 - (vx + E_VIEW_W / 2);
		int dy = ring_ay[a] * 128 + 64 - (vy + E_VIEW_H / 2);
		int d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
		if (best < 0 || d < best_d) {
			best = a;
			best_d = d;
		}
	}
	if (!drew && best >= 0) {
		draw_area(ring_ax[best], ring_ay[best]);
		drew = 1;
	}
	if (!drew && next_ahead(vx, vy, ax0, ay0, ax1, ay1, &ax, &ay))
		draw_area(ax, ay);
	t1 = eb_clock();
	e_stats.build_time += t1 - t0;
	t0 = t1;

	/* the ring under the view: copied, or shown under the frame with the frame cleared where the
	 * movers were drawn in it before */
	ring_shown = !eb_view(frame, &ring, (int16_t) (vx % RING_W), (int16_t) (vy % RING_H));
	if (ring_shown)
		clear_drawn(frame);
	t1 = eb_clock();
	e_stats.copy_time += t1 - t0;
	t0 = t1;
	e_stats.drawn = 0;
	draw_movers(frame, vx, vy);
	draw_barks(frame, vx, vy);
	t1 = eb_clock();
	e_stats.object_time += t1 - t0;
	eb_finish();
	e_stats.finish_time += eb_clock() - t1;
}

/* ---- what is under a point of the view (the original's click on an item) ---- */

/* Whether a frame (from its hot spot drawn at px, py, turned or not) has a pixel at x, y. */
static int frame_hits(uint16_t tf, int px, int py, int x, int y)
{
	const int16_t *f = frame_of(tf & 0x3ff, (tf >> 10) & 31);

	if (f == NULL)
		return 0;
	int dx = x - px, dy = y - py;
	if (tf & 0x8000) {
		int t = dx; dx = dy; dy = t;
	}
	if (dx < -f[1] || dx > f[0] || dy < -f[2] || dy > f[3])
		return 0;
	for (const uint32_t *r = (const uint32_t *) (f + 4); *r != 0;) {
		uint32_t h = *r++;
		int across = (int8_t) (h >> 24), along = (int8_t) (h >> 16), len = (int) E_RUN_LENGTH(h);
		if (across == dy && dx >= along && dx < along + len)
			return 1;
		r += h & E_RUN_FILL ? 1 : (uint32_t) (len + 3) / 4;
	}
	return 0;
}

static int static_hits(const struct Static *s, int x, int y)
{
	return s->z <= ceiling && x >= s->r[0] && x <= s->r[2] && y >= s->r[1] && y <= s->r[3]
		&& frame_hits(s->tf, s->x * 8 + 7 - s->z * 4, s->y * 8 + 7 - s->z * 4, x, y);
}

int e_pick(int16_t sx, int16_t sy, struct EThing *out, int *mover)
{
	int x = view_x * 8 + sx, y = view_y * 8 + sy;
	const struct Static *best = NULL;
	int16_t wx0, wy0, wx1, wy1;

	*mover = -1;
	e_near_window(&wx0, &wy0, &wx1, &wy1);
	for (int cy = wy0 >> 4; cy <= wy1 >> 4 && cy < 192; cy++)
		for (int cx = wx0 >> 4; cx <= wx1 >> 4 && cx < 192; cx++) {
			const struct Chunk *k = chunk_statics(cx, cy);
			if (x < k->reach[0] || x > k->reach[2] || y < k->reach[1] || y > k->reach[3])
				continue;
			for (uint16_t j = 0; j < k->nstatic; j++) {
				const struct Static *s = &k->statics[j];
				if (static_hits(s, x, y) && (best == NULL || order_of(s, best) >= 0))
					best = s;
			}
		}
	for (uint8_t m = 0; m < nmovers; m++)
		if (movers[m].ok && static_hits(&movers[m].s, x, y)
			&& (best == NULL || order_of(&movers[m].s, best) >= 0)) {
			best = &movers[m].s;
			*mover = m;
		}
	if (best == NULL) {
		/* the ground: its cell */
		out->tf = 0;
		out->x = (int16_t) (x >> 3);
		out->y = (int16_t) (y >> 3);
		out->z = 0;
		return 0;
	}
	if (*mover >= 0 && best != &movers[*mover].s)
		*mover = -1;
	out->tf = best->tf;
	out->x = best->x;
	out->y = best->y;
	out->z = best->z;
	return 1;
}
