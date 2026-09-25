#!/usr/bin/env python3
"""Check that occluder order cannot hide visible entities or water sectors."""
from pathlib import Path
import subprocess
import tempfile
from test_runtime_guards import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT/'src/renderer/Occlusion.cpp').read_text()
    code = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
struct CVector {
    float x = 0, y = 0, z = 0;
    CVector() = default;
    CVector(float a, float b, float c): x(a), y(b), z(c) {}
    float Magnitude() const { return std::sqrt(x*x + y*y + z*z); }
};
float Max(float a, float b) { return std::max(a, b); }
static bool projectable = true;
bool CalcScreenCoors(CVector p, CVector *out, float *w = nullptr, float *h = nullptr) {
    *out = p;
    if(w) *w = 1;
    if(h) *h = 1;
    return projectable;
}
struct CActiveOccluder {
    float left, right, bottom, top, radius;
    bool IsPointWithinOcclusionArea(float x, float y, float area) {
        return x-area >= left && x+area <= right && y-area >= bottom && y+area <= top;
    }
};
struct COcclusion {
    static int NumActiveOccluders;
    static CActiveOccluder aActiveOccluders[3];
    static bool IsAABoxOccluded(CVector, float, float, float);
};
int COcclusion::NumActiveOccluders;
CActiveOccluder COcclusion::aActiveOccluders[3];
struct ColModel { struct { CVector min, max; } boundingBox; };
static ColModel col = {{CVector(-10,-10,95), CVector(10,10,105)}};
struct CModelInfo { static ColModel *GetColModel(int) { return &col; } };
struct CMatrix { CVector operator*(CVector p) { return p; } };
struct CEntity {
    int m_modelIndex = 0;
    CMatrix m_matrix;
    CVector GetBoundCentre() { return CVector(0,0,100); }
    float GetBoundRadius() { return 15; }
    bool IsEntityOccluded();
};
'''
    for name in ['COcclusion::IsAABoxOccluded', 'CEntity::IsEntityOccluded']:
        code += '\nbool '+function(source, name)+'\n'
    code += r'''
int main() {
    CEntity entity;
    auto box = [] { return COcclusion::IsAABoxOccluded(CVector(0,0,100),20,20,10); };
    auto &o = COcclusion::aActiveOccluders;
    COcclusion::NumActiveOccluders = 2;
    o[0] = {-1,1,-1,1,10};
    o[1] = {-30,5,-30,5,10};
    assert(!entity.IsEntityOccluded());
    assert(!box());
    std::swap(o[0],o[1]);
    assert(!entity.IsEntityOccluded());
    assert(!box());
    o[1] = {-40,40,-40,40,10};
    assert(entity.IsEntityOccluded() && box());
    o[1].radius = 200;
    assert(!entity.IsEntityOccluded() && !box());
    projectable = false;
    assert(!entity.IsEntityOccluded() && !box());
    projectable = true;
    COcclusion::NumActiveOccluders = 0;
    assert(!entity.IsEntityOccluded() && !box());
}
'''
    with tempfile.TemporaryDirectory(prefix='revc-occlusion-') as d:
        cpp, exe = Path(d)/'test.cpp', Path(d)/'test'
        cpp.write_text(code)
        subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS: visible entities and water remain visible in either occluder order')


if __name__ == '__main__':
    main()
