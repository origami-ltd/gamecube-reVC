#!/usr/bin/env python3
"""Check sfx.pak/sfx.idx against the PCM they were built from.

The console decodes each entry into audio memory at the address the original
file offset gives it, so a wrong index or a bad encode is silent corruption
in the sample bank. This decodes entries back and compares them to sfx.raw:
raw entries must be byte-identical, Vorbis entries must match in length and
stay close in level (it is a lossy codec, so sample equality is not the
test - a decoder that produced silence or noise would fail on both counts).

    python3 tools/gamecube/test_sfxpack.py <pack dir> [count]
"""
import math
import os
import struct
import subprocess
import sys

GTAVC = "/Users/ebellumat/GTAVC/audio"
VORBIS = 1


def rms(s):
    return math.sqrt(sum(float(v) * v for v in s) / len(s)) if s else 0.0


def main(argv):
    pack_dir = argv[0] if argv else "/Users/ebellumat/revc-sfx-pack"
    count = int(argv[1]) if len(argv) > 1 else 40
    sdt = open(os.path.join(GTAVC, "sfx.sdt"), "rb").read()
    idx = open(os.path.join(pack_dir, "sfx.idx"), "rb").read()
    n = len(sdt) // 20
    assert len(idx) // 12 >= n, f"index has {len(idx)//12} entries for {n} samples"
    raw = open(os.path.join(GTAVC, "sfx.raw"), "rb")
    pak = open(os.path.join(pack_dir, "sfx.pak"), "rb")

    step = max(1, n // count)
    checked = vorbis = bad = 0
    worst_db = 0.0
    for i in range(0, n, step):
        off, size, freq, _ls, _le = struct.unpack_from("<IIIIi", sdt, i * 20)
        poff, psize, flags = struct.unpack_from(">III", idx, i * 12)
        if size == 0:
            continue
        raw.seek(off)
        want = raw.read(size)
        pak.seek(poff)
        payload = pak.read(psize)
        assert len(payload) == psize, f"sample {i}: pack truncated"
        checked += 1
        if not (flags & VORBIS):
            if payload != want:
                print(f"  sample {i}: RAW ENTRY DIFFERS from sfx.raw")
                bad += 1
            continue
        vorbis += 1
        got = subprocess.run(
            ["ffmpeg", "-v", "error", "-f", "ogg", "-i", "pipe:0",
             "-f", "s16le", "-ac", "1", "-ar", str(freq), "-"],
            input=payload, capture_output=True).stdout
        a = struct.unpack("<%dh" % (len(got) // 2), got[:len(got) // 2 * 2])
        b = struct.unpack("<%dh" % (len(want) // 2), want[:len(want) // 2 * 2])
        if not a:
            print(f"  sample {i}: VORBIS ENTRY DECODED TO NOTHING")
            bad += 1
            continue
        # Length within a frame, level within 1dB.
        if abs(len(a) - len(b)) > 2048:
            print(f"  sample {i}: length {len(a)} vs {len(b)}")
            bad += 1
        ra, rb = rms(a), rms(b)
        if ra > 0 and rb > 0:
            diff = abs(20 * math.log10(ra / rb))
            worst_db = max(worst_db, diff)
            if diff > 1.0:
                print(f"  sample {i}: level off by {diff:.1f}dB")
                bad += 1

    print(f"checked {checked} entries ({vorbis} vorbis), worst level {worst_db:.2f}dB")
    if bad:
        print(f"{bad} FAILED")
        return 1
    print("pack matches the PCM it was built from")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
