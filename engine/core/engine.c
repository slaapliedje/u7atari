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
	}
}

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
	static struct Item movable[REGION_ITEMS];
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
			int32_t i = 0;
			for (int c = 0; c < 256; c++) {
				int depth = 0;
				movableStart[c] = ireg;
				while (i < len) {
					uint8_t l = scratch[i++];
					if (l == 0 || l == 1) {
						if (depth == 0)
							break;
						depth--;
						continue;
					}
					const uint8_t *d = scratch + i;
					i += l;
					if (depth == 0 && ireg < REGION_ITEMS) {
						struct Item *it = &movable[ireg++];
						it->x = d[0];
						it->y = d[1];
						it->tf = rd16(d + 2);
						it->z = (l == 6 ? d[4] : d[9]) >> 4;
					}
					if (l >= 12 && rd16(d + 4) != 0)
						depth++;
				}
			}
		}
		plat_file_close(f);
	}
	movableStart[256] = ireg;
	/* items: per chunk, its fixed ones then its movable ones */
	static struct Item merged[REGION_ITEMS];
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
};

static struct Chunk *chunks;
static uint8_t *chunk_slot;     /* per chunk of the world, its slot + 1, or 0 */
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

/* ---- the ceiling over a mover, as the original's (UpdateCeiling): the levels the solid statics
 * (not doors) over its row of cells take, its own left out; the highest less one, or 15 ---- */

int e_follow(int i)
{
	const struct Mover *v = &movers[i];
	const struct Info *in = &info[v->tf & 0x3ff];
	int w = v->tf & 0x8000 ? in->fy : in->fx;
	uint16_t taken = 0, own = (uint16_t) (((1u << in->height) - 1) << v->z);
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
	taken &= (uint16_t) ~own;
	if (taken != 0)
		for (top = 15; !(taken & 0x8000); taken = (uint16_t) (taken << 1))
			top--;
	if (taken != 0)
		top--;
	if (top != ceiling) {
		/* the areas with statics between the two drawn again, and the fronts found again */
		int low = top < ceiling ? top : ceiling;
		ceiling = (uint8_t) top;
		for (int a = 0; a < RING_AX * RING_AY; a++)
			if (ring_top[a] > low)
				ring_ax[a] = ring_ay[a] = -1;
		for (uint8_t m = 0; m < nmovers; m++)
			movers[m].cached = 0;
	}
	return ceiling;
}

/* Levels (a bit each) the solid statics take over the cells x0..x1, y0..y1; doors too or not. */
static uint16_t levels_taken(int x0, int y0, int x1, int y1, int doors)
{
	uint16_t taken = 0;

	for (int cy = y0 >> 4; cy <= (y1 + 7) >> 4; cy++)
		for (int cx = x0 >> 4; cx <= (x1 + 7) >> 4; cx++) {
			if (cx < 0 || cy < 0 || cx >= 192 || cy >= 192)
				continue;
			const struct Chunk *k = chunk_statics(cx, cy);
			for (uint16_t j = 0; j < k->nstatic; j++) {
				const struct Static *s = &k->statics[j];
				const struct Info *in = &info[s->tf & 0x3ff];
				if (!(in->flags & INFO_SOLID) || (!doors && (in->flags & INFO_DOOR))
					|| s->height == 0 || y0 > s->y || y1 < s->y - s->fy || x0 > s->x
					|| x1 < s->x - s->fx)
					continue;
				taken |= (uint16_t) (((1u << s->height) - 1) << s->z);
			}
		}
	return taken;
}

int e_blocked(uint16_t tf, int16_t x, int16_t y, uint8_t z)
{
	const struct Info *in = &info[tf & 0x3ff];
	int fx = tf & 0x8000 ? in->fy : in->fx, fy = tf & 0x8000 ? in->fx : in->fy;
	uint16_t body = (uint16_t) (((1u << (in->height ? in->height : 1)) - 1) << z);

	return (levels_taken(x - fx, y - fy, x, y, 0) & body) != 0;
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
			/* what it carries, as a region's movable items: levels ended by 0 or 1 */
			int depth = 0;
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
				if (l >= 12 && rd16(item + 4) != 0)
					depth++;
			}
		}
		if (ref == 0 || (tf & 0x3ff) == 0 || (status & 0x8000) || n >= max)
			continue;
		uint8_t region = rec[6];
		out[n].tf = tf;
		out[n].x = (int16_t) (region % 12 * 256 + rec[0]);
		out[n].y = (int16_t) (region / 12 * 256 + rec[1]);
		out[n].z = rec[9] >> 4;
		out[n].pad = 0;
		out[n].number = (uint16_t) i;
		n++;
	}
done:
	plat_file_close(npc_file);
	return n;
}

void e_movers_near(const struct ENpc *npcs, int n, int16_t x, int16_t y)
{
	static uint16_t near[E_MOVERS];
	static int32_t far[E_MOVERS];
	int nnear = 0;

	/* those in a window round it, then the nearest of them: no multiplying for the rest */
	for (int i = 0; i < n; i++) {
		int dx = npcs[i].x - x, dy = npcs[i].y - y;
		if (npcs[i].number == 0 || dx < -48 || dx > 48 || dy < -40 || dy > 40)
			continue;
		int32_t d = (int32_t) (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
		int at = nnear < E_MOVERS - 1 ? nnear++ : E_MOVERS - 2;
		if (at == E_MOVERS - 2 && nnear == E_MOVERS - 1 && d >= far[at])
			continue;
		while (at > 0 && far[at - 1] > d) {
			far[at] = far[at - 1];
			near[at] = near[at - 1];
			at--;
		}
		far[at] = d;
		near[at] = (uint16_t) i;
	}
	for (int slot = 1; slot < E_MOVERS; slot++)
		if (slot - 1 < nnear) {
			const struct ENpc *p = &npcs[near[slot - 1]];
			e_mover_set(slot, p->tf, p->x, p->y, p->z);
		} else
			e_mover_set(slot, 0, 0, 0, 0);
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
	movers[i].tf = tf;
	movers[i].x = x;
	movers[i].y = y;
	movers[i].z = z;
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

	if (xc == 0 && yc == 0)
		return zc;
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
			|| (chunks[i].bin_list = take(CHUNK_BIN_ENTRIES)) == NULL)
			return -7;
	}
	ring.pitch = ring.w = RING_W;
	ring.h = RING_H;
	if ((ring.pixels = take(RING_W * RING_H)) == NULL || (ring.rows = take(RING_H * 4)) == NULL)
		return -7;
	e_surface_rows(&ring);
	for (int i = 0; i < RING_AX * RING_AY; i++)
		ring_ax[i] = ring_ay[i] = -1;
	if ((loc = take((AREA_OBJECTS + 1) * sizeof *loc)) == NULL
		|| (loc_pairs = take(AREA_EDGES * 4)) == NULL || (loc_preds = take(AREA_EDGES * 2)) == NULL
		|| (loc_order = take(AREA_OBJECTS * 2)) == NULL)
		return -7;
	for (int i = 0; i < E_MOVERS; i++)
		if ((movers[i].near = take(MOVER_CANDS * sizeof *movers[i].near)) == NULL)
			return -7;
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
	t1 = eb_clock();
	e_stats.object_time += t1 - t0;
	eb_finish();
	e_stats.finish_time += eb_clock() - t1;
}
