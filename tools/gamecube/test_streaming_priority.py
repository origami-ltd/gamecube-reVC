#!/usr/bin/env python3
"""Exercise staged LOD loading, dependency priority and adjacent DVD reads."""
from pathlib import Path
import subprocess
import tempfile
from test_runtime_guards import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    src = (ROOT/'src/core/Streaming.cpp').read_text()
    header = (ROOT/'src/core/Streaming.h').read_text()
    scope = src[src.index('struct StreamMeasurementScope {'):].split('};', 1)[0] + '};'
    code = r'''
#include <cassert>
#include <cstdint>
#include <climits>
#include <cstddef>
using uint8 = uint8_t; using uint32 = uint32_t; using int32 = int32_t; using int8 = int8_t;
#define GTA_OGC 1
#define nil nullptr
enum { STREAM_OFFSET_TXD = 64, STREAM_OFFSET_COL = 96, STREAM_OFFSET_ANIM = 128,
       NUMSTREAMINFO = 160, STREAM_HEAP_FLOOR = 512*1024,
       STREAMFLAGS_DONT_REMOVE = 1, STREAMFLAGS_SCRIPTOWNED = 2,
       STREAMFLAGS_DEPENDENCY = 4, STREAMFLAGS_PRIORITY = 8, STREAMFLAGS_NOFADE = 16,
       STREAMFLAGS_LOD = 128, STREAMFLAGS_KEEP_IN_MEMORY = 7 };
enum { STREAMSTATE_NOTLOADED = 0, STREAMSTATE_LOADED = 1, STREAMSTATE_INQUEUE = 2,
       STREAMSTATE_READING = 3, STREAMSTATE_STARTED = 4 };
enum { CHANNELSTATE_IDLE, CHANNELSTATE_READING };
enum { MITYPE_SIMPLE, MITYPE_PED, MITYPE_VEHICLE, STREAM_NONE };
struct CStreamingChannel {
    int state = CHANNELSTATE_IDLE, streamIds[4] = {-1,-1,-1,-1}, offsets[4] = {};
    int field24, size, position, numTries;
};
static int dvdReads, dvdSectors;
int CdStreamGetLastPosn() { return 0; }
int CdStreamRead(int,int8*,int,int size) { dvdReads++; dvdSectors=size; return 1; }
#define debug(...) assert(false)
static uint8 gLoadFails[NUMSTREAMINFO], gFailSec[NUMSTREAMINFO];
static bool gStreamMeasuring;
struct CTimer { static uint32 GetTimeInMillisecondsPauseMode() { return 12000; } };
bool gcBootDone() { return true; }
struct HeapInfo { size_t fordblks; };
static size_t heapRoom = 1024*1024;
HeapInfo mallinfo() { return {heapRoom}; }
struct Model {
    int txd, anim = -1, m_alpha = 0;
    bool m_isBigBuilding = false;
    int GetTxdSlot() { return txd; } int GetAnimFileIndex() { return anim; }
    bool IsSimple() { return true; }
    int GetModelType() { return MITYPE_SIMPLE; }
};
using CSimpleModelInfo = Model; using CBaseModelInfo = Model;
static Model models[64];
struct CModelInfo { static Model *GetModelInfo(int id) { return &models[id]; } };
struct CCutsceneMgr { static bool IsCutsceneProcessing() { return false; } };
struct CStreamingInfo {
    int m_flags = 0, m_loadState = STREAMSTATE_NOTLOADED, m_nextID = -1;
    uint32 position = 1, sectors = 1;
    CStreamingInfo *m_next = nullptr, *m_prev = nullptr;
    bool IsPriority() { return m_flags & STREAMFLAGS_PRIORITY; }
    bool GetCdPosnAndSize(uint32 &p, uint32 &s) { p = position; s = sectors; return sectors != 0; }
    void RemoveFromList() {
        assert(m_prev && m_next); m_prev->m_next = m_next; m_next->m_prev = m_prev;
        m_next = m_prev = nullptr;
    }
    void AddToList(CStreamingInfo *head) {
        assert(!m_next && !m_prev); m_next = head->m_next; m_prev = head;
        head->m_next->m_prev = this; head->m_next = this;
    }
};
struct CStreaming {
    static CStreamingChannel ms_channel[2];
    static CStreamingInfo ms_aInfoForModel[NUMSTREAMINFO];
    static CStreamingInfo ms_startRequestedList, ms_endRequestedList, ms_startLoadedList, ms_endLoadedList;
    static int ms_numModelsRequested, ms_numPriorityRequests;
    static int ms_streamingBufferSize;
    static bool ms_bLoadingBigModel;
    static int8 *ms_pStreamingBuffer[2];
    static void RequestModel(int32,int32);
    static int32 GetNextFileOnCd(int32,bool);
    static void RequestModelStream(int32);
    static bool IsTxdUsedByRequestedModels(int32);
    static bool AreAnimsUsedByRequestedModels(int32);
    static int GetCdImageOffset(int) { return 0; }
    static void RemoveModel(int id) {
        auto &info = ms_aInfoForModel[id];
        if(info.m_loadState == STREAMSTATE_INQUEUE) { DecrementRef(id); info.RemoveFromList(); }
        info.m_loadState = STREAMSTATE_NOTLOADED;
    }
    static void DecrementRef(int32);
    static bool CanRemoveModel(int id) { return !(ms_aInfoForModel[id].m_flags & 3); }
    static void RequestTxd(int id,int flags) { RequestModel(id+STREAM_OFFSET_TXD,flags); }
    static void RequestAnim(int id,int flags) { RequestModel(id+STREAM_OFFSET_ANIM,flags); }
'''
    code += '\nstatic void ' + function(header, 'ReRequestModel') + '\n};\n'
    code += r'''
CStreamingInfo CStreaming::ms_aInfoForModel[NUMSTREAMINFO];
CStreamingChannel CStreaming::ms_channel[2];
CStreamingInfo CStreaming::ms_startRequestedList, CStreaming::ms_endRequestedList,
               CStreaming::ms_startLoadedList, CStreaming::ms_endLoadedList;
int CStreaming::ms_numModelsRequested, CStreaming::ms_numPriorityRequests;
int CStreaming::ms_streamingBufferSize = 128;
bool CStreaming::ms_bLoadingBigModel;
int8 *CStreaming::ms_pStreamingBuffer[2];
'''
    for result, name in [('bool','ModelNotLoaded'), ('bool','TxdNotLoaded'), ('bool','AnimNotLoaded'),
                         ('static bool','HasPendingLods'), ('static bool','LodPassAllows'),
                         ('void','CStreaming::RequestModel'), ('void','CStreaming::DecrementRef'),
                         ('int32','CStreaming::GetNextFileOnCd'),
                         ('bool','CStreaming::IsTxdUsedByRequestedModels'),
                         ('bool','CStreaming::AreAnimsUsedByRequestedModels'),
                         ('void','CStreaming::RequestModelStream')]:
        signature = ('inline bool ' if name in ('TxdNotLoaded','AnimNotLoaded') else '\n')+name+'('
        code += '\n' + result + '\n' + function(src[src.index(signature):],name) + '\n'
    code += scope + r'''
void finish(int id) {
    CStreaming::DecrementRef(id);
    auto &info = CStreaming::ms_aInfoForModel[id];
    info.RemoveFromList(); info.m_loadState = STREAMSTATE_LOADED;
}
void earlyReturn() { StreamMeasurementScope measurement; assert(gStreamMeasuring); }
int main() {
    using S = CStreaming;
    S::ms_startRequestedList.m_next = &S::ms_endRequestedList;
    S::ms_endRequestedList.m_prev = &S::ms_startRequestedList;
    S::ms_startLoadedList.m_next = &S::ms_endLoadedList;
    S::ms_endLoadedList.m_prev = &S::ms_startLoadedList;
    models[1].txd = 10; models[1].anim = 1; models[2].txd = 11;
    S::ms_aInfoForModel[74].position = 4000;
    S::ms_aInfoForModel[129].position = 5000;
    S::ms_aInfoForModel[75].position = 2;
    S::RequestModel(1,0); S::RequestModel(2,0);
    assert(S::ms_numModelsRequested == 5 && S::ms_numPriorityRequests == 0);
    S::RequestModel(1,STREAMFLAGS_PRIORITY);
    assert(S::ms_aInfoForModel[74].IsPriority());
    assert(S::ms_aInfoForModel[129].IsPriority());
    assert(S::ms_numModelsRequested == 5 && S::ms_numPriorityRequests == 3);
    S::RequestModel(1,STREAMFLAGS_PRIORITY);
    assert(S::ms_numPriorityRequests == 3);
    assert(S::GetNextFileOnCd(0,true) == 74);
    finish(74);
    assert(S::GetNextFileOnCd(0,true) == 129);
    finish(129);
    assert(S::GetNextFileOnCd(0,true) == 1);
    finish(1);
    assert(S::ms_numPriorityRequests == 0 && S::ms_numModelsRequested == 2);
    assert(S::GetNextFileOnCd(0,true) == 75);
    models[3].txd = 12; models[3].anim = 2;
    S::RequestModel(3,STREAMFLAGS_PRIORITY);
    assert(S::ms_aInfoForModel[130].IsPriority());
    finish(76);
    S::ms_aInfoForModel[76].m_loadState = STREAMSTATE_NOTLOADED;
    S::ms_aInfoForModel[76].m_flags = STREAMFLAGS_DONT_REMOVE;
    S::GetNextFileOnCd(0,true);
    assert(S::ms_aInfoForModel[76].IsPriority());
    assert(S::ms_aInfoForModel[76].m_flags & STREAMFLAGS_DONT_REMOVE);
    finish(76); finish(130); finish(3);
    assert(S::ms_numPriorityRequests == 0);
    models[4].txd = 13;
    S::RequestModel(4,STREAMFLAGS_PRIORITY);
    finish(77);
    S::ms_aInfoForModel[77].m_loadState = STREAMSTATE_NOTLOADED;
    assert(S::GetNextFileOnCd(0,true) == 77);
    assert(S::ms_numPriorityRequests == 2);
    finish(77); finish(4);
    assert(S::ms_numPriorityRequests == 0);
    finish(75); finish(2);
    models[10].txd = 20;
    models[11].txd = 21; models[11].m_isBigBuilding = true;
    S::ms_aInfoForModel[84].position = 1;
    S::ms_aInfoForModel[85].position = 9000;
    S::ms_aInfoForModel[11].position = 9001;
    S::RequestModel(10,STREAMFLAGS_PRIORITY);
    S::RequestModel(11,0);
    assert(S::ms_aInfoForModel[11].m_flags & STREAMFLAGS_LOD);
    assert(S::ms_aInfoForModel[85].m_flags & STREAMFLAGS_LOD);
    assert(!LodPassAllows(10) && !LodPassAllows(84));
    assert(LodPassAllows(11) && LodPassAllows(85) && LodPassAllows(STREAM_OFFSET_COL));
    assert(S::GetNextFileOnCd(0,true) == 85);
    finish(85);
    assert(S::GetNextFileOnCd(0,true) == 11);
    S::DecrementRef(11);
    S::ms_aInfoForModel[11].RemoveFromList();
    S::ms_aInfoForModel[11].m_loadState = STREAMSTATE_READING;
    S::ms_channel[0].state = CHANNELSTATE_READING;
    S::ms_channel[0].streamIds[0] = 11;
    assert(S::GetNextFileOnCd(0,true) == -1);
    assert(S::ms_numPriorityRequests == 2);
    S::ms_aInfoForModel[11].m_loadState = STREAMSTATE_STARTED;
    assert(S::GetNextFileOnCd(0,false) == -1);
    S::RequestModel(STREAM_OFFSET_COL,STREAMFLAGS_PRIORITY);
    assert(S::GetNextFileOnCd(0,true) == STREAM_OFFSET_COL);
    finish(STREAM_OFFSET_COL);
    S::ms_aInfoForModel[11].m_loadState = STREAMSTATE_LOADED;
    S::ms_channel[0].state = CHANNELSTATE_IDLE;
    assert(S::GetNextFileOnCd(0,true) == 84);
    finish(84);
    assert(S::GetNextFileOnCd(0,true) == 10);
    finish(10);
    assert(S::ms_numPriorityRequests == 0 && S::ms_numModelsRequested == 0);

    models[12].txd = 22; models[13].txd = 22;
    S::RequestModel(12,STREAMFLAGS_PRIORITY);
    S::RequestModel(13,STREAMFLAGS_PRIORITY);
    models[13].m_isBigBuilding = true;
    S::RequestModel(13,0);
    assert(S::ms_numPriorityRequests == 3);
    assert(S::ms_aInfoForModel[86].m_flags & STREAMFLAGS_LOD);
    assert(S::GetNextFileOnCd(0,true) == 86);
    finish(86); assert(S::GetNextFileOnCd(0,true) == 13);
    finish(13); finish(12);

    heapRoom = STREAM_HEAP_FLOOR - 1;
    models[14].txd = 23; models[15].txd = 24; models[15].m_isBigBuilding = true;
    S::RequestModel(14,STREAMFLAGS_PRIORITY);
    assert(S::ms_numModelsRequested == 0);
    S::RequestModel(15,0);
    assert(S::ms_numModelsRequested == 2 && S::ms_numPriorityRequests == 2);
    finish(88); finish(15);
    heapRoom = 1024*1024;

    models[20].txd = 25; models[20].m_isBigBuilding = true;
    models[21].txd = 26;
    S::ms_aInfoForModel[89].m_loadState = STREAMSTATE_LOADED;
    S::ms_aInfoForModel[90].m_loadState = STREAMSTATE_LOADED;
    S::ms_aInfoForModel[20].position = 10;
    S::ms_aInfoForModel[21].position = 11;
    S::ms_aInfoForModel[20].m_nextID = 21;
    S::RequestModel(21,STREAMFLAGS_PRIORITY); S::RequestModel(20,0);
    S::RequestModelStream(0);
    assert(dvdReads == 1 && dvdSectors == 1);
    assert(S::ms_channel[0].streamIds[0] == 20 && S::ms_channel[0].streamIds[1] == -1);
    assert(S::ms_aInfoForModel[21].m_loadState == STREAMSTATE_INQUEUE);
    S::RequestModelStream(1); assert(dvdReads == 1);
    S::ms_aInfoForModel[20].m_loadState = STREAMSTATE_LOADED;
    S::ms_channel[0] = CStreamingChannel{};
    S::RequestModelStream(1);
    assert(dvdReads == 2 && S::ms_channel[1].streamIds[0] == 21);
    S::ms_aInfoForModel[21].m_loadState = STREAMSTATE_LOADED;
    S::ms_channel[1] = CStreamingChannel{};

    models[22].txd = 27; models[22].m_isBigBuilding = true;
    S::ms_aInfoForModel[22].position = 50;
    S::ms_aInfoForModel[91].position = 49;
    S::ms_aInfoForModel[91].m_nextID = 22;
    S::RequestModel(22,0); S::RequestModelStream(0);
    assert(dvdReads == 3 && dvdSectors == 2);
    assert(S::ms_channel[0].streamIds[0] == 91 && S::ms_channel[0].streamIds[1] == 22);
    S::ms_aInfoForModel[91].m_loadState = STREAMSTATE_LOADED;
    S::ms_aInfoForModel[22].m_loadState = STREAMSTATE_LOADED;
    S::ms_channel[0] = CStreamingChannel{};

    // B177: a short unrequested run is read through, a long one still ends the batch.
    assert(S::ms_numModelsRequested == 0);
    const uint32 at[6] = {200, 204, 214, 300, 302, 372}, len[6] = {4, 10, 3, 2, 70, 1};
    for(int k = 0; k < 6; k++){
        models[26+k].txd = 25;
        S::ms_aInfoForModel[26+k].position = at[k];
        S::ms_aInfoForModel[26+k].sectors = len[k];
        if(k != 5) S::ms_aInfoForModel[26+k].m_nextID = 27+k;
    }
    S::RequestModel(26,0); S::RequestModel(28,0);
    S::RequestModelStream(0);
    assert(dvdReads == 4 && dvdSectors == 17);
    assert(S::ms_channel[0].streamIds[0] == 26 && S::ms_channel[0].streamIds[1] == 28 &&
           S::ms_channel[0].streamIds[2] == -1);
    assert(S::ms_channel[0].offsets[0] == 0 && S::ms_channel[0].offsets[1] == 14);
    assert(S::ms_aInfoForModel[27].m_loadState == STREAMSTATE_NOTLOADED);
    S::ms_aInfoForModel[26].m_loadState = S::ms_aInfoForModel[28].m_loadState = STREAMSTATE_LOADED;
    S::ms_channel[0] = CStreamingChannel{};
    S::RequestModel(29,0); S::RequestModel(31,0);
    S::RequestModelStream(0);
    assert(dvdReads == 5 && dvdSectors == 2 && S::ms_channel[0].streamIds[1] == -1);
    S::ms_aInfoForModel[29].m_loadState = STREAMSTATE_LOADED;
    S::ms_channel[0] = CStreamingChannel{};
    S::RequestModelStream(0);
    assert(dvdReads == 6 && S::ms_channel[0].streamIds[0] == 31);
    S::ms_aInfoForModel[31].m_loadState = STREAMSTATE_LOADED;
    S::ms_channel[0] = CStreamingChannel{};
    for(int k = 0; k < 6; k++) S::ms_aInfoForModel[26+k].m_nextID = -1;

    models[23].txd = 28; models[23].m_isBigBuilding = true;
    models[24].txd = 29;
    S::ms_aInfoForModel[92].m_loadState = STREAMSTATE_LOADED;
    S::ms_aInfoForModel[93].m_loadState = STREAMSTATE_LOADED;
    S::ms_aInfoForModel[23].sectors = 0;
    S::RequestModel(24,STREAMFLAGS_PRIORITY); S::RequestModel(23,0);
    assert(S::GetNextFileOnCd(0,true) == 24);
    assert(S::ms_aInfoForModel[23].m_loadState == STREAMSTATE_LOADED);
    finish(24);
    S::ms_aInfoForModel[23].m_loadState = STREAMSTATE_NOTLOADED;
    S::ms_aInfoForModel[24].m_loadState = STREAMSTATE_NOTLOADED;
    S::RequestModel(24,0); S::RequestModel(23,0);
    assert(S::GetNextFileOnCd(0,true) == 24);
    finish(24);
    assert(S::ms_numPriorityRequests == 0 && S::ms_numModelsRequested == 0 && !HasPendingLods());
    models[25].txd = 30;
    S::RequestModel(25,STREAMFLAGS_PRIORITY);
    S::RequestModel(95,STREAMFLAGS_LOD);
    assert(S::ms_aInfoForModel[95].IsPriority());
    assert(S::GetNextFileOnCd(0,true) == 95);
    S::RequestModelStream(0);
    assert(S::ms_aInfoForModel[95].m_loadState == STREAMSTATE_NOTLOADED);
    assert(S::ms_channel[0].streamIds[0] == 94);
    assert(!HasPendingLods());
    assert(!gStreamMeasuring);
    earlyReturn(); assert(!gStreamMeasuring);
    { StreamMeasurementScope outer; earlyReturn(); assert(gStreamMeasuring); }
    assert(!gStreamMeasuring);
}
'''
    with tempfile.TemporaryDirectory(prefix='revc-stream-priority-') as d:
        cpp, exe = Path(d)/'test.cpp', Path(d)/'test'
        cpp.write_text(code)
        subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all',str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('PASS: basic LODs and dependencies load before detail, including both channels and adjacent DVD batches')


if __name__ == '__main__':
    main()
