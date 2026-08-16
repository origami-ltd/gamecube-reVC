# reVC GameCube — next session

## Where things are

- Source: `~/Documents/GitHub/reVC-gamecube`
- Build (the .dol Dolphin runs): `build/wii` → `src/reVC.dol`. The stale build trees
  are alongside it under `build/` and are gitignored; only `wii` is live.
- Host tools and the Dolphin harness: `tools/gamecube/` (`boot.sh`, `drive.sh`,
  `winid.swift`, `dffcensus.cpp`, `dffinfo.cpp`, `gameparse.py`).
- dca3 reference: `~/Documents/GitHub/dca3-game`

```bash
cd /Users/ebellumat/Documents/GitHub/reVC-gamecube/build/wii && ninja
pkill -9 -x Dolphin; pkill -9 -f "Dolphin.app/Contents/MacOS/Dolphin"
fsck_msdos -y ~/Library/Application\ Support/Dolphin/Load/WiiSD.raw
/Applications/Dolphin.app/Contents/MacOS/Dolphin -e /Users/ebellumat/Documents/GitHub/reVC-gamecube/build/wii/src/reVC.dol
```

Reconfiguring that build from scratch needs `CMAKE_MODULE_PATH` explicitly — the
toolchain does not add its own directory, so without it CMake never finds
`Platform/NintendoWii.cmake`, `NINTENDO_OGC` stays unset and the configure dies on
`Illegal REVC_AUDIO=NULL`:

```bash
cmake -S . -B build/wii -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/Users/ebellumat/dkp-wii/cmake/Wii.cmake \
  -DCMAKE_MODULE_PATH=/Users/ebellumat/dkp-wii/cmake \
  -DCMAKE_BUILD_TYPE=Release -DREVC_VENDORED_LIBRW=ON \
  -DLIBRW_PLATFORM=GAMECUBE -DLIBRW_TOOLS=OFF -DLIBRW_INSTALL=OFF
```

`~/dkp-wii` is the one piece of this port still outside the repo: a copy of
`/opt/devkitpro/cmake` plus two local files (`Wii.cmake`,
`Platform/NintendoWii.cmake`) that devkitPro does not ship. Nothing else depends
on that path.

- Gameplay is ~5 min after boot. `SIGTERM does not kill Dolphin` — a survivor holds
  TCP 55020 and the next capture is silently lost.
- SD mounts without root:
  `hdiutil attach -imagekey diskimage-class=CRawDiskImage -nobrowse <WiiSD.raw>` → `/Volumes/REVC`.
- `winid.swift` prints `id<TAB>WxH<TAB>owner<TAB>name`: **cut field 1**, and the render
  window is the one titled `Dolphin <ver> | JITARM64 SC | Metal | HLE`. The 500x500
  one screenshots pure white.
- Gecko truncates past ~25 chars. Structured data goes to the SD.

## Fixed this session

**The AOT archive stall.** `txdconv` wrote `PLATFORM_GAMECUBE` into the TXD
dictionary's deviceId field. The game does not read that field:
`RwTexDictionaryGtaStreamRead` (`src/rw/TexRead.cpp`) takes **all four struct
bytes as one int32 texture count** and rejects anything over `INT16_MAX`, so
every converted dictionary announced `393216 + n` textures and was refused before
a single texture was touched. Every stock VC TXD writes 0 there; now so do we.
That one uint16 is why the port sat on the title card, why `native.log` stayed
empty (the reader was never reached) and why `ms_memoryUsed` froze at 470K with
`strReq` stuck at 78 (the failure path is `RemoveModel` + `ReRequestModel`, which
re-requests forever without allocating).

`txdverify` read the count as a uint16 — the librw way — so it validated a blob
the console rejected outright. It now parses it the way the game does.
**A verifier that does not parse the way the consumer parses is worse than none**,
because it converts "unverified" into "verified" without touching the failure.

**20fps → 60fps.** `psCameraShowRaster` (`src/skel/gamecube/gamecube.cpp`) called
`VIDEO_WaitVSync()` after `gx::showRaster` had already waited for one. A whole
field of pure idle every frame. Measured before the fix: 1507 of 1534 logged
frames were **exactly 50.0ms** — a locked 20fps, not a stutter. After: `work` is
7-18ms. The single-retrace change had been made inside showRaster and this
consumer was missed.

## 480p and 60fps, and what they cost

**480p is on and verified: `vi=2` (`VI_TVMODE_NTSC_PROG`), xfbH=480.** It had to be
forced. `VIDEO_GetPreferredMode` only returns a progressive mode when the console
asks for one, and the obvious guard — `VIDEO_HaveComponentCable()` — is unusable
under Dolphin: **measured `cbl=0` regardless of its progressive-scan setting.**
Writing `IPL.PGS=1` into Dolphin's SYSCONF is rewritten back to 0 on launch, and
`[Display] ProgressiveScan` in Dolphin.ini does nothing because `ProgressiveScan`
is a *per-game* INI override and a loose .dol has no game ID to match. So the
cable check can never pass on the emulator. `GX_FORCE_PROGRESSIVE` in `gx.cpp`
takes the decision instead. It is safe: an emulator has no composite cable to be
incompatible with, and on hardware component cables and the GCVideo/GCHD adapters
all set the DTV bit. **Set it to 0 for a console wired over composite** — there,
progressive output is no picture at all.

**60fps holds at `work11`, and it took work to keep.** Giving the streamer a real
budget put ~4ms of extra geometry into every frame and the frame went to
`work17` — 0.4ms over the retrace, so it fell to the next one and locked at
**exactly 33.3ms, 29fps**. The fix was not to draw less. The per-vertex colour was
computed in floating point and written with four `(u8)(float)` casts, and
**Gekko has no direct path between the float and integer registers** — every one
of those is a store and a reload through memory, eight per vertex. The world takes
that path for essentially every vertex it draws (no normals, so no directional
contributes). Replacing it with the integer equivalent — `mul255`, exact, no
divide — returned **6ms: work17 → work11**.

## The streaming budget: what changed and why the reserve moved

The port was in the state this file described as pinned. Now, measured in the same
alley, textures resident:

| reserve | budget | strMem | strReq | ev/ld per 5s | lodMiss | free (gameplay) |
|---|---|---|---|---|---|---|
| 6MB | 10264K | 9830-12570K, **above the cap** | 43-55 | — | — | 2890K |
| 4MB | 12300K | 11545-12279K | 16-42 | 280/325 | ~17/frame | 4709-5691K |
| **2MB** | **14348K** | 13572-14173K | **6-12** | 175/225 | **0** | **4652-5812K** |

Note what the last row does *not* say: free bytes barely moved between a 4MB and a
2MB reserve. The streamer's own accounting is conservative against real heap use,
so 2MB of budget did not cost 2MB of headroom.

The old state is the one the streamer cannot recover from: `ms_memoryUsed` sat
permanently at or above `ms_memoryAvailable`, so `MakeSpaceFor` evicted on every
single request and `strReq` never drained below 43. Nothing stayed resident, and
`SetupBigBuildingVisibility` holds the far shell whenever the detailed model is
not resident and faded in. That is the LOD fallback, in one sentence.

**The reserve is now 2MB, and yes, that is the trade this file said not to make.**
The rule was right; the precondition is what changed, in two steps and in this
order.

*First, real bytes went in.* `gxPackGeometry` reclaimed them at the **unchanged**
6MB reserve: free bytes went **2890K → 5157K**, measured in the same alley with
textures resident. Nothing was spent until the headroom existed.

*Then the fear was replaced by a measurement.* The stated reason not to trust free
bytes was fragmentation: 5423K over 12085 chunks averages 460 bytes, so a big
`Geometry::create` could fail with megabytes free — and mallinfo has no
largest-block field, so it stayed a fear. The heartbeat now probes it directly:
malloc, halving down from 2MB, freed immediately. **`maxblk=2048K` in 146 of 146
samples** — the probe's own ceiling, never once lower, including at the boot's
467K free low point. The arena is not fragmented in any way that matters.

So judge this reserve by **`maxblk` and `oom`, not `free`**. maxblk falling under
~1MB, or oom leaving 0, is the real signal. Free bytes in gameplay are 4652-5812K
and the tightest moment of the whole run is `LoadScene` at t=8s (467K, oom still
0) — that transient is what the reserve is actually for.

## The LOD fallback is gone, and here is the number

`SetupEntityVisibility` counts, rather than logs, the thing that matters: an
entity **on screen and not occluded**, inside the range of its most detailed
atomic, that still gets nil back. The heartbeat reports it per interval as
`lodMiss=`. On-screen is part of the definition — the frustum and occlusion tests
below it only run on the branch where an atomic exists, so an earlier version that
counted before them was counting everything behind the camera too and read ~37 per
frame where the honest figure was ~17.

Final run, 5s intervals:

```
t=183s lodMiss=351   <- control handed over, camera jumps to the player
t=188s lodMiss=241
t=193s lodMiss=2
t=198s .. t=359s     lodMiss=0    (33 consecutive samples, 161 seconds)
```

**Zero, sustained.** The two large readings are the world streaming in for the
first time at a position it has never been at; it settles in about fifteen
seconds and then nothing on screen falls back to a shell again.

What proved it was capacity and not something else: with the player standing
still, the streamer logged **ev=280 evictions against ld=325 loads every five
seconds**. Nearly every load cost a model, and the evicted model was immediately
re-requested. Standing still should need neither. That pair is worth keeping in
mind as a diagnostic — `ev ≈ ld` is churn, `ev ≈ 0` with loads still trickling
would have meant the budget was fine and the backlog lived somewhere else.

**ARAM turned out not to be needed, and an earlier draft of this file oversold
it.** Worth being precise, because it reads like free memory and is not: the
16MB is not addressable, only DMA-reachable, and RW objects are pointer graphs
that cannot be moved to a different address and back. ARAM can hold *serialised*
bytes — a disc cache in front of a 2-3MB/s drive with >100ms seeks, or audio —
which cuts I/O stalls but does **not** raise the streaming budget by a single
byte. If you go there, go for the load hitches (`max100` still shows), not for
resident capacity.

## In the tree now (uncommitted)

- `tools/gamecube/txdconv.cpp`, `txdverify.cpp` — the deviceId fix. **The archive on
  the SD depends on this.**
- `src/skel/gamecube/gamecube.cpp` — duplicate `VIDEO_WaitVSync` removed.
- `src/core/config.h` — `GTA_REPLAY` off for `GTA_OGC`. `CReplay::Buffers` is 781KB of
  static MEM1, the largest object in the image (`nm --size-sort` on reVC.elf; next
  are ThePaths 352KB, ScriptSpace 260KB, ms_aSectors 256KB). Arena 15564K → 16384K,
  so the budget rises ~820K **at an unchanged reserve**.
- `vendor/librw` `geometry.cpp` + `rwobjects.h` — `dropTrianglesAfterInstancing()`.
  librw keeps connectivity twice: `Triangle triangles[]` at 8 bytes each *and* the
  per-mesh uint16 index lists. The GX path draws only from the mesh indices. A
  census of every DFF in gta3.img put the duplicate at **16.9MB of 87.2MB — 19% of
  all geometry**. Adds `attribBase` so the block can be shrunk; `destroy()` frees
  that instead of `triangles`.
- `vendor/librw` `gxraster.cpp` + `rwgx.h` + `gx.cpp` — **`gxPackGeometry()`**.
  Quantises a streamed geometry's positions to `GX_S16` and texcoords to
  `GX_TEX_ST/GX_S16`, both with a per-geometry binary shift `GX_SetVtxAttrFmt`
  applies in hardware, then shrinks the morph-target and attribute blocks to
  release the float arrays. 20 bytes a vertex become 10. `refP=0 refU=~300` — no
  geometry has extents too large for int16 positions, ~2% tile their UVs past the
  precision floor and keep floats. `tools/gamecube/packtest.c` pins the shift and
  round-trip contract; it caught two off-by-ones in the floor constants already.
- `src/fakerw/fake.cpp` — `RpClumpStreamRead` calls it. That is the choke point
  every streamed clump passes through, and geometry the game builds itself
  (CWaterLevel's wavy mesh, which rewrites its vertices every frame) never reaches
  it, so it keeps its float arrays.
- `vendor/librw` `gx.cpp` — integer vertex colour fast path, `GX_FORCE_PROGRESSIVE`,
  and the `gxViTVMode`/`gxHaveComponent` latches the heartbeat reports.
- `src/core/config.h` — `COMPRESSED_COL_VECTORS` on for `GTA_OGC`. Halves every
  collision vertex. Note it did **not** move `strMem`: VC collision comes from .col
  files loaded outside the streaming budget, so this is general heap, not world.
- `src/core/Streaming.cpp` — reserve 6MB → 2MB with the measurement above, plus
  `gStrEvict`/`gStrLoad` (the churn diagnostic).
- `src/renderer/Renderer.cpp` — `gLodMiss`, one compare per entity.
- `src/core/main.cpp` — heartbeat carries `vi/cbl/xfbH`, `pack/geo/refP/refU`,
  `lodMiss`, `free/blk/maxblk`, `ev/ld`. `maxblk` is a live malloc probe, not
  mallinfo — it is the only way to see the largest obtainable block.

## Ruled out by measurement — do not re-suspect

- **The triangles reclaim did not cause the exterior regression.** A/B'd: with it
  disabled the outdoor scene rendered identically.
- **`GX_NO_VSYNC` did not cause it.** Locking back to the retrace changed nothing.
- **`COMPRESSED_COL_VECTORS` is safe and is now ON.** The reject path was
  instrumented to `dvd:/col.log` and the file was never created — every collision
  vertex in Vice City fits int16 at 1/128 unit.
- **Driving the player from a script does not work.** `drive.sh` posts arrow keys
  through System Events; Dolphin's Quartz input backend polls global key state and
  a synthetic hold does not register, so the player never moves and the scene
  under test never changes. Screenshots taken after it are the same alley. Use the
  computer-use tools with granted access, or a pad, if you need to survey the open
  world — and until then prefer `lodMiss`/`strReq` over eyeballing one street.
- Everything in the previous handoff's do-not-retry list still stands.

## The packing was done at load, not ahead of time — and that was the cheap half

The previous plan here was an AOT DFF converter plus a native geometry read path,
copying dca3's `Geometry::NATIVE`. That is not what shipped, deliberately. dca3
needs its own format because the PVR does; **we only needed the resident bytes**,
and quantising at load gets the identical reduction with no new file format, no
archive repack, and no second parser to keep in sync with the game's. If disc
bytes or load time ever become the constraint, the AOT converter is still the
answer — the quantiser in `gxPackGeometry` is the part that would move into it.

Census for reference (`tools/gamecube/dffcensus.cpp` over gta3.img): 4617 DFFs,
6510 geometries, 2.82M vertices, 2.21M triangles. 25% carry normals, 76% prelit,
**none** have a second UV set. 87.2MB of RW arrays if everything were resident at
once, of which triangles[] were 16.9MB.

## Ruled out or corrected this session

- **Normals are not dead weight.** `lightMask` is forced to `GX_LIGHTNULL` so
  `hwLights` never turns on and `GX_VA_NRM` never reaches the GP — which looks
  like 12 bytes a vertex for nothing. They are read on the CPU, for the per-vertex
  `N·L` that produces the vertex colour. Freeing them would unlight every ped and
  vehicle. (The world has no normals at all, which is why it takes the flat path.)
- **The indexed path and display-list caching are both off** (`GX_USE_INDEXED 0`,
  `GX_DL_BUDGET 0`), so every vertex goes through the FIFO as immediate-mode data
  every frame. `gxPackGeometry` halves the position and texcoord bytes on that
  path too, not just in RAM.
- **`ms_memoryUsed` is real bytes, not cd bytes.** `gResidentCost[]` holds the
  measured heap delta per stream entry, so shrinking a model's real allocation
  does reach the budget. Worth knowing before designing around it.

## How to work on this

**Measure before theorising.** Every real cause this session came from a
measurement and every detour from a plausible story: the deviceId bug fell out of
dumping 32 bytes of a converted TXD next to a stock one; the 20fps lock fell out of
a per-frame phase log; the LOD fallback fell out of a nodraw probe. Reading the
assets directly settles in one command what several boots of guessing cannot.

**Audit the consumers before changing a mechanism.** The duplicate vsync existed
because showRaster was changed without checking who called it. I repeated the same
mistake by moving dca3's evict-and-retry from its specific call sites into the
allocator, where it can fire mid-parse and destroy an object the caller still holds
— that hook is reverted and should be re-added at call sites if at all.

**An empty log is not a passing log.** `native.log` was empty for a whole boot and
that was read as "rejected nothing". It also means the reader never ran.
