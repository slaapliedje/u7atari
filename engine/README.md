# The engine test

A test of a Jaguar-first engine for Ultima VII. It draws the world view from the original's own
data files (`STATIC/` and a save's `GAMEDAT/`), and it is built so the 68000 does almost nothing
per frame. This is not the game: there is no usecode, no schedules, no interface. It measures
whether the world can be shown fast enough on a Jaguar to be worth building a game around.

## How it draws

- **Chunks.** Each chunk in use keeps its cell words and its static objects once read. The
  objects are the chunk's own object shapes, the fixed items (`U7IFIX`) and the movable items
  (`U7IREG`). Each object keeps its picture's rectangle, binned 8 by 8 by where it lies.
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
  walk does not undo it.
- **Depth.** An object stands at the bottom right of its footprint and is lifted 4 pixels up and
  left a level. What lies south, east or above is in front: `compare()` in `core/engine.c`.
- **Shapes** are read on demand into a ring that gives the oldest way; the movers' are kept apart.
  They are in the engine's own form (`engine.h`, `E_RUN_`): per run one long for where it is, how
  long, and whether it is a fill, then its pixels padded to a long. Short fills become raw pixels,
  so a span is mostly one run. The form is made once on the PC (`u7jagdata`, below) and read as
  it is. Without the dataset the original's shapes are converted as they are read: on the Jaguar
  by the GPU (`jaguar/convert_gpu.s`), on the PC by the C in `core/shapes.c`, which the GPU's
  output matches byte for byte. Shapes are drawn from a list the GPU works through while the
  68000 goes on; rows of 16 pixels or more go to the blitter.

## Layout

- `core/` — portable C: the data files, the order, the layer, the movers; `shapes.c`, the
  shape conversion, on its own so the dataset tool can use it. It needs files
  (plat.h's `plat_file_*` names), a clock, and three drawing calls (`eb_copy`, `eb_tiles`,
  `eb_shape`) into 8-bit surfaces.
- `host/` — the PC: draws views to PPM pictures (`u7engine <dir>/ <x> <y> [--walk dx dy n]
  [--movers n] [--out p] [--every k] [--sheet shape]`). Build: `cmake --build build-host
  --target u7engine`.
- `tools/jagdata.c` — `u7jagdata <STATIC dir> <out dir>` writes the dataset, `SHAPES.FLX`
  (7.1 MB): SHAPES.VGA with every object shape converted, in the Jaguar's byte order, checked by
  turning it back. Build: `cmake --build build-host --target u7jagdata`. It goes on the card as
  `U7/ENGINE/SHAPES.FLX`; the PC engine reads it too (`<dir>/ENGINE/`), turning it round.
- `jaguar/` — the Jaguar: GameDrive files, a double-buffered 8-bit bitmap object, the blitter,
  the GPU. Build: `cmake --build <jaguar build> --target u7engine_jag`, which gives `U7ENGINE.rom`.
  It uses the same card as the game (`U7/STATIC`, `U7/GAMEDAT`), and `U7/ENGINE` if there. The pad scrolls the view;
  untouched, it tours Britain with 12 walkers (a cell every `SCROLL_FRAMES` frames, default 1).
  Every 64 frames the frame rate and timings go to `GD_DebugString`, and once a profile of 900
  samples to `U7/PCPROF.BIN` (`tools/jag-profile.py`).

## Measured (2026-11-26, timing core, Britain, 12 walkers)

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

There is no stall at the crossings any more. The converted shapes draw faster (a walker's frame,
200 times: 6 blanks against 9 the original's way, flipped 9 against 11). Converted as read, they
cost the GPU about 0.45 blanks per 10 KB of shape while the 68000 waits, more than the faster
drawing saved; converted on the PC, building the areas ahead costs about half what it did (37-96
blanks per 64 frames against 92-146), and the shape cache gets back the 49 KB the conversion
needed. What is left of a walker's cost is one main-RAM access for each pixel read and one for
each written.

Built with `-DENGINE_BENCH_SHAPES` the Jaguar build logs those timings at the start;
`-DENGINE_CHECK_CONVERT` converts the first 24 shapes in C too and logs any difference;
`-DENGINE_NO_FRAME_SHAPES` leaves the movers and what is in front of them undrawn.

## Not done yet

- **The shape cache** in the GameDrive's 16 MB of cartridge RAM (words and longs only): the
  whole dataset (7.1 MB) would fit, read once at the start.
- **Animation.** Animated statics and terrain (water, fire, clocks) belong with the movers, not in
  the ring.
- **Translucency, roofs** (hidden when inside), **NPCs** from the save, and the avatar.
- **Order.** `compare()` is a simple rule. It still needs checking against the original's frames
  at the same places (the PC reference can draw them). The ring version already draws doors and
  benches that the first version lost.
