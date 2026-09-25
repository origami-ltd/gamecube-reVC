#!/usr/bin/env python3
"""Exercise model residency during cloning, fading, and per-model request distances."""
from pathlib import Path
import subprocess
import tempfile
from test_runtime_guards import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    simple = (ROOT/'src/modelinfo/SimpleModelInfo.cpp').read_text()
    entity = (ROOT/'src/entities/Entity.cpp').read_text()
    renderer = (ROOT/'src/renderer/Renderer.cpp').read_text()
    code = r'''
#include <cassert>
#include <cstdint>
#include <cmath>
using uint8 = uint8_t; using uint16 = uint16_t; using uint32 = uint32_t; using int32 = int32_t;
#define GTA_OGC 1
#define nil nullptr
#define PUSH_MEMID(x)
#define POP_MEMID()
enum { rpATOMIC = 1, rpCLUMP = 2, MITYPE_SIMPLE = 0, MITYPE_TIME = 1,
       STREAM_DISTANCE = 30, FADE_DISTANCE = 20 };
static int frameCounter, refs, atomicCount, frameCount, failStage, gBuildings;
static bool sourceEvicted;
struct CTimer { static int GetFrameCounter() { return frameCounter; } };
struct RwMatrix { int value; };
struct RwFrame { RwMatrix matrix; };
struct RwObject { int type = rpATOMIC; RwFrame *frame = nullptr; };
using RpAtomic = RwObject; using RpClump = RwObject;
RpAtomic *RpAtomicClone(RpAtomic*) {
    if(refs == 0) sourceEvicted = true;
    if(failStage == 1) return nullptr;
    atomicCount++; return new RpAtomic;
}
void RpAtomicDestroy(RpAtomic *a) { atomicCount--; delete a; }
RwFrame *RwFrameCreate() { if(failStage == 2) return nullptr; frameCount++; return new RwFrame; }
void RwFrameDestroy(RwFrame *f) { frameCount--; delete f; }
RwMatrix *RwFrameGetMatrix(RwFrame *f) { return &f->matrix; }
void RpAtomicSetFrame(RpAtomic *a,RwFrame *f) { a->frame = f; }
RwFrame *RpAtomicGetFrame(RpAtomic *a) { return a->frame; }
RwFrame *RpClumpGetFrame(RpClump *a) { return a->frame; }
int RwObjectGetType(RwObject *o) { return o->type; }
struct CSimpleModelInfo {
    RpAtomic *m_atomics[3]; float m_lodDistances[3] = {200,0,0};
    uint8 m_alpha, m_numAtomics; uint16 m_alphaFrame;
    int m_firstDamaged, m_wetRoadReflection, m_isDamaged, m_isBigBuilding, m_noFade,
        m_drawLast, m_additive, m_isSubway, m_ignoreLight, m_noZwrite, m_noShadows,
        m_ignoreDrawDist, m_isCodeGlass, m_isArtistGlass;
    void Init(); void IncreaseAlpha(); float GetLargestLodDistance();
    RwObject *CreateInstance(); RwObject *CreateInstance(RwMatrix*);
    void AddRef() { refs++; } void RemoveRef() { refs--; }
    int GetModelType() { return MITYPE_SIMPLE; }
};
struct CTimeModelInfo : CSimpleModelInfo { int GetTimeOn() { return 0; } int GetTimeOff() { return 24; } };
using CBaseModelInfo = CSimpleModelInfo;
static CTimeModelInfo model;
struct CModelInfo { static CSimpleModelInfo *GetModelInfo(int) { return &model; } };
struct CMatrix { void AttachRW(RwMatrix*,bool) {} };
struct CVector { float x,y,z; CVector operator-(const CVector &o) const { return {x-o.x,y-o.y,z-o.z}; }
    float Magnitude() { return std::sqrt(x*x+y*y+z*z); } };
struct CEntity {
    int m_modelIndex = 0, m_area = 0; RwObject *m_rwObject = nullptr; CMatrix matrix; CVector position;
    void CreateRwObject(); bool IsBuilding() { return true; } CMatrix &GetMatrix() { return matrix; }
    int GetModelIndex() { return m_modelIndex; } CVector GetPosition() { return position; }
};
struct CRenderer { static bool ShouldModelBeStreamed(CEntity*, const CVector&); };
struct CStreaming { static void NoteModelDistance(int,float) {} };
struct CClock { static bool GetIsTimeInRange(int,int) { return true; } };
static bool IsAreaVisible(int area) { return area == 0; }
static struct { float LODDistMultiplier = 1; } TheCamera;
'''
    for src, result, name in [
        (simple, 'void', 'CSimpleModelInfo::Init'),
        (simple, 'void', 'CSimpleModelInfo::IncreaseAlpha'),
        (simple, 'float', 'CSimpleModelInfo::GetLargestLodDistance'),
        (entity, 'void', 'CEntity::CreateRwObject'),
        (renderer, 'bool', 'CRenderer::ShouldModelBeStreamed'),
    ]:
        code += '\n'+result+'\n'+function(src, name)+'\n'
    for signature in ('CSimpleModelInfo::CreateInstance(void)', 'CSimpleModelInfo::CreateInstance(RwMatrix *matrix)'):
        code += '\nRwObject*\n'+function(simple[simple.index(signature):], 'CSimpleModelInfo::CreateInstance')+'\n'
    code += r'''
int main() {
    model.Init(); model.m_numAtomics = 1; RpAtomic source; model.m_atomics[0] = &source;
    for(int failure : {0,1,2}) {
        failStage = failure; CEntity e; e.CreateRwObject();
        assert(!sourceEvicted);
        if(failure) assert(!e.m_rwObject && refs == 0 && atomicCount == 0 && frameCount == 0);
        else {
            assert(refs == 1 && e.m_rwObject && atomicCount == 1);
            RwFrameDestroy(e.m_rwObject->frame); RpAtomicDestroy(e.m_rwObject); model.RemoveRef();
        }
    }
    failStage = 0; RwMatrix matrix {123}; model.AddRef();
    auto a = (RpAtomic*)model.CreateInstance(&matrix);
    assert(a->frame->matrix.value == 123); RwFrameDestroy(a->frame); RpAtomicDestroy(a); model.RemoveRef();
    assert(refs == 0 && atomicCount == 0 && frameCount == 0);
    for(int frame = 0; frame < 16; frame++) {
        frameCounter = frame;
        for(int instance = 0; instance < 50; instance++) model.IncreaseAlpha();
        assert(model.m_alpha == (frame < 15 ? (frame+1)*16 : 255));
    }
    model.m_alpha = 0; frameCounter = 20; model.IncreaseAlpha(); assert(model.m_alpha == 16);
    model.m_alphaFrame = 65535; frameCounter = 65536; model.IncreaseAlpha(); assert(model.m_alpha == 32);
    CEntity e; CVector camera {0,0,0}; e.position = {150,0,0};
    assert(CRenderer::ShouldModelBeStreamed(&e,camera));
    e.position.x = 249; assert(CRenderer::ShouldModelBeStreamed(&e,camera));
    e.position.x = 251; assert(!CRenderer::ShouldModelBeStreamed(&e,camera));
    e.position.x = 235; model.m_noFade = 1; assert(!CRenderer::ShouldModelBeStreamed(&e,camera));
    TheCamera.LODDistMultiplier = 2; assert(CRenderer::ShouldModelBeStreamed(&e,camera));
    e.m_area = 1; assert(!CRenderer::ShouldModelBeStreamed(&e,camera));
}
'''
    code = '#include <initializer_list>\n' + code
    with tempfile.TemporaryDirectory(prefix='revc-model-streaming-') as d:
        cpp, exe = Path(d)/'test.cpp', Path(d)/'test'
        cpp.write_text(code)
        subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS: model lifetime through allocation failures, 50-instance fading, and model draw distances')


if __name__ == '__main__':
    main()
