#!/usr/bin/env python3
"""Exercise queued ARAM transfers and asynchronous dialogue admission."""
from pathlib import Path
import subprocess
import tempfile
from test_runtime_guards import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    audio = (ROOT/'src/audio/sampman_gamecube.cpp').read_text()
    raster = (ROOT/'vendor/librw/src/gx/gxraster.cpp').read_text()
    logic = (ROOT/'src/audio/AudioLogic.cpp').read_text()
    code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
using uint8 = uint8_t; using uint32 = uint32_t; using int16 = int16_t;
using u32 = uintptr_t;
#define GTA_OGC 1
#define MEM_VIRTUAL_TO_PHYSICAL(p) (p)
#define ARQ_MRAMTOARAM 0
#define ARQ_ARAMTOMRAM 1
#define ARQ_PRIO_LO 0
struct ARQRequest {};
static unsigned gxDmaBusy;
static unsigned transfers, dspCompleted;
static bool dspPending;
static uint8 aram[256];
void DCFlushRange(void*, uint32) {}
void DCInvalidateRange(void*, uint32) {}
void ARQ_PostRequest(ARQRequest*, u32, u32 dir, u32 prio, u32 address, u32 mem, u32 n) {
    assert(prio == ARQ_PRIO_LO && address + n <= sizeof(aram));
    assert(n && !(n & 31) && !(address & 31) && !(mem & 31));
    if(dspPending) { aram[0] = 0xA5; dspPending = false; dspCompleted++; }
    if(dir == ARQ_MRAMTOARAM) memcpy(aram + address, (void*)mem, n);
    else memcpy((void*)mem, aram + address, n);
    transfers++;
}
'''
    for text, name in [(audio, 'gcBankWrite'), (audio, 'gcBankRead'), (raster, 'gxAramTransfer')]:
        start = text.index('\n'+name+'(')
        code += '\nstatic void\n'+function(text[start+1:], name)+'\n'
    code += r'''
using uint8 = uint8_t;
struct CVector {};
enum { MISSION_AUDIO_SLOTS = 2, NO_SAMPLE = 999,
       LOADING_STATUS_NOT_LOADED, LOADING_STATUS_LOADING, LOADING_STATUS_LOADED };
static bool primed[3];
static unsigned preloads;
int gcStreamPrimed(int slot) { return primed[slot]; }
struct Samples { void PreloadStreamedFile(unsigned, unsigned slot) { preloads++; primed[slot] = false; } };
static Samples SampleManager;
struct cAudioManager {
    unsigned m_nMissionAudioSampleIndex[2] = { 1, 2 };
    unsigned m_nMissionAudioLoadingStatus[2] = { LOADING_STATUS_NOT_LOADED, LOADING_STATUS_NOT_LOADED };
    void ProcessMissionAudioSlot(uint8);
};
'''
    prefix = logic[logic.index('\ncAudioManager::ProcessMissionAudioSlot(')+1:].split('\n\t\tcase LOADING_STATUS_LOADED:')[0]
    code += '\nvoid\n'+prefix+'\n\t\tdefault: break;\n\t\t}\n\t}\n}\n'
    code += r'''
int main() {
    alignas(32) uint8 input[64], output[64];
    memset(input, 0x37, sizeof(input));
    dspPending = true; gcBankWrite(64, input, 64);
    assert(dspCompleted == 1 && aram[0] == 0xA5);
    dspPending = true; gcBankRead(output, 64, 64);
    assert(dspCompleted == 2 && memcmp(input, output, 64) == 0);
    dspPending = true; gxAramTransfer(ARQ_MRAMTOARAM, input, 128, 64);
    memset(output, 0, 64);
    gxAramTransfer(ARQ_ARAMTOMRAM, output, 128, 64);
    assert(dspCompleted == 3 && transfers == 4 && !gxDmaBusy);
    assert(memcmp(input, output, 64) == 0);
    cAudioManager manager;
    manager.ProcessMissionAudioSlot(0);
    assert(preloads == 1 && manager.m_nMissionAudioLoadingStatus[0] == LOADING_STATUS_LOADING);
    for(int frame = 0; frame < 180; frame++) manager.ProcessMissionAudioSlot(0);
    assert(preloads == 1 && manager.m_nMissionAudioLoadingStatus[0] == LOADING_STATUS_LOADING);
    primed[1] = true; manager.ProcessMissionAudioSlot(0);
    assert(manager.m_nMissionAudioLoadingStatus[0] == LOADING_STATUS_LOADED);
    manager.ProcessMissionAudioSlot(1);
    assert(preloads == 2 && manager.m_nMissionAudioLoadingStatus[1] == LOADING_STATUS_LOADING);
}
'''
    with tempfile.TemporaryDirectory(prefix='revc-audio-dma-') as d:
        cpp, exe = Path(d)/'test.cpp', Path(d)/'test'
        cpp.write_text(code)
        subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS: audio/texture DMA shares the mixer queue; speech waits for a primed stream')


if __name__ == '__main__':
    main()
