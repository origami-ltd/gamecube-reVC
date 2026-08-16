# Credits — GameCube port

## dca3 (GTA III / Vice City for the Dreamcast)

<https://gitlab.com/skmp/dca3-game>

Started by **Stefanos Kornilios Mitsis Poiitidis** and built by **The Gang**,
using [re3](https://github.com/halpz/re3) as a base and
[KallistiOS](https://kos-docs.dreamcast.wiki/).

dca3 fits GTA III and Vice City into a Dreamcast — 16MB of main RAM, less than
the GameCube's 24MB. It is the closest prior art to this port that exists, and
it answers problems this port had been solving badly:

- **Assets are converted ahead of time, not at runtime.** `src/tools/texconv.cpp`
  runs over every `.dff` and `.txd` at build time and writes a repacked IMG in
  the console's native format, so the running game performs no texture
  conversion at all. It is built by linking the game's own librw against an HLE
  layer for the console graphics API, which lets the offline tool run exactly
  the code the console would.

  This port was instead converting D3D8 rasters to GX format at stream time,
  which meant a D3D raster, a full RGBA8 `Image` and an RGBA8 staging buffer
  were all live at once for every texture loaded. Measured on hardware: 12.3MB
  live across ~16300 allocations, with roughly 4.4MB of free heap split into
  ~9200 chunks. Those transient conversion buffers are the fragmentation.

- **`USE_CUSTOM_ALLOCATOR` is disabled in dca3 too.** They did not solve memory
  with a smarter game-side heap; they solved it by removing the runtime work
  that fragments one. That is a correction to the direction this port was
  heading.

- **`vendor/librw/src/dc/alloc.cpp`** is a pool allocator that *compacts*:
  `alloc_run_defrag()` moves allocations and reports each move through a
  callback so owners can fix up their pointers, and `alloc_count_free()` /
  `alloc_count_continuous()` expose total free against largest contiguous
  free. It manages PVR VRAM rather than the general heap, but the design
  transfers. `alloc_count_continuous()` in particular is the metric this port
  wanted and could not obtain from `mallinfo`, which has no largest-block
  field.

Their librw fork carries librw's MIT licence (Copyright (c) 2014 aap), so
anything adapted from it stays under that licence with attribution. The dca3
game repository itself ships no top-level licence file, so game-side code there
is not treated as reusable here.

## librw

<https://github.com/aap/librw> — MIT, Copyright (c) 2014 aap.

The GameCube (GX) backend in `vendor/librw/src/gx` is new work for this port;
librw ships D3D8, D3D9, OpenGL and PS2 backends and had no GameCube one.

## re3 / reVC

Reverse-engineered GTA III and Vice City sources, the base this port builds on.
