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

```bash
git clone --recursive https://github.com/origami-ltd/gamecube-reVC.git
cd gamecube-reVC
python3 build.py --setup    # installs the dependencies for your OS
python3 build.py            # GameCube DOL -> build/cube/src/reVC.dol
python3 build.py wii        # Wii dev DOL  -> build/wii/src/reVC.dol
```

The same commands work on macOS, Linux and Windows. `--setup` uses the
system package manager (Homebrew, apt, pacman or winget) for CMake and
Ninja, then installs [devkitPro](https://devkitpro.org/wiki/Getting_Started)
with the `gamecube-dev` and `wii-dev` package groups. All other build
dependencies are included in the repository (the librw fork with the GX
backend, the xiph ogg/opus/opusfile submodules, and a PowerPC libtheora
build with a Wii toolchain file under `vendor/portlibs/`).

### Installing the dependencies manually

Requirements: Python 3, CMake ≥ 3.13, Ninja, and devkitPro with the
GameCube/Wii toolchains.

- **macOS** — `brew install cmake ninja`, then install
  [devkitPro pacman](https://github.com/devkitPro/pacman/releases)
  (`.pkg` installer) and run
  `sudo dkp-pacman -Sy gamecube-dev wii-dev`.
- **Debian/Ubuntu** — `sudo apt-get install cmake ninja-build`, then run the
  [devkitPro pacman bootstrap](https://apt.devkitpro.org/install-devkitpro-pacman)
  and `sudo dkp-pacman -Sy gamecube-dev wii-dev`.
- **Arch Linux** — `sudo pacman -S cmake ninja`, add the
  [devkitPro repositories](https://devkitpro.org/wiki/devkitPro_pacman) to
  `/etc/pacman.conf` and `sudo pacman -Sy gamecube-dev wii-dev`.
- **Windows** — `winget install Kitware.CMake Ninja-build.Ninja`, then run
  the [devkitPro installer](https://github.com/devkitPro/installer/releases)
  and select the GameCube and Wii development packages.

If devkitPro is installed somewhere non-standard, set the `DEVKITPRO`
environment variable to its root.

## Game data

This repository contains no game assets. A legally owned copy of Grand
Theft Auto: Vice City is required.

Copy the game installation into the git-ignored [`assets/`](assets/) folder
and generate the SD card tree with `tools/gamecube/build_sd.py` (see
`--help` for arguments). The script converts textures to GX-native formats,
repacks `gta3.img` and produces the card layout the game reads.

## Running

### Dolphin

1. Build the SD card tree (see [Game data](#game-data)).
2. Point Dolphin's Wii SD card at it: `Config → Wii → SD Card Settings`,
   then either set the SD card image to one containing the tree, or enable
   folder sync targeting the tree.
3. Open `build/wii/src/reVC.dol` in Dolphin.

### Wii (Homebrew Channel)

1. Copy the built SD card tree to the root of a FAT32 SD card.
2. Copy `build/wii/src/reVC.dol` to the card as `apps/reVC/boot.dol`.
3. Launch reVC from the Homebrew Channel.

### GameCube

Real-hardware disc boot is not functional yet — see [Status](#status).

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
