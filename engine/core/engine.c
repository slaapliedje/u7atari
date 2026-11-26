/* The engine test's core: the world view from the original's files (engine.h, engine/README.md).
 *
 * What is done per frame is kept small. When the view crosses into another chunk, the static
 * objects of the 5 by 4 chunks from its first are put in drawing order, and with the terrain drawn
 * once into the static layer, a picture as large as the view can scroll over before the next
 * crossing. Each frame copies the layer's part under the view (one blit), fits the movers into
 * the order, and draws them and, clipped to them, the statics that stand in front of them.
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
#define REGION_SLOTS 4         /* a window touches at most 2 by 2 */
#define REGION_ITEMS 4096
#define CHUNK_SLOTS 24         /* the window's 20 */
#define WIN_CX 5                /* chunks in the window: the view's 3 or 4 and those beyond */
#define WIN_CY 4
#define WIN_OBJECTS 2048
#define WIN_EDGES 8192
#define BIN 64                  /* pixels a side of a screen bin */
#define BINS_X 16
#define BINS_Y 14
#define BIN_ENTRIES 8192

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

/* ---- the shape cache ---- */

static const uint8_t *shape_at[1024];
static uint8_t *pool, *pool_next, *pool_pinned, *pool_end;

static void shapes_flush(void)
{
	for (int i = 0; i < 1024; i++)
		if (shape_at[i] >= pool_pinned)
			shape_at[i] = NULL;
	pool_next = pool_pinned;
}

static const uint8_t *shape(uint16_t type)
{
	uint32_t at, size;

	if (shape_at[type] != NULL || type >= shapes.count)
		return shape_at[type];
	at = shapes.table[type * 2];
	size = shapes.table[type * 2 + 1];
	if (size == 0 || (uint32_t) (pool_end - pool_next) < size)
		return NULL;
	if (!read_at(shapes.f, at, pool_next, size))
		return NULL;
	shape_at[type] = pool_next;
	pool_next += (size + 7) & ~(uint32_t) 7;
	e_stats.shape_loads++;
	e_stats.shape_bytes = (uint32_t) (pool_next - pool);
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

/* ---- chunks: the cell words of the 16 by 16, and the cells holding object shapes ---- */

#define CHUNK_OBJECTS 96

struct Chunk {
	int16_t cx, cy;
	uint16_t age;
	uint8_t nobj;
	uint32_t flats[5];          /* the flat types its cells use, a bit each */
	uint8_t cells[512];
	uint8_t obj[CHUNK_OBJECTS]; /* cells with a shape of class none: objects at z 0 */
};

static struct Chunk *chunks;

static struct Chunk *chunk(int cx, int cy)
{
	struct Chunk *k, *old = &chunks[0];

	for (int i = 0; i < CHUNK_SLOTS; i++) {
		if (chunks[i].cx == cx && chunks[i].cy == cy) {
			chunks[i].age = ++age_now;
			return &chunks[i];
		}
		if (chunks[i].age < old->age)
			old = &chunks[i];
	}
	k = old;
	k->cx = (int16_t) cx;
	k->cy = (int16_t) cy;
	k->age = ++age_now;
	k->nobj = 0;
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

/* ---- the window's objects and their order ---- */

struct Obj {
	const uint8_t *spans;
	int16_t px, py;             /* hot spot, world pixels */
	int16_t x0, y0, x1, y1;     /* picture's rectangle, world pixels, inclusive */
	int16_t cx0, cx1, cy0, cy1; /* footprint, cells, inclusive */
	uint8_t z0, z1;             /* levels: bottom, top (bottom + height) */
	uint8_t flipped, mark;
	uint16_t pred;              /* first of its "drawn before" list in preds[] */
	uint16_t rank;              /* its place in the order */
};

static struct Obj *objs;        /* the window's statics, standing ones first */
static uint16_t nobj, nstanding;
static uint16_t *order;         /* standing objects in drawing order */
static uint16_t *preds;         /* per object, the objects drawn before it */
static uint32_t *pairs;         /* (to << 16 | from) while building */
static uint16_t *bin_start, *bin_list;
static int16_t win_cx = -1000, win_cy = -1000;
static int16_t bins_x0, bins_y0; /* world pixel at bin (0, 0) */
static int16_t view_x, view_y;

/* The static layer: the terrain and the statics, drawn once a window, as far as the view can
 * reach before the next: the window's first chunk at (0, 0), and 320 by 200 past its far side. */
#define LAYER_W (128 + E_VIEW_W)
#define LAYER_H (128 + E_VIEW_H)
static struct ESurface layer;
static int16_t layer_x0, layer_y0; /* world pixel at its (0, 0) */
static const int16_t layer_clip[4] = { 0, 0, LAYER_W - 1, LAYER_H - 1 };

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

/* Fills o from a cell, a level and a type-frame word; 0 when its shape or frame is missing. */
static int make_obj(struct Obj *o, int x, int y, int z, uint16_t tf)
{
	uint16_t type = tf & 0x3ff, frame = (tf >> 10) & 31;
	const uint8_t *f = frame_of(type, frame);
	int16_t right, left, above, below;
	int fx, fy;

	if (f == NULL)
		return 0;
	right = rds16(f);
	left = rds16(f + 2);
	above = rds16(f + 4);
	below = rds16(f + 6);
	o->flipped = (tf & 0x8000) != 0;
	o->spans = f + 8;
	o->px = (int16_t) (x * 8 + 7 - z * 4);
	o->py = (int16_t) (y * 8 + 7 - z * 4);
	if (o->flipped) {
		int16_t t = left; left = above; above = t;
		t = right; right = below; below = t;
		fx = info[type].fy;
		fy = info[type].fx;
	} else {
		fx = info[type].fx;
		fy = info[type].fy;
	}
	o->x0 = (int16_t) (o->px - left);
	o->x1 = (int16_t) (o->px + right);
	o->y0 = (int16_t) (o->py - above);
	o->y1 = (int16_t) (o->py + below);
	o->cx1 = (int16_t) x;
	o->cx0 = (int16_t) (x - fx);
	o->cy1 = (int16_t) y;
	o->cy0 = (int16_t) (y - fy);
	o->z0 = (uint8_t) z;
	o->z1 = (uint8_t) (z + info[type].height);
	return 1;
}

/* -1: a is drawn before b; 1: after; 0: either. By the ranges of their footprints and levels:
 * what lies south or east, or above, is in front. */
static int compare(const struct Obj *a, const struct Obj *b)
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

static inline int overlap(const struct Obj *a, const struct Obj *b)
{
	return a->x0 <= b->x1 && b->x0 <= a->x1 && a->y0 <= b->y1 && b->y0 <= a->y1;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int bin_x(int x) { return clampi((x - bins_x0) >> 6, 0, BINS_X - 1); }
static inline int bin_y(int y) { return clampi((y - bins_y0) >> 6, 0, BINS_Y - 1); }

static uint16_t norder;

static void visit(uint16_t i)
{
	/* depth first, drawing what lies behind first; a cycle is cut where it closes */
	struct Obj *o = &objs[i];

	if (o->mark)
		return;
	o->mark = 1;
	for (uint16_t k = o->pred; k < objs[i + 1].pred; k++)
		visit(preds[k]);
	o->rank = norder;
	order[norder++] = i;
}

/* Draws the window's terrain into the layer: the 8 by 8 tiles of the chunks under it, by the
 * backend (on the Jaguar, the GPU), from the flats each uses, loaded first. */
static struct EFlat flat_table[E_FLAT_TYPES];

static void draw_terrain(int cx0, int cy0)
{
	struct ETileChunk tc[E_TILE_CHUNKS];
	int n = 0;

	memset(flat_table, 0, sizeof flat_table);
	for (int wy = 0; wy * 128 < LAYER_H; wy++)
		for (int wx = 0; wx * 128 < LAYER_W; wx++) {
			int cx = cx0 + wx, cy = cy0 + wy;
			if (cx < 0 || cy < 0 || cx >= 192 || cy >= 192 || n >= E_TILE_CHUNKS)
				continue;
			struct Chunk *k = chunk(cx, cy);
			for (int t = 0; t < E_FLAT_TYPES; t++)
				if ((k->flats[t >> 5] >> (t & 31) & 1) && flat_table[t].pixels == NULL) {
					const uint8_t *p = shape((uint16_t) t);
					if (p != NULL) {
						flat_table[t].pixels = p;
						flat_table[t].frames = shapes.table[t * 2 + 1] / 64;
					}
				}
			tc[n].cells = k->cells;
			tc[n].to = layer.pixels + wy * 128 * LAYER_W + wx * 128;
			tc[n].across = (uint32_t) ((LAYER_W - wx * 128) / 8 < 16 ? (LAYER_W - wx * 128) / 8 : 16);
			tc[n].down = (uint32_t) ((LAYER_H - wy * 128) / 8 < 16 ? (LAYER_H - wy * 128) / 8 : 16);
			n++;
		}
	eb_tiles(&layer, flat_table, tc, n);
}

static void build_window(int cx0, int cy0)
{
	uint32_t npairs = 0;
	uint16_t nflat = 0;

	e_stats.rebuilds++;
	e_stats.compares = 0;
	eb_finish();
	if ((uint32_t) (pool_end - pool_next) < (uint32_t) (pool_end - pool_pinned) / 4)
		shapes_flush();

	/* the window's objects: standing ones from the front of objs, flat ones from the back */
	nobj = 0;
	for (int wy = 0; wy < WIN_CY; wy++)
		for (int wx = 0; wx < WIN_CX; wx++) {
			int cx = cx0 + wx, cy = cy0 + wy;
			if (cx < 0 || cy < 0 || cx >= 192 || cy >= 192)
				continue;
			struct Chunk *k = chunk(cx, cy);
			struct Region *r = region((cy >> 4) * 12 + (cx >> 4));
			int c = (cy & 15) * 16 + (cx & 15);
			int rx = (cx >> 4) << 8, ry = (cy >> 4) << 8;
			int n = k->nobj + r->start[c + 1] - r->start[c];
			for (int i = 0; i < n; i++) {
				struct Obj *o;
				int x, y, z;
				uint16_t tf;
				if (i < k->nobj) {
					int cell = k->obj[i];
					tf = rd16(k->cells + cell * 2);
					x = cx * 16 + (cell & 15);
					y = cy * 16 + (cell >> 4);
					z = 0;
				} else {
					struct Item *it = &r->items[r->start[c] + i - k->nobj];
					tf = it->tf;
					x = rx + it->x;
					y = ry + it->y;
					z = it->z;
				}
				if ((tf & 0x3ff) < TILE_TYPES || nobj + nflat >= WIN_OBJECTS)
					continue;
				o = info[tf & 0x3ff].height == 0 ? &objs[WIN_OBJECTS - 1 - nflat] : &objs[nobj];
				if (!make_obj(o, x, y, z, tf))
					continue;
				if (info[tf & 0x3ff].height == 0)
					nflat++;
				else
					nobj++;
			}
		}
	nstanding = nobj;
	/* the flat ones after the standing ones, back in the chunks' order */
	for (uint16_t i = 0; i < nflat; i++)
		objs[nobj++] = objs[WIN_OBJECTS - 1 - i];
	e_stats.statics = nobj;

	/* screen bins over the standing objects, then each overlapping pair once */
	bins_x0 = (int16_t) (cx0 * 128 - 2 * BIN);
	bins_y0 = (int16_t) (cy0 * 128 - 2 * BIN);
	memset(bin_start, 0, (BINS_X * BINS_Y + 1) * sizeof *bin_start);
	for (uint16_t i = 0; i < nstanding; i++) {
		struct Obj *o = &objs[i];
		for (int by = bin_y(o->y0); by <= bin_y(o->y1); by++)
			for (int bx = bin_x(o->x0); bx <= bin_x(o->x1); bx++)
				bin_start[by * BINS_X + bx]++;
	}
	uint16_t sum = 0;
	for (int b = 0; b < BINS_X * BINS_Y; b++) {
		uint16_t c = bin_start[b];
		bin_start[b] = sum;
		sum = (uint16_t) (sum + c > BIN_ENTRIES ? BIN_ENTRIES : sum + c);
	}
	bin_start[BINS_X * BINS_Y] = sum;
	for (uint16_t i = 0; i < nstanding; i++) {
		struct Obj *o = &objs[i];
		for (int by = bin_y(o->y0); by <= bin_y(o->y1); by++)
			for (int bx = bin_x(o->x0); bx <= bin_x(o->x1); bx++) {
				int b = by * BINS_X + bx;
				if (bin_start[b] < BIN_ENTRIES)
					bin_list[bin_start[b]++] = i;
			}
	}
	/* the fill moved each start to its end: back one bin */
	for (int b = BINS_X * BINS_Y; b > 0; b--)
		bin_start[b] = bin_start[b - 1];
	bin_start[0] = 0;
	for (int b = 0; b < BINS_X * BINS_Y; b++) {
		int bx = b % BINS_X, by = b / BINS_X;
		for (uint16_t p = bin_start[b]; p < bin_start[b + 1]; p++)
			for (uint16_t q = (uint16_t) (p + 1); q < bin_start[b + 1]; q++) {
				uint16_t i = bin_list[p], j = bin_list[q];
				struct Obj *a = &objs[i], *c = &objs[j];
				if (!overlap(a, c))
					continue;
				/* only in the bin holding the overlap's top-left corner */
				int ox = a->x0 > c->x0 ? a->x0 : c->x0, oy = a->y0 > c->y0 ? a->y0 : c->y0;
				if (bin_x(ox) != bx || bin_y(oy) != by)
					continue;
				int r = compare(a, c);
				if (r != 0 && npairs < WIN_EDGES)
					pairs[npairs++] = r < 0 ? (uint32_t) j << 16 | i : (uint32_t) i << 16 | j;
			}
	}
	e_stats.edges = npairs;
	/* by object: counts, starts, then the lists; objs[nstanding].pred ends the last list (a flat
	 * object or a spare, whose own list is unused) */
	for (uint16_t i = 0; i <= nstanding; i++)
		objs[i].pred = 0;
	for (uint32_t k = 0; k < npairs; k++)
		objs[pairs[k] >> 16].pred++;
	sum = 0;
	for (uint16_t i = 0; i <= nstanding; i++) {
		uint16_t c = objs[i].pred;
		objs[i].pred = sum;
		sum = (uint16_t) (sum + c);
	}
	for (uint32_t k = 0; k < npairs; k++) {
		uint16_t to = (uint16_t) (pairs[k] >> 16);
		preds[objs[to].pred++] = (uint16_t) pairs[k];
	}
	for (uint16_t i = nstanding; i > 0; i--)
		objs[i].pred = objs[i - 1].pred;
	objs[0].pred = 0;
	for (uint16_t i = 0; i < nstanding; i++)
		objs[i].mark = 0;
	norder = 0;
	for (uint16_t i = 0; i < nstanding; i++)
		visit(i);

	/* the layer: terrain, the flat objects, then the standing ones in order */
	layer_x0 = (int16_t) (cx0 * 128);
	layer_y0 = (int16_t) (cy0 * 128);
	draw_terrain(cx0, cy0);
	for (uint16_t k = nstanding; k < nobj + norder; k++) {
		struct Obj *o = &objs[k < nobj ? k : order[k - nobj]];
		if (o->x1 < layer_x0 || o->x0 >= layer_x0 + LAYER_W || o->y1 < layer_y0
			|| o->y0 >= layer_y0 + LAYER_H)
			continue;
		eb_shape(&layer, layer_clip, o->spans, (int16_t) (o->px - layer_x0),
			(int16_t) (o->py - layer_y0), o->flipped);
	}
	win_cx = (int16_t) cx0;
	win_cy = (int16_t) cy0;
}

/* ---- movers ---- */

static struct Mover {
	uint16_t tf;
	int16_t x, y;
	uint8_t z, used;
	struct Obj o;
	int16_t slot;               /* drawn after the standing object of this rank (-1: first) */
} movers[E_MOVERS];
static uint8_t nmovers;

int e_mover_add(uint16_t tf, int16_t x, int16_t y, uint8_t z)
{
	if (nmovers >= E_MOVERS)
		return -1;
	/* its shape stays: loaded among the pinned ones */
	if (pool_next == pool_pinned && shape(tf & 0x3ff) != NULL)
		pool_pinned = pool_next;
	movers[nmovers].used = 1;
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

/* What is drawn over the layer this frame: the movers, and each static in front of one, clipped
 * to the movers it covers; by key, the static of rank r at 2r + 2, a mover after it at 2r + 3. */
#define FRONT 128
static struct Front {
	uint16_t key;
	int16_t mover;              /* or -1: a static */
	uint16_t obj;
	int16_t clip[4];            /* world pixels */
} front[FRONT];
static uint8_t nfront;

static void add_front(uint16_t key, int16_t mover, uint16_t obj, const struct Obj *covered)
{
	struct Front *f;
	uint8_t k;

	if (mover < 0)
		for (k = 0; k < nfront; k++)
			if (front[k].mover < 0 && front[k].obj == obj) {
				/* already in: its clip grows to this mover too */
				f = &front[k];
				if (covered->x0 < f->clip[0]) f->clip[0] = covered->x0;
				if (covered->y0 < f->clip[1]) f->clip[1] = covered->y0;
				if (covered->x1 > f->clip[2]) f->clip[2] = covered->x1;
				if (covered->y1 > f->clip[3]) f->clip[3] = covered->y1;
				return;
			}
	if (nfront >= FRONT)
		return;
	/* in key order; a mover after the ones of its key already in */
	k = nfront++;
	while (k > 0 && front[k - 1].key > key) {
		front[k] = front[k - 1];
		k--;
	}
	f = &front[k];
	f->key = key;
	f->mover = mover;
	f->obj = obj;
	f->clip[0] = covered->x0;
	f->clip[1] = covered->y0;
	f->clip[2] = covered->x1;
	f->clip[3] = covered->y1;
}

static void place_movers(void)
{
	nfront = 0;
	for (uint8_t m = 0; m < nmovers; m++) {
		struct Mover *v = &movers[m];
		if (!make_obj(&v->o, v->x, v->y, v->z, v->tf))
			continue;
		int16_t slot = -1;
		for (int pass = 0; pass < 2; pass++)
			for (int by = bin_y(v->o.y0); by <= bin_y(v->o.y1); by++)
				for (int bx = bin_x(v->o.x0); bx <= bin_x(v->o.x1); bx++) {
					int b = by * BINS_X + bx;
					for (uint16_t p = bin_start[b]; p < bin_start[b + 1]; p++) {
						uint16_t i = bin_list[p];
						struct Obj *s = &objs[i];
						if (!overlap(s, &v->o))
							continue;
						if (pass == 0) {
							/* the latest static that must be behind it */
							if ((int16_t) s->rank > slot && compare(s, &v->o) < 0)
								slot = (int16_t) s->rank;
						} else if ((int16_t) s->rank > slot) {
							/* drawn over it in the layer: drawn again after it */
							add_front((uint16_t) (2 * s->rank + 2), -1, i, &v->o);
						}
					}
				}
		v->slot = slot;
		add_front((uint16_t) (2 * slot + 3), (int16_t) m, 0, &v->o);
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
	if ((chunks = take(CHUNK_SLOTS * sizeof *chunks)) == NULL)
		return -7;
	for (int i = 0; i < CHUNK_SLOTS; i++)
		chunks[i].cx = chunks[i].cy = -1000;
	layer.pitch = layer.w = LAYER_W;
	layer.h = LAYER_H;
	if ((layer.pixels = take(LAYER_W * LAYER_H)) == NULL || (layer.rows = take(LAYER_H * 4)) == NULL)
		return -7;
	e_surface_rows(&layer);
	if ((objs = take((WIN_OBJECTS + 1) * sizeof *objs)) == NULL
		|| (order = take(WIN_OBJECTS * 2)) == NULL || (preds = take(WIN_EDGES * 2)) == NULL
		|| (pairs = take(WIN_EDGES * 4)) == NULL
		|| (bin_start = take((BINS_X * BINS_Y + 1) * 2)) == NULL
		|| (bin_list = take(BIN_ENTRIES * 2)) == NULL)
		return -7;
	pool = pool_next = pool_pinned = mem_next;
	pool_end = mem_end;
	if (pool_end - pool < 256 * 1024)
		return -8;
	return 0;
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

void e_set_view(int16_t x, int16_t y)
{
	view_x = x;
	view_y = y;
}

void e_draw(struct ESurface *frame)
{
	int16_t cx0 = (int16_t) (view_x >> 4), cy0 = (int16_t) (view_y >> 4);
	int16_t vx = (int16_t) (view_x * 8), vy = (int16_t) (view_y * 8);
	uint32_t t0 = eb_clock(), t1;

	if (cx0 != win_cx || cy0 != win_cy)
		build_window(cx0, cy0);
	t1 = eb_clock();
	e_stats.build_time += t1 - t0;
	t0 = t1;

	/* the layer, then over it the movers and what stands in front of them */
	eb_copy(frame, 0, 0, &layer, (int16_t) (vx - layer_x0), (int16_t) (vy - layer_y0), E_VIEW_W,
		E_VIEW_H);
	t1 = eb_clock();
	e_stats.copy_time += t1 - t0;
	t0 = t1;
	place_movers();
	e_stats.drawn = 0;
	for (uint8_t k = 0; k < nfront; k++) {
		struct Front *f = &front[k];
		const struct Obj *o = f->mover >= 0 ? &movers[f->mover].o : &objs[f->obj];
		int16_t clip[4] = {
			(int16_t) (f->clip[0] - vx), (int16_t) (f->clip[1] - vy),
			(int16_t) (f->clip[2] - vx), (int16_t) (f->clip[3] - vy),
		};
		if (clip[0] < 0) clip[0] = 0;
		if (clip[1] < 0) clip[1] = 0;
		if (clip[2] > E_VIEW_W - 1) clip[2] = E_VIEW_W - 1;
		if (clip[3] > E_VIEW_H - 1) clip[3] = E_VIEW_H - 1;
		if (clip[0] > clip[2] || clip[1] > clip[3])
			continue;
		eb_shape(frame, clip, o->spans, (int16_t) (o->px - vx), (int16_t) (o->py - vy), o->flipped);
		e_stats.drawn++;
	}
	t1 = eb_clock();
	e_stats.object_time += t1 - t0;
	eb_finish();
	e_stats.finish_time += eb_clock() - t1;
}
