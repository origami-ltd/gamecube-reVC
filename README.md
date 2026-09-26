# reVC — GameCube

A Nintendo GameCube port of Grand Theft Auto: Vice City, based on the
reverse-engineered engine from [mrxenginner/reVC](https://github.com/mrxenginner/reVC).

The port targets real GameCube hardware constraints: the heap is limited to
the console's 24 MB of MEM1, audio sample storage uses the 16 MB ARAM, and
MEM2 (Wii-only memory) is never used — including in the Wii development
build, which enforces the same limits.

## Status

Work in progress.

- The game boots and plays from an **SD card** (Wii homebrew loader, or
  Dolphin).
- Generating a mini-DVD **ISO that boots on a real GameCube does not work
  yet**. The ISO9660 path runs under Dolphin, but real-hardware disc boot is
  an open problem.

## Architecture

- **Renderer** — a native GX backend for librw
  (`vendor/librw/src/gx`). Textures are converted ahead of time to
  GameCube-native formats (CMPR / RGB5A3) at full original quality; memory
  pressure is handled by streaming and eviction, not by reducing asset
  quality. World geometry is quantised to packed int16 vertex streams,
  static meshes can be replayed as GP display lists, and lighting is
  implemented with TEV stages (prelight plus timecycle ambient, with
  optional env-map, rim-light and lightmap stages).
- **Audio** — streamed music, radio and speech are Ogg Vorbis, decoded with
  Tremor (fixed-point) on a dedicated thread so decoding never interrupts
  the game frame. Mixing uses AESND's 32 hardware voices. Mission speech
  (IMA ADPCM) is cached in ARAM. FMVs are decoded with Theora.
- **Filesystem and streaming** — an ISO9660 driver written for this port
  (`src/skel/gamecube/dvdfs.c`) plus libfat SD support, with sector-aligned
  DMA reads and a streaming layer tuned for the 24 MB memory budget.
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
streams and sample bank, Vorbis radio) and the movies (Theora):

- **GameCube** (MEM1 + ARAM): textures taller than 128 px at 75%, CI8
  palettes and a shared texel pool, laid out on a 1.46 GB mini-DVD image.
- **Wii** (MEM2, work in progress — boots, plays the movies and reaches the
  menu, then hangs loading the game): full-size textures on an SD card tree
  with `apps/reVC/boot.dol` for the Homebrew Channel.

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
Append `release` to the `docker run` line to also build the
(work-in-progress) Wii SD card tree. Mount real folders: a symlink inside
`assets/` that points outside the mount does not resolve in the container —
mount that folder on its own instead (`-v ~/GTAVC:/assets/GTAVC:ro`).

### Native build (Linux, macOS, Windows)

```bash
git clone --recursive https://github.com/origami-ltd/gamecube-reVC.git
cd gamecube-reVC
python3 build.py --setup    # CMake, Ninja and devkitPro for your OS
python3 build.py            # GameCube DOL -> build/cube/src/reVC.dol
python3 build.py wii        # Wii DOL (MEM2 on) -> build/wii/src/reVC.dol
python3 build.py iso        # GameCube -> build/release/reVC-GameCube.iso
python3 build.py sd         # Wii      -> build/release/reVC-Wii-SD/ (work in progress)
python3 build.py release    # both
```

The DOLs need Python 3, CMake ≥ 3.13, Ninja and devkitPro with the
`gamecube-dev` and `wii-dev` package groups; everything else they link is
in the repository (the librw fork with the GX backend, the xiph
submodules, a PowerPC libtheora under `vendor/portlibs/`). The ISO and SD
targets also need a host C++ compiler, `xorriso`, FFmpeg 8 or newer (older
FFmpeg breaks the movies: Debian 12's 5.1 drops Theora's duplicate-frame
packets when remuxing), SoX with Vorbis support, and libtheora 1.2.0's
`encoder_example` — or the pre-encoded movies in `assets/movies`.
`--game`, `--gamefiles`, `--movies` and `--out` override the default paths.
If devkitPro is installed somewhere non-standard, set `DEVKITPRO` to its
root.

- **macOS** — `brew install cmake ninja xorriso ffmpeg sox libogg libvorbis`,
  then install [devkitPro pacman](https://github.com/devkitPro/pacman/releases)
  (`.pkg` installer) and run `sudo dkp-pacman -Sy gamecube-dev wii-dev`.
- **Debian/Ubuntu** — `sudo apt-get install cmake ninja-build build-essential
  xorriso sox libsox-fmt-all libogg-dev libvorbis-dev curl xz-utils`, then run the
  [devkitPro pacman bootstrap](https://apt.devkitpro.org/install-devkitpro-pacman)
  and `sudo dkp-pacman -Sy gamecube-dev wii-dev`. If the distribution's
  `ffmpeg -version` is below 8, put a static
  [FFmpeg 8 build](https://github.com/BtbN/FFmpeg-Builds/releases) first on
  `PATH` (the Dockerfile does exactly this).
- **Arch Linux** — `sudo pacman -S cmake ninja base-devel xorriso ffmpeg sox
  libogg libvorbis`, add the
  [devkitPro repositories](https://devkitpro.org/wiki/devkitPro_pacman) to
  `/etc/pacman.conf` and `sudo pacman -Sy gamecube-dev wii-dev`.
- **Windows** — `winget install Kitware.CMake Ninja-build.Ninja
  Python.Python.3.12 Gyan.FFmpeg ChrisBagwell.SoX`, then run the
  [devkitPro installer](https://github.com/devkitPro/installer/releases)
  and select the GameCube and Wii development packages. `xorriso` and the
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

### GameCube (Dolphin)

Open `build/release/reVC-GameCube.iso` in Dolphin. Use DSP LLE with
`DSPThread = False` under `[Core]` in `Dolphin.ini`: DSP HLE on its own
thread can deliver the DSP interrupt before its mail and freeze the game.
Real-hardware disc boot is untested — see [Status](#status).

### Wii (Homebrew Channel or Dolphin) — work in progress

Copy the **contents** of `build/release/reVC-Wii-SD/` to the root of a FAT32
SD card (the card must contain `/models/gta3.img`, not
`/reVC-Wii-SD/models/gta3.img`) and launch reVC from the Homebrew Channel.
In Dolphin, enable SD card folder sync (`Config → Wii → SD Card Settings`)
targeting that folder and open `apps/reVC/boot.dol`.

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
