#!/usr/bin/env python3
"""Repack gta3.img with GameCube-native textures.

Every .txd in the archive is run through txdconv, which converts a PC (D3D8)
texture dictionary into a GameCube one whose rasters are already tiled. Every
other entry is copied through unchanged. Offsets are recomputed because the
converted dictionaries are a different size.

The point is what the console then does *not* do. Converting at stream time
keeps a D3D raster, a full RGBA8 Image and an RGBA8 staging buffer live at once
for every texture loaded, and that churn is what shatters the heap: measured in
gameplay, 7.1MB free split across 9765 chunks, averaging 745 bytes, at 4fps.
Allocation fails on fragmentation long before it fails on capacity. With the
conversion moved here, loading a TXD is a read into a correctly sized buffer.

Modelled on dca3's imgtool (https://gitlab.com/skmp/dca3-game).

Usage: repack_img.py [--exclude-list FILE] [--max-dim N]
                     <in.img> <in.dir> <out.img> <out.dir> <txdconv>

--exclude-list drops entries whose base name (without extension) appears in
FILE, one per line. Intended for textures nothing references: 411 of the 1361
TXDs in the archive appear in no .ide, and after removing the ones loaded by
name from code, by cutscene, or from an .ipl, 234 remain — 6.7MB. Excluding
rather than deleting keeps the decision reversible: build both archives, swap
the file, compare.

Static analysis cannot prove a texture is unused, because a name can be built
at runtime by string concatenation. Treat the list as a hypothesis to test by
looking for missing textures, not as a fact.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

SECTOR = 2048
ENTRY = 32


def read_dir(path):
    data = open(path, 'rb').read()
    out = []
    for i in range(len(data) // ENTRY):
        off, size, raw = struct.unpack_from('<II24s', data, i * ENTRY)
        name = raw.split(b'\0')[0].decode('latin1')
        out.append((name, off, size))
    return out


def sectors(n):
    return (n + SECTOR - 1) // SECTOR


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exclude-list', help='file of base names to drop')
    ap.add_argument('--max-dim', type=int,
                    help='cap the largest texture axis, passed to txdconv')
    ap.add_argument('in_img'); ap.add_argument('in_dir')
    ap.add_argument('out_img'); ap.add_argument('out_dir')
    ap.add_argument('txdconv')
    args = ap.parse_args()
    in_img, in_dir = args.in_img, args.in_dir
    out_img, out_dir, txdconv = args.out_img, args.out_dir, args.txdconv

    excluded = set()
    if args.exclude_list:
        for line in open(args.exclude_list):
            line = line.strip().lower()
            if line and not line.startswith('#'):
                excluded.add(os.path.splitext(line)[0])

    entries = read_dir(in_dir)
    src = open(in_img, 'rb')
    dst = open(out_img, 'wb')
    newdir = bytearray()

    tmp = tempfile.mkdtemp(prefix='repack-')
    cursor = 0
    converted = failed = copied = 0
    bytes_before = bytes_after = 0

    dropped = dropped_bytes = 0
    for name, off, size in entries:
        if os.path.splitext(name)[0].lower() in excluded:
            dropped += 1
            dropped_bytes += size * SECTOR
            continue
        src.seek(off * SECTOR)
        payload = src.read(size * SECTOR)

        if name.lower().endswith('.txd'):
            a = os.path.join(tmp, 'in.txd')
            b = os.path.join(tmp, 'out.txd')
            with open(a, 'wb') as f:
                f.write(payload)
            cmd = [txdconv]
            if args.max_dim:
                cmd += ['--max-dim', str(args.max_dim)]
            cmd += [a, b]
            r = subprocess.run(cmd,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
            if r.returncode == 0 and os.path.exists(b) and os.path.getsize(b) > 0:
                new = open(b, 'rb').read()
                # A dictionary that fails to convert is copied through rather
                # than dropped: a missing texture is a worse outcome than one
                # that still has to be converted at load time.
                bytes_before += len(payload)
                bytes_after += len(new)
                payload = new
                converted += 1
            else:
                failed += 1
        else:
            copied += 1

        nsec = sectors(len(payload))
        padded = payload + b'\0' * (nsec * SECTOR - len(payload))
        dst.write(padded)
        newdir += struct.pack('<II24s', cursor, nsec,
                              name.encode('latin1')[:23].ljust(24, b'\0'))
        cursor += nsec

    src.close()
    dst.close()
    open(out_dir, 'wb').write(bytes(newdir))

    print('entries      : %d' % len(entries))
    print('dropped      : %d (%.1f MB) from the exclude list'
          % (dropped, dropped_bytes / 1048576.0))
    print('txd converted: %d' % converted)
    print('txd failed   : %d (copied through unchanged)' % failed)
    print('other copied : %d' % copied)
    if bytes_before:
        print('txd bytes    : %.1fMB -> %.1fMB' %
              (bytes_before / 1048576.0, bytes_after / 1048576.0))
    print('image size   : %.1fMB' % (cursor * SECTOR / 1048576.0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
