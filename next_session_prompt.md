# reVC GameCube — next session

Everything is still uncommitted, in `~/Documents/GitHub/reVC-gamecube` (+ `vendor/librw`).

## Build & run

```bash
cd /Users/ebellumat/Documents/GitHub/reVC-wii && ninja
pkill -9 -x Dolphin; pkill -9 -f "Dolphin.app/Contents/MacOS/Dolphin"
fsck_msdos -y ~/Library/Application\ Support/Dolphin/Load/WiiSD.raw
/Applications/Dolphin.app/Contents/MacOS/Dolphin -e /Users/ebellumat/Documents/GitHub/reVC-wii/src/reVC.dol
```

Gameplay lands ~5.5 min after boot — do not screenshot before that, the intro
cutscene is a dark dock scene and looks like a broken renderer.
`SIGTERM does not kill Dolphin`; a survivor keeps port 55020 bound and the next
instance logs "Failed to bind listener socket", silently losing the gecko capture.
Boot helper that handles all of this: `<scratchpad>/boot.sh`, window id via
`<scratchpad>/winid.swift` (`.optionAll` — Dolphin can live on another Space).

## Fixed and verified this session

1. **OOM `exit 1` — fixed.** `Geometry::create` allocated two large contiguous
   blocks with `rwNew`/`mustmalloc`, which calls `exit(1)`. Both are now
   soft-fail (`rwMalloc` + nil check) in `vendor/librw/src/geometry.cpp`; the nil
   propagates cleanly through `Geometry::streamRead` -> `Clump::streamRead`
   (`failgeo`) -> `LoadAtomicFile` -> `ConvertBufferToObject`, so a failed load
   is retried by the streamer instead of killing the process. This is the same
   contract `rasterCreate` and the d3d texture path already used. Verified over
   two cold boots that previously died at ~5 min.
2. **FPS counter lying — fixed.** `FramesPerSecond` is a 30-sample mean and read
   a flat 30 through a 266ms hitch. The HUD now derives the rate from the
   measured frame period (`gxSnapFrame`, Idle-entry to Idle-entry) and holds the
   worst period of the last 5s: `29 f33.3 max50 work29 end20 vs4`.
3. **`TrimStreamedModels()` removed** (`src/core/Streaming.cpp`) — the cause of
   bugs 2/3 (LOD flicker, missing trees/posts) and probably 1. It evicted 64
   least-used models every 16th `ConvertBufferToObject`, regardless of pressure.
   `RemoveLeastUsedModel` protects models with live refs, but a model that just
   streamed in has *no* refs until its entity instantiates it — so its favourite
   targets were exactly what had just been loaded, on a cadence driven by
   loading itself. `MakeSpaceFor` already evicts correctly, bounded by the
   budget. **Not yet visually confirmed** — needs a drive through the city
   looking for trees/lamp posts and LOD stability.
4. **Black characters — largely fixed.** Two real bugs, both found from probe
   data, not theory:
   - **The big one:** 165 of 235 sampled ped meshes carry `gflags` `0x11`/`0x13`
     — **no `TEXTURED` flag, so no texture coordinates** — yet the draw loop
     bound the material's texture anyway and streamed `UV (0,0)` for every
     vertex (the `uv ? uv[vi].u : 0.0f` fallback), painting each mesh with one
     corner texel. `tex` is now gated on `uv != nil`. This fixed ped hair and
     shoes, car tail lights and plates, and building detail across the scene.
   - Meshes with neither prelight nor `LIGHT` had no colour source and streamed
     `(0,0,0,255)` vertex colours (librw's gl3 leans on GL's `(0,0,0,1)` attrib
     default; it never shows on PC because skinned atomics take the skin
     pipeline, and GX has no skin pipe). `sendColor` is now decided once in
     `atomicRenderCB` and passed into `setup3DDraw`, falling back to the
     material register — RW fixed-function behaviour.

## Still open

- **Tommy's shirt/torso mesh is still black** while his jeans, arms, hair and
  shoes render. Same clump, same white material (`mat=ffffffff` on every one of
  235 samples), so it is per-mesh.
  **It is not a lighting problem.** Confirmed on a long soak: the game clock
  reached 10:16, full daylight, blue sky, every other surface correctly lit —
  and the torso was still pure black. That rules out the `lit=1 hw=1` /
  near-zero-`ambL` path (34/119 samples had `ambL` as low as 0.01) as the cause
  for this mesh.
  So the fault is the texture or the material binding for that one mesh. Next
  step: re-enable `GX_PROBE_SKIN` and log the torso mesh specifically —
  `mat` pointer, `texture` pointer, `texture->raster`, `gxFmt`, tiled first
  word — to separate "this raster converts to black" from "no texture is bound
  and it falls back to a black-ish material path". Check the CMPR encoder
  (`tileCMPR`) against a known texture first: a block where every texel is
  transparent leaves `mn`/`mx` at their init values (255/0), and the player
  torso is exactly the kind of texture the player-clothes callback swaps at
  render time.
- **Bug 1 (interiors)** — not investigated. Test whether removing the trim
  already fixed it; "walls missing, character reads as a wall" is consistent
  with interior models being evicted before instantiation.
- **Bugs 2/3** — fix in place, needs the visual drive test.

## Diagnostics in the tree

- `gx.cpp`: `GX_PROBE_SKIN` (**0**; set to 1 to dump every input to the skinned
  draw to `dvd:/skin.log`, one sample/second so it spans intro *and* gameplay).
  Also `GX_WIREFRAME`, `GX_MARK_SKINNED`, `GX_NO_SKIN`, `GX_SKIN_LOCALSPACE`,
  `GX_USE_INDEXED`, `GX_DL_BUDGET`.
- 5s heartbeat now writes `dvd:/hb.log` as well as the gecko.

**Read SD logs without mounting** (`mount_msdos` needs root here):
```bash
LC_ALL=C grep -a -o "SKIN [^|]\{0,220\}" ~/Library/Application\ Support/Dolphin/Load/WiiSD.raw
```
SD logs **append across boots** — old generations stay in the image, so filter or
compare counts rather than assuming the newest run is all that is there.

## Do not re-try these (measured, no effect)

- **Texture cap 512 -> 256**: free-at-crash moved 2289K -> 2198K, i.e. nothing.
  The OOM was fragmentation on one large contiguous request, not resident
  texture bytes.
- **Scaling `StreamedSize` above ~1.09x**: budget is 7428K against a resident
  world of ~6.8MB, so any real expansion factor leaves the streamer permanently
  over budget and evicting on every load — it re-creates the churn that
  `TrimStreamedModels` caused. Measured at 3/2: `strMem=13569K` vs budget 7428K.
- The eight perf experiments from last session (display lists, LOD distance,
  early-Z, im2D memo, indexed arrays, `RGB565_Z16`, hoisting `loadWorldMtx`,
  `MASTER`). The only win was removing the double `VIDEO_WaitVSync`.
- Gecko as a transport for anything long: Dolphin's emulated gecko truncates
  past ~20-30 chars. Every heartbeat in the first captures was cut mid-string.
  Use the SD log for structured data.

## Note on the previous session's plan

The prescriptive plan (GAMECUBE_RENDER_CORRECTNESS_MODE, one frame in flight,
command-buffer ownership, immutable draw items so "the render worker" cannot
re-read LOD) targets an asynchronous renderer **that does not exist**. There are
no threads, no mutexes, no command queue and no frames in flight anywhere in the
GX backend or the game loop — one FIFO, written synchronously. None of the four
render bugs traced back to a race; they were a streaming sledgehammer, a missing
UV guard, a missing colour source, and a smoothed FPS counter.
