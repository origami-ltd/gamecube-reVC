# reVC on GameCube — next session

Two priorities, in this order:

1. **The pause menu freezes**, on opening or on closing.
2. **Music, VFX and audio at 32kHz stereo**, and on the card.

Everything else — 60fps in dense scenes, the missing effects — waits.

- Source: `~/Documents/GitHub/reVC-gamecube`
- Build (the .dol Dolphin runs): `build/wii` → `src/reVC.dol`
- Tools and the Dolphin harness: `tools/gamecube/`

```bash
cd ~/Documents/GitHub/reVC-gamecube/build/wii && ninja
cd <a scratch dir> && ~/Documents/GitHub/reVC-gamecube/tools/gamecube/boot.sh
```

Reconfiguring needs `CMAKE_MODULE_PATH` explicitly, or CMake never finds
`Platform/NintendoWii.cmake`, `NINTENDO_OGC` stays unset, and the configure dies
on `Illegal REVC_AUDIO`:

```bash
cmake -S . -B build/wii -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/Users/ebellumat/dkp-wii/cmake/Wii.cmake \
  -DCMAKE_MODULE_PATH=/Users/ebellumat/dkp-wii/cmake \
  -DCMAKE_BUILD_TYPE=Release -DREVC_VENDORED_LIBRW=ON -DREVC_AUDIO=GC \
  -DLIBRW_PLATFORM=GAMECUBE -DLIBRW_TOOLS=OFF -DLIBRW_INSTALL=OFF
```

---

## 1. The menu freeze

Press Start and the game stops: frozen image, no exception, no crash.log, CPU
at 1-6% because the main thread is blocked rather than spinning.

**Read this before forming a hypothesis. All of it is ruled out by
measurement, and each one cost boots:**

- **Not the GP.** `GX_GetGPStatus` from the watchdog reported **`r1c1`** — read
  idle and command idle, so the GP had finished and was waiting. Four earlier
  hypotheses were aimed at a GP stall. Note also that the bounded draw-sync
  wait in `showRaster` is too late to catch one by construction: when the GP
  stalls the FIFO fills and the CPU parks in whichever GX call comes next,
  `GX_CopyDisp`, long before `GX_DrawDone`.
- **Not vsync.** An earlier revision concluded the retrace stall was what let
  the menu open; that came from comparing builds differing in several things.
  Tested directly afterwards: it freezes with the stall on too.
- **Not the filesystem lock.** `CdStream_gamecube.cpp` serialises every libfat
  user behind one recursive mutex, including all five CFileMgr primitives. The
  freeze survived it.
- **Not the periodic SD logging.** `hb.log`/`alloc.log` are off by default
  (`gLogToSd` in main.cpp). The freeze survived that too.
- **Not the unconverted loose TXDs.** They are converted on the card now
  (backup at `WiiSD.raw.bak-preconvert`) and it still freezes.

**What the evidence does say.** The thread sits inside `DoRWStuffEndOfFrame`
while the watchdog's own `fopen` never lands — even unguarded, even with all
other SD writing disabled — and `GeckoLog` (EXI, a separate transport) gets
through every single time. newlib serialises stdio globally, so a main thread
stuck inside a file call explains every observation at once.

**The strongest lead came from the user, not from the code:** the game's own
Save menu opens and closes cleanly; only the pause menu freezes.
`CMenuManager::LoadAllTextures` (Frontend.cpp) is what separates them — it
reserves 716KB with `MakeSpaceFor(350 sectors)` then loads `fronten1.txd` and
friends. The Save menu loads none of that.

**One experiment succeeded and then regressed.** Making `SaveSettings` a no-op
produced a clean open-and-close cycle, once. That pointed at the write path, so
the last commit fixed the write path properly rather than skipping it:
`CFileMgr` now buffers writers and commits once on close, because `SaveSettings`
was issuing about forty tiny writes and each is a read-modify-write of a FAT
sector. **The user reports it froze on opening again after that change, so the
write path is not the whole story — or not the story at all.** Treat the
SaveSettings result as one data point, not a solved cause.

Corroborating detail worth keeping in mind: `boot.sh` has had to run
`fsck_msdos` on the card before every boot for the life of this project. That is
what damage from small scattered FAT writes looks like, and it was read as a
symptom of the freeze for a long time.

### The instrument, and how it lied before

The watchdog thread reports over Gecko in short lines, GP status first because
Gecko drops what it cannot drain:

```
HANG r<rdIdle>c<cmdIdle> s<state>
at <phase>
```

Phase markers now exist inside the frontend too — `menu-loadtex`,
`menu-unloadtex`, `menu-process`, `menu-draw`, `menu-switch` — so the next
freeze should name the step rather than the frame.

**The frame-phase markers were broken for most of this project**: set on
entering a phase and never cleared, so every hang reported `endofframe` because
that is the last marker `Idle` sets. Any conclusion in an older log resting on
`phase=endofframe` is worth nothing. They close with `after-<name>` now.

### Things not yet tried

- **Done, unread:** `LoadAllTextures` is now instrumented step by step —
  `menu-space1`, `menu-fronten1`, `menu-usedmem`, `menu-space2`,
  `menu-fronten2`. The next freeze on opening should name one of them, and
  which one decides the fix: a stall in `MakeSpaceFor` is the streamer evicting
  under a 716KB reservation, a stall in `LoadTxd` is the file path.
- Check whether `MakeSpaceFor(350 sectors)` before the load is enough, given
  `d3d8::readNativeTexture` keeps a D3D raster, a full RGBA8 Image and a
  staging buffer live per texture. The card's dictionaries are converted now,
  which should have removed that path — verify it actually did rather than
  assuming.
- `AR_Alloc` is a stack allocator and `UnloadSampleBank` cannot release out of
  order; check nothing in the menu path is exhausting ARAM.

---

## 2. Audio at 32kHz stereo

**The user's decision: music, VFX and audio all at 32kHz stereo.** `DIGITALRATE`
in sampman.h is already 32000, so the engine's own mixer rate agrees.

What that costs, measured, so it is a choice and not a surprise: over
`sfx.sdt`, 9941 samples, **81% at 12kHz**, 13% at 16kHz, eleven at 32kHz, all
mono, 324.5MB as PCM. Resampling them to 32kHz stereo multiplies the bank
roughly fivefold. The disc has room (the card totals about 1352MB of 1500MB
with everything else), but it is upsampling — the fidelity is not in the input.

Radio is Vorbis, decided by arithmetic: 32kHz stereo in ADPCM is 1060MB and the
card would total 1608MB against a 1536MB disc. In Vorbis the same 32kHz stereo
is 504MB.

### What is already built

`src/audio/sampman_gamecube.cpp`, `REVC_AUDIO=GC`, replacing the 47 empty
methods of `sampman_null.cpp`:

- AESND voices, one per channel, with the voice callback clearing a per-channel
  flag because AESND has no way to ask whether a voice is still sounding.
- Sample banks DMA'd from disc into ARAM; a sample crosses to MEM1 only when a
  voice starts. 324MB of samples never sit in a 16MB arena.
- Three streaming voices, double-buffered, pumped from `Service()`.
- A 4MB ARAM ring in front of the radio: 250 seconds of lookahead at 128kbps,
  so the disc is read once every four minutes per stream instead of every few
  frames. This matters on a Mini-DVD, where the seek is the expensive part.
- Tremor (`ppc-libvorbisidec`, installed) decoding Vorbis out of that ring.
- Radio files resolve through the game's own `StreamedNameTable`, not a second
  numbering that would drift from the enum.

**Correction worth carrying:** libogc's AESND takes PCM only. The DSP's hardware
ADPCM decode lives in Nintendo's AX microcode, which libogc does not ship, so
ADPCM would be a CPU decode. An earlier session called it free; it is not.

### What is left

**The card has no audio on it.** `~/revc-sd` has no audio directory, so nothing
can play regardless of the backend. Two commands from the repository root:

```bash
python3 tools/gamecube/convert_audio.py ~/GTAVC/audio ~/revc-audio-ogg
python3 tools/gamecube/build_sd.py --game ~/GTAVC --out ~/revc-sd \
    --audio ~/revc-audio-ogg --txdconv /tmp/txdconv
```

`convert_audio.py` currently keeps each file at its source rate. For the 32kHz
stereo decision it needs a resample step — sox can do it in the same pipe
(`rate 32000` and `channels 2`).

`build_sd.py` also converts the loose texture dictionaries, which
`repack_img.py` never touched. `frontend_ds2.txd` fails to convert (the host
librw cannot read it); those are DualShock 2 button icons.

---

## Held for later

**60fps in dense scenes.** `work` is 11-16ms in a quiet street and was 17-20ms
on Ocean Drive. Measured with the GP idle (`gp0`), so it is CPU-bound.

The profiler says where, and it is not where anyone guessed:
**`sky96(sz0 cl0 cd0)`** — 9.6ms in `DoRWStuffStartOfFrame_Horizon`, and *none*
of it in `CameraSize`, `RwCameraClear` or `CClouds::RenderBackground`. It is in
`RsCameraBeginUpdate`, the only call between them that is not timed. Clearing
should be nearly free on GX, since it happens during the EFB→XFB copy. Time
that call before optimising anything else.

`GX_USE_INDEXED` is written, gated and left at 0 deliberately: it moves work
from the CPU to the GP, and would touch `rnd` (7.5ms), not `sky`.

**Missing effects.** No water spray at a hydrant, and the user reports every
additional effect missing. One real cause was found and fixed — the GX backend
blended only on `stVertexAlpha` while gl3 and d3d9 blend on
`vertexAlpha || textureAlpha` — but that does not explain the hydrant, because
`Particle.cpp:1796` already sets vertex alpha. One shared cause is more likely
than several.

A wrong turn not to repeat: the loose PC dictionaries are **not** silently
rejected. `d3d8.cpp` is compiled into this build, so `Texture::streamReadNative`
dispatches them to `d3d8::readNativeTexture` and they load. `gx::readNativeTexture`
only rejects textures claiming to be GameCube. The HUD rendering on screen was
the evidence against that conclusion and should have outweighed the code
reading.

---

## The HUD

One block, one string:

```
60 f16.6 max20 work16 oom0/0 m12902K tex150K fr3312K blk11519 str0 ar0%
 | sim0 rnd84 sky96(sz0 cl0 cd0) fx8 hud2 lit0 str0 cpy0 gp0 vs0 oth0
```

`oom<geo>/<tex>` — allocation failures split; the texture half was invisible
until recently, which made "black silhouettes with oom 0" look like a lighting
bug. `ar%` is the ARAM disc-cache hit rate. `oth` is the residual: frame period
minus everything instrumented, so a large one means the cost is somewhere
nothing measures yet. L + A held three seconds toggles the boot console.

## The streaming budget is a cliff on both sides

`reserve` in `CStreaming::Init`; `ms_memoryAvailable = arena - reserve`, arena
about 16.4MB. **Reserve is the leftover, budget is what the streamer gets — do
not restate one as the other.** A revision that turned "reserve = 2MB" into
"budget = 3MB" cut resident world to a fifth and stopped the picture.

Too large a reserve and the streamer evicts: far LOD shells, characters without
textures. Too small and it fills the arena until an allocation fails silently
mid-cutscene — measured at 616K free with a 404ms spike, then a stop. Measured
either side in the intro cutscene, 2MB gave 616K free and 6MB gave 908K: four
megabytes of reserve bought under 300K, because there the cutscene's own working
set dominates.

Demand side counts too. Draw distance at 1.8 (the top of the Options slider)
brings the LOD flicker straight back. It is at the engine default of 1.2.

## How to work on this

**Measure before theorising, and measure the thing you are about to change.**
The expensive failures were all one shape: a number measured in one scene and
generalised. Free bytes measured standing in an alley said there was room to
spare, the reserve was cut on that basis, and the intro cutscene froze.

**Change one thing per boot.** Several conclusions in this project came from
comparing builds that differed in more than one way. All of them were wrong.

**An empty log is not a passing log.** `native.log` empty was read as "nothing
rejected" when it meant the reader had not run. The phase markers were the same
trap: `endofframe` was not where the hang was, it was the last marker set.

**Trust the screen over the code reading.** The conclusion that loose TXDs were
rejected came from reading `gx::readNativeTexture` and stopping before the
dispatch that never calls it. The HUD was on screen the whole time.

**Instrumentation must not depend on what it is instrumenting.** The hang
reporter wrote to the SD and produced nothing, because the thing it was
reporting on had wedged the filesystem. Gecko is independent and works.

**Ask the user what they saw.** Two of the three real breakthroughs this
session came from the user's observations, not from the code: that the Save
menu works while the pause menu does not, and that two debug HUDs were
overlapping. Neither was visible from here.
