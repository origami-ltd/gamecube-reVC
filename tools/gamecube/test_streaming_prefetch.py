#!/usr/bin/env python3
"""Exercise driving prediction, bounds, queue pressure and changes of direction."""
from pathlib import Path
import subprocess
import tempfile
from test_runtime_guards import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT / 'src/core/Streaming.cpp').read_text()
    code = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>
using int32 = int32_t; using uint8 = uint8_t; using uint32 = uint32_t;
using std::isfinite;
#define nil nullptr
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
template<class T> T Min(T a,T b) { return std::min(a,b); }
template<class T> T Max(T a,T b) { return std::max(a,b); }
float Sqrt(float n) { return std::sqrt(n); }
enum { MODELINFOSIZE=128, STREAM_HD_M=80, STREAM_NEAR_M=60,
       NUMSECTORS_X=20, NUMSECTORS_Y=20, MITYPE_TIME=1,
       ENTITYLIST_BUILDINGS, ENTITYLIST_BUILDINGS_OVERLAP,
       ENTITYLIST_OBJECTS, ENTITYLIST_OBJECTS_OVERLAP, ENTITYLIST_DUMMIES,
       STREAMSTATE_NOTLOADED=0, STREAMSTATE_INQUEUE=1, STREAMSTATE_LOADED=2,
       STREAMFLAGS_PREFETCH=64, STREAMFLAGS_PRIORITY=8 };
struct CVector {
    float x=0,y=0,z=0;
    CVector operator+(CVector b) const { return {x+b.x,y+b.y,z+b.z}; }
    CVector operator-(CVector b) const { return {x-b.x,y-b.y,z-b.z}; }
    CVector operator*(float b) const { return {x*b,y*b,z*b}; }
    void operator/=(float b) { x/=b; y/=b; z/=b; }
};
struct CBaseModelInfo {
    bool IsSimple() { return true; }
    void *GetColModel() { return this; }
};
struct CTimeModelInfo : CBaseModelInfo {
    int GetModelType() { return 0; } int GetTimeOn() { return 0; } int GetTimeOff() { return 24; }
};
CTimeModelInfo models[MODELINFOSIZE];
struct CModelInfo { static CBaseModelInfo *GetModelInfo(int id) { return &models[id]; } };
struct CClock { static bool GetIsTimeInRange(int,int) { return true; } };
bool IsAreaVisible(int area) { return area==0; }
struct CEntity {
    int id=0, m_scanCode=0, m_area=0;
    bool bDontStream=false,bStreamingDontDelete=false,bIsVisible=true,bIsBIGBuilding=false;
    CVector pos; float radius=0;
    int GetModelIndex() { return id; } CVector GetPosition() { return pos; }
    CVector GetBoundCentre() { return pos; } float GetBoundRadius() { return radius; }
};
struct CPtrNode { void *item; CPtrNode *next; };
struct List { CPtrNode *first=nullptr; };
struct CSector { List m_lists[32]; };
CSector sectors[20][20];
struct CWorld {
    static int scan;
    static int GetSectorIndexX(float x) { return int(std::floor(x/50))+10; }
    static int GetSectorIndexY(float y) { return int(std::floor(y/50))+10; }
    static CSector *GetSector(int x,int y) { return &sectors[x][y]; }
    static void AdvanceCurrentScanCode() { scan++; } static int GetCurrentScanCode() { return scan; }
};
int CWorld::scan;
struct Info { int m_loadState=0, flags=0; };
static std::vector<int> order;
struct CStreaming {
    static bool ms_disableStreaming; static int ms_numModelsRequested;
    static Info ms_aInfoForModel[MODELINFOSIZE];
    static bool HasModelLoaded(int id) { return ms_aInfoForModel[id].m_loadState==STREAMSTATE_LOADED; }
    static void RequestModel(int id,int flags) {
        auto &info=ms_aInfoForModel[id]; order.push_back(id); info.flags|=flags;
        if(info.m_loadState==STREAMSTATE_NOTLOADED) { info.m_loadState=STREAMSTATE_INQUEUE; ms_numModelsRequested++; }
    }
};
bool CStreaming::ms_disableStreaming; int CStreaming::ms_numModelsRequested;
Info CStreaming::ms_aInfoForModel[MODELINFOSIZE];
bool inCut=false,inVehicle=true,lodPending=false,near[MODELINFOSIZE];
bool HasPendingLods() { return lodPending; }
struct CCutsceneMgr { static bool IsCutsceneProcessing() { return inCut; } };
struct { bool m_WideScreenOn=false; } TheCamera;
CVector velocity{0.8f,0,0};
void *FindPlayerVehicle() { return inVehicle ? &inVehicle : nullptr; }
CVector FindPlayerSpeed() { return velocity; } CVector FindPlayerCoors() { return {}; }
bool IsNearModel(int id) { return near[id]; }
uint8 gAheadModels[(MODELINFOSIZE+7)/8]; uint32 gAheadN,gAheadMiss,gAheadDistance;
'''
    start = source.index('enum { STREAM_AHEAD_MODELS')
    code += source[start:source.index('static void\nInsertAheadRequest', start)]
    for result, name in [('bool', 'InStreamingCorridor'), ('void', 'InsertAheadRequest'),
                         ('float', 'StreamingLookAhead'), ('void', 'BuildAheadSet')]:
        code += '\n'+result+'\n'+function(source, name)+'\n'
    code += r'''
CEntity entities[128]; CPtrNode nodes[128]; int used;
void add(int id,float x,float y,float radius=0,int list=ENTITYLIST_BUILDINGS) {
    auto &e=entities[used]; auto &n=nodes[used++]; e.id=id;e.pos={x,y,0};e.radius=radius;
    auto &head=CWorld::GetSector(CWorld::GetSectorIndexX(x),CWorld::GetSectorIndexY(y))->m_lists[list].first;
    n={&e,head};head=&n;
}
int main() {
    add(2,180,0); add(90,100,0); add(91,120,65,25,ENTITYLIST_OBJECTS_OVERLAP);
    add(92,-100,0); add(93,0,200); add(94,260,0);
    for(int id=10;id<40;id++) { add(id,10,0); near[id]=true; CStreaming::ms_aInfoForModel[id].m_loadState=STREAMSTATE_LOADED; }
    BuildAheadSet();
    assert(gAheadDistance==200 && gAheadN==3 && gAheadMiss==3);
    assert((order==std::vector<int>{91,90,2}));
    assert(CStreaming::ms_aInfoForModel[91].flags&STREAMFLAGS_PRIORITY);
    assert(CStreaming::ms_aInfoForModel[90].flags&STREAMFLAGS_PRIORITY);
    assert(!(CStreaming::ms_aInfoForModel[2].flags&STREAMFLAGS_PRIORITY));
    order.clear(); velocity={-0.8f,0,0}; BuildAheadSet();
    assert((order==std::vector<int>{92}));
    assert(!(gAheadModels[2>>3]&(1<<(2&7))));
    order.clear(); CStreaming::ms_numModelsRequested=32;
    velocity={0.8f,0,0}; CStreaming::ms_aInfoForModel[90].flags=0;
    BuildAheadSet(); assert(CStreaming::ms_numModelsRequested==32);
    assert(CStreaming::ms_aInfoForModel[90].flags&STREAMFLAGS_PRIORITY);
    order.clear(); lodPending=true; BuildAheadSet();
    assert(order.empty() && gAheadN==0 && gAheadMiss==0 && gAheadDistance==0);
    lodPending=false; BuildAheadSet(); assert(!order.empty() && gAheadN==3);
    order.clear(); inCut=true; BuildAheadSet(); assert(order.empty() && gAheadN==0);
    inCut=false; velocity={0,0,0}; BuildAheadSet(); assert(gAheadN==0);
    assert(StreamingLookAhead(2)==240 && StreamingLookAhead(0.1f)==95);
    AheadRequest requests[STREAM_AHEAD_MODELS]; int32 count=0;
    for(int id=99;id>=0;id--) InsertAheadRequest(requests,count,id,float(id));
    assert(count==24); for(int i=0;i<count;i++) assert(requests[i].id==i);
    InsertAheadRequest(requests,count,20,0.5f); assert(count==24 && requests[1].id==20);
    InsertAheadRequest(requests,count,20,30); assert(count==24 && requests[1].id==20);
}
'''
    with tempfile.TemporaryDirectory(prefix='revc-prefetch-') as tmp:
        cpp, exe = Path(tmp)/'test.cpp', Path(tmp)/'test'
        cpp.write_text(code)
        subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS: driving prediction waits for basic LODs; bounds, queue limits and direction changes')


if __name__ == '__main__':
    main()
