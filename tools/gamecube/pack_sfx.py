#!/usr/bin/env python3
"""Losslessly pack Vice City's random-access PCM sample bank.

The GameCube disc cannot spare 340 MB for sfx.raw. Each sample is predicted
independently, zig-zag encoded, byte-shuffled, then DEFLATE-compressed so the
runtime can still seek and decode one ped comment without inflating the bank.
The reconstructed 16-bit PCM is byte-for-byte identical to sfx.raw.
"""
import argparse
import os
import struct
import sys
import zlib


MAGIC = b"GCSFXP2\0"
HEADER = struct.Struct(">8sII")
ENTRY = struct.Struct(">II")
RAW_FLAG = 0x80000000
ALIGNMENT = 32
# Matches SAMPLEBANK_PED_START in AudioSamples.h. This first 15MB is loaded
# once at boot, so keeping it raw and contiguous avoids hundreds of tiny
# DEFLATE operations on the Gekko. The remaining 325MB is random-access data.
RESIDENT_SAMPLES = 524


def align(value):
    return (value + ALIGNMENT - 1) & -ALIGNMENT


def read_sdt(path):
    data = open(path, "rb").read()
    if len(data) % 20:
        raise ValueError("sfx.sdt size is not a multiple of 20")
    return [struct.unpack_from("<IIIII", data, offset)
            for offset in range(0, len(data), 20)]


def predict(data):
    if len(data) % 2:
        raise ValueError("16-bit PCM sample has an odd byte count")
    count = len(data) // 2
    low = bytearray(count)
    high = bytearray(count)
    previous = 0
    for index, (sample,) in enumerate(struct.iter_unpack("<h", data)):
        delta = (sample - previous) & 0xFFFF
        if delta >= 0x8000:
            delta -= 0x10000
        previous = sample
        zigzag = ((delta << 1) ^ (delta >> 15)) & 0xFFFF
        low[index] = zigzag & 0xFF
        high[index] = zigzag >> 8
    return low + high


def restore(data):
    if len(data) % 2:
        raise ValueError("predicted sample has an odd byte count")
    count = len(data) // 2
    out = bytearray(len(data))
    previous = 0
    for index in range(count):
        zigzag = data[index] | data[count + index] << 8
        delta = (zigzag >> 1) ^ -(zigzag & 1)
        previous = (previous + delta) & 0xFFFF
        struct.pack_into("<H", out, index * 2, previous)
    return out


def pack(raw_path, sdt_path, output_path, verify):
    samples = read_sdt(sdt_path)
    table_end = HEADER.size + ENTRY.size * len(samples)
    data_start = align(table_end)
    entries = []
    original_bytes = packed_bytes = 0

    with open(raw_path, "rb") as raw, open(output_path, "wb+") as output:
        output.write(b"\0" * data_start)
        for index, (offset, size, _rate, _loop_start, _loop_end) in enumerate(samples):
            raw.seek(offset)
            original = raw.read(size)
            if len(original) != size:
                raise ValueError(f"sample {index}: sfx.raw is truncated")
            encoded = original if index < RESIDENT_SAMPLES else zlib.compress(predict(original), 9)
            flags = RAW_FLAG if index < RESIDENT_SAMPLES else 0
            if not flags and len(encoded) >= size:
                encoded = original
                flags = RAW_FLAG
            position = output.tell()
            output.write(encoded)
            # Resident entries must be one direct, seekable run. Random
            # entries are aligned individually for disc reads.
            padding = 0 if index + 1 < RESIDENT_SAMPLES else align(output.tell()) - output.tell()
            if padding:
                output.write(b"\0" * padding)
            entries.append((position, len(encoded) | flags))
            original_bytes += size
            packed_bytes += len(encoded)
            if verify:
                decoded = encoded if flags else restore(zlib.decompress(encoded))
                if decoded != original:
                    raise ValueError(f"sample {index}: lossless verification failed")

        output.seek(0)
        output.write(HEADER.pack(MAGIC, len(samples), data_start))
        for entry in entries:
            output.write(ENTRY.pack(*entry))

    saved = original_bytes - os.path.getsize(output_path)
    print(f"packed {len(samples)} samples: {original_bytes} -> "
          f"{os.path.getsize(output_path)} bytes ({saved} saved)")


def verify_pack(raw_path, sdt_path, pack_path):
    samples = read_sdt(sdt_path)
    with open(pack_path, "rb") as packed, open(raw_path, "rb") as raw:
        magic, count, data_start = HEADER.unpack(packed.read(HEADER.size))
        if magic != MAGIC or count != len(samples):
            raise ValueError("pack header does not match sfx.sdt")
        if data_start < HEADER.size + count * ENTRY.size:
            raise ValueError("pack data overlaps its table")
        table = [ENTRY.unpack(packed.read(ENTRY.size)) for _ in range(count)]
        pack_size = os.path.getsize(pack_path)
        for index, ((raw_offset, raw_size, *_), (offset, size_flags)) in enumerate(zip(samples, table)):
            stored = bool(size_flags & RAW_FLAG)
            size = size_flags & ~RAW_FLAG
            if offset < data_start or size == 0 or offset + size > pack_size:
                raise ValueError(f"sample {index}: invalid packed range")
            packed.seek(offset)
            data = packed.read(size)
            decoded = data if stored else restore(zlib.decompress(data))
            raw.seek(raw_offset)
            if len(decoded) != raw_size or decoded != raw.read(raw_size):
                raise ValueError(f"sample {index}: differs from sfx.raw")
    print(f"PASS: {len(samples)}/{len(samples)} packed samples are bit-exact")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", help="source sfx.raw")
    parser.add_argument("sdt", help="source sfx.sdt")
    parser.add_argument("output", help="output sfx.pak")
    parser.add_argument("--verify", action="store_true",
                        help="decode and compare every sample while packing")
    parser.add_argument("--check", action="store_true",
                        help="verify an existing pack instead of replacing it")
    args = parser.parse_args()
    try:
        if args.check:
            verify_pack(args.raw, args.sdt, args.output)
        else:
            pack(args.raw, args.sdt, args.output, args.verify)
    except (OSError, ValueError, zlib.error) as error:
        sys.exit(f"pack_sfx: {error}")


if __name__ == "__main__":
    main()
