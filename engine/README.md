# The engine test

A test of a Jaguar-first engine for Ultima VII. It draws the world view from the original's own
data files (`STATIC/` and a save's `GAMEDAT/`), and it is built so the 68000 does almost nothing
per frame. This is not the game: there is no usecode, no schedules, no interface. It measures
whether the world can be shown fast enough on a Jaguar to be worth building a game around.

## How it draws

- **The window.** It covers the 5 by 4 chunks from the view's first chunk. When the view crosses
  into another chunk, the static objects of the window are gathered. These are the chunks' own
  object shapes, the fixed items (`U7IFIX`) and the movable items (`U7IREG`). They are then put in
  drawing order once, by a depth-first walk over "drawn before" pairs. Pairs are tested only
  between objects whose pictures overlap, found through 64-pixel screen bins.
- **The static layer.** The terrain and the statics are drawn once, in that order, into a 448 by
  328 picture: as far as the view can scroll before the next crossing. The terrain goes in by the
  GPU (`jaguar/tiles_gpu.s`, a phrase a row) and the shapes by the GPU span drawer
  (`platform/jaguar/spans_gpu.s`).
- **Each frame.** One blitter copy puts the layer's part under the view into the frame. The movers
  (NPCs) are then fitted into the order: each goes after the last static that must stand behind
  it. Each static drawn after that point and overlapping a mover is drawn again, clipped to the
  movers it covers.
- **Depth.** An object stands at the bottom right of its footprint and is lifted 4 pixels up and
  left a level. What lies south, east or above is in front: `compare()` in `core/engine.c`.

## Layout

- `core/` — portable C: the data files, the order, the layer, the movers. It needs files
  (plat.h's `plat_file_*` names), a clock, and three drawing calls (`eb_copy`, `eb_tiles`,
  `eb_shape`) into 8-bit surfaces.
- `host/` — the PC: draws views to PPM pictures (`u7engine <dir>/ <x> <y> [--walk dx dy n]
  [--movers n] [--out p] [--every k] [--sheet shape]`). Build: `cmake --build build-host
  --target u7engine`.
- `jaguar/` — the Jaguar: GameDrive files, a double-buffered 8-bit bitmap object, the blitter,
  the GPU. Build: `cmake --build <jaguar build> --target u7engine_jag`, which gives `U7ENGINE.rom`.
  It uses the same card as the game (`U7/STATIC`, `U7/GAMEDAT`). The pad scrolls the view;
  untouched, it tours Britain with 12 walkers. Every 64 frames the frame rate and timings go to
  `GD_DebugString`, and once a profile of 900 samples to `U7/PCPROF.BIN` (`tools/jag-profile.py`).

## Measured (2026-11-26, timing core, Britain, 12 walkers, scrolling a cell a frame)

| Version | Frames a second (64-frame averages) |
|---|---|
| Straight port of the game (for comparison) | 0.4-0.6 |
| Every object drawn every frame by the GPU | 5-8 |
| Static layer, terrain by the 68000 | 7-18 |
| Static layer, terrain by the GPU | 13-31 |

Between chunk crossings, a frame takes under one vertical blank: one copy, a dozen walkers and the
statics in front of them. It runs at the 60 Hz cap, idle about a fifth of the time. All the cost
is in the crossings, every 16 cells, at 0.2-0.8 s each. About a third of that is the GPU drawing
the layer's statics; the rest is gathering, ordering and loading.

## Not done yet

- **Crossings without a stall.** Build the next layer a little each frame (the idle time is
  there), or keep the layer as a ring and draw only the new strip.
- **The shape cache.** It empties when three quarters full and reloads; it should keep what is in
  use. The GameDrive's 16 MB of cartridge RAM could hold every shape (words and longs only).
- **Animation.** Animated statics and terrain (water, fire, clocks) belong with the movers, not in
  the layer.
- **Translucency, roofs** (hidden when inside), **NPCs** from the save, and the avatar.
- **Order.** `compare()` is a simple rule. It still needs checking against the original's frames
  at the same places (the PC reference can draw them).
