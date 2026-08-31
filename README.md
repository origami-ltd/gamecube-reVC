# reVC — GameCube

A port of Grand Theft Auto: Vice City to the **Nintendo GameCube**, built on
the reVC reverse-engineered engine. It is a real GameCube game, not a "runs
on Wii hardware" shortcut: the heap is clamped to the console's **24MB MEM1**,
audio sample storage lives in the **16MB ARAM**, and nothing ever touches the
Wii's MEM2 — even the Wii development build enforces the same limits, so what
runs in the dev loop is what real hardware gets.

> **Work in progress.** The game currently boots and plays **via SD card**
> (Wii homebrew loader or Dolphin). Generating a mini-DVD **ISO that boots on
> a real GameCube is still giving us trouble** — the ISO9660 path works under
> Dolphin but real-hardware boot is not there yet. Contributions welcome.

## What's inside (architecture)

- **Renderer** — a native **GX backend for librw** (`vendor/librw/src/gx`).
  Textures are converted ahead of time to GameCube-native formats (CMPR /
  RGB5A3) at **full original quality — no downscaling, ever**; memory
  pressure is handled by eviction and streaming, never by degrading assets.
  World geometry is quantised to packed int16 vertex streams, static meshes
  can replay as GP display lists, and lighting runs through hand-built TEV
  stages (prelight + timecycle ambient, neo env/rim/lightmap extras).
- **Audio** — streams are **Ogg Vorbis decoded with Tremor** (fixed-point,
  console-friendly) on a **dedicated decode thread** so a decode chunk never
  bites the game frame; a starved voice plays silence instead of replaying
  stale buffers. Mixing is AESND's 32 hardware-fed voices (28 generic + a
  reserved police-radio voice + streams); mission speech (IMA ADPCM) is
  cached in **ARAM** through a 16MB shim — the only Arena2 use in the port.
  FMVs decode through Theora at native rate.
- **Filesystem/streaming** — a from-scratch ISO9660 driver (`dvdfs`) plus
  libfat SD support, one mutex per mount, sector-aligned DMA reads, and a
  streaming layer tuned against the 24MB wall (LRU model eviction, no-refade
  snaps so eviction churn is invisible).
- **Frontend** — GameCube-native controls page with a real 3D controller
  model, per-action button badges in the physical button colours, and help
  boxes that name the buttons this port actually binds.

## Building

Dependencies you install once:

1. **[devkitPro](https://devkitpro.org/wiki/Getting_Started)** with the
   GameCube/Wii packages (`gamecube-dev` / `wii-dev` groups): devkitPPC,
   libogc, cmake support files.
2. **CMake ≥ 3.13**, **Ninja** and **Python 3** (devkitPro's installer can
   provide cmake/ninja; any system install works too).

Everything else is vendored in this repository (librw with the GX backend,
xiph ogg/opus/opusfile submodules, a PowerPC libtheora build plus the Wii
toolchain shim under `vendor/portlibs/`).

```bash
git clone --recursive https://github.com/origami-ltd/gamecube-reVC.git
cd gamecube-reVC
python3 build.py            # GameCube DOL -> build/cube/src/reVC.dol
python3 build.py wii        # Wii dev DOL  -> build/wii/src/reVC.dol
```

The same command works on macOS, Linux and Windows.

### Game data

You must own Grand Theft Auto: Vice City. Copy your installation into the
git-ignored **`assets/`** folder and build the SD card tree with
`tools/gamecube/build_sd.py` (it converts every texture to GX-native format,
repacks `gta3.img`, and lays out the card the game expects — run it with
`--help` for the arguments). **No Rockstar data ships in this repository**,
and `assets/` is ignored so none can slip in.

## Credits

- **[reVC / re3](https://github.com/GTAmodding/re3)** — the reverse-engineered
  engine this port stands on.
- **[librw](https://github.com/aap/librw)** by aap — the RenderWare
  reimplementation; our fork adds the GameCube GX backend.
- **[dca3](https://gitlab.com/skmp/dca3-game)** by skmp and contributors —
  the Dreamcast GTA III port whose approach corrected this port's direction.
  Concrete debts: `tools/gamecube/repack_img.py` is modelled on dca3's
  imgtool; `tools/gamecube/txdconv.cpp` follows its ahead-of-time native
  texture conversion; `tools/gamecube/dffcensus.cpp` reproduces its packed
  native-geometry cost analysis; the pre-instanced static DFF format and the
  small-allocation heap discipline in `vendor/librw/src/gx/gxraster.cpp`
  reach the same conclusions dca3 proved first.
- **[Polyphase Engine](https://github.com/Polyphase-Labs/Polyphase-Engine)** —
  a proven GX forward renderer we used as the reference for the GX channel
  and TEV layout in `vendor/librw/src/gx/gx.cpp` (see the channel-layout
  comments crediting it inline).
- **GameCube controller 3D model** by
  [Cory Richards](https://sketchfab.com/3d-models/gamecube-controller-21983501bac64993ac09cdc7936ffdf2),
  licensed [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) —
  converted to RenderWare format for the controls page
  (`tools/gamecube/assets/`, licence preserved alongside).
- **Xiph.Org** — ogg, opus, opusfile, Tremor and theora.
- **devkitPro / libogc / AESND** — the toolchain and runtime that make
  homebrew GameCube work possible.

## License

The port's own contributions are released under the
[MIT License with Proof-of-Usage Condition (MIT-PoU)](LICENSE.md).
Upstream components keep their original licenses (librw is MIT by aap; the
xiph libraries are BSD; reVC code remains under its upstream terms). This
project distributes **no** Rockstar-owned assets; you need your own copy of
Vice City to play.
