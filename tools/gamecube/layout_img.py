#!/usr/bin/env python3
"""Group an existing IMG by texture dependencies and map location, without conversion."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import statistics
import struct

SECTOR = 2048


def read_entries(path):
    data = path.read_bytes()
    if len(data) % 32:
        raise ValueError(f"Invalid directory size: {path}")
    return [(name.split(b'\0')[0].decode('ascii').lower(), offset, size)
            for offset, size, name in struct.iter_unpack('<II24s', data)]


def records(path, sections):
    section = None
    for line in path.read_text(encoding='latin1').splitlines():
        line = line.split('#', 1)[0].strip().lower()
        if line in sections:
            section = line
        elif line == 'end':
            section = None
        elif section and line:
            yield [value.strip() for value in line.split(',')]


def morton(x, y):
    x = max(0, min(65535, int((x + 3000) / 64)))
    y = max(0, min(65535, int((y + 3000) / 64)))
    return sum(((x >> bit) & 1) << (2*bit) |
               ((y >> bit) & 1) << (2*bit+1) for bit in range(16))


def dependencies(root):
    textures, locations = {}, defaultdict(list)
    for path in sorted((root/'data').rglob('*.ide')):
        for row in records(path, {'objs', 'tobj', 'cars', 'peds', 'weap', 'hier'}):
            if len(row) >= 3:
                textures[row[1] + '.dff'] = row[2] + '.txd'
    for path in sorted((root/'data').rglob('*.ipl')):
        for row in records(path, {'inst'}):
            if len(row) >= 6:
                locations[row[1] + '.dff'].append((float(row[3]), float(row[4])))
    return textures, locations


def order_entries(entries, textures, locations):
    existing = {name for name, _, _ in entries}
    groups = defaultdict(list)
    for index, (name, _, _) in enumerate(entries):
        txd = textures.get(name, name.rsplit('.', 1)[0] + '.txd')
        groups[txd if txd in existing else name].append(index)

    def group_key(item):
        name, members = item
        points = [point for index in members for point in locations.get(entries[index][0], [])]
        if not points:
            return (0, 0, name)
        x = statistics.median(p[0] for p in points)
        y = statistics.median(p[1] for p in points)
        return (1, morton(x, y), name)

    order = []
    for txd, members in sorted(groups.items(), key=group_key):
        order.extend(sorted(members, key=lambda index: (entries[index][0] != txd, entries[index][0], index)))
    assert len(order) == len(entries) == len(set(order))
    return order


def dependency_distances(entries, textures):
    offsets = {}
    for name, offset, _ in entries:
        offsets.setdefault(name, offset)
    distances = [abs(offsets[model] - offsets[txd])*SECTOR
                 for model, txd in textures.items() if model in offsets and txd in offsets]
    return {'pairs': len(distances), 'median_bytes': statistics.median(distances),
            'mean_bytes': statistics.mean(distances), 'max_bytes': max(distances)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    source = args.root/'models/gta3.img'
    entries = read_entries(args.root/'models/gta3.dir')
    textures, locations = dependencies(args.root)
    order = order_entries(entries, textures, locations)
    args.out.mkdir(parents=True, exist_ok=True)
    target = args.out/'gta3.img'
    if source.resolve() == target.resolve() or target.exists():
        raise ValueError(f"Refusing to overwrite archive: {target}")
    rewritten, manifest, cursor = [], [], 0
    with source.open('rb') as src, target.open('wb') as dst:
        for index in order:
            name, offset, size = entries[index]
            src.seek(offset*SECTOR)
            data = src.read(size*SECTOR)
            if len(data) != size*SECTOR:
                raise ValueError(f"Truncated archive entry: {name}")
            dst.write(data)
            rewritten.append((name, cursor, size))
            manifest.append({'name': name, 'source_index': index, 'old_sector': offset, 'sector': cursor,
                             'sectors': size, 'sha256': hashlib.sha256(data).hexdigest()})
            cursor += size
    (args.out/'gta3.dir').write_bytes(b''.join(
        struct.pack('<II24s', offset, size, name.encode('ascii'))
        for name, offset, size in rewritten))
    with target.open('rb') as stream:
        for entry in manifest:
            stream.seek(entry['sector']*SECTOR)
            assert hashlib.sha256(stream.read(entry['sectors']*SECTOR)).hexdigest() == entry['sha256']
    report = {'source': str(source), 'target': str(target), 'bytes': cursor*SECTOR,
              'before': dependency_distances(entries, textures),
              'after': dependency_distances(rewritten, textures), 'entries': manifest}
    args.report.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'entries'}, indent=2))
    print(f"PASS: {len(entries)} entries preserved byte for byte")


if __name__ == '__main__':
    main()
