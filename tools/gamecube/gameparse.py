#!/usr/bin/env python3
"""Parse every TXD in an archive the way the console does, not the way librw does.

Mirrors src/rw/TexRead.cpp (RwTexDictionaryGtaStreamRead / RwTextureGtaStreamRead),
src/fakerw/fake.cpp (rwNativeTextureHackRead) and gx::readNativeTexture, including
the checks that have no log behind them on the console. A dictionary that fails
here is one the streamer will re-request forever.

Usage: gameparse.py <img> <dir> [limit]
"""
import struct
import sys
import zlib

SECTOR = 2048
ID_STRUCT, ID_EXTENSION, ID_TEXTURENATIVE, ID_TEXDICTIONARY = 1, 3, 0x15, 0x16
GXFMT_CMPR = 0xE
GXFMT_CI8 = 0x9
HEADER = 88
TXD_MAGIC = b'GCTXDZ1\0'
TXD_HEADER = struct.Struct('<8sII')


class Fail(Exception):
    pass


class Stream:
    def __init__(self, buf, end, pos=0):
        self.b, self.p, self.end = buf, pos, end

    def u32(self):
        v, = struct.unpack_from('<I', self.b, self.p)
        self.p += 4
        return v

    def find(self, want):
        # findChunk: skip chunks until `want`, mirroring librw
        while self.p + 12 <= self.end:
            cid, size, _ver = struct.unpack_from('<III', self.b, self.p)
            self.p += 12
            if cid == want:
                return size
            self.p += size
        raise Fail('chunk %02x not found' % want)


def read_native(s, end):
    """gx::readNativeTexture + the extension read + the bounded-end check."""
    struct_size = s.find(ID_STRUCT)
    if struct_size < HEADER:
        raise Fail('structSize %d < 88' % struct_size)
    h = s.b[s.p:s.p + HEADER]
    s.p += HEADER
    plat, = struct.unpack_from('<I', h, 0)
    if plat != 6:
        raise Fail('platform %d != PLATFORM_GAMECUBE' % plat)
    if b'\0' not in h[8:40] or b'\0' not in h[40:72]:
        raise Fail('name/mask not NUL-terminated')
    tw, th = struct.unpack_from('<HH', h, 80)
    if not (0 < tw <= 1024 and 0 < th <= 1024):
        raise Fail('dims %dx%d' % (tw, th))
    gxfmt = h[87]
    size = s.u32()
    expect = (tw * th // 2 if gxfmt == GXFMT_CMPR else
              tw * th + 512 if gxfmt == GXFMT_CI8 else tw * th * 2)
    if size != expect:
        raise Fail('payload %d != expected %d' % (size, expect))
    s.p += size
    s.find(ID_EXTENSION)          # Texture::s_plglist.streamRead
    if s.p != end:
        raise Fail('native read ended at %d, chunk ends at %d' % (s.p, end))


def read_dict(buf):
    s = Stream(buf, len(buf))
    dict_size = s.find(ID_TEXDICTIONARY)
    end = s.p + dict_size
    if end > len(buf) or end - s.p < 12:
        raise Fail('dictionary end %d past buffer %d' % (end, len(buf)))
    size = s.find(ID_STRUCT)
    if size != 4:
        raise Fail('dict struct is %d bytes, not 4' % size)
    n = s.u32()                    # the game reads all four bytes as one int32
    if n > 0x7FFF:
        raise Fail('texture count %d > INT16_MAX (deviceId bled into it?)' % n)
    for _ in range(n):
        if s.p > end or end - s.p < 12:
            raise Fail('ran past dictionary end')
        tex_size = s.find(ID_TEXTURENATIVE)
        native_start = s.p
        if native_start > end or tex_size > end - native_start:
            raise Fail('texture chunk overruns dictionary')
        stop = native_start + tex_size
        read_native(Stream(buf, stop, native_start), stop)
        s.p = native_start + tex_size
    if s.p > end:
        raise Fail('ended past dictionary end')
    return n


def unpack_entry(buf):
    if not buf.startswith(TXD_MAGIC):
        return buf
    _magic, raw_size, packed_size = TXD_HEADER.unpack_from(buf)
    start = TXD_HEADER.size
    end = start + packed_size
    if end > len(buf):
        raise Fail('packed TXD overruns IMG entry')
    raw = zlib.decompress(buf[start:end])
    if len(raw) != raw_size:
        raise Fail('packed TXD expanded to %d, expected %d' %
                   (len(raw), raw_size))
    return raw


def main():
    img_path, dir_path = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    d = open(dir_path, 'rb').read()
    img = open(img_path, 'rb')
    ok = bad = 0
    for i in range(len(d) // 32):
        off, size, raw = struct.unpack_from('<II24s', d, i * 32)
        name = raw.split(b'\0')[0].decode('latin1')
        if not name.lower().endswith('.txd'):
            continue
        img.seek(off * SECTOR)
        buf = img.read(size * SECTOR)
        try:
            buf = unpack_entry(buf)
            read_dict(buf)
            ok += 1
        except Fail as e:
            bad += 1
            if bad <= 20:
                print('FAIL %-24s %s' % (name, e))
        except Exception as e:                                  # noqa: BLE001
            bad += 1
            if bad <= 20:
                print('FAIL %-24s malformed: %r' % (name, e))
        if limit and ok + bad >= limit:
            break
    print('parsed as the game does: %d ok, %d rejected' % (ok, bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
