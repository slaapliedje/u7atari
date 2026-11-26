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
#define CHUNK_SLOTS 36         /* the ring's 4 by 3 areas and those around them, 6 by 5 */
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
#define CLASS_NONE 0

static struct Info info[1024];
static uint8_t palette[768];
static uint16_t *map;           /* chunk number of each of the 192 by 192 chunks */
static struct Flex shapes;
static int16_t chunk_file;

/* ---- the shape cache: a ring, the oldest shapes giving way; the movers' kept below it ---- */

#define SHAPE_QUEUE 1024
static const uint8_t *shape_at[1024];
static uint8_t *pool, *pool_next, *pool_pinned, *pool_end;
static struct Resident {
	uint16_t type;
	uint8_t *start;
} queue[SHAPE_QUEUE];
static uint16_t queue_head, queue_count;

static void evict_oldest(void)
{
	shape_at[queue[queue_head].type] = NULL;
	queue_head = (uint16_t) ((queue_head + 1) % SHAPE_QUEUE);
	queue_count--;
}

/* A shape's bytes, read in if need be; NULL when it has none or cannot fit. On a backend that
 * draws as the CPU goes on, reading in waits for the drawing first (its plat_file_read): a shape
 * given way to may still be being drawn from. */
static const uint8_t *shape(uint16_t type)
{
	uint32_t at, size;

	if (type >= shapes.count || shape_at[type] != NULL)
		return type < shapes.count ? shape_at[type] : NULL;
	at = shapes.table[type * 2];
	size = (shapes.table[type * 2 + 1] + 7) & ~(uint32_t) 7;
	if (size == 0 || size > (uint32_t) (pool_end - pool_pinned))
		return NULL;
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
	if (!read_at(shapes.f, at, pool_next, shapes.table[type * 2 + 1]))
		return NULL;
	shape_at[type] = pool_next;
	queue[(queue_head + queue_count) % SHAPE_QUEUE].type = type;
	queue[(queue_head + queue_count) % SHAPE_QUEUE].start = pool_next;
	queue_count++;
	pool_next += size;
	e_stats.shape_loads++;
	e_stats.shape_bytes = (uint32_t) (pool_end - pool_pinned);
	return shape_at[type];
}

/* An object shape's frame header: xright, xleft, yabove, ybelow; its first span after it. */
static const uint8_t *frame_of(uint16_t type, uint16_t frame)
{
	const uint8_t *s = shape(type);
	uint32_t first;

	if (s == NULL)
		return NULL;
	first = rd32(s + 4);
	if ((frame + 1) * 4u >= first)
		return NULL;
	return s + rd32(s + 4 + frame * 4u);
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

	/* fixed items: a flex of 256 entries, one a chunk, of 4-byte records; and the movable ones:
	 * 256 trees, one a chunk, of records by length (6, 12, 18; 0 or 1 ends a level), whose
	 * containers' contents follow them a level down (not drawn) */
	uint16_t fixedStart[257];
	uint8_t *fixed = NULL;
	snprintf(name, sizeof name, "U7IFIX%02X", id);
	if ((f = open_file("STATIC", name)) >= 0) {
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
	if ((f = open_file("GAMEDAT", name)) >= 0) {
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
};

struct Chunk {
	int16_t cx, cy;
	uint16_t age;
	uint8_t nobj, built;
	uint16_t nstatic;
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
static uint16_t chunk_epoch;    /* counts slots given to another chunk: statics moved */

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
	chunk_epoch++;
	chunk_slot[cy * 192 + cx] = (uint8_t) (k - chunks + 1);
	k->cx = (int16_t) cx;
	k->cy = (int16_t) cy;
	k->age = ++age_now;
	k->nobj = 0;
	k->built = 0;
	k->nstatic = 0;
	memset(k->flats, 0, sizeof k->flats);
	if (!read_at(chunk_file, map[cy * 192 + cx] * 512u, k->cells, 512))
		memset(k->cells, 0, 512);
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
	const uint8_t *f = frame_of(tf & 0x3ff, (tf >> 10) & 31);
	int16_t right, left, above, below, px, py;

	if (f == NULL)
		return 0;
	right = rds16(f);
	left = rds16(f + 2);
	above = rds16(f + 4);
	below = rds16(f + 6);
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
		const uint8_t *s = shape(type);
		int16_t *b = shape_box[type];
		b[0] = b[1] = b[2] = b[3] = 0;
		if (s != NULL) {
			uint32_t frames = (rd32(s + 4) - 4) / 4;
			for (uint32_t f = 0; f < frames; f++) {
				const uint8_t *h = s + rd32(s + 4 + f * 4);
				for (int i = 0; i < 4; i++)
					if (rds16(h + i * 2) > b[i])
						b[i] = rds16(h + i * 2);
			}
		}
		shape_boxed[type] = 1;
	}
	return shape_box[type];
}

/* The chunk with its statics listed, in the chunk's own order. */
static struct Chunk *chunk_statics(int cx, int cy)
{
	struct Chunk *k = chunk(cx, cy);

	if (k->built)
		return k;
	struct Region *r = region((cy >> 4) * 12 + (cx >> 4));
	int c = (cy & 15) * 16 + (cx & 15);
	int rx = (cx >> 4) << 8, ry = (cy >> 4) << 8;
	int n = k->nobj + r->start[c + 1] - r->start[c];

	k->nstatic = 0;
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
		s->flat = info[s->tf & 0x3ff].height == 0;
		k->nstatic++;
	}
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

	if (q[2] < ox || q[3] < oy || q[0] >= ox + CHUNK_BINS * 32 || q[1] >= oy + CHUNK_BINS * 32)
		return 0;
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
	uint16_t type = s->tf & 0x3ff;
	int fx = info[type].fx, fy = info[type].fy;

	if (s->tf & 0x8000) {
		int t = fx; fx = fy; fy = t;
	}
	l->s = s;
	memcpy(l->r, s->r, sizeof l->r);
	l->cx1 = s->x;
	l->cx0 = (int16_t) (s->x - fx);
	l->cy1 = s->y;
	l->cy0 = (int16_t) (s->y - fy);
	l->z0 = s->z;
	l->z1 = (uint8_t) (s->z + info[type].height);
}

/* -1: a is drawn before b; 1: after; 0: either. By the ranges of their footprints and levels:
 * what lies south or east, or above, is in front. */
static int compare(const struct Local *a, const struct Local *b)
{
	int xc = a->cx1 < b->cx0 ? -1 : a->cx0 > b->cx1 ? 1 : 0;
	int yc = a->cy1 < b->cy0 ? -1 : a->cy0 > b->cy1 ? 1 : 0;
	int zc = a->z1 <= b->z0 ? -1 : a->z0 >= b->z1 ? 1 : 0;

	e_stats.compares++;
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

static inline int overlaps(const int16_t *a, int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
	return a[0] <= x1 && x0 <= a[2] && a[1] <= y1 && y0 <= a[3];
}

/* Draws a static into dst, world pixel (ox, oy) at dst's (0, 0), clipped. */
static void draw_static(struct ESurface *dst, const int16_t clip[4], const struct Static *s,
	int16_t ox, int16_t oy)
{
	const uint8_t *f = frame_of(s->tf & 0x3ff, (s->tf >> 10) & 31);

	if (f == NULL)
		return;
	eb_shape(dst, clip, f + 8, (int16_t) (s->x * 8 + 7 - s->z * 4 - ox),
		(int16_t) (s->y * 8 + 7 - s->z * 4 - oy), (s->tf & 0x8000) != 0);
}

/* ---- the ring of areas ---- */

#define RING_AX 4
#define RING_AY 3
#define RING_W (RING_AX * 128)
#define RING_H (RING_AY * 128)
static struct ESurface ring;
static int16_t ring_ax[RING_AX * RING_AY], ring_ay[RING_AX * RING_AY];
static struct EFlat flat_table[E_FLAT_TYPES];

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
					if (pass == 0)
						draw_static(&ring, clip, s, ox, oy);
					else if (nloc < AREA_OBJECTS)
						make_local(&loc[nloc++], s);
				}
			}
	e_stats.statics = nloc;

	/* each overlapping pair once; then by object its list, and the walk */
	for (uint16_t i = 0; i < nloc; i++)
		for (uint16_t j = (uint16_t) (i + 1); j < nloc; j++) {
			if (!overlaps(loc[i].r, loc[j].r[0], loc[j].r[1], loc[j].r[2], loc[j].r[3]))
				continue;
			int r = compare(&loc[i], &loc[j]);
			if (r != 0 && npairs < AREA_EDGES)
				loc_pairs[npairs++] = r < 0 ? (uint32_t) j << 16 | i : (uint32_t) i << 16 | j;
		}
	e_stats.edges = npairs;
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
	for (uint16_t i = 0; i < nlocorder; i++)
		draw_static(&ring, clip, loc[loc_order[i]].s, ox, oy);
	ring_ax[slot] = (int16_t) ax;
	ring_ay[slot] = (int16_t) ay;
}

static int in_ring(int ax, int ay)
{
	int slot = (ay % RING_AY) * RING_AX + ax % RING_AX;

	return ring_ax[slot] == ax && ring_ay[slot] == ay;
}

/* ---- movers ---- */

#define MOVER_FRONTS 32
static struct Mover {
	uint16_t tf;
	int16_t x, y;
	uint8_t z, ok;
	struct Static s;
	struct Local l;
	/* the statics in front of it, in drawing order, worked out for it as it is (s) while no
	 * chunk's statics have moved since (epoch); valid, or not yet */
	uint8_t cached, nfront;
	uint16_t epoch;
	const struct Static *fronts[MOVER_FRONTS];
} movers[E_MOVERS];
static uint8_t nmovers;

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
static void draw_movers(struct ESurface *frame, int16_t vx, int16_t vy)
{
	uint8_t sorted[E_MOVERS], n = 0;
	static struct Local cand[128], front[64];

	for (uint8_t m = 0; m < nmovers; m++) {
		struct Mover *v = &movers[m];
		/* what is in front of it depends on where it stands and its shape, not on which frame it
		 * shows: the statics over the box of all its frames are found once it stands somewhere */
		if (v->s.x != v->x || v->s.y != v->y || v->s.z != v->z
			|| ((v->s.tf ^ v->tf) & 0x83ff) != 0)
			v->cached = 0;
		v->s.tf = v->tf;
		v->s.x = v->x;
		v->s.y = v->y;
		v->s.z = v->z;
		v->ok = picture_rect(v->tf, v->x, v->y, v->z, v->s.r);
		if (!v->ok || v->s.r[2] < vx || v->s.r[0] >= vx + E_VIEW_W || v->s.r[3] < vy
			|| v->s.r[1] >= vy + E_VIEW_H)
			continue;
		make_local(&v->l, &v->s);
		uint8_t k = n++;
		while (k > 0 && compare(&v->l, &movers[sorted[k - 1]].l) < 0) {
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
		uint8_t ncand = 0, nfront = 0;

		draw_static(frame, whole, &v->s, vx, vy);
		e_stats.drawn++;
		uint32_t f0 = eb_clock();
		if (!v->cached || v->epoch != chunk_epoch) {
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
		/* the statics over it, from the chunks whose objects can reach it */
		for (int cy = (q[1] - REACH_AFTER) >> 7; cy <= (q[3] + REACH_BEFORE) >> 7; cy++)
			for (int cx = (q[0] - REACH_AFTER) >> 7; cx <= (q[2] + REACH_BEFORE) >> 7; cx++) {
				if (cx < 0 || cy < 0 || cx >= 192 || cy >= 192)
					continue;
				struct Chunk *k = chunk_statics(cx, cy);
				uint8_t found[CHUNK_STATICS];
				int count = chunk_find(k, q, 0, found, CHUNK_STATICS);
				for (int j = 0; j < count && ncand < 128; j++)
					make_local(&cand[ncand++], &k->statics[found[j]]);
			}
		/* in front of it, or of one found in front of it: each new one is tried against those
		 * left; then they are drawn each after those it is in front of */
		uint8_t draw[64];
		for (int8_t against = -1; against < (int8_t) nfront; against++) {
			const struct Local *by = against < 0 ? &v->l : &front[against];
			for (uint8_t j = 0; j < ncand && nfront < 64; j++)
				if (cand[j].s != NULL && compare(&cand[j], by) > 0) {
					front[nfront++] = cand[j];
					cand[j].s = NULL;
				}
		}
		for (uint8_t f = 0; f < nfront; f++) {
			uint8_t at = f;
			while (at > 0 && compare(&front[f], &front[draw[at - 1]]) < 0) {
				draw[at] = draw[at - 1];
				at--;
			}
			draw[at] = f;
		}
		v->nfront = nfront < MOVER_FRONTS ? nfront : MOVER_FRONTS;
		for (uint8_t f = 0; f < v->nfront; f++)
			v->fronts[f] = front[draw[f]].s;
		/* the chunks looked in may have taken slots: the epoch after them */
		v->epoch = chunk_epoch;
		v->cached = 1;
		}
		e_stats.front_time += eb_clock() - f0;
		if (clip[0] < 0) clip[0] = 0;
		if (clip[1] < 0) clip[1] = 0;
		if (clip[2] > E_VIEW_W - 1) clip[2] = E_VIEW_W - 1;
		if (clip[3] > E_VIEW_H - 1) clip[3] = E_VIEW_H - 1;
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
		info[i].flags = (uint8_t) ((t[0] & 4 ? INFO_ANIMATED : 0) | (t[2] & 0x80 ? INFO_TRANSLUCENT : 0));
	}

	if (flex_open(&pal, "STATIC", "PALETTES.FLX") < 0 || !read_at(pal.f, pal.table[0], palette, 768))
		return -3;
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
	if (flex_open(&shapes, "STATIC", "SHAPES.VGA") < 0)
		return -6;

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

const uint8_t *e_frame_spans(uint16_t type, uint16_t frame)
{
	const uint8_t *f = frame_of(type, frame);

	return f != NULL ? f + 8 : NULL;
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
static int next_ahead(int ax0, int ay0, int ax1, int ay1, int *ax, int *ay)
{
	if (heading_x != 0 && ax1 - ax0 + 1 < RING_AX) {
		int col = heading_x > 0 ? ax1 + 1 : ax0 - 1;
		for (int y = ay0; y <= ay1; y++)
			if (col >= 0 && col < 192 && !in_ring(col, y)) {
				*ax = col;
				*ay = y;
				return 1;
			}
	}
	if (heading_y != 0 && ay1 - ay0 + 1 < RING_AY) {
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
	if (!drew && next_ahead(ax0, ay0, ax1, ay1, &ax, &ay))
		draw_area(ax, ay);
	t1 = eb_clock();
	e_stats.build_time += t1 - t0;
	t0 = t1;

	/* the ring under the view, in up to four pieces where it wraps */
	int16_t tx = (int16_t) (vx % RING_W), ty = (int16_t) (vy % RING_H);
	int16_t w = (int16_t) (RING_W - tx < E_VIEW_W ? RING_W - tx : E_VIEW_W);
	int16_t h = (int16_t) (RING_H - ty < E_VIEW_H ? RING_H - ty : E_VIEW_H);
	eb_copy(frame, 0, 0, &ring, tx, ty, w, h);
	if (w < E_VIEW_W)
		eb_copy(frame, w, 0, &ring, 0, ty, (int16_t) (E_VIEW_W - w), h);
	if (h < E_VIEW_H) {
		eb_copy(frame, 0, h, &ring, tx, 0, w, (int16_t) (E_VIEW_H - h));
		if (w < E_VIEW_W)
			eb_copy(frame, w, h, &ring, 0, 0, (int16_t) (E_VIEW_W - w), (int16_t) (E_VIEW_H - h));
	}
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
