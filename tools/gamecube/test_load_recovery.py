#!/usr/bin/env python3
"""Exercise production load deferral and bounded allocation recovery with host stubs."""
from pathlib import Path
import subprocess
import tempfile
from test_runtime_guards import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT / 'src/core/Streaming.cpp').read_text()
    admission = function(source, 'CStreaming::ConvertBufferToObject').split('\n\tRwMemory mem;')[0]
    code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstddef>
using uint32 = uint32_t; using int32 = int32_t; using int8 = int8_t;
using bool8 = bool; using u64 = uint64_t;
#define GTA_OGC 1
#define nil nullptr
#define TRUE true
#define FALSE false
enum { STREAM_OFFSET_TXD = 64, STREAMFLAGS_SCRIPTOWNED = 2,
       STREAMFLAGS_LOD = 128, STREAM_HEAP_FLOOR = 512*1024 };
struct Info { int m_flags = 0; void *m_prev = nullptr; };
static bool bootDone = true;
static int gcEssentialLoad, removed, failures;
static const char *gFailWhy;
static size_t heapRoom, chunkRoom, releasedPerModel;
static int droppedModels, maxModels, behindCalls;
static char heapToken, chunkToken;
static unsigned gHeapEmergency;
static int gMainLwp = 1, currentLwp = 1;
int LWP_GetSelf() { return currentLwp; }
int gcBootDone() { return bootDone; }
struct HeapInfo { size_t fordblks; };
HeapInfo mallinfo() { return {heapRoom}; }
size_t gcHeapFreeTotal() { return heapRoom + chunkRoom; }
void *__real_malloc(size_t n) { return heapRoom >= n ? &heapToken : nullptr; }
void *gcBigAlloc(size_t n) { return chunkRoom >= n ? &chunkToken : nullptr; }
bool gcBigContains(const void *p) { return p == &chunkToken; }
void gcBigFree(void *p) { assert(p == &chunkToken); }
void releaseProbe(void *p) { assert(p == &heapToken); }
#define free releaseProbe
u64 gettime() { return 0; }
u64 ticks_to_millisecs(u64 ticks) { return ticks; }
void FailedLoad(int) { failures++; }
struct CStreaming {
    static Info ms_aInfoForModel[128], ms_endLoadedList;
    static size_t ms_memoryUsed;
    static bool ConvertBufferToObject(int8_t*, int32);
    static void RemoveModel(int) { removed++; }
    static bool RemoveLeastUsedModel(unsigned, unsigned, bool = false) {
        if(droppedModels == maxModels) return false;
        droppedModels++; chunkRoom += releasedPerModel; return true;
    }
    static void DeleteRwObjectsBehindCamera(size_t) { behindCalls++; }
};
Info CStreaming::ms_aInfoForModel[128], CStreaming::ms_endLoadedList;
size_t CStreaming::ms_memoryUsed = 1024*1024;
'''
    code += '\nbool\n' + admission + '\n\treturn true;\n}\n'
    start = source.index('extern "C" int\ngcStreamEmergencyShed(')
    code += '\nint\n' + function(source[start:], 'gcStreamEmergencyShed') + '\n'
    code += r'''
int main() {
    heapRoom = STREAM_HEAP_FLOOR - 1;
    for(int i = 0; i < 5; i++) assert(!CStreaming::ConvertBufferToObject(nullptr, 5));
    assert(removed == 5 && failures == 0 && gcEssentialLoad == 0);
    heapRoom = STREAM_HEAP_FLOOR;
    assert(CStreaming::ConvertBufferToObject(nullptr, 5));
    heapRoom = 0;
    CStreaming::ms_aInfoForModel[5].m_flags = STREAMFLAGS_SCRIPTOWNED;
    assert(CStreaming::ConvertBufferToObject(nullptr, 5));
    assert(gcEssentialLoad == 0);
    CStreaming::ms_aInfoForModel[5].m_flags = STREAMFLAGS_LOD;
    assert(CStreaming::ConvertBufferToObject(nullptr, 5));
    assert(gcEssentialLoad == 0 && removed == 5);
    assert(CStreaming::ConvertBufferToObject(nullptr, STREAM_OFFSET_TXD));
    CStreaming::ms_aInfoForModel[5].m_flags = 0;
    bootDone = false;
    assert(CStreaming::ConvertBufferToObject(nullptr, 5));
    assert(removed == 5 && failures == 0);

    CStreaming::ms_endLoadedList.m_prev = &heapToken;
    gHeapEmergency = 2;
    chunkRoom = 3072;
    assert(gcStreamEmergencyShed(3072));
    assert(droppedModels == 0 && gHeapEmergency == 2);
    chunkRoom = 0; maxModels = 4; releasedPerModel = 1024;
    assert(gcStreamEmergencyShed(4096));
    assert(droppedModels == 4 && behindCalls == 0 && gHeapEmergency == 2);
    chunkRoom = 0; droppedModels = 0; maxModels = 24;
    releasedPerModel = 4096;
    assert(gcStreamEmergencyShed(24*4096));
    assert(droppedModels == 24 && gHeapEmergency == 2);
    chunkRoom = 0; droppedModels = 0;
    assert(!gcStreamEmergencyShed(25*4096));
    assert(droppedModels == 24 && gHeapEmergency == 2);
    currentLwp = 2; droppedModels = 0;
    assert(!gcStreamEmergencyShed(1024) && droppedModels == 0);
    currentLwp = 1; CStreaming::ms_endLoadedList.m_prev = nullptr;
    assert(!gcStreamEmergencyShed(1024) && droppedModels == 0);
}
'''
    with tempfile.TemporaryDirectory(prefix='revc-load-recovery-') as tmp:
        cpp, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        cpp.write_text(code)
        subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS: deferral without failure cooldown; recovery stops once the allocation fits')


if __name__ == '__main__':
    main()
