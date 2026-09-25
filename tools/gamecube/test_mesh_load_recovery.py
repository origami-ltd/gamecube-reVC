#!/usr/bin/env python3
"""Inject the streamed mesh allocation failure and retry it without losing data."""
from pathlib import Path
import subprocess
import tempfile
from test_runtime_guards import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT/'vendor/librw/src/geoplg.cpp').read_text()
    code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
using int32=int32_t; using uint32=uint32_t; using uint16=uint16_t; using bool32=int;
#define nil nullptr
#define MEMDUR_EVENT 0
#define ID_GEOMETRY 0
static size_t largest=18*1024; static int mustCalls=0;
unsigned rwGeoAllocFails; static uint16 nextSerialNum=1;
void *tryAlloc(size_t n) { return n<=largest ? std::malloc(n) : nullptr; }
void *tryResize(void *p,size_t n) { return n<=largest ? std::realloc(p,n) : nullptr; }
void *mustAlloc(size_t n) { mustCalls++; auto p=tryAlloc(n); assert(p); return p; }
void *mustResize(void *p,size_t n) { mustCalls++; p=tryResize(p,n); assert(p); return p; }
#define rwMalloc(n,h) tryAlloc(n)
#define rwRealloc(p,n,h) tryResize(p,n)
#define rwNew(n,h) mustAlloc(n)
#define rwResize(p,n,h) mustResize(p,n)
struct Mesh { uint16 *indices; uint32 numIndices; void *material; };
struct alignas(void*) MeshHeader {
    uint32 flags; uint16 numMeshes,serialNum; uint32 totalIndices;
    Mesh *getMeshes() { return (Mesh*)(this+1); }
};
struct Geometry {
    enum { NATIVE=0x01000000 };
    int flags=0; MeshHeader *meshHeader=nullptr;
    struct { void **materials; } matList;
    MeshHeader *allocateMeshes(int32,uint32,bool32,bool32=false);
};
struct Stream {
    std::vector<uint32> words; size_t cursor=0;
    void read32(void *p,size_t n) { assert(cursor+n<=words.size()*4); std::memcpy(p,(char*)words.data()+cursor,n);cursor+=n; }
    void read16(void *p,size_t n) { read32(p,n); }
};
'''
    for result, name in [('MeshHeader*', 'Geometry::allocateMeshes')]:
        code += '\n'+result+'\n'+function(source, name)+'\n'
    structs = source[source.index('struct MeshHeaderStream'):source.index('static Stream*\nreadMesh')]
    code += structs+'\nStream*\n'+function(source, 'readMesh')+'\n'
    code += r'''
int main() {
    int material; void *materials[]={&material}; Geometry geo; geo.matList.materials=materials;
    Stream stream; stream.words={0,1,13500,13500,0};
    for(int i=0;i<13500;i++) stream.words.push_back(i%3000);
    assert(readMesh(&stream,int(stream.words.size()*4),&geo,0,0)==nullptr);
    assert(!geo.meshHeader && rwGeoAllocFails==1 && mustCalls==0);
    largest=64*1024; stream.cursor=0;
    assert(readMesh(&stream,int(stream.words.size()*4),&geo,0,0)==&stream);
    assert(geo.meshHeader->totalIndices==13500 && geo.meshHeader->numMeshes==1);
    auto mesh=geo.meshHeader->getMeshes(); assert(mesh->material==&material);
    for(int i=0;i<13500;i++) assert(mesh->indices[i]==i%3000);
    auto saved=geo.meshHeader; auto serial=saved->serialNum;
    largest=18*1024;
    assert(!geo.allocateMeshes(2,16000,0,true));
    assert(geo.meshHeader==saved && saved->serialNum==serial);
    for(int i=0;i<13500;i++) assert(saved->getMeshes()->indices[i]==i%3000);
    assert(mustCalls==0 && rwGeoAllocFails==2);
    std::free(geo.meshHeader);
}
'''
    with tempfile.TemporaryDirectory(prefix='revc-mesh-load-') as tmp:
        cpp, exe = Path(tmp)/'test.cpp', Path(tmp)/'test'
        cpp.write_text(code)
        subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS: mesh load survives a 26 KiB request with an 18 KiB free span and retries with intact indices')


if __name__ == '__main__':
    main()
