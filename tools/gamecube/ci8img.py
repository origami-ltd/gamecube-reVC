#!/usr/bin/env python3
"""Run ci8pack's lossless RGB5A3->CI8 compaction over every .txd inside a gta3.img.
usage: ci8img.py <in.img> <in.dir> <out.img> <out.dir>"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ci8pack
SECTOR = 2048
in_img, in_dir, out_img, out_dir = sys.argv[1:5]
dirb = open(in_dir, 'rb').read(); img = open(in_img, 'rb')
out = open(out_img, 'wb'); dout = bytearray(); pos = 0
files = converted = saved = 0
for i in range(len(dirb) // 32):
    off, sz = struct.unpack_from('<II', dirb, i * 32); name = dirb[i * 32 + 8:i * 32 + 32].split(b'\0')[0].decode('ascii', 'ignore')
    img.seek(off * SECTOR); data = img.read(sz * SECTOR)
    if name.lower().endswith('.txd'):
        t, s, v = struct.unpack_from('<III', data, 0)
        body = data[:12 + s]
        try:
            new, count, delta, colors = ci8pack.convert_bytes(body)
        except Exception as e:
            new, count, delta = body, 0, 0
        if count:
            files += 1; converted += count; saved += delta; data = new
    nsec = (len(data) + SECTOR - 1) // SECTOR
    out.write(data); out.write(b'\0' * (nsec * SECTOR - len(data)))
    dout += struct.pack('<II', pos, nsec) + name.encode('ascii')[:24].ljust(24, b'\0'); pos += nsec
out.close(); open(out_dir, 'wb').write(dout)
print(f"ci8: {converted} textures in {files} txds, {saved // 1024}K saved, archive {pos * SECTOR // (1024 * 1024)}MB")
