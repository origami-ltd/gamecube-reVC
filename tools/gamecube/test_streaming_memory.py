#!/usr/bin/env python3
"""Exercise the production MEM1 span operations and driving corridor on the host."""
from pathlib import Path
import subprocess
import tempfile

from test_runtime_guards import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    allocator = (ROOT/'src/skel/gamecube/gamecube.cpp').read_text()
    streaming = (ROOT/'src/core/Streaming.cpp').read_text()
    graphics = (ROOT/'vendor/librw/src/gx/gxraster.cpp').read_text()
    code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <random>
#include <vector>
using uint8 = uint8_t; using uint32 = uint32_t; using int32 = int32_t; using u32 = uint32_t;
enum { BIG_SPANS = 32, BIG_CHUNKS = 7 };
static bool interrupts = true;
#define _CPU_ISR_Disable(level) do { level = interrupts; interrupts = false; } while(0)
#define _CPU_ISR_Restore(level) do { interrupts = level; } while(0)
void hcDel(void*) {}
void __real_free(void*) { std::abort(); }
void gcBigFree(void*);
'''
    start = allocator.index('struct BigSpan {')
    end = allocator.index('// B110:', start)
    code += allocator[start:end]
    code += '\nstatic int gBigKeep = 1; static uint32 gBigUsed;\n'
    for name, result in [('gcBigAllocIn', 'void*'), ('gcBigChunkOf', 'BigChunk*'),
                         ('gcBigSizeOf', 'size_t'), ('gcBigResize', 'int'),
                         ('gcBigMove', 'void*'), ('gcBigFree', 'void')]:
        marker = f'{name}('
        # Skip declarations in the wrapper section.
        source = allocator[allocator.index('struct BigSpan {'):]
        if name == 'gcBigFree':
            source = source[source.index('extern "C" void gcBigFree(void *p)\n{'):]
        code += '\n'+result+'\n'+function(source, name)+'\n'
    code += '\nbool\n'+function(streaming, 'InStreamingCorridor')
    code += r'''
using uintptr = uintptr_t; using int16 = int16_t; using int8 = int8_t; using uint16 = uint16_t; using bool32 = bool;
#define nil nullptr
struct V3d { float x,y,z; }; struct RGBA { uint8 r,g,b,a; }; struct TexCoords { float u,v; };
struct Triangle { uint16 v[3], material; };
struct MorphTarget { void *parent; V3d *vertices, *normals; };
struct Mesh { uint16 *indices; uint32 numIndices; void *material; };
struct MeshHeader { uint32 flags; uint16 numMeshes, serial; uint32 total, pad; Mesh *getMeshes() { return (Mesh*)(this+1); } };
struct GxGeoExt { void *packBase; int16 *pos,*uv; int8 *nrm; uint8 arraysFlushed;
    RGBA *colors = nullptr; int colorCount = 0; };
void DCFlushRange(void*, size_t) {}
struct Geometry {
    enum { NATIVE = 1 }; int flags = 0, numTexCoordSets = 1, numMorphTargets = 1;
    uint8 *attribBase; Triangle *triangles; RGBA *colors; TexCoords *texCoords[8] = {};
    MorphTarget *morphTargets; MeshHeader *meshHeader; GxGeoExt gx; bool skin = false;
};
struct Skin { static bool get(Geometry *g) { return g->skin; } };
#define PLUGINOFFSET(T, object, offset) (&(object)->gx)
'''
    code += '\nbool32\n'+function(graphics, 'gxMoveGeometryMemory')
    code += r'''
void testGeometry() {
    alignas(32) uint8 memory[4096]; auto &c = gBig[0];
    c.base = memory; c.size = sizeof(memory); c.n = 1; c.used = gBigUsed = 0;
    c.sp[0] = {0, sizeof(memory), 0};
    void *holes[5]; uint8 *blocks[5];
    for(int i = 0; i < 5; i++) {
        holes[i] = gcBigAllocIn(&c, 32); blocks[i] = (uint8*)gcBigAllocIn(&c, 128);
        memset(blocks[i], 0, 128);
    }
    Geometry g;
    g.attribBase = blocks[0]; g.triangles = (Triangle*)blocks[0];
    g.colors = (RGBA*)(blocks[0]+16); g.texCoords[0] = (TexCoords*)(blocks[0]+32);
    g.triangles[0].v[0] = 2; g.colors[0].r = 53; g.texCoords[0][0].u = 0.75f;
    g.morphTargets = (MorphTarget*)blocks[1];
    g.morphTargets[0].vertices = (V3d*)(blocks[1]+32); g.morphTargets[0].normals = (V3d*)(blocks[1]+64);
    g.morphTargets[0].vertices[0].x = 42.0f; g.morphTargets[0].normals[0].z = 1.0f;
    g.gx = {blocks[2], (int16*)blocks[2], (int16*)(blocks[2]+32), (int8*)(blocks[2]+64), 1};
    g.gx.colors = (RGBA*)blocks[4]; g.gx.colorCount = 1; g.gx.colors[0].g = 75;
    g.gx.pos[0] = 1024; g.gx.uv[0] = 256; g.gx.nrm[0] = 64;
    g.meshHeader = (MeshHeader*)blocks[3]; g.meshHeader->numMeshes = 1;
    g.meshHeader->getMeshes()[0].indices = (uint16*)(blocks[3]+64);
    g.meshHeader->getMeshes()[0].indices[0] = 7;
    for(auto hole : holes) gcBigFree(hole);
    assert(gxMoveGeometryMemory(&g, gcBigMove, false));
    assert(g.attribBase != blocks[0] && (uint8*)g.morphTargets != blocks[1]);
    assert(g.gx.packBase != blocks[2] && (uint8*)g.meshHeader != blocks[3]);
    assert(g.triangles[0].v[0] == 2 && g.colors[0].r == 53 && g.texCoords[0][0].u == 0.75f);
    assert(g.morphTargets[0].vertices[0].x == 42.0f && g.morphTargets[0].normals[0].z == 1.0f);
    assert(g.gx.pos[0] == 1024 && g.gx.uv[0] == 256 && g.gx.nrm[0] == 64 && !g.gx.arraysFlushed);
    assert(g.meshHeader->getMeshes()[0].indices[0] == 7);
    assert((uint8*)g.gx.colors != blocks[4] && g.gx.colors[0].g == 75);
    g.skin = true; assert(!gxMoveGeometryMemory(&g, gcBigMove, false));
}
'''
    code += r'''
struct Live { uint8 *p; size_t bytes; uint8 value; };
void testPinnedBarrier() {
    alignas(32) uint8 memory[4096]; auto &c = gBig[0];
    c.base = memory; c.size = sizeof(memory); c.n = 1; c.used = gBigUsed = 0;
    c.sp[0] = {0, sizeof(memory), 0};
    auto hole = gcBigAllocIn(&c, 512);
    auto pinned = gcBigAllocIn(&c, 32);
    auto moving = gcBigAllocIn(&c, 512);
    auto tail = gcBigAllocIn(&c, 512);
    auto end = gcBigAllocIn(&c, sizeof(memory)-1568);
    memset(moving, 0xA5, 512);
    gcBigFree(hole); gcBigFree(tail);
    assert(!gcBigAllocIn(&c, 1024));
    auto moved = (uint8*)gcBigMove(moving);
    assert(moved == hole);
    for(int i=0;i<512;i++) assert(moved[i] == 0xA5);
    auto joined = gcBigAllocIn(&c, 1024); assert(joined);
    gcBigFree(pinned); gcBigFree(end); gcBigFree(moved); gcBigFree(joined);
    assert(c.n == 1 && c.sp[0].size == sizeof(memory));
}
void check(const std::vector<Live> &live) {
    BigChunk &c = gBig[0]; uint32 pos = 0, used = 0;
    for(int i = 0; i < c.n; i++) {
        assert(c.sp[i].addr == pos && c.sp[i].size && !(c.sp[i].size & 31));
        if(i) assert(c.sp[i-1].used || c.sp[i].used);
        pos += c.sp[i].size; if(c.sp[i].used) used += c.sp[i].size;
    }
    assert(pos == c.size && used == c.used && used == gBigUsed && interrupts);
    for(const auto &v : live) {
        assert(gcBigSizeOf(v.p) >= v.bytes);
        for(size_t i = 0; i < v.bytes; i++) assert(v.p[i] == v.value);
    }
}
int main() {
    alignas(32) uint8 memory[32768];
    gBigChunks = 1; auto &c = gBig[0];
    c.base = memory; c.size = sizeof(memory); c.n = 1; c.used = 0;
    c.sp[0] = {0, sizeof(memory), 0};
    std::vector<Live> live; std::mt19937 rng(159);
    for(unsigned step = 0; step < 40000; step++) {
        unsigned op = rng()%4;
        if(live.empty() || op == 0) {
            size_t bytes = (rng()%32+1)*32;
            auto p = (uint8*)gcBigAllocIn(&c, bytes);
            if(p) { uint8 value = rng()%255+1; memset(p, value, bytes); live.push_back({p, bytes, value}); }
        } else {
            size_t index = rng()%live.size(); auto &v = live[index];
            if(op == 1) { gcBigFree(v.p); live.erase(live.begin()+index); }
            else if(op == 2) {
                size_t bytes = (rng()%(v.bytes/32)+1)*32;
                assert(gcBigResize(v.p, bytes)); v.bytes = bytes;
            } else v.p = (uint8*)gcBigMove(v.p);
        }
        check(live);
    }
    for(auto v : live) gcBigFree(v.p);
    live.clear(); check(live); assert(c.n == 1 && c.sp[0].size == sizeof(memory));
    assert(InStreamingCorridor(100, 10, 1, 0, 120, 45));
    assert(!InStreamingCorridor(-100, 10, 1, 0, 120, 45));
    assert(!InStreamingCorridor(100, 50, 1, 0, 120, 45));
    assert(!InStreamingCorridor(121, 0, 1, 0, 120, 45));
    assert(InStreamingCorridor(-100, 0, -1, 0, 120, 45));
    assert(InStreamingCorridor(10, 100, 0, 1, 120, 45));
    testPinnedBarrier(); testGeometry();
}
'''
    with tempfile.TemporaryDirectory(prefix='revc-memory-test-') as tmp:
        source, binary = Path(tmp)/'test.cpp', Path(tmp)/'test'
        source.write_text(code)
        subprocess.run(['clang++', '-std=c++17', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS: 40000 MEM1 operations; GX pointer relocation; directional prefetch bounds')


if __name__ == '__main__':
    main()
