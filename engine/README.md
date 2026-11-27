# The engine test

A test of a Jaguar-first engine for Ultima VII. It draws the world view from the original's own
data files (`STATIC/` and a save's `GAMEDAT/`), and it is built so the 68000 does almost nothing
per frame. This is not the game: there is no usecode, no schedules, no interface. It measures
whether the world can be shown fast enough on a Jaguar to be worth building a game around.

## How it draws

- **Chunks.** Each chunk in use keeps its cell words and its static objects once read. The
  objects are the chunk's own object shapes, the fixed items (`U7IFIX`) and the movable items
  (`U7IREG`). Each object keeps its picture's rectangle, binned 8 by 8 by where it lies. With the
  dataset, a chunk's own objects and fixed items come ready made, binned, with the flat types its
  cells use; only the movable items are worked out as the chunk is read.
- **The ring.** The static layer is a ring of 4 by 3 chunk areas (512 by 384 pixels). An area holds
  its chunk's terrain and every static over it, drawn once. A shape reaches at most 123 pixels left
  or up from its hot spot and 8 right or down, so only the chunk's own objects and those of the 8
  around it can draw over it. They are put in order among themselves alone: a depth-first walk
  over the "drawn before" pairs of overlapping pictures. The terrain is drawn by the GPU
  (`jaguar/tiles_gpu.s`, a phrase a row) and the shapes too (`jaguar/shapes_gpu.s`).
- **Ahead of the view.** The areas under the view are drawn when it reaches them (at the start,
  or on a jump). One area ahead of it, the way it last moved, is drawn each frame while the ring
  has room beside the view: 7 or 8 cells of travel per column or row.
- **Each frame.** One to four blitter copies (where the ring wraps) put the ring under the view
  into the frame. The movers (NPCs) follow, behind ones first, each followed by the statics in
  front of it, and those in front of them, clipped to it. A mover's front list is kept while it
  stands still: it is worked out over the box of all its shape's frames, so a new frame of the
  walk does not undo it. The statics near it (24 pixels round that box) are kept too, while it
  walks within them and their chunks keep their slots, so a step only compares. On the Jaguar
  the GPU does the rest each frame (`jaguar/movers_gpu.s`): each mover's picture from its frame,
  those in view put in order, and the shape list for them and their fronts; the 68000 only keeps
  the fronts and the shapes they use in the cache.
- **The NPCs** (`core/world.c`) keep the original's schedules (`SCHEDULE.DAT`): for each three
  hours of the day, what each takes up and where. As the original, an NPC goes on with what it
  is doing (its work type in `NPC.DAT`) until its next entry comes round. Then those near the
  avatar walk to the place (a cell every 10 blanks) on a route, and the rest are put there. At
  the place the roaming kinds (loiter, wander, pace, patrol, graze, ...) walk to places within 4
  cells in turn; the others stand. A game hour is a real minute for now
  (`E_WORLD_MINUTE`); `-DSTART_MINUTES=` sets the time to start at (535: five minutes before the
  9:00 change, to see them go).
- **Roofs.** As the original works it out (`UpdateCeiling`): of the levels the solid statics (not
  doors) take over the avatar's row of cells, those above it; the lowest of them less one is the
  ceiling (so an upper floor goes with the roof), and statics and movers above it are not drawn
  (`e_follow`). When it changes, the
  areas holding statics between the two ceilings are drawn again, one a frame, the one nearest
  the middle of the view first (those out of view are given up): the roof comes away over the
  avatar first and the rest follows in a few frames. Drawn all at once, it was a pause of 13-19
  blanks going in or out of a building; now the worst frame is 4-6.
- **Walking** (`e_step`, `e_walk`), as the original's `CheckMove`: a step is made at the walker's
  level, or failing that one up or one down, where nothing solid is in the way and something is
  under it (the ground, or a solid static's top): so up and down stairs, a level a step. Doors
  are kept apart from the solid statics, as the original's collision grid has them: they stop the
  avatar, which otherwise veers an eighth either way round what is in its way. Solid ground
  (water, rock) stops a walker at level 0. What the statics take is kept as the original keeps
  it, a grid of levels per cell (here one per chunk held, made when first asked for, made again
  where a door turns). The solid movers (the avatar, NPCs) are in it too, as the original adds
  NPCs to its grid: stamped again when one has moved, each walker left out of its own steps
  (`e_mover_self`, the original's taking it out of the grid for them). So the avatar steps round
  NPCs, and they round it and each other (a route blocked by one is planned again round it).
- **Routes** (`core/route.c`), as the original plans them (`Route::plan`): a straight walk at the
  goal (straight, else along the longer axis, else the shorter); where that is blocked, two
  walkers along the obstacle, one each way round, until one is nearer the goal than it has been
  and can step straight at it; waypoints where they turn and where the level changes; planned
  from the top when the goal is higher; then straightened, a waypoint either side of each door
  walked through. Doors count as open while planning. The original's limits: 150 steps searched
  to a schedule's place (at level 0), 45 roaming. Walking one is the original's too: a step at
  the next waypoint, a shut door in the way opened, an open leaf walked round, planned again
  after more than 3 steps not made, given up after half as many steps again as it is long (then
  it walks straight at the goal). Routes are planned within the chunks held round the view, as
  the original's collision grid covers only the world near the avatar: a place beyond them is
  walked towards (to the nearest cell within them it could stand on, then on from there), or,
  the NPC off the screen, it is put there. One route is planned a world step; the rest wait.
- **Doors** (`e_door_at`, `e_door_toggle`) are movable items. Opening or shutting one does what
  the doors' usecode and the original's `toggleDoor` do: its type swapped for its other (270 and
  376, 432 and 433, the abbey's 225 and 392, 246 and 250), the frame's state flipped, the 432/433
  and 246/250 kinds moved 3 cells; states 2 and 3 are locked. A door changed is kept in a table
  and put back when its region is read again; its chunks are made again and the ring's areas
  under it drawn again. A double door's other leaf turns with it, found as the usecode finds it:
  the partner type (270 and 432, 376 and 433, 225 and 246, 392 and 250) within 7 cells, as open
  or shut, on the same row or column. A door is shut when something lies past each end of it (a
  wall, or the other leaf), as Exult tells. NPCs open a shut door in their way and shut it once 2
  cells further from it than they came, as the original's routes do. (The sounds are not done.)
- **Depth.** An object stands at the bottom right of its footprint and is lifted 4 pixels up and
  left a level. What lies south, east or above is in front: `order_of()` in `core/engine.c`.
- **Shapes** are read on demand into a ring that gives the oldest way; the movers' are kept apart.
  They are in the engine's own form (`engine.h`, `E_RUN_`): per run one long for where it is, how
  long, and whether it is a fill, then its pixels padded to a long; colour 0 kept as 255, black
  in the palette too, so a layer over the ring can take 0 as clear. Short fills become raw pixels,
  so a span is mostly one run. The form is made once on the PC (`u7jagdata`, below) and read as
  it is. Without the dataset the original's shapes are converted as they are read: on the Jaguar
  by the GPU (`jaguar/convert_gpu.s`), on the PC by the C in `core/shapes.c`, which the GPU's
  output matches byte for byte. Shapes are drawn from a list the GPU works through while the
  68000 goes on; rows of 16 pixels or more go to the blitter.

## Layout

- `core/` — portable C: the data files, the order, the layer, the movers; `shapes.c`, the
  shape conversion, on its own so the dataset tool can use it; `world.c`, the clock and the
  NPCs on their schedules; `route.c`, their routes. It needs files
  (plat.h's `plat_file_*` names), a clock, and three drawing calls (`eb_copy`, `eb_tiles`,
  `eb_shape`) into 8-bit surfaces.
- `host/` — the PC: draws views to PPM pictures (`u7engine <dir>/ <x> <y> [--walk dx dy n]
  [--movers n] [--out p] [--every k] [--sheet shape] [--layers] [--avatar [--path
  "dx,dy,n;..."] [--time m]]`): `--avatar` walks the avatar as the Jaguar does. Build: `cmake --build build-host
  --target u7engine`.
- `tools/jagdata.c` — `u7jagdata <data dir> <out dir>` (the data dir holding `STATIC/`) writes
  the dataset, in the Jaguar's byte order: `SHAPES.FLX` (7.1 MB), SHAPES.VGA with every object
  shape converted, checked by turning it back; `CHUNKS.DAT` (10.8 MB), each of the 36,864
  world chunks' fixed objects as the engine makes them; and a new game's `NPC.DAT` and
  `U7IREGxx`, as `INITGAME.DAT` holds them. For those the tool runs the engine's own
  core, shown `STATIC/` alone. Build: `cmake --build build-host --target u7jagdata`. The files go
  on the card in `U7/ENGINE/`; the PC engine reads them too (`<dir>/ENGINE/`), turning them
  round. Without them the engine makes it all from the original's files, as before.
- `jaguar/` — the Jaguar: GameDrive files, a triple-buffered 8-bit bitmap object, the blitter,
  the GPU. Build: `cmake --build <jaguar build> --target u7engine_jag`, which gives `U7ENGINE.rom`.
  It uses the same card as the game (`U7/STATIC`, `U7/GAMEDAT`) and `U7/ENGINE`. It starts a new
  game's world at Trinsic at 6:00: the avatar and the NPCs of `ENGINE/NPC.DAT` (the 31 nearest
  it), and the new game's movable items (`ENGINE/U7IREGxx`, before `GAMEDAT/`'s). The pad walks the
  avatar, 7.5 cells a second, the view on it; a roof over it is lifted (below). It walks as the
  original's avatar does (below, walking); walking into a shut door opens it, and A opens or
  shuts the door ahead. Untouched, it walks into the stables and out.
  Built with `-DENGINE_TOUR`, it tours Britain with 12 walkers instead (a cell every
  `SCROLL_FRAMES` frames, default 1): the measurements below.
  Every 64 frames the frame rate and timings go to `GD_DebugString` (in blanks, timed by the
  vertical count to a half-line), and once a profile of 900 samples to `U7/PCPROF.BIN`
  (`tools/jag-profile.py`). The samples are taken by Tom's timer at 37 Hz, apart from the frames:
  taken at the blanks, they fell where the frame loop always is then, and misled.

## Measured (2026-11-26, timing core, `-DENGINE_TOUR`: Britain, 12 walkers)

The walk at Trinsic (into the stables and out, about 10 NPCs in view) runs at 30-59 frames a
second, the worst frame 2-10 blanks. (It was 41-59 and 4-6 before the movable items were read
right: many were lost, and the areas hold more now.) At 21:00 (`-DSTART_MINUTES=1259`), when NPCs
near the avatar set off for their evening places, the first 64 frames are 21 a second, the
worst 16 blanks; then as at 6:00. A route costs the 68000 about 0.4 ms a step tried: planning to
every sixth cell near Trinsic's gate (`-DENGINE_ROUTE_BENCH`), 4.7 blanks a route, the most 20.

Frames a second, averaged over 64 frames; the view scrolls a cell every frame (60 cells a second,
many times walking pace) or every fourth frame (15 cells a second):

| Version | 60 cells/s | 15 cells/s |
|---|---|---|
| Straight port of the game (for comparison) | 0.4-0.6 | |
| Every object drawn every frame by the GPU | 5-8 | |
| Whole static layer redrawn at each chunk crossing | 13-31 | |
| Ring of areas drawn ahead of the view | 18-31 | 23-28 with the walkers in view, 44-51 without |
| Shapes converted as read, drawn from a list | 17-21 | 24-30 with the walkers in view, 38-40 without |
| Shapes converted on the PC, read as they are | 23-35 | 25-34 with the walkers in view, 43-53 without |
| Three frames, so the 68000 does not wait for a blank | 25-39 | 35-44 with the walkers in view, 49-56 without |
| Chunks' fixed objects from the PC, area order without calls | 28-46 | 33-43 with the walkers in view, 44-56 without |
| Statics near a mover kept, footprints in the records | 29-49 | 40-50 with the walkers in view, 45-57 without |
| The movers' pictures, order and list on the GPU | 29-49 | 47-53 with the walkers in view, 45-57 without |
| All the movable items read (many were lost before) | 27-47 | 40-59 |

There is no stall at the crossings any more. The converted shapes draw faster (a walker's frame,
200 times: 6 blanks against 9 the original's way, flipped 9 against 11). Converted as read, they
cost the GPU about 0.45 blanks per 10 KB of shape while the 68000 waits, more than the faster
drawing saved; converted on the PC, building the areas ahead costs about half what it did (37-96
blanks per 64 frames against 92-146), and the shape cache gets back the 49 KB the conversion
needed. What is left of a walker's cost is one main-RAM access for each pixel read and one for
each written.

Reading shapes now costs almost nothing (0-7 blanks in 64 frames). What is left is the 68000's
work, and the 68000 is slow: on the timing core it runs at about 70% of its 13.3 MHz (main RAM's
waits; `-DENGINE_BENCH_CPU` measures it), and a line of C that tests two objects is some 300
cycles. Reading a chunk cost about a blank: its 256 cells looked through, and a rectangle and
bins worked out for each object. From the dataset it is a read, and the chunks' share of the
areas' work fell from 28 to 8 blanks in 64 frames at 60 cells a second; the worst frame from 7-12
blanks to 6-8. An area's order still compares every pair of its objects that overlap, and most of
them do (the pictures of one area lie close): 10-25 blanks in 64 frames at 60 cells a second.
Copying the ring to the frame is 0.2 blanks a frame, the blitter's own speed. Not copying, the
object processor showing the ring itself under a layer for the movers (`-DENGINE_LAYERS`; the
PC's `--layers` puts the picture together the same way, and it is the same), saves that, but
the 68000 then runs the movers about 20% slower, and with walkers in view the frames come out
slower (33-39 against 40-45 a second): the object processor reads up to five bitmaps a line and
comes before the 68000. Real hardware may differ; it is kept as a switch.

A mover's fronts were worked out anew at each step it took, finding the statics near it in up
to nine chunks; and whenever any chunk was read, for every mover. Now the statics near it are
kept, each chunk slot counts the chunks it is given, and a chunk's box round all its pictures
lets a search pass over the chunks that do not reach. The fronts fell from 27 to 4-12 blanks in
64 frames at 15 cells a second; 6 of them were my own clock, read for every mover. What was left
of the movers was spread thin over every statement run for each mover each frame; on the GPU
the 68000's share fell from 58 to 22 blanks in 64 frames.

A larger ring (4 by 4, 5 by 4: `-DRING_AX= -DRING_AY= -DCHUNK_SLOTS=`) changes nothing; getting
chunks ready in the frames before cost more than it saved (region files read again); binning
an area's objects to test fewer pairs cost more than testing them all.

Built with `-DENGINE_BENCH_SHAPES` the Jaguar build logs those timings at the start;
`-DENGINE_CHECK_CONVERT` converts the first 24 shapes in C too and logs any difference;
`-DENGINE_NO_FRAME_SHAPES` leaves the movers and what is in front of them undrawn;
`-DENGINE_DOUBLE_BUFFER` uses two frames, waiting for the blank; `-DENGINE_BENCH_CPU` logs the
68000's speed; `-DENGINE_LAYERS` shows the ring by the object processor (above);
`-DENGINE_C_MOVERS` draws the movers by the core's C; `-DENGINE_FRAME_SUMS` logs a sum of each
of the first 400 frames' pixels, which hang on the frame count alone: the GPU's movers and the
C's give the same 400; `-DENGINE_ROUTE_BENCH` plans routes from the avatar to every sixth cell
near the view at the start and logs what they cost. With
`U7_JAG_FRAME_POINTERS` (or `-fno-omit-frame-pointer -DJAG_PROF_FRAMES`) the profile has whole
call chains (`tools/jag-profile.py --frames`).

## Not done yet

- **An area's order from the PC.** The order among its fixed objects (the pairs that matter)
  could be in the dataset, leaving only the movable items to compare at run time.
- **The 68000 free while the GPU draws the movers.** It waits for them at the frame's end
  (finish: about 23 blanks in 64 frames with 12 walkers in view); a game would run then.
- **The shape cache** in the GameDrive's 16 MB of cartridge RAM (words and longs only): the
  whole dataset (7.1 MB) would fit, read once at the start.
- **Animation.** Animated statics and terrain (water, fire, clocks) belong with the movers, not in
  the ring.
- **Translucency**; **NPCs** from a save (it holds them as the game's memory, `U7NBUF.DAT` and
  `U7IBUF.DAT`, not as `NPC.DAT`); what their schedules' kinds do at the place (sleep, eat,
  work, sell); door sounds; the party.
- **Order.** `order_of()` is a simple rule. It still needs checking against the original's frames
  at the same places (the PC reference can draw them). The ring version already draws doors and
  benches that the first version lost.
