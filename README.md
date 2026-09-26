# reVC — GameCube

A Nintendo GameCube port of Grand Theft Auto: Vice City, based on the
reverse-engineered engine from [mrxenginner/reVC](https://github.com/mrxenginner/reVC).

The port runs from a GameCube mini-DVD ISO within the console's limits:
24 MB of MEM1 for the game and 16 MB of ARAM for texels, the sound bank,
audio read-ahead and a disc read cache.

## Status

Playable from the **GameCube ISO**, still a work in progress.

- The ISO boots through the movies, the menus and the intro into free roam.
  Sessions of more than 30 minutes run without a crash.
- 30 fps most of the time (94% of frames within 33 ms), dropping to about
  20 fps under load: rain, crashes, police chases. Loads triggered by
  mission scripts can stall for 1–3 s.
- Trade-offs for memory: motion blur is off, and with the heap near its
  limit a car occasionally fails to spawn.
- Not tested yet: missions beyond the opening, memory card saves and
  sessions of several hours.

## Architecture

- **Renderer** — a native GX backend for librw (`vendor/librw/src/gx`).
  Textures are converted ahead of time to GameCube formats (CMPR, RGB5A3,
  lossless CI8 palettes); textures shared between TXDs are stored once in a
  shared texel pool, and texels that do not fit in MEM1 live in an ARAM
  tier. Map, vehicle and pad geometry is pre-instanced as native GX meshes
  inside `gta3.img` (packed int16 vertex streams), static meshes are
  replayed as display lists, and lighting runs on TEV stages (prelight plus
  timecycle ambient, with optional env-map, rim-light and lightmap stages).
- **Audio** — the sound effect bank is IMA ADPCM held in ARAM. Cutscene and
  mission speech and pedestrian comments are IMA ADPCM streamed from the
  disc through an ARAM read-ahead ring; the radio is Ogg Vorbis decoded with
  Tremor (fixed-point) on its own thread, so decoding never stalls the game
  frame. AESND does the mixing. The movies are Theora.
- **Disc and streaming** — an ISO9660 driver written for this port
  (`src/skel/gamecube/dvdfs.c`) with sector-aligned DMA reads, `gta3.img`
  laid out in seek order (models next to their textures, the map in spatial
  order), an ARAM cache in front of the streaming reads and a streaming
  budget sized to what is left of MEM1. Audio reads go through their own
  channel so music never waits behind the world.
- **Frontend** — a GameCube controls page with a 3D controller model, and
  help boxes that display the port's actual button bindings as coloured
  GameCube button badges.

## Building

This repository contains no game assets. A legally owned copy of Grand
Theft Auto: Vice City is required. Put the game installation in
`assets/GTAVC` and reVC's `gamefiles` folder (TEXT, neo, models, data) in
`assets/gamefiles` (the [`assets/`](assets/) folder is git-ignored).
Pre-encoded `opening.ogv` and `titles.ogv` in `assets/movies` are optional;
without them the PC movies are encoded during the build.

The build converts every texture to GX-native formats, repacks `gta3.img`
with native map, vehicle and pad geometry, converts the audio (IMA ADPCM
streams and sample bank, Vorbis radio) and the movies (Theora), laid out on
a 1.46 GB mini-DVD image. Textures of 256 texels and up are halved (a power
of two, as the GX hardware needs for repeating textures) unless the half
size, magnified back the way the GPU draws it, scores a mean SSIM under
0.70 — thin structure such as mesh fences, lettering and grilles stays at
full size. Everything else is lossless: the PC's DXT1 blocks move to CMPR
bit for bit, 16-bit textures with few colours become CI8 palettes, and
textures shared between dictionaries are stored once.

### Docker (recommended)

Docker is the recommended way to build: every dependency resolves itself
inside the image (devkitPro, FFmpeg 8, SoX, xorriso and libtheora's movie
encoder) and nothing is installed on your system. It works the same on
Linux, macOS and Windows (Docker Desktop, colima or Docker Engine).

```bash
git clone --recursive https://github.com/origami-ltd/gamecube-reVC.git
cd gamecube-reVC
docker build -t revc .
docker run --rm -v "$PWD/assets":/assets:ro -v "$PWD/out":/out revc
```

`out/` receives `reVC-GameCube.iso` (about 6 minutes from a fresh install).
Mount real folders: a symlink inside
`assets/` that points outside the mount does not resolve in the container —
mount that folder on its own instead (`-v ~/GTAVC:/assets/GTAVC:ro`).

### Native build (Linux, macOS, Windows)

```bash
git clone --recursive https://github.com/origami-ltd/gamecube-reVC.git
cd gamecube-reVC
python3 build.py --setup    # CMake, Ninja and devkitPro for your OS
python3 build.py            # DOL -> build/cube/src/reVC.dol
python3 build.py iso        # ISO -> build/release/reVC-GameCube.iso
```

The DOL needs Python 3, CMake ≥ 3.13, Ninja and devkitPro with the
`gamecube-dev` package group; everything else it links is
in the repository (the librw fork with the GX backend, the xiph
submodules, a PowerPC libtheora under `vendor/portlibs/`). The ISO also
needs a host C++ compiler, `xorriso`, FFmpeg 8 or newer (older
FFmpeg breaks the movies: Debian 12's 5.1 drops Theora's duplicate-frame
packets when remuxing), SoX with Vorbis support, and libtheora 1.2.0's
`encoder_example` — or the pre-encoded movies in `assets/movies`.
`--game`, `--gamefiles`, `--movies` and `--out` override the default paths.
If devkitPro is installed somewhere non-standard, set `DEVKITPRO` to its
root.

- **macOS** — `brew install cmake ninja xorriso ffmpeg sox libogg libvorbis`,
  then install [devkitPro pacman](https://github.com/devkitPro/pacman/releases)
  (`.pkg` installer) and run `sudo dkp-pacman -Sy gamecube-dev`.
- **Debian/Ubuntu** — `sudo apt-get install cmake ninja-build build-essential
  xorriso sox libsox-fmt-all libogg-dev libvorbis-dev curl xz-utils`, then run the
  [devkitPro pacman bootstrap](https://apt.devkitpro.org/install-devkitpro-pacman)
  and `sudo dkp-pacman -Sy gamecube-dev`. If the distribution's
  `ffmpeg -version` is below 8, put a static
  [FFmpeg 8 build](https://github.com/BtbN/FFmpeg-Builds/releases) first on
  `PATH` (the Dockerfile does exactly this).
- **Arch Linux** — `sudo pacman -S cmake ninja base-devel xorriso ffmpeg sox
  libogg libvorbis`, add the
  [devkitPro repositories](https://devkitpro.org/wiki/devkitPro_pacman) to
  `/etc/pacman.conf` and `sudo pacman -Sy gamecube-dev`.
- **Windows** — `winget install Kitware.CMake Ninja-build.Ninja
  Python.Python.3.12 Gyan.FFmpeg ChrisBagwell.SoX`, then run the
  [devkitPro installer](https://github.com/devkitPro/installer/releases)
  and select the GameCube development packages. `xorriso` and the
  movie encoder are not packaged for Windows: install
  [MSYS2](https://www.msys2.org/), `pacman -S xorriso` in its shell and add
  its `usr\bin` to `PATH`; build the encoder below in MSYS2 with a compiler
  and the libogg/libvorbis development packages, or use pre-encoded movies.
  Building inside WSL2 with the Debian steps avoids all of this.

libtheora's `encoder_example` (all systems; on Windows in the MSYS2 shell):

```bash
curl -fsSL https://downloads.xiph.org/releases/theora/libtheora-1.2.0.tar.xz | tar xJ
cd libtheora-1.2.0
./configure --disable-shared --disable-doc --disable-spec --disable-oggtest \
            --disable-vorbistest --disable-sdltest
make -C lib && make -C examples encoder_example
export THEORA_ENCODER_EXAMPLE="$PWD/examples/encoder_example"
```

Tested end to end: Docker (from a fresh install, movies encoded in the
container) and macOS natively (with pre-encoded movies). The native Linux
and Windows steps follow the same requirements but have not been run.

## Running

Open `build/release/reVC-GameCube.iso` in Dolphin. Use DSP LLE with
`DSPThread = False` under `[Core]` in `Dolphin.ini`: DSP HLE on its own
thread can deliver the DSP interrupt before its mail and freeze the game.
On a console, load the ISO with Swiss or an optical drive emulator.

## Credits

- [mrxenginner/reVC](https://github.com/mrxenginner/reVC) — the
  reverse-engineered Vice City engine this port is based on.
- [librw](https://github.com/aap/librw) by aap — the RenderWare
  reimplementation. [This port's fork](https://github.com/origami-ltd/gamecube-librw)
  adds the GameCube GX backend.
- [dca3](https://gitlab.com/skmp/dca3-game) by skmp and contributors — the
  Dreamcast GTA III port. Specific derivations:
  `tools/gamecube/repack_img.py` is modelled on dca3's imgtool;
  `tools/gamecube/txdconv.cpp` follows its ahead-of-time native texture
  conversion; `tools/gamecube/dffcensus.cpp` reproduces its packed
  native-geometry cost analysis; the pre-instanced static DFF format and
  allocation strategies in `vendor/librw/src/gx/gxraster.cpp` follow
  conclusions established by dca3.
- [Polyphase Engine](https://github.com/Polyphase-Labs/Polyphase-Engine) —
  reference for the GX channel and TEV configuration in
  `vendor/librw/src/gx/gx.cpp`, credited inline where used.
- [GameCube controller 3D model](https://sketchfab.com/3d-models/gamecube-controller-21983501bac64993ac09cdc7936ffdf2)
  by Cory Richards, licensed
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Converted to
  RenderWare format in `tools/gamecube/assets/`, licence file included.
- [Xiph.Org](https://xiph.org/) — ogg, opus, opusfile, Tremor and theora.
- [devkitPro](https://devkitpro.org/) — devkitPPC, libogc and AESND.

## License

The port's original contributions are licensed under the
[MIT License with Proof-of-Usage Condition (MIT-PoU)](LICENSE.md). Upstream
components keep their original licenses: librw is MIT (aap), the xiph
libraries are BSD, and code inherited from reVC remains under its upstream
terms.
