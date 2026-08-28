#!/usr/bin/env python3
"""Extract native texture chunks for demand-loaded GameCube world TXDs."""

import argparse
import os
import struct

from repack_img import read_dir, unpack_txd, SECTOR

ID_TEXDICTIONARY = 0x16
ID_STRUCT = 0x01
ID_TEXTURENATIVE = 0x15
BUNDLE_MAGIC = b"GCTEXB1\0"


def fnv_name(name):
    value = 2166136261
    for byte in name.lower().encode("latin1"):
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def texture_chunks(data):
    if len(data) < 28:
        raise ValueError("short TXD")
    chunk, size, _version = struct.unpack_from("<III", data)
    if chunk != ID_TEXDICTIONARY or 12 + size > len(data):
        raise ValueError("bad TXD root")
    struct_id, struct_size, _version = struct.unpack_from("<III", data, 12)
    if struct_id != ID_STRUCT or struct_size != 4:
        raise ValueError("bad TXD dictionary struct")
    count = struct.unpack_from("<H", data, 24)[0]
    offset = 28
    for _ in range(count):
        if offset + 36 > len(data):
            raise ValueError("truncated native texture")
        kind, length, _version = struct.unpack_from("<III", data, offset)
        end = offset + 12 + length
        if kind != ID_TEXTURENATIVE or end > len(data):
            raise ValueError("bad native texture chunk")
        inner, inner_size, _version = struct.unpack_from("<III", data, offset + 12)
        if inner != ID_STRUCT or inner_size < 88 or offset + 24 + inner_size > end:
            raise ValueError("bad native texture struct")
        raw_name = data[offset + 32:offset + 64]
        name = raw_name.split(b"\0", 1)[0].decode("latin1")
        if not name:
            raise ValueError("empty native texture name")
        yield name, data[offset:end]
        offset = end


def write_bundle(path, named_chunks):
    hashes = {}
    chunks = []
    for name, chunk in named_chunks:
        hashed = fnv_name(name)
        previous = hashes.setdefault(hashed, name.lower())
        if previous != name.lower():
            raise ValueError("hash collision: %s / %s" % (previous, name))
        # A merged loose dictionary may repeat the same texture name. Match
        # RenderWare's final dictionary contents and keep the later chunk.
        chunks = [item for item in chunks if item[0] != hashed]
        chunks.append((hashed, chunk))
    chunks.sort(key=lambda item: item[0])
    header_size = 16 + 12 * len(chunks)
    offset = header_size
    index = []
    for hashed, chunk in chunks:
        index.append(struct.pack("<III", hashed, offset, len(chunk)))
        offset += len(chunk)
    with open(path, "wb") as out:
        out.write(struct.pack("<8sII", BUNDLE_MAGIC, len(chunks), 0))
        out.write(b"".join(index))
        for _hashed, chunk in chunks:
            out.write(chunk)
    return len(chunks)


def selected_texture_chunks(img_path, dir_path, selections):
    """Return exact native chunks selected as (txd_base, texture_name)."""
    entries = {name.lower(): (off, size)
               for name, off, size in read_dir(dir_path)}
    by_txd = {}
    for txd, name in selections:
        by_txd.setdefault(txd.lower(), set()).add(name.lower())
    selected = []
    with open(img_path, "rb") as image:
        for txd, wanted in by_txd.items():
            entry = entries.get(txd + ".txd")
            if entry is None:
                raise ValueError("missing %s.txd" % txd)
            off, sectors = entry
            image.seek(off * SECTOR)
            chunks = {name.lower(): (name, chunk)
                      for name, chunk in texture_chunks(
                          unpack_txd(image.read(sectors * SECTOR)))}
            for name in sorted(wanted):
                if name not in chunks:
                    raise ValueError("missing %s in %s.txd" % (name, txd))
                selected.append(chunks[name])
    return selected


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("img")
    parser.add_argument("dir")
    parser.add_argument("out")
    parser.add_argument("txd", nargs="+")
    args = parser.parse_args()

    wanted = {os.path.splitext(name)[0].lower() for name in args.txd}
    entries = {name.lower(): (off, size) for name, off, size in read_dir(args.dir)}
    os.makedirs(args.out, exist_ok=True)
    total = 0
    with open(args.img, "rb") as image:
        for base in sorted(wanted):
            entry = entries.get(base + ".txd")
            if entry is None:
                raise SystemExit("missing %s.txd in IMG" % base)
            off, sectors = entry
            image.seek(off * SECTOR)
            data = unpack_txd(image.read(sectors * SECTOR))
            bundle = os.path.join(args.out, base + ".gtb")
            count = write_bundle(bundle, texture_chunks(data))
            total += count
            print("%s: %d textures" % (base, count))
    print("sidecars: %d native textures" % total)


if __name__ == "__main__":
    main()
