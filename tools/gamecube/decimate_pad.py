#!/usr/bin/env python3
"""Decimate the Gamepad Settings 3D pad (gamecube_controller.dff) in place.

The Sketchfab mesh is 14,300 vertices / 25,564 triangles. Decoded on the
console that is a 181K vertex block plus a 149K index block, and neither
contiguous hole exists in a 24MB MEM1 after gameplay: b183 opened Gamepad
Settings from the pause menu and 'controller geometry could not be loaded'
every time. Drawn ~400 px wide, a third of the triangles looks the same.

Quadric (xyz + uv) half-edge collapse. Only interior vertices go: anything on
an index-space border (true borders and UV seams alike) stays, so the texture
never tears; collapses that flip a triangle in 3D or in UV are refused. Only
the Geometry STRUCT and BINMESH are rewritten; every other chunk is copied.

  python3 tools/gamecube/decimate_pad.py IN.dff OUT.dff --triangles 9000
  python3 tools/gamecube/decimate_pad.py --self-test
"""

import argparse
import heapq
import struct
import sys

import numpy as np

sys.path.insert(0, __import__('os').path.dirname(__file__))
from dffnative import (Chunk, _children, _one_chunk, ID_BINMESH, ID_CLUMP,
                       ID_EXTENSION, ID_GEOMETRY, ID_GEOMETRYLIST, ID_STRUCT)

UV_WEIGHT = 1.0   # one UV unit costs as much as one model unit (pad is ~2.6 wide)


def decimate(pos, uv, tris, target):
    """Return triangles (n,3) over the same vertex arrays, <= target if possible."""
    tris = [tuple(t) for t in tris
            if len(set(t)) == 3 and np.linalg.norm(np.cross(pos[t[1]] - pos[t[0]], pos[t[2]] - pos[t[0]])) > 1e-12]
    nv = len(pos)
    x = np.hstack([pos, uv * UV_WEIGHT])
    A = np.zeros((nv, 5, 5)); B = np.zeros((nv, 5)); C = np.zeros(nv)
    vtris = [set() for _ in range(nv)]
    edges = {}
    for i, (a, b, c) in enumerate(tris):
        for v in (a, b, c):
            vtris[v].add(i)
        for e in ((a, b), (b, c), (c, a)):
            k = (min(e), max(e)); edges[k] = edges.get(k, 0) + 1
        p, q, r = x[a], x[b], x[c]
        e1 = q - p; e1 /= np.linalg.norm(e1) or 1
        e2 = r - p; e2 -= e1 * (e2 @ e1); n2 = np.linalg.norm(e2)
        if n2 < 1e-12:
            continue
        e2 /= n2
        area = 0.5 * np.linalg.norm(np.cross(pos[b] - pos[a], pos[c] - pos[a]))
        Aq = np.eye(5) - np.outer(e1, e1) - np.outer(e2, e2)
        bq = (p @ e1) * e1 + (p @ e2) * e2 - p
        cq = p @ p - (p @ e1) ** 2 - (p @ e2) ** 2
        for v in (a, b, c):
            A[v] += area * Aq; B[v] += area * bq; C[v] += area * cq
    locked = np.zeros(nv, bool)
    for (a, b), n in edges.items():
        if n != 2:
            locked[a] = locked[b] = True
    alive = [True] * len(tris)
    count = len(tris)

    def neighbours(u):
        return {w for t in vtris[u] for w in tris[t] if w != u}

    def valid(u, v):
        # link condition: an interior edge shares exactly two neighbours
        if len(neighbours(u) & neighbours(v)) != 2:
            return False
        for t in vtris[u]:
            if v in tris[t]:
                continue
            a, b, c = tris[t]
            na = [v if w == u else w for w in (a, b, c)]
            n0 = np.cross(pos[b] - pos[a], pos[c] - pos[a])
            n1 = np.cross(pos[na[1]] - pos[na[0]], pos[na[2]] - pos[na[0]])
            if n0 @ n1 <= 0.2 * np.linalg.norm(n0) * np.linalg.norm(n1):
                return False
            u0 = np.cross(np.append(uv[b] - uv[a], 0), np.append(uv[c] - uv[a], 0))[2]
            u1 = np.cross(np.append(uv[na[1]] - uv[na[0]], 0), np.append(uv[na[2]] - uv[na[0]], 0))[2]
            if u0 * u1 <= 0:
                return False
        return True

    def best(u):
        Aq, bq, cq, out = None, None, None, (None, None)
        for v in neighbours(u):
            Aq = A[u] + A[v]; bq = B[u] + B[v]; cq = C[u] + C[v]
            e = x[v] @ Aq @ x[v] + 2 * bq @ x[v] + cq
            if out[0] is None or e < out[0]:
                out = (e, v)
        return out

    version = [0] * nv
    heap = []
    for u in range(nv):
        if not locked[u] and vtris[u]:
            e, v = best(u)
            if v is not None:
                heap.append((e, u, 0))
    heapq.heapify(heap)
    while heap and count > target:
        e, u, ver = heapq.heappop(heap)
        if ver != version[u] or locked[u] or not vtris[u]:
            continue
        cands = sorted(((x[v] @ (A[u] + A[v]) @ x[v] + 2 * (B[u] + B[v]) @ x[v] + C[u] + C[v], v)
                        for v in neighbours(u)))
        v = next((v for _, v in cands if valid(u, v)), None)
        if v is None:
            continue
        for t in list(vtris[u]):
            if v in tris[t]:
                alive[t] = False; count -= 1
                for w in tris[t]:
                    vtris[w].discard(t)
            else:
                tris[t] = tuple(v if w == u else w for w in tris[t])
                vtris[v].add(t)
        vtris[u].clear()
        A[v] += A[u]; B[v] += B[u]; C[v] += C[u]
        for w in neighbours(v) | {v}:
            if not locked[w] and vtris[w]:
                version[w] += 1
                e, z = best(w)
                if z is not None:
                    heapq.heappush(heap, (e, w, version[w]))
    return np.array([t for t, ok in zip(tris, alive) if ok], dtype=np.int64)


def read_geometry(struct_payload):
    flags, nt, nv, nm = struct.unpack_from('<Iiii', struct_payload)
    if nm != 1 or flags & 0x01000008 or ((flags >> 16) & 0xFF) != 1:
        raise SystemExit('expected one morph target, one UV set, not prelit, not native')
    at = 16
    uv = np.frombuffer(struct_payload, '<f4', nv * 2, at).reshape(nv, 2).astype(np.float64); at += nv * 8
    tb = np.frombuffer(struct_payload, '<u4', nt * 2, at).reshape(nt, 2); at += nt * 8
    tris = np.stack([tb[:, 0] >> 16, tb[:, 0] & 0xFFFF, tb[:, 1] >> 16], 1).astype(np.int64)
    sphere = struct_payload[at:at + 16]; at += 16
    has_v, has_n = struct.unpack_from('<ii', struct_payload, at); at += 8
    if not (has_v and has_n):
        raise SystemExit('expected vertices and normals')
    pos = np.frombuffer(struct_payload, '<f4', nv * 3, at).reshape(nv, 3).astype(np.float64); at += nv * 12
    nrm = np.frombuffer(struct_payload, '<f4', nv * 3, at).reshape(nv, 3).astype(np.float64)
    return flags, pos, nrm, uv, tris, sphere


def write_geometry(flags, pos, nrm, uv, tris, sphere):
    used = np.unique(tris)
    remap = np.full(len(pos), -1, np.int64); remap[used] = np.arange(len(used))
    t = remap[tris]
    nv, nt = len(used), len(t)
    out = struct.pack('<Iiii', flags, nt, nv, 1)
    out += uv[used].astype('<f4').tobytes()
    tb = np.stack([(t[:, 0] << 16) | t[:, 1], t[:, 2] << 16], 1).astype('<u4')
    out += tb.tobytes() + sphere + struct.pack('<ii', 1, 1)
    out += pos[used].astype('<f4').tobytes() + nrm[used].astype('<f4').tobytes()
    idx = t.reshape(-1).astype('<u4')
    binmesh = struct.pack('<III', 0, 1, len(idx)) + struct.pack('<II', len(idx), 0) + idx.tobytes()
    return out, binmesh, nv, nt


def rebuild(chunk, fn):
    """Depth-first copy of the chunk tree; fn(chunk, parent_id) may replace a leaf."""
    containers = (ID_CLUMP, ID_GEOMETRYLIST, ID_GEOMETRY, ID_EXTENSION)
    kids = _children(chunk.payload) if chunk.chunk_id in containers else None
    if kids is None:
        return chunk
    new = []
    for k in kids:
        k = fn(k, chunk.chunk_id) or rebuild(k, fn)
        new.append(k)
    return Chunk(chunk.chunk_id, chunk.library_id, b''.join(k.encode() for k in new))


def convert(data, target):
    top, _ = _one_chunk(data)
    geo = next(k for k in _children(next(k for k in _children(top.payload) if k.chunk_id == ID_GEOMETRYLIST).payload)
               if k.chunk_id == ID_GEOMETRY)
    st = next(k for k in _children(geo.payload) if k.chunk_id == ID_STRUCT)
    flags, pos, nrm, uv, tris, sphere = read_geometry(st.payload)
    kept = decimate(pos, uv, tris, target)
    new_struct, new_binmesh, nv, nt = write_geometry(flags, pos, nrm, uv, kept, sphere)

    def fn(k, parent):
        if k.chunk_id == ID_STRUCT and parent == ID_GEOMETRY:
            return Chunk(k.chunk_id, k.library_id, new_struct)
        if k.chunk_id == ID_BINMESH:
            return Chunk(k.chunk_id, k.library_id, new_binmesh)
        return None
    print('pad: %d verts %d tris -> %d verts %d tris' % (len(pos), len(tris), nv, nt))
    return rebuild(top, fn).encode()


def self_test():
    # 20x20 flat grid with a UV seam down the middle: the seam and the rim must
    # survive, the interior must collapse, nothing may flip.
    n = 20
    pos, uv, ids = [], [], {}
    for j in range(n + 1):
        for i in range(n + 1):
            for side in ((0, 1) if i == n // 2 else ((0,) if i < n // 2 else (1,))):
                ids[(i, j, side)] = len(pos)
                pos.append((i / n, j / n, 0.0)); uv.append((i / n * 0.5 + side * 0.5, j / n))
    tris = []
    for j in range(n):
        for i in range(n):
            side = 0 if i < n // 2 else 1
            a, b = ids[(i, j, side)], ids[(i + 1, j, side)]
            c, d = ids[(i, j + 1, side)], ids[(i + 1, j + 1, side)]
            tris += [(a, b, d), (a, d, c)]
    pos, uv = np.array(pos), np.array(uv)
    out = decimate(pos, uv, np.array(tris), 200)
    assert len(out) <= 400 and len(out) < len(tris), len(out)
    kept = set(out.reshape(-1))
    for (i, j, side), v in ids.items():
        if i in (0, n, n // 2) or j in (0, n):
            assert v in kept, ('border/seam vertex lost', i, j, side)
    for a, b, c in out:
        assert np.cross(pos[b] - pos[a], pos[c] - pos[a])[2] > 0, 'flipped'
    print('self-test ok: %d -> %d triangles' % (len(tris), len(out)))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('src', nargs='?'); ap.add_argument('dst', nargs='?')
    ap.add_argument('--triangles', type=int, default=9000)
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        self_test(); sys.exit()
    data = open(a.src, 'rb').read()
    open(a.dst, 'wb').write(convert(data, a.triangles))
