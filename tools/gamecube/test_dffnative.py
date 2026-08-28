#!/usr/bin/env python3
import struct
import unittest

import dffnative as dff


LIBRARY_ID = 0x1803FFFF
DUMMY_PLUGIN = dff.Chunk(0x253F2F9, LIBRARY_ID, b'unknown-gta-plugin\0')
BINMESH_PLUGIN = dff.Chunk(
    dff.ID_BINMESH, LIBRARY_ID,
    struct.pack('<III', 0, 1, 3) + struct.pack('<IiIII', 3, 0, 0, 1, 2))


def make_texture(name, mask=''):
    payload = dff.Chunk(dff.ID_STRUCT, LIBRARY_ID,
                        struct.pack('<I', 0x1101)).encode()
    for value in (name, mask):
        raw = value.encode('latin1') + b'\0'
        raw += b'\0' * (-len(raw) & 3)
        payload += dff.Chunk(dff.ID_STRING, LIBRARY_ID, raw).encode()
    payload += dff.Chunk(dff.ID_EXTENSION, LIBRARY_ID, b'').encode()
    return dff.Chunk(dff.ID_TEXTURE, LIBRARY_ID, payload)


def make_dff(extra_plugins=(), texture_names=()):
    flags = 0x02 | dff.GEO_TEXTURED | dff.GEO_PRELIT | 0x10 | (1 << 16)
    vertices = (-1.0, 0.0, 1.0,
                2.0, -2.0, 0.5,
                0.0, 1.0, -0.5)
    normals = (0.0, 0.0, 1.0) * 3
    uv = (0.0, 0.0, 1.0, 0.0, 0.5, 1.0)
    colors = bytes((255, 0, 0, 255,
                    0, 255, 0, 255,
                    0, 0, 255, 255))
    triangle = struct.pack('<II', 0 << 16 | 1, 2 << 16 | 0)
    sphere = struct.pack('<4f', 0.0, 0.0, 0.0, 4.0)
    geometry_struct = dff.GEO_HEADER.pack(flags, 1, 3, 1)
    geometry_struct += colors + struct.pack('<6f', *uv) + triangle
    geometry_struct += sphere + struct.pack('<ii', 1, 1)
    geometry_struct += struct.pack('<9f', *vertices)
    geometry_struct += struct.pack('<9f', *normals)

    extension = dff.Chunk(
        dff.ID_EXTENSION, LIBRARY_ID,
        b''.join(plugin.encode() for plugin in
                 (DUMMY_PLUGIN, BINMESH_PLUGIN) + tuple(extra_plugins)))
    materials = []
    for name in texture_names:
        material = dff.Chunk(dff.ID_STRUCT, LIBRARY_ID,
                             b'\0' * 28).encode()
        material += make_texture(name).encode()
        material += dff.Chunk(dff.ID_EXTENSION, LIBRARY_ID, b'').encode()
        materials.append(dff.Chunk(dff.ID_MATERIAL, LIBRARY_ID, material))
    matlist_struct = struct.pack('<I', len(materials))
    if materials:
        matlist_struct += struct.pack('<%di' % len(materials),
                                     *([-1] * len(materials)))
    matlist = dff.Chunk(dff.ID_STRUCT, LIBRARY_ID,
                        matlist_struct).encode()
    matlist += b''.join(material.encode() for material in materials)

    geometry = dff.Chunk(
        dff.ID_GEOMETRY, LIBRARY_ID,
        dff.Chunk(dff.ID_STRUCT, LIBRARY_ID, geometry_struct).encode() +
        dff.Chunk(dff.ID_MATLIST, LIBRARY_ID, matlist).encode() +
        extension.encode())
    geometry_list = dff.Chunk(
        dff.ID_GEOMETRYLIST, LIBRARY_ID,
        dff.Chunk(dff.ID_STRUCT, LIBRARY_ID,
                  struct.pack('<I', 1)).encode() + geometry.encode())
    clump = dff.Chunk(
        dff.ID_CLUMP, LIBRARY_ID,
        dff.Chunk(dff.ID_STRUCT, LIBRARY_ID, b'clump-struct').encode() +
        geometry_list.encode() +
        dff.Chunk(dff.ID_EXTENSION, LIBRARY_ID, b'').encode())
    return clump.encode()


def geometry_children(data):
    top, _ = dff._one_chunk(data)
    geometry_list = next(c for c in dff._children(top.payload)
                         if c.chunk_id == dff.ID_GEOMETRYLIST)
    geometry = next(c for c in dff._children(geometry_list.payload)
                    if c.chunk_id == dff.ID_GEOMETRY)
    return dff._children(geometry.payload)


class NativeDffTest(unittest.TestCase):
    def test_static_geometry_is_native_and_idempotent(self):
        source = make_dff() + b'\0' * 2048
        converted, stats = dff.convert_bytes(source)
        self.assertEqual(stats.converted, 1)
        self.assertNotEqual(converted, source[:-2048])
        self.assertLess(len(converted), len(source))
        self.assertEqual(dff.verify_bytes(converted), 1)

        children = geometry_children(converted)
        structure = next(c for c in children if c.chunk_id == dff.ID_STRUCT)
        flags, triangles, vertices, morphs = dff.GEO_HEADER.unpack_from(
            structure.payload)
        self.assertTrue(flags & dff.GEO_NATIVE)
        self.assertEqual((triangles, vertices, morphs), (1, 3, 1))
        self.assertEqual(len(structure.payload), 16 + 16 + 8)

        extension = next(c for c in children if c.chunk_id == dff.ID_EXTENSION)
        plugins = dff._children(extension.payload)
        self.assertIn(DUMMY_PLUGIN.encode(), [plugin.encode() for plugin in plugins])
        binmesh = next(plugin for plugin in plugins
                       if plugin.chunk_id == dff.ID_BINMESH)
        self.assertEqual(struct.unpack_from('<III', binmesh.payload), (0, 1, 3))
        self.assertEqual(struct.unpack_from('<Ii', binmesh.payload, 12), (3, 0))
        self.assertEqual(struct.unpack_from('<3H', binmesh.payload, 20), (0, 1, 2))
        self.assertEqual(len(binmesh.payload), 26)
        native = next(c for c in plugins if c.chunk_id == dff.ID_NATIVEDATA)
        nested = dff._children(native.payload)[0]
        platform, version, count, attributes, shifts, data_size = \
            struct.unpack_from('<IIIIII', nested.payload)
        self.assertEqual((platform, version, count),
                         (dff.PLATFORM_GAMECUBE, dff.NATIVE_VERSION, 3))
        self.assertEqual(attributes, dff.NATIVE_POS | dff.NATIVE_NORMAL |
                         dff.NATIVE_COLOR | dff.NATIVE_UV0)
        self.assertEqual(len(nested.payload), 24 + data_size)
        self.assertGreater(shifts & 0xFF, 2)
        self.assertGreater((shifts >> 8) & 0xFF, 8)

        converted_again, again_stats = dff.convert_bytes(converted)
        self.assertEqual(converted_again, converted)
        self.assertEqual(again_stats.already_native, 1)

    def test_skin_is_preserved(self):
        skin = dff.Chunk(dff.ID_SKIN, LIBRARY_ID, b'skinned')
        source = make_dff((skin,))
        converted, stats = dff.convert_bytes(source)
        self.assertEqual(converted, source)
        self.assertEqual(stats.skipped_skin, 1)

    def test_model_texture_dependencies_are_embedded_and_idempotent(self):
        source = make_dff(texture_names=('hedge', 'Star aluminium', 'hedge'))
        converted, stats = dff.convert_bytes(source)
        self.assertEqual(stats.prefetch_models, 1)
        self.assertEqual(stats.prefetch_textures, 2)
        dff.verify_bytes(converted)

        top, _ = dff._one_chunk(converted)
        extension = next(child for child in dff._children(top.payload)
                         if child.chunk_id == dff.ID_EXTENSION)
        plugin = next(child for child in dff._children(extension.payload)
                      if child.chunk_id == dff.ID_GC_MODEL_TEXTURES)
        magic, count = dff.GC_MODEL_TEXTURES_HEADER.unpack_from(plugin.payload)
        self.assertEqual((magic, count), (dff.GC_MODEL_TEXTURES_MAGIC, 2))
        names = []
        for index in range(count):
            start = dff.GC_MODEL_TEXTURES_HEADER.size + \
                index * dff.GC_MODEL_TEXTURE_NAME_BYTES
            names.append(plugin.payload[
                start:start + dff.GC_MODEL_TEXTURE_NAME_BYTES].split(b'\0')[0])
        self.assertEqual(names, [b'hedge', b'Star aluminium'])

        converted_again, _ = dff.convert_bytes(converted)
        self.assertEqual(converted_again, converted)


if __name__ == '__main__':
    unittest.main()
