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
/* The movers drawn by the backend's own means, when it has them (else 0, and the core draws
 * them): each its static (s, NULL when not in view) and those in front of it in drawing order,
 * clipped to its picture. A static (20 bytes): type-frame word (bits 0-9 type, 10-14 frame,
 * 15 turned), x, y (cells), z, a byte, its picture's rectangle (4 words), its footprint west,
 * north and up (bytes), a byte. shapes: the core's table of shapes by type (each the size, the
 * frames' offsets, the frames as eb_shape's runs follow their box; NULL when not read in: all a
 * mover and its fronts use are in). The movers go behind ones first: order_of() in the core. */
struct EMoverDraw {
	const void *s;
	uint32_t nfront;
	const void *const *fronts;
	uint32_t pad;
};
int eb_movers(struct ESurface *frame, const struct EMoverDraw *list, int n, int16_t vx,
	int16_t vy, const void *const *shapes);
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
 * solid: words but z, flat, the footprint and solid), then its bin entries. */
#define E_CHUNKS_TITLE "U7 engine chunks 4, big-endian"
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
/* A sound started: n samples (16-bit, big-endian, signed, mono, at E_SOUND_RATE) at samples,
 * which stay put while it plays; volume and pan 0-127 (0 left). On a free channel, else the one
 * nearest its end. */
#define E_SOUND_RATE 15980
void eb_sound(const uint8_t *samples, uint32_t n, int volume, int pan);
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
	uint32_t doors_turned;      /* doors opened or shut */
	uint32_t routes_planned;    /* routes found */
	uint32_t routes_failed;     /* routes looked for and not found */
	uint32_t route_steps;       /* steps tried planning them */
	uint32_t route_time;        /* eb_clock units planning routes (found or not) */
	uint32_t route_worst;       /* the most one took */
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

/* The dataset's sounds (engine/tools/mksfx.sh): ENGINE/SFX.FLX, a flex file as the original's,
 * each entry the original's AdLib effect of that number (STATIC/ADLIBSFX.DAT) played by its
 * effects driver and rendered, as eb_sound takes it. Its title starts E_SOUNDS_TITLE. */
#define E_SOUNDS_TITLE "U7 engine sounds 1, 16-bit big-endian at 15980 Hz"
/* Sound number played at world cell x, y, heard from the mover followed (e_follow), as the
 * original plays one at an item. Nothing without the dataset's sounds. */
void e_sound(int number, int16_t x, int16_t y);

/* The game's text (STATIC/TEXT.FLX, Black Gate's sections): entry n of section s (1: the
 * barks), into a buffer of the engine's kept till the next call ("" when there is none). */
const char *e_text(int section, int n);
/* A line said over mover i's head (as the original's barks: yellow, above its picture) for
 * blanks; its last one gives way. */
void e_bark(int mover, const char *text, int blanks);
/* The barks' time going on (e_world_step does it). */
void e_barks_age(int blanks);

/* Things that move (NPCs): up to E_MOVERS, re-placed every frame. */
#define E_MOVERS 32
int e_mover_add(uint16_t typeFrame, int16_t x, int16_t y, uint8_t z);
void e_mover_set(int i, uint16_t typeFrame, int16_t x, int16_t y, uint8_t z);
/* Up to most bytes of dir/name under the data's root into to: how many, or -1. */
int32_t e_read_file(const char *dir, const char *name, uint8_t *to, uint32_t most);
/* The NPCs on the map and alive in NPC.DAT: ENGINE/NPC.DAT (a new game's, from INITGAME.DAT: the
 * dataset tool) or else GAMEDAT/NPC.DAT. Up to max into out; how many. */
struct ENpc {
	uint16_t tf;                /* type-frame word */
	int16_t x, y;               /* world cell */
	uint8_t z;
	uint8_t activity;           /* what it is doing (a schedule's type: 11 loiter, 14 sleep, ..) */
	uint8_t intelligence;
	uint16_t status;            /* the original's status bits (dead, asleep, in the party, ...) */
	uint8_t strength, dexterity, combat, hit_points, magic, mana, face;
	char name[16];
	uint16_t number;            /* in NPC.DAT; the avatar is 0 */
};
int e_npcs(struct ENpc *out, int max);

/* What NPCs carry (core/held.c), read by e_npcs from NPC.DAT's contents: each item in an NPC
 * (E_HELD_ON(its number)), in another held item (its index), or loose (made or taken up, not
 * yet put anywhere). The original's rules: a counted item's count in its quality's low 7 bits;
 * what a locked chest or sealed box holds not counted; an NPC carries twice its strength in
 * stones, a container what its bulk leaves room for. */
#define E_HELD_MOST 1200
#define E_HELD_LOOSE (-1)
#define E_HELD_ON(n) (-2 - (n))
void e_held_clear(void);
int e_held_add(uint16_t tf, uint8_t quality, int in);   /* its index, or -1: no room */
int e_held_ok(int i);
int e_held_in(int i);
uint16_t e_held_tf(int i);
void e_held_set_tf(int i, uint16_t tf);
int e_held_quality(int i);
int e_held_set_quality(int i, int q);   /* 0: it has none */
int e_held_quantity(int i);             /* (1 for those not counted) */
int e_held_set_quantity(int i, int n);
int e_held_owner(int i);                /* the NPC (number) it is on, deep, or -1 */
/* The next held item after "after" (deep) in "in" of type, quality, frame (< 0: any) and
 * counted (none locked between), or -1; and the sum of their counts. */
int e_held_find(int in, int type, int quality, int frame, int after);
int e_held_count(int in, int type, int quality, int frame);
/* i put in "in" (E_HELD_ON: its first container with room, else on it), the NPC it comes to
 * of the strength given: 0, or the original's errors: 1 too heavy, 2 into itself, 3 no place
 * (locked), 4 no room. */
int e_held_place(int i, int in, int strength);
void e_held_remove(int i);              /* and what it holds */
int e_held_used(void);
int e_shape_class(int type);            /* a type's class (TFA.DAT) */

/* ---- usecode (core/usecode.c): the original's (STATIC/USECODE), run as its interpreter runs
 * it, stopping where the player is waited for ---- */

#define E_UC_DONE 0             /* the run has ended */
#define E_UC_SAY 1              /* text to show (e_uc_text); then e_uc_resume(NULL) */
#define E_UC_ASK 2              /* an answer to pick (e_uc_answers); then e_uc_resume(it) */
#define E_UC_NUMBER 3           /* a number to choose (e_uc_number); then e_uc_resume_number */
#define E_UC_CODE (128 * 1024)  /* room for the functions a run calls (the engine's store) */
#define E_UC_HEAP (64 * 1024)   /* ... the values it makes */
/* Usecode function (an NPC's conversation is 0x400 + its number) for item (an NPC: -its
 * number, the avatar -356) on event (1: used, as spoken to): E_UC_. */
int e_uc_start(int function, int item, int event);
int e_uc_resume(const char *answer);
/* The number asked for: its range, step and first value (1), or 0 when none is. */
int e_uc_number(int *low, int *high, int *step, int *initial);
int e_uc_resume_number(int value);
/* What is said (the original's text: '~' a new line, '*' wait for a key, "@" quotes). */
const char *e_uc_text(void);
/* The answers offered, up to most into list: how many. */
int e_uc_answers(const char **list, int most);
/* Whose face is shown (an NPC's number, 0 the avatar; -1 none), and its frame. */
int e_uc_speaker(int *frame);
/* A game flag. */
int e_uc_flag(int n);
/* Faces shown: the speaker in each of the two places (an NPC's number, 0 the avatar; -1 none)
 * and its frame; which speaks (0 or 1, or -1). */
int e_uc_face(int place, int *frame);
int e_uc_speaking(void);
/* The conversation's pictures: face n (FACES.VGA) frame fr with its hot spot at x, y (its
 * width and height into w, h unless NULL; frame NULL: only measured); the pointer's frame fr;
 * text in the original's yellow font (font 0) with its letters' feet at y, n letters at most;
 * a letter's and a text's width (as the original's printer measures them), the lines' height. */
void e_draw_face(struct ESurface *frame, int face, int fr, int16_t x, int16_t y, int16_t *w, int16_t *h);
void e_draw_pointer(struct ESurface *frame, int fr, int16_t x, int16_t y);
void e_draw_text(struct ESurface *frame, int16_t x, int16_t y, const char *s, int n);
int e_letter_width(uint8_t c);
int e_text_width(const char *s);
int e_line_height(void);
/* The engine's own: a static file opened, or -1; the usecode's rooms given it. */
int16_t e_open_static(const char *name);
void e_uc_memory(void *code_room, void *heap_room);

/* ---- a conversation shown (core/talk.c), as the original's: faces, what is said a page at a
 * time, the answers with the pointer on the one picked ---- */

#define E_KEY_NEXT 1            /* on: the next page, or the answer picked */
#define E_KEY_LEFT 2            /* the answer before */
#define E_KEY_RIGHT 3           /* ... after */
#define E_KEY_UP 4              /* ... on the line above */
#define E_KEY_DOWN 5            /* ... below */
#define E_KEY_CLEAR 6           /* a number typed cleared (the Jaguar's * key) */
#define E_KEY_DIGIT 10          /* E_KEY_DIGIT + d: digit d typed (the Jaguar's keypad) */
/* NPC number's conversation begun (its usecode as spoken to): 1 when there is one. */
int e_talk_start(int number);
int e_talk_active(void);
void e_talk_key(int key);
/* An answer picked by its words (for tests): 1 when answers were asked for. */
int e_talk_choose(const char *answer);
/* Line n of what face place p shows now (for tests), or NULL. */
const char *e_talk_line(int place, int line);
/* Gump n (GUMPS.VGA's entry) drawn with its hot spot at x, y (frame NULL: only its size);
 * text in font 2 (the slider's numbers) from x on its line at y. */
void e_draw_gump(struct ESurface *frame, int n, int fr, int16_t x, int16_t y, int16_t *w, int16_t *h);
void e_draw_small(struct ESurface *frame, int16_t x, int16_t y, const char *s);
/* Counts the changes to what the conversation shows (while it is the same, so is the
 * picture: the world waits). */
uint32_t e_talk_changes(void);
/* The conversation drawn over the view (after e_draw). */
void e_talk_draw(struct ESurface *frame);

/* ---- the world (core/world.c): a clock, and the NPCs on their schedules (SCHEDULE.DAT) ---- */

/* The NPCs to keep (npcs stays the caller's, changed as they move), the time to start at (minutes
 * from midnight) and the clock then (blanks). Each goes on with what it is doing where it is,
 * until its schedule's next period (3 hours). */
void e_world_init(struct ENpc *npcs, int n, int start, uint32_t blanks);
/* The world at clock (eb_clock's units: blanks), the avatar at x, y: the time; the NPCs whose
 * period has come walk to their place if near the avatar, else are put there; those near walk
 * and wander; movers 1 to E_MOVERS - 1 are the NPCs nearest the avatar. */
void e_world_step(uint32_t blanks, int16_t x, int16_t y);
/* The NPC (by number) whose schedule's kinds and states are logged as they change (eb_log), or -1:
 * for tests. */
extern int e_world_trace;
/* The NPC (its number) ahead of one at x, y facing dx, dy (1 or 2 cells), else the nearest
 * within 2 cells; -1 for none. */
int e_world_npc_near(int16_t x, int16_t y, int dx, int dy);
/* The time of day, in minutes from midnight. */
int e_world_minutes(void);
/* Blanks a game minute (the original's is slower: a game hour a real minute here, for now). */
#define E_WORLD_MINUTE 60
/* The ceiling over mover i (the avatar), as the original works it out: statics and movers above
 * it are not drawn (roofs over it lifted). The view is drawn again when it changes. Call when it
 * has moved; returns the ceiling (15: none). */
int e_follow(int i);
/* Whether one at x0, y0, z0 can see one at x1, y1, z1: nothing solid at chest height (2 levels
 * up) on the cells between (a stand-in for the original's test missile). */
int e_clear_line(int16_t x0, int16_t y0, uint8_t z0, int16_t x1, int16_t y1, uint8_t z1);
/* Whether a shape (type-frame word) standing at x, y, z would be in a solid static or a door. */
int e_blocked(uint16_t tf, int16_t x, int16_t y, uint8_t z);
/* A walker's step of dx, dy (a cell) from x, y, z, as the original's: at its level, or failing
 * that one up or one down, where it is clear and stood on (the ground, or a solid static's top):
 * so up and down stairs. E_STEP_MADE with x, y, z moved; E_STEP_DOOR when a door is in the way
 * (e_door_at it); else E_STEP_BLOCKED. */
#define E_STEP_BLOCKED 0
#define E_STEP_MADE 1
#define E_STEP_DOOR 2
int e_step(uint16_t tf, int16_t *x, int16_t *y, uint8_t *z, int dx, int dy);
/* A walker's body, for its steps: its footprint (cells west and north of where it stands) and
 * the levels it takes standing at 0. */
struct EBody {
	int8_t fx, fy;
	uint16_t levels;
};
void e_body(uint16_t tf, struct EBody *b);
/* e_step for a body; when through, into a door's cells as if it were open (E_STEP_DOOR, moved):
 * routes are planned so, as the original's. */
int e_step_body(const struct EBody *b, int16_t *x, int16_t *y, uint8_t *z, int dx, int dy, int through);
/* Whether a shape could stand at x, y, z: clear of solid statics, doors and solid ground, and
 * stood on something. */
int e_can_stand(uint16_t tf, int16_t x, int16_t y, uint8_t z);
/* The solid movers stand in the walkers' way, as the original's NPCs in its collision grid; the
 * walker stepping now (mover i, or -1 for none) is left out of its own way, as the original
 * takes it out of the grid for its steps. */
void e_mover_self(int i);
/* The cells (inclusive) of the chunks under the view and one round them: as the original's
 * collision grid covers only the world near the avatar, routes are planned within these. */
void e_near_window(int16_t *x0, int16_t *y0, int16_t *x1, int16_t *y1);
/* The avatar's step towards dx, dy (each -1 to 1), as the original's party step: that way, or
 * failing that an eighth to one side or the other. A shut door in the way opened when open_doors
 * (as if it were used): E_STEP_DOOR, not moved. */
int e_walk(uint16_t tf, int16_t *x, int16_t *y, uint8_t *z, int dx, int dy, int open_doors);
/* The door just ahead of a shape at x, y, z facing dx, dy (or beside that, or one further), to
 * use; or -1. */
int e_door_ahead(uint16_t tf, int16_t x, int16_t y, uint8_t z, int dx, int dy);

/* Routes (route.c), as the original plans them: a straight walk, along obstacles where that is
 * blocked (two walkers, one each way round), then straightened. Up to E_ROUTE_WAYS waypoints, the
 * last the goal; planned within e_near_window. */
#define E_ROUTE_WAYS 100
struct EWay {
	int16_t x, y;
	uint8_t z;
};
/* Ways a walker takes a door: walks through it as if open (planning), turns from it, or stops
 * at it (walking: the step is "taken" without moving, *door set, to open it). */
#define E_ROUTE_THROUGH 0
#define E_ROUTE_AVOID 1
#define E_ROUTE_STOP 2
/* A route for shape tf from x, y, z to tx, ty, tz, in at most limit steps searched: the count of
 * waypoints put in way (E_ROUTE_WAYS of room), or 0 when there is none. */
int e_route_plan(uint16_t tf, int16_t x, int16_t y, uint8_t z, int16_t tx, int16_t ty, uint8_t tz,
	int limit, struct EWay *way);
/* A walker's step at tx, ty, tz as the original's: straight, else along the longer axis, else
 * the shorter. The way (0-7, north clockwise) taken, x, y, z moved; or -1. */
int e_route_step(uint16_t tf, int16_t *x, int16_t *y, uint8_t *z, int16_t tx, int16_t ty, uint8_t tz,
	int mode, int *door);
/* The way (0-7, north clockwise) of a step dx, dy; -1 for none. */
int e_way_of(int dx, int dy);

/* Doors. The door a shape standing at x, y, z would be in, or -1: a handle, kept while the game
 * runs, as is what has been done to the door. */
int e_door_at(uint16_t tf, int16_t x, int16_t y, uint8_t z);
/* Opens a shut door or shuts an open one, as the original (its type swapped for its other, turned
 * a quarter, some moved 3 cells; the frame's state flipped): 1. 0 when it is locked. */
int e_door_toggle(int door);
/* Whether the door is shut: in a wall, something solid past each end of it (as Exult tells). */
int e_door_shut(int door);
/* Whether a body at x, y, z would be in a door that is open (its leaf in the way: planned round,
 * not through). */
int e_open_door_at(const struct EBody *b, int16_t x, int16_t y, uint8_t z);
/* Where the door is now (its hot spot). */
void e_door_place(int door, int16_t *x, int16_t *y);

/* Movable items (as doors) the game looks for and changes: a bed, a chair, food. */
struct EThing {
	uint16_t tf;                /* type-frame word */
	int16_t x, y;               /* its hot spot's cell */
	uint8_t z;
};
/* Up to most items and objects of type within cells x0, y0 to x1, y1 (inclusive; only those in
 * e_near_window) into out: how many. */
int e_things(uint16_t type, int16_t x0, int16_t y0, int16_t x1, int16_t y1, struct EThing *out,
	int most);
/* A movable item made tf where it is (t too), kept so when its region is read again: 1; 0 when
 * it is not there (or not a movable item). */
int e_thing_set(struct EThing *t, uint16_t tf);
/* A movable item tf put into the world at x, y, z (kept so when its region is read again):
 * 1, with it in out (unless NULL); 0 when it cannot be. */
int e_thing_add(uint16_t tf, int16_t x, int16_t y, uint8_t z, struct EThing *out);
/* How many frames an object shape has (0: none, or a flat). */
int e_shape_frames(uint16_t type);
/* A movable item's number for the usecode (1 on, kept while the game runs), or 0; and the item
 * a number is now: 1, or 0 when it is none (or gone). */
int e_thing_ref(const struct EThing *t);
int e_ref_thing(int ref, struct EThing *t);
/* A movable item's quality (a path marker's what to do), or -1 when it is not one there. */
int e_thing_quality(const struct EThing *t);
/* A movable item taken out of the world, as eaten: 1; 0 when it is not there. */
int e_thing_remove(const struct EThing *t);

/* An NPC shape's type-frame word in a pose (frames 0-15 as the original's: 0 standing, 1 and 2
 * walking, 10 sitting, 11 bending, 13 lying) facing 0-3 (north, east, south, west): frames 0-15
 * face north, 16-31 south; east and west are those turned (0x8000). */
#define E_POSE_STAND 0
#define E_POSE_SIT 10
#define E_POSE_BEND 11
#define E_POSE_LIE 13
static inline uint16_t e_pose_frame(uint16_t type, int dir, int pose)
{
	uint16_t frame = (uint16_t) (pose + (dir == 2 || dir == 1 ? 16 : 0));

	return (uint16_t) (type | frame << 10 | (dir & 1 ? 0x8000 : 0));
}
/* ... walking, step n. */
static inline uint16_t e_walk_frame(uint16_t type, int dir, int n)
{
	static const uint8_t steps[4] = { 0, 1, 0, 2 };

	return e_pose_frame(type, dir, steps[n & 3]);
}

#endif
