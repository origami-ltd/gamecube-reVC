#!/usr/bin/env python3
"""Convert the map-object DFFs of a disc root's gta3.img to GameCube-native geometry.

Map objects and vehicles (IDE objs/tobj/cars) go through dffnative.convert_bytes;
every other entry (TXD, COL, IFP, peds, weapons) is copied byte for byte. Entry
order is kept, so the archive's spatial layout survives; offsets are repacked.
The console reads the result with gxReadNativeGeometry (B178): the geometry
arrives already packed, so the float blocks that failed on a fragmented heap
never get allocated. models/frontend_gcc.dff (the Gamepad Settings pad) is
converted too and written beside the archive.

usage: nativeimg.py --root DISC_ROOT --out OUT_DIR [--report report.json]
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import dffnative

SECTOR = 2048


def read_dir(path):
    data = path.read_bytes()
    if len(data) % 32:
        raise ValueError(f"bad directory size: {path}")
    return [(offset, size, name.split(b'\0')[0].decode('ascii'))
            for offset, size, name in struct.iter_unpack('<II24s', data)]


def map_objects(root):
    """Model names from the objs/tobj/cars sections of every IDE under data/."""
    names = set()
    for ide in sorted((root/'data').rglob('*.ide')):
        section = None
        for line in ide.read_text(encoding='latin1').splitlines():
            line = line.split('#', 1)[0].strip().lower()
            if line in ('objs', 'tobj', 'cars', 'peds', 'weap', 'hier', 'path', '2dfx', 'txdp'):
                section = line
            elif line == 'end':
                section = None
            elif section in ('objs', 'tobj', 'cars') and line:   # B186: vehicles too — their 24-40K float read blocks failed under load
                fields = [f.strip() for f in line.split(',')]
                if len(fields) >= 2:
                    names.add(fields[1] + '.dff')
    return names


def convert(root, out):
    entries = read_dir(root/'models/gta3.dir')
    objects = map_objects(root)
    total = dffnative.Stats()
    src = (root/'models/gta3.img').open('rb')
    out.mkdir(parents=True, exist_ok=True)
    img_path, dir_path = out/'gta3.img', out/'gta3.dir'
    if img_path.exists() or dir_path.exists():
        raise SystemExit(f"refusing to overwrite {out}")
    rows, cursor, converted_files = [], 0, 0
    with img_path.open('wb') as dst:
        for offset, size, name in entries:
            src.seek(offset*SECTOR)
            raw = src.read(size*SECTOR)
            if len(raw) != size*SECTOR:
                raise SystemExit(f"truncated entry {name}")
            data = raw
            if name.lower() in objects:
                new, stats = dffnative.convert_bytes(raw)
                total.add(stats)
                if stats.converted and not stats.failed:
                    dffnative.verify_bytes(new)
                    data = new + b'\0'*(-len(new) % SECTOR)
                    converted_files += 1
            dst.write(data)
            sectors = len(data)//SECTOR
            rows.append(struct.pack('<II24s', cursor, sectors, name.encode('ascii')))
            cursor += sectors
    dir_path.write_bytes(b''.join(rows))
    src.close()
    # The Gamepad Settings pad is a loose DFF with the same 100K+ float blocks
    # (decimated by decimate_pad.py; native it is ~130K in two blocks).
    pad = root/'models/frontend_gcc.dff'
    if pad.exists():
        new, stats = dffnative.convert_bytes(pad.read_bytes())
        if stats.converted and not stats.failed:
            dffnative.verify_bytes(new)
            (out/'frontend_gcc.dff').write_bytes(new)
            converted_files += 1
    return {
        'entries': len(entries), 'map_object_names': len(objects), 'converted_files': converted_files,
        'before_bytes': sum(size for _, size, _ in entries)*SECTOR, 'after_bytes': cursor*SECTOR,
        'img_sha256': hashlib.sha256(img_path.read_bytes()).hexdigest(),
        'stats': total.__dict__,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    report = convert(args.root, args.out)
    text = json.dumps(report, indent=2)
    if args.report:
        args.report.write_text(text + '\n')
    print(text)


if __name__ == '__main__':
    main()
