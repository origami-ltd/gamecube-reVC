#!/usr/bin/env python3
"""Convert static RenderWare DFF geometry to a compact GameCube-native blob.

The transformer edits only a Geometry STRUCT and its EXTENSION. Materials,
bin-mesh topology and every unknown GTA plugin are preserved; only BINMESH
indices narrow from the PC stream's u32 to the native stream's u16. Skinned,
multi-morph, already-native or unsupported geometry is left unchanged.
"""

import dataclasses
import math
import struct


HEADER = struct.Struct('<III')
GEO_HEADER = struct.Struct('<Iiii')

ID_STRUCT = 0x01
ID_STRING = 0x02
ID_EXTENSION = 0x03
ID_TEXTURE = 0x06
ID_MATERIAL = 0x07
ID_MATLIST = 0x08
ID_GEOMETRY = 0x0F
ID_CLUMP = 0x10
ID_GEOMETRYLIST = 0x1A
ID_SKIN = 0x116
ID_BINMESH = 0x50E
ID_NATIVEDATA = 0x510

# Unknown RenderWare plugins are skipped by old readers. Static DFFs carry this
# tiny, GameCube-only dependency list in the Clump extension so the console can
# restore every exact texture before publishing any part of the model.
ID_GC_MODEL_TEXTURES = 0x47435458
GC_MODEL_TEXTURES_MAGIC = b'GCMTEX1\0'
GC_MODEL_TEXTURES_HEADER = struct.Struct('<8sI')
GC_MODEL_TEXTURE_NAME_BYTES = 32
GC_MODEL_TEXTURES_MAX = 128

PLATFORM_GAMECUBE = 6
NATIVE_VERSION = 1

GEO_TEXTURED = 0x04
GEO_PRELIT = 0x08
GEO_TEXTURED2 = 0x80
GEO_NATIVE = 0x01000000

NATIVE_POS = 1
NATIVE_NORMAL = 2
NATIVE_COLOR = 4
NATIVE_UV0 = 8
NATIVE_UV2 = 16


class DffError(ValueError):
    pass


@dataclasses.dataclass
class Stats:
    files: int = 0
    geometries: int = 0
    converted: int = 0
    already_native: int = 0
    skipped_skin: int = 0
    skipped_morph: int = 0
    skipped_no_binmesh: int = 0
    skipped_range: int = 0
    failed: int = 0
    generic_bytes: int = 0
    native_bytes: int = 0
    prefetch_models: int = 0
    prefetch_textures: int = 0

    def add(self, other):
        for field in dataclasses.fields(self):
            setattr(self, field.name,
                    getattr(self, field.name) + getattr(other, field.name))
        return self


@dataclasses.dataclass(frozen=True)
class Chunk:
    chunk_id: int
    library_id: int
    payload: bytes

    def encode(self):
        return HEADER.pack(self.chunk_id, len(self.payload), self.library_id) + self.payload


def _one_chunk(data, offset=0):
    if offset < 0 or offset + HEADER.size > len(data):
        raise DffError('truncated chunk header')
    chunk_id, length, library_id = HEADER.unpack_from(data, offset)
    end = offset + HEADER.size + length
    if end > len(data):
        raise DffError('chunk payload overruns input')
    return Chunk(chunk_id, library_id,
                 data[offset + HEADER.size:end]), end


def _children(payload):
    result = []
    at = 0
    while at < len(payload):
        child, at = _one_chunk(payload, at)
        result.append(child)
    if at != len(payload):
        raise DffError('child chunks do not fill parent')
    return result


def _library_version(library_id):
    if library_id & 0xFFFF0000:
        return (((library_id >> 14) & 0x3FF00) + 0x30000) | \
               ((library_id >> 16) & 0x3F)
    return library_id << 8


def _texcoord_sets(flags):
    count = (flags >> 16) & 0xFF
    if count:
        return count
    if flags & GEO_TEXTURED:
        return 1
    if flags & GEO_TEXTURED2:
        return 2
    return 0


def _take(payload, at, length, what):
    end = at + length
    if length < 0 or end > len(payload):
        raise DffError('truncated geometry %s' % what)
    return payload[at:end], end


def _shift_for(values, floor_shift):
    max_abs = 0.0
    for value in values:
        if not math.isfinite(value):
            return -1
        max_abs = max(max_abs, abs(value))
    if max_abs <= 0.0:
        return 15
    shift = 0
    while shift < 15 and max_abs * (1 << (shift + 1)) <= 32767.0:
        shift += 1
    return shift if shift >= floor_shift else -1


def _quantize(values, shift):
    scale = 1 << shift
    out = []
    for value in values:
        q = value * scale
        if q > 32767.0:
            q = 32767.0
        elif q < -32768.0:
            q = -32768.0
        else:
            q = math.ceil(q - 0.5) if q < 0.0 else math.floor(q + 0.5)
        out.append(int(q))
    return struct.pack('<%dh' % len(out), *out)


def _append_aligned(out, data):
    out += b'\0' * (-len(out) & 3)
    out += data


def _native_blob(vertex_count, colors, uv_sets, vertices, normals):
    position_values = struct.unpack('<%df' % (vertex_count * 3), vertices)
    pos_shift = _shift_for(position_values, 3)
    if pos_shift < 0:
        return None

    uv0 = uv_sets[0] if uv_sets else None
    uv_shift = 0
    if uv0 is not None:
        uv_values = struct.unpack('<%df' % (vertex_count * 2), uv0)
        uv_shift = _shift_for(uv_values, 9)
        if uv_shift < 0:
            return None

    attributes = NATIVE_POS
    attributes |= NATIVE_NORMAL if normals is not None else 0
    attributes |= NATIVE_COLOR if colors is not None else 0
    attributes |= NATIVE_UV0 if uv0 is not None else 0
    attributes |= NATIVE_UV2 if len(uv_sets) > 1 else 0

    data = bytearray()
    _append_aligned(data, _quantize(position_values, pos_shift))
    if normals is not None:
        _append_aligned(data, normals)
    if colors is not None:
        _append_aligned(data, colors)
    if uv0 is not None:
        _append_aligned(data, _quantize(uv_values, uv_shift))
    if len(uv_sets) > 1:
        _append_aligned(data, uv_sets[1])

    metadata = struct.pack('<IIIIII', PLATFORM_GAMECUBE, NATIVE_VERSION,
                           vertex_count, attributes,
                           pos_shift | (uv_shift << 8), len(data))
    return metadata + bytes(data)


def _native_binmesh(plugin, vertex_count):
    """Narrow generic u32 BINMESH indices to the native u16 stream format."""
    payload = plugin.payload
    if len(payload) < 12:
        raise DffError('short BINMESH header')
    flags, num_meshes, total_indices = struct.unpack_from('<III', payload)
    if num_meshes > 65535 or total_indices > 10000000:
        raise DffError('implausible BINMESH dimensions')
    at = 12
    out = bytearray(payload[:12])
    seen_indices = 0
    for _mesh_index in range(num_meshes):
        header, at = _take(payload, at, 8, 'BINMESH mesh header')
        num_indices, _material = struct.unpack('<Ii', header)
        if num_indices > total_indices - seen_indices:
            raise DffError('BINMESH index count exceeds total')
        raw, at = _take(payload, at, num_indices * 4, 'BINMESH indices')
        indices = struct.unpack('<%dI' % num_indices, raw) if num_indices else ()
        if any(index >= vertex_count or index > 0xFFFF for index in indices):
            raise DffError('BINMESH vertex index is out of range')
        out += header
        if indices:
            out += struct.pack('<%dH' % num_indices, *indices)
        seen_indices += num_indices
    if at != len(payload) or seen_indices != total_indices:
        raise DffError('BINMESH payload length/count mismatch')
    return Chunk(plugin.chunk_id, plugin.library_id, bytes(out))


def _verify_native_binmesh(plugin, vertex_count):
    payload = plugin.payload
    if len(payload) < 12:
        raise DffError('short native BINMESH header')
    _flags, num_meshes, total_indices = struct.unpack_from('<III', payload)
    at = 12
    seen_indices = 0
    for _mesh_index in range(num_meshes):
        header, at = _take(payload, at, 8, 'native BINMESH mesh header')
        num_indices, _material = struct.unpack('<Ii', header)
        raw, at = _take(payload, at, num_indices * 2, 'native BINMESH indices')
        indices = struct.unpack('<%dH' % num_indices, raw) if num_indices else ()
        if any(index >= vertex_count for index in indices):
            raise DffError('native BINMESH vertex index is out of range')
        seen_indices += num_indices
    if at != len(payload) or seen_indices != total_indices:
        raise DffError('native BINMESH payload length/count mismatch')


def _convert_geometry(chunk, stats):
    stats.geometries += 1
    children = _children(chunk.payload)
    struct_indexes = [i for i, child in enumerate(children)
                      if child.chunk_id == ID_STRUCT]
    extension_indexes = [i for i, child in enumerate(children)
                         if child.chunk_id == ID_EXTENSION]
    if len(struct_indexes) != 1 or len(extension_indexes) != 1:
        raise DffError('geometry does not have one STRUCT and EXTENSION')

    struct_index = struct_indexes[0]
    extension_index = extension_indexes[0]
    structure = children[struct_index]
    extension = children[extension_index]
    plugins = _children(extension.payload)

    if len(structure.payload) < GEO_HEADER.size:
        raise DffError('short geometry STRUCT')
    flags, num_triangles, num_vertices, num_morphs = \
        GEO_HEADER.unpack_from(structure.payload)
    if flags & GEO_NATIVE:
        stats.already_native += 1
        return chunk
    if any(plugin.chunk_id == ID_SKIN for plugin in plugins):
        stats.skipped_skin += 1
        return chunk
    binmesh_indexes = [i for i, plugin in enumerate(plugins)
                       if plugin.chunk_id == ID_BINMESH]
    if len(binmesh_indexes) != 1:
        stats.skipped_no_binmesh += 1
        return chunk
    if num_morphs != 1 or num_vertices <= 0 or num_triangles < 0:
        stats.skipped_morph += 1
        return chunk

    texcoord_count = _texcoord_sets(flags)
    if texcoord_count > 2:
        stats.skipped_range += 1
        return chunk

    payload = structure.payload
    at = GEO_HEADER.size
    surface = b''
    if _library_version(structure.library_id) < 0x34000:
        surface, at = _take(payload, at, 12, 'surface properties')
    colors = None
    if flags & GEO_PRELIT:
        colors, at = _take(payload, at, num_vertices * 4, 'prelight colors')
    uv_sets = []
    for uv_index in range(texcoord_count):
        uv, at = _take(payload, at, num_vertices * 8,
                       'texture coordinates %d' % uv_index)
        uv_sets.append(uv)
    _triangles, at = _take(payload, at, num_triangles * 8, 'triangles')

    sphere, at = _take(payload, at, 16, 'morph sphere')
    morph_flags, at = _take(payload, at, 8, 'morph flags')
    has_vertices, has_normals = struct.unpack('<ii', morph_flags)
    if not has_vertices:
        stats.skipped_morph += 1
        return chunk
    vertices, at = _take(payload, at, num_vertices * 12, 'positions')
    normals = None
    if has_normals:
        normals, at = _take(payload, at, num_vertices * 12, 'normals')
    if at != len(payload):
        raise DffError('unexpected bytes at end of geometry STRUCT')

    native = _native_blob(num_vertices, colors, uv_sets, vertices, normals)
    if native is None:
        stats.skipped_range += 1
        return chunk
    native_binmesh = _native_binmesh(plugins[binmesh_indexes[0]], num_vertices)

    new_flags = flags | GEO_NATIVE
    new_structure_payload = GEO_HEADER.pack(new_flags, num_triangles,
                                            num_vertices, num_morphs)
    new_structure_payload += surface + sphere + struct.pack('<ii', 0, 0)
    children[struct_index] = Chunk(ID_STRUCT, structure.library_id,
                                   new_structure_payload)

    # Idempotence: replace only a GameCube native-data plugin. Foreign native
    # chunks cannot accompany a generic Geometry, but preserving them is safer
    # than deleting data whose owner we do not understand.
    kept_plugins = []
    for plugin_index, plugin in enumerate(plugins):
        if plugin_index == binmesh_indexes[0]:
            kept_plugins.append(native_binmesh)
            continue
        if plugin.chunk_id == ID_NATIVEDATA:
            try:
                nested = _children(plugin.payload)
                if (nested and nested[0].chunk_id == ID_STRUCT and
                        len(nested[0].payload) >= 4 and
                        struct.unpack_from('<I', nested[0].payload)[0] ==
                        PLATFORM_GAMECUBE):
                    continue
            except DffError:
                pass
        kept_plugins.append(plugin)
    nested = Chunk(ID_STRUCT, chunk.library_id, native)
    kept_plugins.append(Chunk(ID_NATIVEDATA, chunk.library_id, nested.encode()))
    children[extension_index] = Chunk(
        ID_EXTENSION, extension.library_id,
        b''.join(plugin.encode() for plugin in kept_plugins))

    result = Chunk(ID_GEOMETRY, chunk.library_id,
                   b''.join(child.encode() for child in children))
    stats.converted += 1
    stats.generic_bytes += len(chunk.encode())
    stats.native_bytes += len(result.encode())
    return result


def _convert_geometry_list(chunk, stats):
    children = _children(chunk.payload)
    converted = []
    for child in children:
        converted.append(_convert_geometry(child, stats)
                         if child.chunk_id == ID_GEOMETRY else child)
    return Chunk(chunk.chunk_id, chunk.library_id,
                 b''.join(child.encode() for child in converted))


def _texture_name(texture):
    """Return the first STRING in a validated RenderWare Texture chunk."""
    if texture.chunk_id != ID_TEXTURE:
        return None
    children = _children(texture.payload)
    strings = [child for child in children if child.chunk_id == ID_STRING]
    if len(strings) < 2:
        return None
    raw = strings[0].payload
    end = raw.find(b'\0')
    if end <= 0 or end >= GC_MODEL_TEXTURE_NAME_BYTES:
        return None
    return raw[:end].decode('latin1')


def _geometry_texture_names(chunk):
    """Find ordinary and MatFX Texture chunks inside Geometry MATLISTs.

    MatFX embeds Texture chunks after scalar fields rather than as direct
    children of its plugin. RW chunks are four-byte aligned, so a validated
    scan of MATLIST payloads catches both layouts without reinterpreting
    third-party material plugins.
    """
    names = []
    for child in _children(chunk.payload):
        if child.chunk_id != ID_MATLIST:
            continue
        payload = child.payload
        for at in range(0, max(0, len(payload) - HEADER.size + 1), 4):
            chunk_id, length, library_id = HEADER.unpack_from(payload, at)
            if chunk_id != ID_TEXTURE:
                continue
            end = at + HEADER.size + length
            if end > len(payload):
                continue
            try:
                name = _texture_name(Chunk(chunk_id, library_id,
                                           payload[at + HEADER.size:end]))
            except DffError:
                continue
            if name:
                names.append(name)
    return names


def _clump_texture_names(children):
    names = []
    seen = set()
    for child in children:
        if child.chunk_id != ID_GEOMETRYLIST:
            continue
        for geometry in _children(child.payload):
            if geometry.chunk_id != ID_GEOMETRY:
                continue
            for name in _geometry_texture_names(geometry):
                folded = name.lower()
                if folded not in seen:
                    seen.add(folded)
                    names.append(name)
    return names


def _add_model_texture_plugin(children, names, library_id, stats):
    extension_indexes = [i for i, child in enumerate(children)
                         if child.chunk_id == ID_EXTENSION]
    if len(extension_indexes) > 1:
        raise DffError('clump has more than one EXTENSION')
    if extension_indexes:
        extension_index = extension_indexes[0]
        extension = children[extension_index]
        plugins = _children(extension.payload)
    else:
        extension_index = len(children)
        extension = Chunk(ID_EXTENSION, library_id, b'')
        plugins = []

    plugins = [plugin for plugin in plugins
               if plugin.chunk_id != ID_GC_MODEL_TEXTURES]
    if names:
        if len(names) > GC_MODEL_TEXTURES_MAX:
            raise DffError('model has too many texture dependencies')
        encoded = bytearray(GC_MODEL_TEXTURES_HEADER.pack(
            GC_MODEL_TEXTURES_MAGIC, len(names)))
        for name in names:
            raw = name.encode('latin1')
            if not raw or len(raw) >= GC_MODEL_TEXTURE_NAME_BYTES:
                raise DffError('texture name does not fit dependency plugin')
            encoded += raw.ljust(GC_MODEL_TEXTURE_NAME_BYTES, b'\0')
        plugins.append(Chunk(ID_GC_MODEL_TEXTURES, library_id, bytes(encoded)))
        stats.prefetch_models += 1
        stats.prefetch_textures += len(names)

    rebuilt = Chunk(ID_EXTENSION, extension.library_id,
                    b''.join(plugin.encode() for plugin in plugins))
    if extension_index == len(children):
        children.append(rebuilt)
    else:
        children[extension_index] = rebuilt


def _convert_clump(chunk, stats):
    children = _children(chunk.payload)
    texture_names = _clump_texture_names(children)
    converted = []
    for child in children:
        converted.append(_convert_geometry_list(child, stats)
                         if child.chunk_id == ID_GEOMETRYLIST else child)
    _add_model_texture_plugin(converted, texture_names, chunk.library_id, stats)
    return Chunk(chunk.chunk_id, chunk.library_id,
                 b''.join(child.encode() for child in converted))


def convert_bytes(data):
    """Return ``(converted_top_chunk, Stats)``; sector padding is discarded."""
    stats = Stats(files=1)
    try:
        top, _end = _one_chunk(data)
        if top.chunk_id != ID_CLUMP:
            return top.encode(), stats
        return _convert_clump(top, stats).encode(), stats
    except (DffError, OverflowError, struct.error, ValueError):
        # Conversion is transactional at file granularity. Do not report
        # geometries from a partially rebuilt clump that is being discarded.
        stats = Stats(files=1, failed=1)
        try:
            top, _end = _one_chunk(data)
            return top.encode(), stats
        except DffError:
            return data, stats


def verify_bytes(data):
    """Validate chunk boundaries plus the metadata/layout of GX native blobs."""
    top, end = _one_chunk(data)
    if end != len(data):
        raise DffError('trailing data after top chunk')
    if top.chunk_id != ID_CLUMP:
        return 0
    top_children = _children(top.payload)
    texture_plugins = []
    for child in top_children:
        if child.chunk_id == ID_EXTENSION:
            texture_plugins.extend(plugin for plugin in _children(child.payload)
                                   if plugin.chunk_id == ID_GC_MODEL_TEXTURES)
    if len(texture_plugins) > 1:
        raise DffError('duplicate GameCube model texture plugin')
    if texture_plugins:
        payload = texture_plugins[0].payload
        if len(payload) < GC_MODEL_TEXTURES_HEADER.size:
            raise DffError('short GameCube model texture plugin')
        magic, texture_count = GC_MODEL_TEXTURES_HEADER.unpack_from(payload)
        if texture_count > GC_MODEL_TEXTURES_MAX:
            raise DffError('too many GameCube model texture dependencies')
        expected = GC_MODEL_TEXTURES_HEADER.size + \
            texture_count * GC_MODEL_TEXTURE_NAME_BYTES
        if magic != GC_MODEL_TEXTURES_MAGIC or len(payload) != expected:
            raise DffError('invalid GameCube model texture plugin')
        for index in range(texture_count):
            start = GC_MODEL_TEXTURES_HEADER.size + \
                index * GC_MODEL_TEXTURE_NAME_BYTES
            raw = payload[start:start + GC_MODEL_TEXTURE_NAME_BYTES]
            if not raw[0] or b'\0' not in raw:
                raise DffError('invalid model texture dependency name')
    count = 0
    for child in top_children:
        if child.chunk_id != ID_GEOMETRYLIST:
            continue
        for geometry in _children(child.payload):
            if geometry.chunk_id != ID_GEOMETRY:
                continue
            geo_children = _children(geometry.payload)
            structures = [geo_child for geo_child in geo_children
                          if geo_child.chunk_id == ID_STRUCT]
            if len(structures) != 1 or len(structures[0].payload) < 16:
                raise DffError('invalid geometry STRUCT during verification')
            flags, _triangles, vertex_count, _morphs = \
                GEO_HEADER.unpack_from(structures[0].payload)
            saw_gamecube_native = False
            for geo_child in geo_children:
                if geo_child.chunk_id != ID_EXTENSION:
                    continue
                for plugin in _children(geo_child.payload):
                    if plugin.chunk_id != ID_NATIVEDATA:
                        continue
                    nested = _children(plugin.payload)
                    if len(nested) != 1 or nested[0].chunk_id != ID_STRUCT:
                        raise DffError('malformed native-data plugin')
                    blob = nested[0].payload
                    if len(blob) < 24:
                        raise DffError('short native-data STRUCT')
                    platform, version, _n, _attrs, shifts, size = \
                        struct.unpack_from('<IIIIII', blob)
                    if (platform != PLATFORM_GAMECUBE or
                            version != NATIVE_VERSION or shifts >> 16 or
                            len(blob) != 24 + size):
                        raise DffError('invalid GameCube native metadata')
                    saw_gamecube_native = True
                    count += 1
            if saw_gamecube_native:
                if not flags & GEO_NATIVE:
                    raise DffError('native-data plugin on generic geometry')
                extensions = [geo_child for geo_child in geo_children
                              if geo_child.chunk_id == ID_EXTENSION]
                binmeshes = [plugin for plugin in _children(extensions[0].payload)
                             if plugin.chunk_id == ID_BINMESH]
                if len(binmeshes) != 1:
                    raise DffError('native geometry lacks one BINMESH')
                _verify_native_binmesh(binmeshes[0], vertex_count)
    return count
