/* A Jaguar-first engine test for Ultima VII: the world view drawn from the original's data files,
 * built so the 68000 does little per frame. See engine/README.md.
 *
 * The core is portable C. A backend (engine/host, engine/jaguar) gives it files (plat.h's
 * plat_file_* names), a clock, and the drawing: rectangle copies and shape frames, into surfaces.
 */
#ifndef ENGINE_H
#define ENGINE_H

#include <stdint.h>

#define E_VIEW_W 320
#define E_VIEW_H 200
#define E_VIEW_CELLS_X 40
#define E_VIEW_CELLS_Y 25
#define E_WORLD_CELLS 3072

/* An 8-bit picture: pitch bytes a row (a multiple of 8, pixels phrase aligned), and per row the
 * offset of its first pixel as a little-endian long (the GPU's row table). */
struct ESurface {
	uint8_t *pixels;
	uint16_t pitch, w, h;
	uint8_t *rows;
};
/* Fills in rows (h * 4 bytes, the caller's) for pixels, pitch, w and h. */
void e_surface_rows(struct ESurface *s);

/* ---- from the backend ---- */

#define PLAT_FILE_READ 1
#define PLAT_SEEK_SET 0
int16_t plat_file_open(const char *name, int16_t mode);
void plat_file_close(int16_t file);
int32_t plat_file_read(int16_t file, void *buffer, int32_t count);
int32_t plat_file_seek(int16_t file, int32_t offset, int16_t origin);
int32_t plat_file_length(int16_t file);

/* w by h pixels from (sx, sy) of src to (dx, dy) of dst; x and w multiples of 8. */
void eb_copy(struct ESurface *dst, int16_t dx, int16_t dy, const struct ESurface *src, int16_t sx,
	int16_t sy, int16_t w, int16_t h);
/* The view's background: the ring's window from (sx, sy), E_VIEW_W by E_VIEW_H, wrapping at the
 * ring's edges; the movers are then drawn into frame. 1 when the backend copied it into frame
 * (e_copy_view does); 0 when it shows the ring itself under frame, frame being a layer whose 0 is
 * clear: then the core clears frame (eb_clear) where it drew in it before. */
int eb_view(struct ESurface *frame, const struct ESurface *ring, int16_t sx, int16_t sy);
/* w by h pixels at (x, y) of dst set to 0; x and w multiples of 8. */
void eb_clear(struct ESurface *dst, int16_t x, int16_t y, int16_t w, int16_t h);
/* A shape frame from its first run (E_RUN_ below), hot spot at (x, y) of dst, clipped to clip
 * (x0, y0, x1, y1, inclusive); flipped: mirrored across the diagonal, as the original. */
void eb_shape(struct ESurface *dst, const int16_t clip[4], const uint32_t *runs, int16_t x,
	int16_t y, uint8_t flipped);

/* The engine's frames, made from the original's spans as a shape is read (core/engine.c), for a
 * drawer that loads longs: the frame's box (right, left, above, below from the hot spot, int16),
 * then its runs, each a long in the CPU's byte order:
 *   bits 24-31 the row from the hot spot (signed)  16-23 the first column (signed)
 *   8-15 the length (1-255)  0 a fill
 * then a fill's pixel (the byte four times), or the raw pixels padded to a long. 0 ends the frame.
 * Flipped, a run goes down a column: its row is the column from the hot spot, and its first
 * column the first row. Fills are only kept from E_FILL_SHORTEST pixels; shorter ones are made
 * raw, so a span is mostly one run. */
#define E_RUN_ACROSS(h) ((int8_t) ((h) >> 24))
#define E_RUN_ALONG(h) ((int8_t) ((h) >> 16))
#define E_RUN_LENGTH(h) (((h) >> 8) & 255)
#define E_RUN_FILL 1
/* Colour 0 (black) is kept as E_BLACK, which no shape uses, and the palette has E_BLACK as black
 * too: so a layer drawn over another can take 0 as clear (the Jaguar's object processor's
 * transparent colour). */
#define E_BLACK 255
#define E_FILL_SHORTEST 8
/* An object shape of the original's (size bytes at raw, its first long its size) made into the
 * engine's form at out, or only measured when out is NULL: the size made, 0 when it cannot be,
 * or (writing) when it might need more than room bytes. The form: the size, the frames' offsets
 * from the start (as many as the original's: the first offset - 4, over 4), then the frames.
 * e_convert_shape is the core's own, in C. */
#define E_CONVERT_SLACK 1024    /* room is checked at each span: a span makes less than this */
uint32_t eb_convert(const uint8_t *raw, uint32_t size, uint8_t *out, uint32_t room);
uint32_t e_convert_shape(const uint8_t *raw, uint32_t size, uint8_t *out, uint32_t room);
/* A converted shape's longs and words (not its pixels) turned to the other byte order, in place:
 * foreign, from the other order to the CPU's; else from the CPU's. 0 when it is not well formed. */
int e_swap_shape(uint8_t *s, uint32_t size, int foreign);
int e_little_endian(void);
/* The dataset (engine/tools/jagdata.c): ENGINE/SHAPES.FLX under the data's root, a flex file as
 * the original's (little-endian table) of SHAPES.VGA's entries, the flats as they are and the
 * object shapes converted, big-endian (the Jaguar's order). Its title starts E_SHAPES_TITLE. */
#define E_SHAPES_TITLE "U7 engine shapes 2, big-endian"
/* And ENGINE/CHUNKS.DAT: each world chunk's fixed statics (its own objects and its U7IFIX items),
 * as the engine makes them, binned. Its title starts E_CHUNKS_TITLE (in the first 64 bytes);
 * then at E_CHUNKS_TABLE, 192 * 192 + 1 big-endian longs, chunk (cy * 192 + cx)'s record's
 * offset (the next one's is where it ends). A record: its statics and bin entries (big-endian
 * words), the flat types its cells use (150 bits in 5 longs), its 65 bin starts, then 20 bytes a
 * static (type-frame, x, y, z, flat, its picture's rectangle, its footprint west, north and up,
 * a byte spare: words but z, flat and the footprint), then its bin entries. */
#define E_CHUNKS_TITLE "U7 engine chunks 3, big-endian"
#define E_CHUNKS_TABLE 64
#define E_CHUNK_HEAD (4 + 5 * 4 + 65 * 2)
#define E_CHUNK_STATIC 20
#define E_CHUNK_MOST (E_CHUNK_HEAD + 224 * E_CHUNK_STATIC + 1024)
/* A world chunk's record, made from the original's files (the dataset tool); its size. */
uint32_t e_chunk_record(int cx, int cy, uint8_t *out);
/* Terrain: n chunks of 16 by 16 cells into dst. Each chunk: its cell words (little-endian: type in
 * bits 0-9, frame in 10-14), dst's pixel under its first cell, and the cells across and down that
 * fall in dst. A cell is the 64 bytes at flats[type].pixels + 64 * frame, for a type below
 * E_FLAT_TYPES with pixels set and frame below its frames; else black. All phrase aligned. */
#define E_FLAT_TYPES 150
struct EFlat {
	const uint8_t *pixels;
	uint32_t frames;
};
struct ETileChunk {
	const uint8_t *cells;
	uint8_t *to;
	uint32_t across, down;
};
#define E_TILE_CHUNKS 12
void eb_tiles(struct ESurface *dst, const struct EFlat flats[E_FLAT_TYPES],
	const struct ETileChunk *chunks, int n);
/* All drawing asked for so far is done when this returns (before the CPU touches a surface). */
void eb_finish(void);
void eb_log(const char *text);
/* A clock for the stats: any unit, counting up (the Jaguar's: vertical blanks). */
uint32_t eb_clock(void);

/* ---- the engine ---- */

struct EStats {
	uint32_t rebuilds;          /* times the static layer was built */
	uint32_t statics;           /* objects in the window at the last build */
	uint32_t edges;             /* "drawn before" pairs found then */
	uint32_t drawn;             /* shapes drawn in the last frame */
	uint32_t shape_bytes;       /* shape cache in use */
	uint32_t shape_loads;       /* shapes read from the file so far */
	uint32_t build_time;        /* eb_clock units spent building the static layer, in all */
	uint32_t copy_time;         /* ... copying it to the frame */
	uint32_t object_time;       /* ... placing movers and handing out their shapes */
	uint32_t finish_time;       /* ... waiting for the drawing to finish */
	uint32_t front_time;        /* of object_time, working out what is in front of movers */
	uint32_t front_misses;      /* movers whose fronts were worked out anew */
	uint32_t load_time;         /* of build_time and object_time, reading shapes in */
	uint32_t chunk_time;        /* of build_time, reading chunks and their objects (regions too) */
	uint32_t order_time;        /* of build_time, putting an area's objects in order */
	uint32_t pair_tests;        /* pictures tested for overlap then */
	uint32_t region_time;       /* of chunk_time, reading regions' items */
	uint32_t cells_time;        /* of chunk_time, reading chunks' cells */
	uint32_t near_finds;        /* statics near a mover looked for anew */
	uint32_t front_cands;       /* statics over movers tried for being in front */
	uint32_t front_compares;    /* compares then */
};
extern struct EStats e_stats;

/* Opens the data: STATIC/ and GAMEDAT/ under the backend's root (prefix: "" or "dir/").
 * Memory: the engine's working store, all it uses. 0 when it worked. */
int e_init(const char *prefix, uint8_t *memory, uint32_t size);
/* The palette, 256 RGB triples of 0-63. */
const uint8_t *e_palette(void);
/* The view's top-left cell. */
void e_set_view(int16_t x, int16_t y);
/* A frame's first run, for tests; NULL when missing. */
const uint32_t *e_frame_runs(uint16_t type, uint16_t frame);
/* Draws the view into frame (E_VIEW_W by E_VIEW_H), or the movers into it (eb_view). */
void e_draw(struct ESurface *frame);
/* The ring's window copied into frame, in up to four pieces where it wraps (for eb_view). */
void e_copy_view(struct ESurface *frame, const struct ESurface *ring, int16_t sx, int16_t sy);

/* Things that move (NPCs): up to E_MOVERS, re-placed every frame. */
#define E_MOVERS 32
int e_mover_add(uint16_t typeFrame, int16_t x, int16_t y, uint8_t z);
void e_mover_set(int i, uint16_t typeFrame, int16_t x, int16_t y, uint8_t z);
/* An NPC shape's type-frame word walking in direction 0-3 (north, east, south, west), step n:
 * frames 0-15 face north, 16-31 south; east and west are those turned (0x8000). */
static inline uint16_t e_walk_frame(uint16_t type, int dir, int n)
{
	static const uint8_t steps[4] = { 0, 1, 0, 2 };
	uint16_t frame = (uint16_t) (steps[n & 3] + (dir == 2 || dir == 1 ? 16 : 0));

	return (uint16_t) (type | frame << 10 | (dir & 1 ? 0x8000 : 0));
}

#endif
