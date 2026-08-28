#!/usr/bin/env python3
"""Losslessly compact eligible GX-native RGB5A3 textures to CI8.

The palette stores the exact 16-bit RGB5A3 words already present in the TXD,
so this changes neither dimensions nor decoded colour/alpha values.  CI8 uses
one byte per texel plus a 512-byte palette instead of two bytes per texel.
"""

import argparse
import os
import struct
import tempfile

ID_STRUCT = 0x01
ID_TEXTURENATIVE = 0x15
ID_TEXDICTIONARY = 0x16
PLATFORM_GAMECUBE = 6
GXFMT_RGB5A3 = 0x05
GXFMT_CI8 = 0x09
GX_HEADER = 88


def chunks(data):
    pos = 0
    while pos + 12 <= len(data):
        cid, size, version = struct.unpack_from("<III", data, pos)
        end = pos + 12 + size
        if end > len(data):
            raise ValueError("chunk overruns parent")
        yield cid, version, data[pos + 12:end]
        pos = end
    if pos != len(data):
        raise ValueError("trailing partial chunk")


def chunk(cid, version, payload):
    return struct.pack("<III", cid, len(payload), version) + payload


def rgb5a3_to_ci8(header, pixels, width, height):
    palette = []
    lookup = {}
    indices = bytearray(width * height)

    for y in range(height):
        for x in range(width):
            tile = ((y // 4) * (width // 4) + x // 4) * 32
            off = tile + ((y & 3) * 4 + (x & 3)) * 2
            value = (pixels[off] << 8) | pixels[off + 1]
            index = lookup.get(value)
            if index is None:
                if len(palette) == 256:
                    return None
                index = len(palette)
                lookup[value] = index
                palette.append(value)
            indices[y * width + x] = index

    tiled = bytearray(width * height)
    out = 0
    for ty in range(0, height, 4):
        for tx in range(0, width, 8):
            for y in range(4):
                row = (ty + y) * width + tx
                tiled[out:out + 8] = indices[row:row + 8]
                out += 8

    pal = bytearray(512)
    for i, value in enumerate(palette):
        struct.pack_into(">H", pal, i * 2, value)
    header[84] = 8
    header[87] = GXFMT_CI8
    return bytes(tiled + pal), len(palette)


def convert_native(payload):
    children = list(chunks(payload))
    if not children or children[0][0] != ID_STRUCT:
        return payload, 0, 0
    cid, version, body = children[0]
    if len(body) < GX_HEADER + 4:
        return payload, 0, 0
    header = bytearray(body[:GX_HEADER])
    platform = struct.unpack_from("<I", header, 0)[0]
    width, height = struct.unpack_from("<HH", header, 80)
    if platform != PLATFORM_GAMECUBE or header[87] != GXFMT_RGB5A3:
        return payload, 0, 0
    old_size = struct.unpack_from("<I", body, GX_HEADER)[0]
    if width == 0 or height == 0 or old_size != width * height * 2:
        return payload, 0, 0
    start = GX_HEADER + 4
    if start + old_size != len(body):
        return payload, 0, 0
    converted = rgb5a3_to_ci8(header, body[start:], width, height)
    if converted is None:
        return payload, 0, 0
    pixels, colors = converted
    new_body = bytes(header) + struct.pack("<I", len(pixels)) + pixels
    rebuilt = [chunk(ID_STRUCT, version, new_body)]
    rebuilt.extend(chunk(ccid, cver, cpayload) for ccid, cver, cpayload in children[1:])
    return b"".join(rebuilt), old_size - len(pixels), colors


def convert_dictionary(payload):
    rebuilt = []
    saved = converted = 0
    max_colors = 0
    for cid, version, body in chunks(payload):
        if cid == ID_TEXTURENATIVE:
            body, delta, colors = convert_native(body)
            if delta:
                converted += 1
                saved += delta
                max_colors = max(max_colors, colors)
        rebuilt.append(chunk(cid, version, body))
    return b"".join(rebuilt), converted, saved, max_colors


def convert_bytes(original):
    rebuilt = []
    converted = saved = max_colors = 0
    for cid, version, payload in chunks(original):
        if cid == ID_TEXDICTIONARY:
            payload, count, delta, colors = convert_dictionary(payload)
            converted += count
            saved += delta
            max_colors = max(max_colors, colors)
        rebuilt.append(chunk(cid, version, payload))
    return b"".join(rebuilt), converted, saved, max_colors


def convert_file(path):
    original = open(path, "rb").read()
    output, converted, saved, max_colors = convert_bytes(original)
    if converted:
        directory = os.path.dirname(os.path.abspath(path))
        mode = os.stat(path).st_mode & 0o777
        fd, temp = tempfile.mkstemp(prefix=".ci8-", dir=directory)
        try:
            with os.fdopen(fd, "wb") as stream:
                stream.write(output)
            os.chmod(temp, mode)
            os.replace(temp, path)
        except Exception:
            os.unlink(temp)
            raise
    print(f"{path}: {converted} textures, saved {saved // 1024} KiB, "
          f"largest palette {max_colors}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()
    for path in args.files:
        convert_file(path)


if __name__ == "__main__":
    main()
