# reVC on GameCube — where this stands

Vice City (reVC) on GameCube hardware, run in Dolphin as a Wii DOL. The goal is
the game at 480p and 60fps with its effects on.

- Source: `~/Documents/GitHub/reVC-gamecube`
- Build (the .dol Dolphin runs): `build/wii` → `src/reVC.dol`
- Host tools and the Dolphin harness: `tools/gamecube/`
- dca3 reference: `~/Documents/GitHub/dca3-game`

```bash
cd /Users/ebellumat/Documents/GitHub/reVC-gamecube/build/wii && ninja
cd <a scratch dir> && /Users/ebellumat/Documents/GitHub/reVC-gamecube/tools/gamecube/boot.sh
```

Reconfiguring from scratch needs `CMAKE_MODULE_PATH` explicitly, or CMake never
finds `Platform/NintendoWii.cmake`, `NINTENDO_OGC` stays unset and the configure
dies on `Illegal REVC_AUDIO`:

```bash
cmake -S . -B build/wii -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/Users/ebellumat/dkp-wii/cmake/Wii.cmake \
  -DCMAKE_MODULE_PATH=/Users/ebellumat/dkp-wii/cmake \
  -DCMAKE_BUILD_TYPE=Release -DREVC_VENDORED_LIBRW=ON -DREVC_AUDIO=GC \
  -DLIBRW_PLATFORM=GAMECUBE -DLIBRW_TOOLS=OFF -DLIBRW_INSTALL=OFF
```

## The three open problems, in the order they block each other

**1. The pause menu freezes.** Press Start and the game stops: frozen image, no
exception, no crash.log, CPU at 1-6% because the main thread is blocked rather
than spinning. The in-game Save menu, which the game opens itself, works
fine — enters and exits. That difference is the best lead anyone has produced:
`CMenuManager::LoadAllTextures` (Frontend.cpp) reserves 716KB with
`MakeSpaceFor(350 sectors)` and then loads `fronten1.txd` and friends; the Save
menu loads none of that.

Everything below has been ruled out by measurement, so do not spend a boot on
any of it again:

- **Not the GP.** A bounded draw-sync wait replaced `GX_DrawDone` and never
  fired. Note the wait is also too late by construction: when the GP stalls the
  FIFO fills and the CPU parks in whichever GX call runs next — `GX_CopyDisp` —
  long before `GX_DrawDone`. Ask `GX_GetGPStatus` from the watchdog instead.
- **Not vsync.** An earlier revision concluded the retrace stall was what let
  the menu open. That came from comparing builds that differed in several
  things at once, which is not a measurement. Tested directly: it freezes with
  the stall on too.
- **Not the filesystem lock.** `CdStream_gamecube.cpp` now serialises every
  libfat user behind one recursive mutex, including all of CFileMgr's five
  primitives. The freeze survived it.
- **Not the periodic SD logging.** `hb.log`/`alloc.log` writes are off by
  default now (`gLogToSd`). The freeze survived that too.
- **Not the unconverted loose TXDs.** They were converted on the card and it
  still freezes.

What is left and fits every observation: **the main thread is stuck inside
stdio**. newlib serialises stdio globally, which is why the watchdog's own
`fopen("dvd:/hang.log")` comes back empty even unguarded, while `GeckoLog` (EXI,
independent) gets through every time. Three runs, same pattern.

**The instrument to use, and a warning about it.** The watchdog thread reports
over Gecko in two short lines:

```
HANG@<phase>
H s<state> m<menu> <gxpath> v<vsync> r<rdIdle>c<cmdIdle>
```

`r1c1` means the GP is idle and the CPU is stuck elsewhere. `r0c0` means the GP
stopped. That single distinction has never been read, and it decides the whole
question.

The phase markers were **broken until the last commit**: they were set on
entering a phase and never cleared, so every hang reported `endofframe` because
that is the last marker `Idle` sets. Four rounds of hypotheses were aimed at a
value that could not have said anything else. They close with `after-<name>`
now. Any conclusion in an older log that rests on `phase=endofframe` is worth
nothing.

**2. Effects are missing.** No water spray when driving over a hydrant, and the
mission markers looked opaque. One cause was found and fixed: the GX backend
enabled blending only on `stVertexAlpha`, while gl3 and d3d9 both blend on
`vertexAlpha || textureAlpha` — so anything transparent by virtue of its
texture's alpha drew opaque. `gxRasterHasAlpha` already existed; nothing read
it.

That does **not** explain the hydrant. `Particle.cpp:1796` already sets
`rwRENDERSTATEVERTEXALPHAENABLE`, so particles always had blending. The cause is
still unknown, and the user reports every additional effect missing, not just
water — which suggests one shared cause rather than several.

A wrong turn worth not repeating: the loose PC texture dictionaries are **not**
silently rejected. `d3d8.cpp` is compiled into the GameCube build, so
`Texture::streamReadNative` dispatches PC dictionaries to `d3d8::readNativeTexture`
and they load. `gx::readNativeTexture` only rejects textures claiming to be
GameCube. The HUD rendering on screen was the evidence against that conclusion
and should have outweighed the code reading.

**3. 60fps holds in a quiet street and not in a busy one.** Measured `work` is
11-15ms in a closed alley and was 17-20ms on Ocean Drive with pedestrians and
vegetation — over the 16.6ms retrace, so the frame fell to the next one and
locked at exactly 33.3ms.

Three CPU reductions landed after that measurement and **none of them have been
measured**: the integer vertex colour (worth 6ms on its own when it landed),
int16 positions and texcoords halving the write-gather traffic, and the cached
vertex colour now used on the immediate path too. Re-measure before optimising
anything else.

The next lever is chosen by two numbers on the profiler line, not by intuition:
`oth` is the residual — frame period minus everything instrumented — and a
large one means the cost is somewhere nothing measures yet. `gp` says whether
the GP is the bottleneck, in which case moving work onto it makes things worse.
`GX_USE_INDEXED` is written and gated but left at 0 for exactly that reason.

## What the HUD tells you

```
60 f16.6 max20 work16 oom0/0 m12902K tex150K fr3312K blk11519 str0 ar0%
sim.. rnd.. sky.. fx.. hud.. lit.. tile0 str6 cpy0 gp0 vs0 oth0
```

- `oom<geo>/<tex>` — allocation failures, split. The texture half was invisible
  until recently, which is why "black silhouettes with oom 0" looked like a
  lighting bug: geometry loaded, the texture allocation failed silently,
  `gxGetTexture` returned nil and the mesh drew untextured.
- `tex<KB>` — live tiled-texture bytes.
- `ar%` — ARAM disc-cache hit rate.
- second line — per-phase times in tenths of a millisecond, `oth` last.

L + A held for three seconds toggles the boot console (the full-screen printf
wall), not the readout.

## The streaming budget is a cliff on both sides

`reserve` in `CStreaming::Init`, and `ms_memoryAvailable = arena - reserve`.
Arena is about 16.4MB. **Reserve is the leftover, budget is what the streamer
gets; do not restate one as the other.** A revision that changed "reserve = 2MB"
into "budget = 3MB" cut resident world to a fifth and stopped the picture.

- Too large a reserve → smaller budget → the streamer evicts → far LOD shells
  and characters without textures.
- Too small → the streamer fills the arena and an allocation fails silently
  mid-cutscene. Measured: free bytes at 616K with a 404ms spike, then a stop.

Measured either side in the intro cutscene: 2MB reserve gave 616K free, 6MB gave
908K. Four megabytes of reserve bought under 300K of real headroom, because in
that scene the cutscene's own working set dominates, not the streamer.

The window is narrow because `gResidentCost` is an estimate. One systematic
error was found and fixed — texture buffers allocated outside a load's
measurement window were charged to nobody, so `ms_memoryUsed` always read low —
but tuning this number is guesswork until the accounting is exact or failure is
loud.

Demand side matters too: draw distance at 1.8 (the top of the Options slider)
brings the LOD flicker straight back, because it asks for more world than the
budget holds. It is at the engine default of 1.2.

## What landed, with the measurement that justified it

- **480p**, forced. `VIDEO_HaveComponentCable()` is unusable under Dolphin —
  measured `cbl=0` regardless of its setting, the SYSCONF byte is rewritten on
  launch, and `ProgressiveScan` is a per-game INI key a loose .dol cannot match.
  `GX_FORCE_PROGRESSIVE` decides instead. Verified `vi=2`. **Set it to 0 for a
  console on composite.**
- **Quantised vertices.** int16 positions and texcoords with a per-geometry
  shift the GP applies in hardware; the float arrays are freed. 20 bytes a
  vertex become 10. Free bytes 2890K → 5157K, same scene, same reserve.
  `tools/gamecube/packtest.c` pins the quantiser's contract and caught two
  off-by-ones in it.
- **Integer vertex colour.** Gekko has no direct path between float and integer
  registers, so every `(u8)(float)` is a store and a reload; the float version
  paid eight per vertex. `work17 → work11`.
- **ARAM.** A disc cache under `CdStreamRead` keyed by the (offset, size) the
  streamer already asks for, and a 4MB ring in front of the radio — 250 seconds
  of lookahead at 128kbps, so the disc is read once every four minutes per
  stream instead of every few frames. It does not reduce resident memory; it
  makes being wrong cheap, which is what lets a smaller resident set stop
  costing image quality.
- **Audio, from nothing.** `REVC_AUDIO=NULL` compiled 47 empty methods.
  `sampman_gamecube.cpp` is AESND voices, sample banks in ARAM, three streaming
  voices, and Tremor decoding Vorbis out of the ARAM ring. Radio files resolve
  through the game's own `StreamedNameTable` rather than a second numbering.
- **325KB** of unwind tables. `-fno-asynchronous-unwind-tables` is the flag that
  matters on PowerPC, and it has to be in the top-level CMakeLists before
  `add_subdirectory(vendor/librw)` or most of `.eh_frame` survives.

## Audio: what is decided and what is left

Vorbis for the radio, because the disc arithmetic decided it: 32kHz stereo in
ADPCM is 1060MB and with PC textures intact the card totals 1608MB against a
1536MB disc. In Vorbis the same 32kHz stereo is 504MB and the card comes to
about 1065MB.

SFX stay as PCM at their authored rates, and that is deliberate. Measured over
`sfx.sdt`: 9941 samples, 81% at 12kHz, 13% at 16kHz, eleven at 32kHz.
Resampling them up would multiply the bank for fidelity that was never
recorded, and AESND plays PCM natively at zero CPU cost — converting to ADPCM
would trade disc space that is spare for CPU time that is not.

**The card has no audio on it yet.** `~/revc-sd` carries no audio directory. Two
commands, from the repository root:

```bash
python3 tools/gamecube/convert_audio.py ~/GTAVC/audio ~/revc-audio-ogg
python3 tools/gamecube/build_sd.py --game ~/GTAVC --out ~/revc-sd \
    --audio ~/revc-audio-ogg --txdconv /tmp/txdconv
```

`build_sd.py` also converts the loose texture dictionaries, which
`repack_img.py` never touched. They were shipping as PC dictionaries: they load,
but through the D3D8 path, which keeps a D3D raster, a full RGBA8 Image and a
staging buffer live per texture — the churn `repack_img.py` documents as what
shatters the heap. Converting them is 18.3MB → 14.8MB and removes that churn.
`frontend_ds2.txd` fails to convert (the host librw cannot read it); it is the
DualShock 2 button icons.

`tools/gamecube/unused_textures.txt` lists 234 TXDs (6.7MB) that appear in no
.ide after removing those loaded by name from code, by cutscene, or from an
.ipl. It is a hypothesis to test by looking for missing textures, not a fact:
a name can be assembled at runtime and no static scan sees that.

## How to work on this

**Measure before theorising, and measure the thing you are about to change.**
Every real cause this session came from a measurement and every detour from a
plausible story. The costly failures were all the same shape: a value measured
in one scene and generalised. Free bytes measured standing in an alley said
there was room to spare, the reserve was cut to 2MB on that basis, and the
intro cutscene froze — its working set is far larger and nobody had looked.

**Change one thing per boot.** Several conclusions in this project's history
came from comparing builds that differed in more than one way. They were all
wrong, and each cost several boots to unwind.

**An empty log is not a passing log.** `native.log` being empty was read as
"nothing was rejected" when it meant the reader had never run. The same trap
caught the phase markers: `endofframe` was not where the hang was, it was the
last marker set.

**Trust the screen over the code reading.** The conclusion that loose TXDs were
being rejected was reached by reading `gx::readNativeTexture` and stopping
before the dispatch that never calls it. The HUD was on screen the whole time,
which was proof the dictionaries loaded.

**Instrumentation must not depend on what it is instrumenting.** The hang
reporter wrote to the SD and produced nothing, because the thing it was
reporting on had wedged the filesystem. Gecko is a separate transport and works.
