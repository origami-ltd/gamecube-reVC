#!/usr/bin/env python3
"""Check the console's IMA ADPCM voice decoder against ffmpeg.

The runtime decoder lives in src/audio/sampman_gamecube.cpp (gcImaNibble /
gcWavDecode); this is the same algorithm in Python, so a mismatch here means
the C is wrong too. Voice ships native — 4-bit ADPCM, mono, 512-byte blocks,
exactly the game's own files — so this decode is the whole voice path.

    python3 tools/gamecube/test_adpcm.py [wav ...]

Defaults to a few files from the install if none are named.
"""
import glob
import struct
import subprocess
import sys

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
        41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
        190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
        724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
        6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
        16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def nibble(n, pred, idx):
    step = STEP[idx]
    diff = step >> 3
    if n & 1:
        diff += step >> 2
    if n & 2:
        diff += step >> 1
    if n & 4:
        diff += step
    if n & 8:
        diff = -diff
    pred = max(-32768, min(32767, pred + diff))
    return pred, max(0, min(88, idx + INDEX[n & 15]))


def decode(path):
    d = open(path, "rb").read()
    pos, block, data, size = 12, None, None, 0
    while pos + 8 <= len(d):
        chunk = d[pos:pos + 4]
        sz = struct.unpack_from("<I", d, pos + 4)[0]
        if chunk == b"fmt ":
            block = struct.unpack_from("<H", d, pos + 8 + 12)[0]
        elif chunk == b"data":
            data, size = pos + 8, sz
            break
        pos += 8 + ((sz + 1) & ~1)
    if block is None or data is None:
        raise SystemExit(f"{path}: not a WAVE with fmt+data")
    out = []
    for off in range(data, data + size - block + 1, block):
        blk = d[off:off + block]
        pred = struct.unpack_from("<h", blk, 0)[0]
        idx = min(88, blk[2])
        out.append(pred)
        for byte in blk[4:]:
            pred, idx = nibble(byte & 15, pred, idx)
            out.append(pred)
            pred, idx = nibble(byte >> 4, pred, idx)
            out.append(pred)
    return out


def main(paths):
    if not paths:
        paths = sorted(glob.glob("/Users/ebellumat/GTAVC/audio/*.wav"))[:5]
    if not paths:
        raise SystemExit("no wavs given and none found in the install")
    for path in paths:
        mine = decode(path)
        raw = subprocess.run(
            ["ffmpeg", "-v", "error", "-i", path, "-f", "s16le", "-"],
            capture_output=True).stdout
        ref = list(struct.unpack(f"<{len(raw) // 2}h", raw[:len(raw) // 2 * 2]))
        n = min(len(mine), len(ref))
        worst = max((abs(a - b) for a, b in zip(mine[:n], ref[:n])), default=0)
        assert n > 0, f"{path}: decoded nothing"
        assert worst == 0, f"{path}: differs from ffmpeg by {worst}"
        print(f"ok {path.split('/')[-1]}: {n} samples, bit-exact")
    print("ADPCM decoder matches ffmpeg")


if __name__ == "__main__":
    main(sys.argv[1:])
