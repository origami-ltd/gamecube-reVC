#!/usr/bin/env python3
"""Check color-cache lifetime and exact material/ambient matching."""
from pathlib import Path
import subprocess
import tempfile
from test_runtime_guards import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT/'vendor/librw/src/gx/gx.cpp').read_text()
    code = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
using uint8 = uint8_t; using uint32 = uint32_t; using int32 = int32_t; using bool32 = bool;
#define nil nullptr
struct RGBA { uint8 red, green, blue, alpha; };
struct Geometry { int32 numVertices; };
struct GxGeoExt { RGBA *colors = nullptr; int32 colorCount = 0;
    uint32 colorKey = 0, colorAmbient = 0, colorFrame = 0; };
static unsigned gxFrameNo = 1, gxColorBytes, allocations, retired;
static bool failAlloc;
void *gcBigAlloc(size_t n) { if(failAlloc) return nullptr; allocations++; return malloc(n); }
void *__real_malloc(size_t) { return nullptr; }
void gxColorRetire(void *p) { if(p) { retired++; free(p); } }
void DCFlushRange(void*, size_t) {}
int clamp255i(int n) { return n > 255 ? 255 : n < 0 ? 0 : n; }
int mul255(int a, int b) { return (a*b+127)/255; }
'''
    at = source.index('\ngxBuildColorCache(')
    code += '\nstatic bool32\n'+function(source[at+1:], 'gxBuildColorCache')+'\n'
    code += r'''
int main() {
    Geometry geo{2}; GxGeoExt cache;
    RGBA prelit[] = {{10,20,30,255},{100,110,120,255}};
    RGBA white{255,255,255,255}, red{255,0,0,255};
    assert(gxBuildColorCache(&geo,&cache,prelit,0,10,20,30,white));
    assert(cache.colors[0].red == 20 && cache.colors[0].blue == 60);
    auto first = cache.colors;
    gxFrameNo++;
    assert(gxBuildColorCache(&geo,&cache,prelit,0,10,20,30,white));
    assert(!gxBuildColorCache(&geo,&cache,prelit,0,0,0,0,red));
    assert(cache.colors == first && cache.colors[0].green == 40);
    gxFrameNo++;
    failAlloc = true;
    assert(gxBuildColorCache(&geo,&cache,prelit,0,80,70,60,red));
    assert(cache.colors == first && allocations == 1 && retired == 0);
    assert(cache.colors[0].red == 90 && cache.colors[0].green == 0);
    gxFrameNo++;
    assert(gxBuildColorCache(&geo,&cache,prelit,0,80,71,60,white));
    assert(cache.colors[0].green == 91);
    GxGeoExt missing;
    assert(!gxBuildColorCache(&geo,&missing,prelit,0,80,71,60,white));
    white.alpha = 127;
    assert(!gxBuildColorCache(&geo,&cache,prelit,0,80,71,60,white));
    assert(allocations == 1 && gxColorBytes == 8);
    free(cache.colors);
}
'''
    with tempfile.TemporaryDirectory(prefix='revc-gx-colors-') as d:
        cpp, exe = Path(d)/'test.cpp', Path(d)/'test'
        cpp.write_text(code)
        subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all',str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('PASS: exact color keys, no current-frame overwrite, no allocation churn on ambient changes')


if __name__ == '__main__':
    main()
