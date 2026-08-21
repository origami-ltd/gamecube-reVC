#!/usr/bin/env python3
"""Pack sfx.raw into a compressed bank the console reads per sample.

sfx.raw is 340MB of raw PCM and the disc is a 1.46GB mini-DVD, so most of
that has to go. Vorbis is the right tool for the long sounds - the ped
speech that makes up the bulk is 4x to 7x smaller - but NOT for the short
ones: a 1400-byte effect encodes to 3817 bytes, because a Vorbis stream
carries three header packets whatever the payload. Measured across the
bank, everything under about 5KB comes out bigger.

So the choice is per sample: encode it, keep whichever is smaller, and
record which one it was. The console reads sfx.idx to know.

    python3 tools/gamecube/pack_sfx.py <GTAVC/audio> <out dir> [--quality 3]

Writes sfx.pak (payloads) and sfx.idx (big-endian u32 offset, u32 size,
u32 flags - flag 1 means Vorbis). sfx.sdt still carries rate and decoded
size and is unchanged.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor

VORBIS = 1


def encode_one(job):
    index, pcm, freq, quality = job
    if len(pcm) < 64 or freq == 0:
        return index, pcm, 0
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "s.raw")
        dst = os.path.join(tmp, "s.ogg")
        open(src, "wb").write(pcm)
        r = subprocess.run(
            ["sox", "-t", "raw", "-r", str(freq), "-e", "signed", "-b", "16",
             "-c", "1", "-L", src, "-C", str(quality), dst],
            capture_output=True)
        if r.returncode != 0 or not os.path.exists(dst):
            return index, pcm, 0
        ogg = open(dst, "rb").read()
    # Only worth it if it is actually smaller.
    return (index, ogg, VORBIS) if len(ogg) < len(pcm) else (index, pcm, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", help="the install's audio directory")
    ap.add_argument("dst", help="where sfx.pak and sfx.idx go")
    ap.add_argument("--quality", type=float, default=3,
                    help="sox -C for the samples; 3 is transparent on these")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    args = ap.parse_args()

    sdt = open(os.path.join(args.src, "sfx.sdt"), "rb").read()
    count = len(sdt) // 20
    raw = open(os.path.join(args.src, "sfx.raw"), "rb")
    print(f"{count} samples")

    jobs = []
    for i in range(count):
        off, size, freq, _ls, _le = struct.unpack_from("<IIIIi", sdt, i * 20)
        raw.seek(off)
        jobs.append((i, raw.read(size), freq, args.quality))

    results = [None] * count
    done = 0
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for index, payload, flags in pool.map(encode_one, jobs, chunksize=32):
            results[index] = (payload, flags)
            done += 1
            if done % 500 == 0:
                print(f"  {done}/{count}", flush=True)

    os.makedirs(args.dst, exist_ok=True)
    pak = open(os.path.join(args.dst, "sfx.pak"), "wb")
    idx = open(os.path.join(args.dst, "sfx.idx"), "wb")
    offset = 0
    vorbis_count = raw_bytes = packed_bytes = 0
    for i in range(count):
        payload, flags = results[i]
        # 32-byte aligned: the console DMAs these into audio memory.
        pad = (-offset) & 31
        if pad:
            pak.write(b"\0" * pad)
            offset += pad
        pak.write(payload)
        idx.write(struct.pack(">III", offset, len(payload), flags))
        offset += len(payload)
        packed_bytes += len(payload)
        vorbis_count += 1 if flags & VORBIS else 0
        _o, size, _f, _a, _b = struct.unpack_from("<IIIIi", sdt, i * 20)
        raw_bytes += size
    pak.close()
    idx.close()

    print(f"vorbis {vorbis_count}/{count} samples")
    print(f"sfx.raw {raw_bytes/1048576:.1f}MB -> sfx.pak {packed_bytes/1048576:.1f}MB "
          f"({packed_bytes/raw_bytes:.2f}x)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
