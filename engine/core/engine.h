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
/* A shape frame from its first span (the original's format), hot spot at (x, y) of dst, clipped
 * to clip (x0, y0, x1, y1, inclusive); flipped: mirrored across the diagonal, as the original. */
void eb_shape(struct ESurface *dst, const int16_t clip[4], const uint8_t *spans, int16_t x,
	int16_t y, uint8_t flipped);
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
	uint32_t compares;          /* pair tests then */
	uint32_t drawn;             /* shapes drawn in the last frame */
	uint32_t shape_bytes;       /* shape cache in use */
	uint32_t shape_loads;       /* shapes read from the file so far */
	uint32_t build_time;        /* eb_clock units spent building the static layer, in all */
	uint32_t copy_time;         /* ... copying it to the frame */
	uint32_t object_time;       /* ... placing movers and handing out their shapes */
	uint32_t finish_time;       /* ... waiting for the drawing to finish */
	uint32_t front_time;        /* of object_time, working out what is in front of movers */
	uint32_t front_misses;      /* movers whose fronts were worked out anew */
};
extern struct EStats e_stats;

/* Opens the data: STATIC/ and GAMEDAT/ under the backend's root (prefix: "" or "dir/").
 * Memory: the engine's working store, all it uses. 0 when it worked. */
int e_init(const char *prefix, uint8_t *memory, uint32_t size);
/* The palette, 256 RGB triples of 0-63. */
const uint8_t *e_palette(void);
/* The view's top-left cell. */
void e_set_view(int16_t x, int16_t y);
/* A frame's first span, for tests; NULL when missing. */
const uint8_t *e_frame_spans(uint16_t type, uint16_t frame);
/* Draws the view into frame (E_VIEW_W by E_VIEW_H). */
void e_draw(struct ESurface *frame);

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
