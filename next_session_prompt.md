# reVC GameCube — next session

## The frame

The reference for this port is **dca3** — <https://gitlab.com/skmp/dca3-game>,
GTA III and Vice City on a Dreamcast, by Stefanos Kornilios Mitsis Poiitidis
and The Gang, on re3 + KallistiOS. Cloned at `~/Documents/GitHub/dca3-game`.

They fit that game into **16MB of main RAM**. This port has **24MB**, plus 16MB
of A-RAM they do not have, plus a faster CPU and a real texture cache. If a
Dreamcast can hold Liberty City, a GameCube can hold Vice City — the ceiling
here is not the hardware, and every time this port hit a wall the cause turned
out to be something the port was doing, not something the console lacked.

So: **treat dca3 as the working proof and follow its architecture.** Its central
decision is that assets are converted at build time and the running game does no
conversion at all (`src/tools/texconv.cpp`, per-file Makefile rules producing a
repacked IMG). Notably `USE_CUSTOM_ALLOCATOR` is disabled in dca3 too — they did
not answer memory with a smarter heap, they removed the runtime work that
fragments one. Their `vendor/librw/src/dc/alloc.cpp` is a compacting pool
allocator (`alloc_run_defrag()` moves blocks and reports each move via callback;
`alloc_count_continuous()` gives largest-contiguous-free, which `mallinfo`
cannot). Their librw fork is **MIT**, so adapting from it is fine with
attribution; the dca3 game repo has no top-level licence, so game-side code
there is not reusable. Credited in `CREDITS-GAMECUBE.md`.

## Build & run

```bash
cd /Users/ebellumat/Documents/GitHub/reVC-wii && ninja
pkill -9 -x Dolphin; pkill -9 -f "Dolphin.app/Contents/MacOS/Dolphin"
fsck_msdos -y ~/Library/Application\ Support/Dolphin/Load/WiiSD.raw
/Applications/Dolphin.app/Contents/MacOS/Dolphin -e /Users/ebellumat/Documents/GitHub/reVC-wii/src/reVC.dol
```

- Gameplay is ~5.5 min after boot; the intro cutscene before it is dark and
  looks like a broken renderer. Do not judge a build before gameplay.
- `SIGTERM does not kill Dolphin`. A survivor holds TCP 55020 and the next
  instance logs "Failed to bind listener socket", silently losing the capture.
- **The SD image mounts without root**, which the old notes said was impossible:
  `hdiutil attach -imagekey diskimage-class=CRawDiskImage -nobrowse <WiiSD.raw>`
  → `/Volumes/REVC`. This is how converted assets get installed and how logs are
  read. `mount_msdos` is the thing that needs root; `hdiutil` does not.
- Helpers in the session scratchpad: `boot.sh` (kill/fsck/boot/capture gecko),
  `winid.swift` (Dolphin window id — `.optionAll`, it can sit on another Space).
- Gecko truncates anything past ~25 characters. Structured data goes to the SD.

## Fixed and confirmed on screen

- **Character textures.** `gxGetTexture` frees the staging pixels after tiling;
  `rasterLock` then fabricated a zeroed buffer on any later lock and
  `rasterUnlock` marked the raster dirty, so a good texture was rebuilt from
  zeros or from one mip level in a level-0-sized allocation.
- **Lighting on everything non-prelit** (cars, peds, props). `worldBeginUpdateCB`
  set `engine->currentWorld` and the end-update never restored it; CShadowCamera
  is created with `RwCameraCreate()` and never added to a world, so everything
  drawn after a shadow pass evaluated its LIGHT flag against a nil world.
- **Interior walls and ceilings.** The shadow camera was drawing its
  stripped-flag silhouette straight into the scene framebuffer (there is no
  render-to-texture in the GX backend) and its white clear became the frame's
  copy-clear colour. Offscreen passes are skipped.
- **Radar mask.** GX does not update Z while the depth compare is disabled, so
  `CRadar::DrawRadarMask`'s "don't test, do write" has to be an always-passing
  compare.
- **Analog sticks.** `PAD_StickX` reaches about ±72 at the physical gate; the
  game scales against ±128, so full deflection asked for barely half speed.
- **OOM `exit 1`.** `Geometry::create`'s large allocations soft-fail now, and the
  nil propagates cleanly to the streamer, which retries.
- **Honest metrics.** HUD shows presentation rate from the measured frame period,
  free heap, free-chunk count, and streaming time.

## Open, with the numbers

Measured in gameplay on the original archive:
`29 f33.3 max483 work31 oom0 m11585K fr6991K blk10701 str0`

- **Stutter**: nominal 29fps with 483ms worst frames.
- **Everything draws at the far LOD**; trees, lamp posts and props flicker.
- **Fragmentation**: ~7MB free split across ~10700 chunks. Allocation fails on
  shape, not capacity.

These are one problem, not three. VC map objects have exactly one atomic each
(verified across all 3511 IDE entries) — LOD is entity-level, separate `LODxxx`
models, and `SetupBigBuildingVisibility` only hides the far LOD once the
detailed model is resident. Starve streaming and the world draws from LODs and
flickers as models come and go. Do not attack the renderer for this.

Allocation profile, live, from `dvd:/alloc.log`:
16176 allocations under 16KB holding 5.7MB, and 124 of 16KB or more holding
6.6MB. Two thirds of the bytes in 124 blocks, 99% of the blocks small and
long-lived scattered between them.

## The AOT pipeline — built, verified offline, not yet paying off

`tools/gamecube/`: `txdconv` (PC TXD → GameCube native, full resolution, CMPR
or RGB5A3), `txdverify` (decodes a converted blob the way the console does),
`repack_img.py` (whole archive, modelled on dca3's imgtool). librw side:
`PLATFORM_GAMECUBE = 6`, `gx::readNativeTexture/writeNativeTexture/
getSizeNativeTexture`, wired into `Texture::streamReadNative`.

Verified: the player atlas survives PC → GX-native → decode byte-exactly, and
chunk arithmetic closes on `nativeEnd`. 1367 of 6043 entries convert.

**It still stalls.** With the converted archive the game stops on the intro
title card; the same binary on the original archive reaches gameplay.

What that stall is *not*, all measured:
- not the native texture reader — `dvd:/native.log` stayed empty for a whole
  boot, so it rejected nothing
- not the directory — names and entry count are byte-identical after repack
- not the chunk format — the per-texture extension chunk is written and the
  arithmetic verifies
- not a retry loop — `str0` and `ms_memoryUsed` frozen at 470K mean the streamer
  is **idle**, not spinning. (An earlier "leak" reading was sampling noise
  across boot phases; do not trust a single sample.)

**Start here**: instrument `ConvertBufferToObject` with success/failure counts
per asset type (DFF, TXD, COL, IFP) written to the SD. Today's instrumentation
only covers geometry allocation (`oom`) and native texture reads, which is why
the stall has a hole in the middle. Something makes the game decide it does not
need to load; find which asset type stops resolving.

## Do not re-try — all measured

- Texture cap 512 → 256: free-at-crash moved 2289K → 2198K. No effect.
- Scaling `StreamedSize`: the reserve was swung 8 → 4 → 6MB and each value
  traded one failure mode for the other. The budget is not the problem, the
  shape of the heap is.
- `USE_CUSTOM_ALLOCATOR`: `MemoryHeap.cpp` does not exist in reVC (it is a re3
  file). Disabled in dca3 as well, deliberately.
- `ms_lodDistScale = 0.925`: that is the PC options-slider minimum, not a
  console value. Stock is `1.2`.
- Forcing the highest-detail atomic per model: no-op, every map object declares
  `numAtomics = 1`.
- Both `CULLBACK` mappings drop interior walls. `GX_CULL_NONE` is the only
  setting verified correct; the winding is not simply inverted. Do not "fix"
  this without an A/B in an interior.
- Gecko for anything structured.

## How to work on this

Two things cost the most time in the last session, both avoidable.

**Audit the consumers before changing a mechanism.** Every regression came from
swapping something out without checking who depended on it: a semaphore whose
count was not reset per request (loading-screen deadlock), an eviction that
refunded zero bytes so `MakeSpaceFor` stripped the world (pause-menu stall), a
cull mapping changed on reasoning instead of an A/B (interior walls lost). The
one structural change that broke nothing — `rw::platform` — was the one where
all seven consumers were listed first.

**Measure before theorising.** Every real cause this session came from a
measurement, and every long detour came from a plausible story: the allocation
histogram found the fragmentation, the scene-transition log named
`cl_tablesetlrg` at distance 8.4 against a threshold of exactly 8.4, the host
tools proved the atlas and UVs correct in seconds where a console boot costs
eight minutes, and an A/B with the same binary proved the archive was the
trigger. Reading the assets directly settled in one command what several build
cycles of guessing had not.
