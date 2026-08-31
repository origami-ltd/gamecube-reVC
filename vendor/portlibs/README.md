# vendor/portlibs/

PowerPC build dependencies bundled so the project builds from a fresh clone:

- `cmake/` — Wii toolchain file used by `build.py` for the Wii target.
  It requires a devkitPro installation (`DEVKITPRO` environment variable)
  for `ogc-common.cmake`.
- `ppc/` — libtheora headers and static libraries built for
  PowerPC/GameCube, used for FMV decoding. Theora is developed by the
  Xiph.Org Foundation and is BSD-licensed.
