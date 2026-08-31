Vendored PowerPC build dependencies so a fresh clone builds without hunting
packages:

- `cmake/` — the Wii toolchain shim (stands in for devkitPro's wii-cmake
  package; still requires a devkitPro install for `ogc-common.cmake`).
- `ppc/` — libtheora built for PowerPC/GameCube (FMV decode). Theora is
  BSD-licensed by the Xiph.Org Foundation.
