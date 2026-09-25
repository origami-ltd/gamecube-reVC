#!/usr/bin/env python3
"""Shared texture pool for the GameCube archive.

52% of the texel bytes in gta3.img are exact duplicates across TXDs (white128a
x56, road2_256 x38, ...). This tool moves the most-repeated textures into one
resident dictionary (models/shared.txd, loaded once at boot, pinned in ARAM)
and replaces every copy in the archive with a 100-byte reference chunk: the
GX-native header with bit 31 of filterAddressing set, the texel size, and the
64-bit content hash the runtime already computes for ARAM sharing
(gxraster.cpp readNativeTexture). Nothing is re-encoded: same texels, same
formats, same dimensions -- only fewer disc bytes and DMAs per area.

usage: sharedpool.py <in.img> <in.dir> <out.img> <out.dir> <shared.txd> [--pool-kb 2048] [--min-count 2] [--dry-run]
"""
import argparse, os, struct, sys

SECTOR = 2048
FNV_OFF, FNV_PRIME, M64 = 14695981039346656037, 1099511628211, (1 << 64) - 1
REF_FLAG = 0x80000000


def rhash(texels, size, gxfmt, tw, th):
    """Bit-exact twin of gxHashInit/gxHashBytes + the size/fmt/dims mix in readNativeTexture."""
    h = FNV_OFF
    for i in range(0, len(texels), 4):
        h = ((h ^ texels[i]) * FNV_PRIME) & M64
    return h ^ (((size << 40) ^ (gxfmt << 56) ^ (tw << 16) ^ th) & M64)


def chunks(b, p, end):
    while p + 12 <= end:
        t, s, v = struct.unpack_from('<III', b, p)
        yield t, p, p + 12, min(p + 12 + s, end), v
        p += 12 + s


def parse_txd(data):
    """-> (version, [(chunk_start, chunk_end, struct_payload_start, hdr, size, texels)...], count_pos)"""
    t, s, ver = struct.unpack_from('<III', data, 0)
    if t != 0x16:
        return None
    texs = []
    count_pos = None
    for tt, cs, a, e, v in chunks(data, 12, 12 + s):
        if tt == 0x01 and count_pos is None:
            count_pos = a
        elif tt == 0x15:
            for t2, cs2, a2, e2, v2 in chunks(data, a, e):
                if t2 == 0x01:
                    hdr = data[a2:a2 + 88]
                    size = struct.unpack_from('<I', data, a2 + 88)[0]
                    texels = data[a2 + 92:a2 + 92 + size]
                    texs.append((cs, e, a2, hdr, size, texels))
                    break
    return ver, texs, count_pos


def hdr_fields(hdr):
    fa = struct.unpack_from('<I', hdr, 4)[0]
    name = hdr[8:40].split(b'\0')[0].decode('ascii', 'ignore')
    tw, th = struct.unpack_from('<HH', hdr, 80)
    return fa, name, tw, th, hdr[87]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('in_img'); ap.add_argument('in_dir'); ap.add_argument('out_img'); ap.add_argument('out_dir'); ap.add_argument('shared_txd')
    ap.add_argument('--pool-kb', type=int, default=2048)
    ap.add_argument('--min-count', type=int, default=2)
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()

    dirb = open(a.in_dir, 'rb').read()
    entries = []
    for i in range(len(dirb) // 32):
        off, sz = struct.unpack_from('<II', dirb, i * 32)
        name = dirb[i * 32 + 8:i * 32 + 32].split(b'\0')[0].decode('ascii', 'ignore')
        entries.append((name, off, sz))
    img = open(a.in_img, 'rb')

    # pass 1: hash every native texture in every TXD
    occ = {}          # key -> [(entry_index, tex_index)]
    info = {}         # key -> (size, name, tw, th, fmt)
    parsed = {}       # entry_index -> (data, version, texs, count_pos)
    version = None
    for ei, (name, off, sz) in enumerate(entries):
        if not name.lower().endswith('.txd'):
            continue
        img.seek(off * SECTOR); data = img.read(sz * SECTOR)
        r = parse_txd(data)
        if r is None:
            continue
        ver, texs, count_pos = r
        version = version or ver
        parsed[ei] = (data, ver, texs, count_pos)
        for ti, (cs, e, a2, hdr, size, texels) in enumerate(texs):
            fa, tname, tw, th, fmt = hdr_fields(hdr)
            if fa & REF_FLAG or len(texels) != size:
                continue
            key = (rhash(texels, size, fmt, tw, th), size, fmt)
            occ.setdefault(key, []).append((ei, ti))
            info.setdefault(key, (size, tname, tw, th, fmt))

    # pass 2: pick the pool by bytes saved
    cands = [(len(v) - 1) * info[k][0] for k, v in occ.items()]
    ranked = sorted(occ.items(), key=lambda kv: -(len(kv[1]) - 1) * info[kv[0]][0])
    pool, pool_bytes, saved = [], 0, 0
    for k, v in ranked:
        if len(v) < a.min_count:
            continue
        size = info[k][0]
        if pool_bytes + size > a.pool_kb * 1024:
            continue
        pool.append(k); pool_bytes += size; saved += (len(v) - 1) * size
    total_tex = sum(info[k][0] * len(v) for k, v in occ.items())
    print(f"textures {sum(len(v) for v in occ.values())} ({total_tex // 1024}K), distinct {len(occ)}; "
          f"pool {len(pool)} textures {pool_bytes // 1024}K; archive bytes saved {saved // 1024}K "
          f"(references {sum(len(occ[k]) for k in pool)})")
    for k in pool[:12]:
        size, tname, tw, th, fmt = info[k]
        print(f"  {tname:24s} {tw}x{th} fmt {fmt:2d} {size // 1024:4d}K x{len(occ[k])}")
    if a.dry_run:
        return

    # pass 3: write shared.txd from the first instance of each pool texture
    poolset = set(pool)
    native_chunks = []
    for k in pool:
        ei, ti = occ[k][0]
        data, ver, texs, count_pos = parsed[ei]
        cs, e, a2, hdr, size, texels = texs[ti]
        native_chunks.append(data[cs:e])
    body = struct.pack("<III", 0x01, 4, version) + struct.pack("<HH", len(native_chunks), 0) + b''.join(native_chunks)
    body += struct.pack('<III', 0x03, 0, version)
    open(a.shared_txd, 'wb').write(struct.pack('<III', 0x16, len(body), version) + body)

    # pass 4: rewrite the archive with reference chunks for pool members
    out = open(a.out_img, 'wb'); dir_out = bytearray(); pos = 0
    refs_written = 0
    for ei, (name, off, sz) in enumerate(entries):
        if ei in parsed:
            data, ver, texs, count_pos = parsed[ei]
            pieces = []; cursor = 0
            for ti, (cs, e, a2, hdr, size, texels) in enumerate(texs):
                fa, tname, tw, th, fmt = hdr_fields(hdr)
                key = (rhash(texels, size, fmt, tw, th), size, fmt) if not (fa & REF_FLAG) and len(texels) == size else None
                if key not in poolset:
                    continue
                # rebuild this 0x15 chunk: struct = hdr(flagged) + size + hash; keep the trailing extension chunk
                struct_start = a2 - 12
                ext_start = a2 + 92 + size
                newhdr = bytearray(hdr); struct.pack_into('<I', newhdr, 4, fa | REF_FLAG)
                payload = bytes(newhdr) + struct.pack('<IQ', size, key[0])
                new_struct = struct.pack('<III', 0x01, len(payload), ver) + payload
                tail = data[ext_start:e]
                new_chunk = struct.pack('<III', 0x15, len(new_struct) + len(tail), ver) + new_struct + tail
                pieces.append(data[cursor:cs]); pieces.append(new_chunk); cursor = e
                refs_written += 1
            pieces.append(data[cursor:])
            newdata = bytearray(b''.join(pieces))
            struct.pack_into('<I', newdata, 4, len(newdata) - 12)          # TXD chunk size
            newdata = bytes(newdata)
        else:
            img.seek(off * SECTOR); newdata = img.read(sz * SECTOR)
        nsec = (len(newdata) + SECTOR - 1) // SECTOR
        out.write(newdata); out.write(b'\0' * (nsec * SECTOR - len(newdata)))
        dir_out += struct.pack('<II', pos, nsec) + name.encode('ascii')[:24].ljust(24, b'\0')
        pos += nsec
    out.close(); open(a.out_dir, 'wb').write(dir_out)
    print(f"wrote {a.out_img} ({pos * SECTOR // (1024 * 1024)}MB), {a.shared_txd} ({(12 + len(body)) // 1024}K), {refs_written} references")


if __name__ == '__main__':
    main()
