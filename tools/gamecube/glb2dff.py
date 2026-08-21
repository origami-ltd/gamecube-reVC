#!/usr/bin/env python3
"""Convert a static textured GLB into a RenderWare DFF for the GX frontend.

The output geometry stays platform independent so librw can instance it with
the GameCube pipeline.  The embedded base-colour PNG is also emitted as an
uncompressed TGA; txdconv turns that into the separately shipped GX-native
texture dictionary.

Only the small, deliberate subset needed by static menu props is accepted:
triangle primitives with POSITION, NORMAL and TEXCOORD_0 attributes, one
base-colour texture, and no skinning or morph targets.  Unsupported input is a
hard error instead of a partially converted model.
"""

import argparse
import json
import math
import os
import struct
import sys
import zlib


RW_VERSION = 0x36003
RW_BUILD = 0xFFFF

ID_STRUCT = 0x01
ID_STRING = 0x02
ID_EXTENSION = 0x03
ID_TEXTURE = 0x06
ID_MATERIAL = 0x07
ID_MATLIST = 0x08
ID_FRAMELIST = 0x0E
ID_GEOMETRY = 0x0F
ID_CLUMP = 0x10
ID_ATOMIC = 0x14
ID_GEOMETRYLIST = 0x1A
ID_MESH = (5 << 8) | 0x0E

GEOMETRY_POSITIONS = 0x02
GEOMETRY_TEXTURED = 0x04
GEOMETRY_NORMALS = 0x10
GEOMETRY_LIGHT = 0x20
GEOMETRY_MODULATE = 0x40

COMPONENT_FORMAT = {
    5120: "b",
    5121: "B",
    5122: "h",
    5123: "H",
    5125: "I",
    5126: "f",
}
TYPE_WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
              "MAT2": 4, "MAT3": 9, "MAT4": 16}


def fail(message):
    raise ValueError(message)


def library_id():
    return (((RW_VERSION - 0x30000) & 0x3FF00) << 14 |
            (RW_VERSION & 0x3F) << 16 | RW_BUILD)


def chunk(kind, payload=b""):
    return struct.pack("<III", kind, len(payload), library_id()) + payload


def extension(*plugins):
    return chunk(ID_EXTENSION, b"".join(plugins))


def string_chunk(value):
    data = value.encode("ascii") + b"\0"
    data += b"\0" * (-len(data) & 3)
    return chunk(ID_STRING, data)


def load_glb(path):
    data = open(path, "rb").read()
    if len(data) < 20:
        fail("file is too small to be a GLB")
    magic, version, total = struct.unpack_from("<4sII", data)
    if magic != b"glTF" or version != 2 or total != len(data):
        fail("expected a complete GLB 2.0 file")
    document = binary = None
    offset = 12
    while offset < len(data):
        length, kind = struct.unpack_from("<II", data, offset)
        offset += 8
        payload = data[offset:offset + length]
        offset += length
        if len(payload) != length:
            fail("truncated GLB chunk")
        if kind == 0x4E4F534A:
            document = json.loads(payload)
        elif kind == 0x004E4942:
            binary = payload
    if document is None or binary is None:
        fail("GLB must contain JSON and BIN chunks")
    if document.get("asset", {}).get("version") != "2.0":
        fail("unsupported glTF version")
    if len(document.get("buffers", [])) != 1:
        fail("exactly one embedded buffer is required")
    return document, binary


def accessor_data(document, binary, index):
    accessor = document["accessors"][index]
    if "sparse" in accessor:
        fail("sparse accessors are not supported")
    component = accessor["componentType"]
    if component not in COMPONENT_FORMAT:
        fail("unsupported accessor component type %r" % component)
    width = TYPE_WIDTH.get(accessor["type"])
    if width is None:
        fail("unsupported accessor type %r" % accessor["type"])
    view = document["bufferViews"][accessor["bufferView"]]
    if view.get("buffer", 0) != 0:
        fail("accessor refers to a non-embedded buffer")
    fmt = "<" + COMPONENT_FORMAT[component] * width
    packed = struct.calcsize(fmt)
    stride = view.get("byteStride", packed)
    if stride < packed:
        fail("accessor stride is smaller than its element")
    start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    end = start + (accessor["count"] - 1) * stride + packed
    if start < 0 or end > len(binary):
        fail("accessor lies outside the GLB binary chunk")
    return [struct.unpack_from(fmt, binary, start + i * stride)
            for i in range(accessor["count"])]


def identity():
    return [1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0]


def multiply(a, b):
    # glTF matrices are column-major: result = a * b.
    out = [0.0] * 16
    for column in range(4):
        for row in range(4):
            out[column * 4 + row] = sum(
                a[k * 4 + row] * b[column * 4 + k] for k in range(4))
    return out


def quaternion_matrix(q):
    x, y, z, w = q
    n = x*x + y*y + z*z + w*w
    if n == 0.0:
        return identity()
    s = 2.0 / n
    xx, yy, zz = x*x*s, y*y*s, z*z*s
    xy, xz, yz = x*y*s, x*z*s, y*z*s
    wx, wy, wz = w*x*s, w*y*s, w*z*s
    return [1.0 - yy - zz, xy + wz, xz - wy, 0.0,
            xy - wz, 1.0 - xx - zz, yz + wx, 0.0,
            xz + wy, yz - wx, 1.0 - xx - yy, 0.0,
            0.0, 0.0, 0.0, 1.0]


def node_matrix(node):
    if "matrix" in node:
        if any(key in node for key in ("translation", "rotation", "scale")):
            fail("node cannot contain both matrix and TRS transforms")
        if len(node["matrix"]) != 16:
            fail("node matrix must have 16 elements")
        return [float(value) for value in node["matrix"]]
    translation = node.get("translation", (0.0, 0.0, 0.0))
    scale = node.get("scale", (1.0, 1.0, 1.0))
    result = identity()
    result[0], result[5], result[10] = map(float, scale)
    result = multiply(quaternion_matrix(node.get("rotation", (0, 0, 0, 1))),
                      result)
    result[12], result[13], result[14] = map(float, translation)
    return result


def transform_point(matrix, value):
    x, y, z = value
    w = matrix[3]*x + matrix[7]*y + matrix[11]*z + matrix[15]
    if abs(w) < 1.0e-12:
        fail("node transform produced a point at infinity")
    return ((matrix[0]*x + matrix[4]*y + matrix[8]*z + matrix[12]) / w,
            (matrix[1]*x + matrix[5]*y + matrix[9]*z + matrix[13]) / w,
            (matrix[2]*x + matrix[6]*y + matrix[10]*z + matrix[14]) / w)


def inverse_transpose_3x3(matrix):
    # Rows of the affine 3x3, followed by inverse transpose.
    a, b, c = matrix[0], matrix[4], matrix[8]
    d, e, f = matrix[1], matrix[5], matrix[9]
    g, h, i = matrix[2], matrix[6], matrix[10]
    det = a*(e*i-f*h) - b*(d*i-f*g) + c*(d*h-e*g)
    if abs(det) < 1.0e-12:
        fail("node transform has a singular normal matrix")
    inv = [
        (e*i-f*h)/det, (c*h-b*i)/det, (b*f-c*e)/det,
        (f*g-d*i)/det, (a*i-c*g)/det, (c*d-a*f)/det,
        (d*h-e*g)/det, (b*g-a*h)/det, (a*e-b*d)/det,
    ]
    return [inv[0], inv[3], inv[6],
            inv[1], inv[4], inv[7],
            inv[2], inv[5], inv[8]]


def transform_normal(normal_matrix, value):
    x, y, z = value
    out = (normal_matrix[0]*x + normal_matrix[1]*y + normal_matrix[2]*z,
           normal_matrix[3]*x + normal_matrix[4]*y + normal_matrix[5]*z,
           normal_matrix[6]*x + normal_matrix[7]*y + normal_matrix[8]*z)
    length = math.sqrt(sum(component*component for component in out))
    if length < 1.0e-12:
        fail("normal became zero length")
    return tuple(component / length for component in out)


def collect_geometry(document, binary):
    scenes = document.get("scenes", [])
    if not scenes:
        fail("GLB has no scene")
    scene_index = document.get("scene", 0)
    if scene_index >= len(scenes):
        fail("default scene index is invalid")

    vertices, normals, texcoords, indices = [], [], [], []
    material_index = None

    def visit(node_index, parent):
        nonlocal material_index
        node = document["nodes"][node_index]
        world = multiply(parent, node_matrix(node))
        if "mesh" in node:
            mesh = document["meshes"][node["mesh"]]
            for primitive in mesh.get("primitives", []):
                if primitive.get("mode", 4) != 4:
                    fail("only triangle-list primitives are supported")
                attrs = primitive.get("attributes", {})
                for required in ("POSITION", "NORMAL", "TEXCOORD_0"):
                    if required not in attrs:
                        fail("primitive is missing %s" % required)
                if "indices" not in primitive:
                    fail("unindexed primitives are not supported")
                this_material = primitive.get("material")
                if this_material is None:
                    fail("primitive has no material")
                if material_index is None:
                    material_index = this_material
                elif material_index != this_material:
                    fail("all primitives must use the same material")

                pos = accessor_data(document, binary, attrs["POSITION"])
                nrm = accessor_data(document, binary, attrs["NORMAL"])
                uv = accessor_data(document, binary, attrs["TEXCOORD_0"])
                ind = accessor_data(document, binary, primitive["indices"])
                if len(pos) != len(nrm) or len(pos) != len(uv):
                    fail("POSITION, NORMAL and TEXCOORD_0 counts differ")
                if any(len(value) != 1 for value in ind):
                    fail("index accessor must be SCALAR")
                if len(ind) % 3:
                    fail("triangle index count is not divisible by three")
                base = len(vertices)
                normal_matrix = inverse_transpose_3x3(world)
                vertices.extend(transform_point(world, value) for value in pos)
                normals.extend(transform_normal(normal_matrix, value) for value in nrm)
                # Keep the model-authored UVs. txdconv preserves the decoded
                # image's top-row convention when it tiles the GX raster.
                texcoords.extend((float(value[0]), float(value[1])) for value in uv)
                for value in ind:
                    index = int(value[0])
                    if index < 0 or index >= len(pos):
                        fail("primitive index is out of range")
                    indices.append(base + index)
        for child in node.get("children", []):
            visit(child, world)

    for root in scenes[scene_index].get("nodes", []):
        visit(root, identity())

    if not vertices or not indices:
        fail("scene contains no indexed geometry")
    if len(vertices) > 65535:
        fail("RenderWare DFF requires at most 65535 vertices per geometry")

    # The frontend camera basis reverses both face-plane axes relative to the
    # GLB. Reflect X/Z (and their normal components) before centring so the
    # handles stay down and A/B/X/Y remain on the physical right.
    vertices = [(-value[0], value[1], -value[2]) for value in vertices]
    normals = [(-value[0], value[1], -value[2]) for value in normals]

    # Put the pivot at the visual centre, then give the frontend a predictable
    # 2.6-unit width.  No vertices or triangles are removed.
    minimum = [min(value[axis] for value in vertices) for axis in range(3)]
    maximum = [max(value[axis] for value in vertices) for axis in range(3)]
    centre = [(minimum[axis] + maximum[axis]) * 0.5 for axis in range(3)]
    width = maximum[0] - minimum[0]
    if width <= 0.0:
        fail("model has zero width")
    scale = 2.6 / width
    vertices = [tuple((value[axis] - centre[axis]) * scale for axis in range(3))
                for value in vertices]
    converted_min = [min(value[axis] for value in vertices) for axis in range(3)]
    converted_max = [max(value[axis] for value in vertices) for axis in range(3)]
    return vertices, normals, texcoords, indices, material_index, {
        "source_min": minimum,
        "source_max": maximum,
        "source_center": centre,
        "scale": scale,
        "converted_min": converted_min,
        "converted_max": converted_max,
    }


def texture_name_for_material(document, material_index):
    material = document["materials"][material_index]
    pbr = material.get("pbrMetallicRoughness", {})
    info = pbr.get("baseColorTexture")
    if info is None or info.get("texCoord", 0) != 0:
        fail("material must have a TEXCOORD_0 base-colour texture")
    texture = document["textures"][info["index"]]
    if "source" not in texture:
        fail("base-colour texture has no image source")
    return texture["source"], pbr.get("baseColorFactor", (1, 1, 1, 1))


def embedded_image(document, binary, image_index):
    image = document["images"][image_index]
    if image.get("mimeType") != "image/png" or "bufferView" not in image:
        fail("base-colour image must be an embedded PNG")
    view = document["bufferViews"][image["bufferView"]]
    start = view.get("byteOffset", 0)
    end = start + view["byteLength"]
    if start < 0 or end > len(binary):
        fail("embedded image lies outside the GLB binary chunk")
    return binary[start:end]


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def decode_png_rgb(data):
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        fail("embedded image is not a PNG")
    offset = 8
    width = height = None
    compressed = bytearray()
    while offset < len(data):
        if offset + 12 > len(data):
            fail("truncated PNG chunk")
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset+4:offset+8]
        payload = data[offset+8:offset+8+length]
        if len(payload) != length:
            fail("truncated PNG payload")
        offset += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, compression, filtering, interlace = \
                struct.unpack(">IIBBBBB", payload)
            if depth != 8 or colour != 2 or compression or filtering or interlace:
                fail("base-colour PNG must be non-interlaced 8-bit RGB")
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
    if not width or not height or not compressed:
        fail("PNG is missing IHDR or IDAT data")
    raw = zlib.decompress(compressed)
    row_bytes = width * 3
    if len(raw) != height * (row_bytes + 1):
        fail("unexpected decompressed PNG size")
    rows = []
    previous = bytearray(row_bytes)
    offset = 0
    for _ in range(height):
        filter_type = raw[offset]
        scan = bytearray(raw[offset+1:offset+1+row_bytes])
        offset += row_bytes + 1
        for x in range(row_bytes):
            left = scan[x-3] if x >= 3 else 0
            up = previous[x]
            upper_left = previous[x-3] if x >= 3 else 0
            if filter_type == 1:
                scan[x] = (scan[x] + left) & 0xFF
            elif filter_type == 2:
                scan[x] = (scan[x] + up) & 0xFF
            elif filter_type == 3:
                scan[x] = (scan[x] + ((left + up) >> 1)) & 0xFF
            elif filter_type == 4:
                scan[x] = (scan[x] + paeth(left, up, upper_left)) & 0xFF
            elif filter_type != 0:
                fail("unsupported PNG filter %d" % filter_type)
        rows.append(bytes(scan))
        previous = scan
    return width, height, rows


def write_tga(path, png):
    width, height, rows = decode_png_rgb(png)
    if width > 65535 or height > 65535:
        fail("TGA dimensions exceed 16-bit fields")
    # The supplied atlas bakes the asymmetric Nintendo/GameCube decal into a
    # small, otherwise flat body region. The GX face basis turns that decal
    # vertically while the solid-colour controls remain visually symmetric.
    # Mirror only the known decal rectangle; moving whole UV islands sends
    # button shells into unrelated atlas colours.
    if width == 1024 and height == 1024:
        left, top, right, bottom = 334, 298, 420, 336
        source = [bytes(row[left*3:right*3]) for row in rows[top:bottom]]
        mutable = [bytearray(row) for row in rows]
        for y in range(bottom-top):
            for x in range(right-left):
                src = source[bottom-top-1-y]
                src_offset = x*3
                dst_offset = (left+x)*3
                mutable[top+y][dst_offset:dst_offset+3] = src[src_offset:src_offset+3]
        rows = [bytes(row) for row in mutable]
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0,
                         width, height, 24, 0x20)
    with open(path, "wb") as out:
        out.write(header)
        for row in rows:
            bgr = bytearray(len(row))
            for offset in range(0, len(row), 3):
                bgr[offset:offset+3] = row[offset+2], row[offset+1], row[offset]
            out.write(bgr)


def make_texture(name):
    payload = chunk(ID_STRUCT, struct.pack("<I", 0x1102))
    payload += string_chunk(name)
    payload += string_chunk("")
    payload += extension()
    return chunk(ID_TEXTURE, payload)


def make_material(texture_name, colour_factor):
    rgba = bytes(max(0, min(255, round(float(component) * 255)))
                 for component in colour_factor)
    material_struct = struct.pack("<I", 0) + rgba + struct.pack("<II3f", 0, 1,
                                                                 1.0, 0.0, 1.0)
    payload = chunk(ID_STRUCT, material_struct)
    payload += make_texture(texture_name)
    payload += extension()
    return chunk(ID_MATERIAL, payload)


def make_geometry(vertices, normals, texcoords, indices, texture_name, colour):
    triangle_count = len(indices) // 3
    flags = (GEOMETRY_POSITIONS | GEOMETRY_TEXTURED | GEOMETRY_NORMALS |
             GEOMETRY_LIGHT | GEOMETRY_MODULATE)
    geometry_struct = struct.pack("<4I", flags | (1 << 16), triangle_count,
                                  len(vertices), 1)
    geometry_struct += b"".join(struct.pack("<2f", *value) for value in texcoords)
    for offset in range(0, len(indices), 3):
        a, b, c = indices[offset:offset+3]
        geometry_struct += struct.pack("<II", (a << 16) | b, c << 16)

    radius = max(math.sqrt(x*x + y*y + z*z) for x, y, z in vertices)
    geometry_struct += struct.pack("<4fII", 0.0, 0.0, 0.0, radius, 1, 1)
    geometry_struct += b"".join(struct.pack("<3f", *value) for value in vertices)
    geometry_struct += b"".join(struct.pack("<3f", *value) for value in normals)

    matlist = chunk(ID_MATLIST,
                    chunk(ID_STRUCT, struct.pack("<Ii", 1, -1)) +
                    make_material(texture_name, colour))

    # RenderWare's BIN MESH plugin stores non-native indices as uint32.  The
    # GX loader consumes this list directly and can then release the duplicate
    # Triangle array after streaming.
    mesh_data = struct.pack("<3I2I", 0, 1, len(indices), len(indices), 0)
    mesh_data += b"".join(struct.pack("<I", value) for value in indices)
    mesh = chunk(ID_MESH, mesh_data)

    payload = chunk(ID_STRUCT, geometry_struct) + matlist + extension(mesh)
    return chunk(ID_GEOMETRY, payload)


def make_dff(vertices, normals, texcoords, indices, texture_name, colour):
    frame = struct.pack("<12fii",
                        1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, -1, 0)
    frame_list = chunk(ID_FRAMELIST,
                       chunk(ID_STRUCT, struct.pack("<I", 1) + frame) +
                       extension())
    geometry = make_geometry(vertices, normals, texcoords, indices,
                             texture_name, colour)
    geometry_list = chunk(ID_GEOMETRYLIST,
                          chunk(ID_STRUCT, struct.pack("<I", 1)) + geometry)
    atomic = chunk(ID_ATOMIC,
                   chunk(ID_STRUCT, struct.pack("<4I", 0, 0, 5, 0)) +
                   extension())
    payload = chunk(ID_STRUCT, struct.pack("<3I", 1, 0, 0))
    payload += frame_list + geometry_list + atomic + extension()
    return chunk(ID_CLUMP, payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="source GLB")
    parser.add_argument("dff", help="output RenderWare DFF")
    parser.add_argument("tga", help="output base-colour TGA for txdconv")
    parser.add_argument("--texture-name", default="gc_controller",
                        help="RenderWare texture name (default: gc_controller)")
    parser.add_argument("--metadata", help="optional conversion metadata JSON")
    args = parser.parse_args()

    document, binary = load_glb(args.input)
    vertices, normals, texcoords, indices, material, metadata = \
        collect_geometry(document, binary)
    image_index, colour = texture_name_for_material(document, material)
    png = embedded_image(document, binary, image_index)

    os.makedirs(os.path.dirname(os.path.abspath(args.dff)), exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.tga)), exist_ok=True)
    with open(args.dff, "wb") as out:
        out.write(make_dff(vertices, normals, texcoords, indices,
                           args.texture_name, colour))
    write_tga(args.tga, png)

    metadata.update({
        "vertices": len(vertices),
        "triangles": len(indices) // 3,
        "indices": len(indices),
        "texture_name": args.texture_name,
        "source": os.path.basename(args.input),
    })
    if args.metadata:
        with open(args.metadata, "w", encoding="utf-8") as out:
            json.dump(metadata, out, indent=2, sort_keys=True)
            out.write("\n")
    print("%s -> %s: %d vertices, %d triangles; texture -> %s" %
          (args.input, args.dff, len(vertices), len(indices) // 3, args.tga))


if __name__ == "__main__":
    try:
        main()
    except (KeyError, IndexError, OSError, ValueError, zlib.error) as error:
        sys.exit("glb2dff: %s" % error)
