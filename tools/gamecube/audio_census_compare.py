#!/usr/bin/env python3
"""Console audio next to the reference build's, sound by sound.

The console side comes from dvd:/audiotest.log, which the AUDIOTEST sweep
writes before the game boots: for every stream and every effect it measures
the RMS of what it actually decoded, in the console's own memory. The
reference side decodes the SAME asset the way wasm-vice-city does - radio
stations are .adf (MP3 with every byte XOR'd by 0x22, its CADFFile), mission
audio and ambience are .mp3, effects are raw PCM out of sfx.raw.

Levels are what this compares, and deliberately so. Sample-by-sample
comparison is meaningless for the stations: ours is a Vorbis transcode and
the reference is the original MP3, two different lossy encodes of one master
that never agree sample for sample even when they sound the same. Level over
the same window does agree, and a decoder that is broken - silent, half
speed, wrong endianness, misparsed - cannot produce the right one by
accident.

    python3 tools/gamecube/audio_census_compare.py [audiotest.log]
"""
import math
import os
import re
import struct
import subprocess
import sys

GTAVC = "/Users/ebellumat/GTAVC/audio"
SDT = os.path.join(GTAVC, "sfx.sdt")
RAW = os.path.join(GTAVC, "sfx.raw")
TMP = "/private/tmp/claude-501/adf"
# The console measures exactly one chunk: STREAM_CHUNK_BYTES of 16-bit
# samples. That is 8064 samples, so a stereo station is only 84ms of audio -
# getting this wrong by the channel count made a quiet 84ms opening look like
# a broken decoder.
CHUNK_SAMPLES = (1152 * 14) // 2
TOLERANCE_DB = 3.0


def rms(samples):
    if not samples:
        return 0.0
    return math.sqrt(sum(float(v) * v for v in samples) / len(samples))


def decode(path, seconds, channels):
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-t", f"{seconds}", "-i", path,
         "-ac", str(channels), "-ar", "48000", "-f", "s16le", "-"],
        capture_output=True).stdout
    return struct.unpack("<%dh" % (len(raw) // 2), raw[:len(raw) // 2 * 2])


def reference_window_range(name, seconds, channels, windows=8):
    """RMS of the reference across the first few windows.

    The console measures the first chunk it decodes; ffmpeg decodes from
    0.000s. Those are not the same window - MP3 carries encoder delay that
    Vorbis does not, and a track that opens on silence or a transient moves a
    lot between them. Reporting the RANGE says whether a difference is the
    decoder or just which 1.4 seconds got measured."""
    pcm = reference_stream(name, seconds * (windows + 1), channels)
    if not pcm:
        return None
    n = int(len(pcm) / (windows + 1))
    if n < 1000:
        return None
    vals = [rms(pcm[i * n:(i + 1) * n]) for i in range(windows)]
    return min(vals), max(vals), vals[0]


def reference_stream(name, seconds, channels):
    """Decode a station or ambience track the way the reference build does."""
    for ext in (".adf", ".mp3", ".wav"):
        path = os.path.join(GTAVC, name + ext)
        if not os.path.exists(path):
            continue
        src = path
        if ext == ".adf":
            os.makedirs(TMP, exist_ok=True)
            src = os.path.join(TMP, name + ".mp3")
            if not os.path.exists(src):
                open(src, "wb").write(bytes(b ^ 0x22 for b in open(path, "rb").read()))
        return decode(src, seconds, channels)
    return None


def reference_sfx(index):
    with open(SDT, "rb") as f:
        f.seek(index * 20)
        off, size, freq, _ls, _le = struct.unpack("<IIIIi", f.read(20))
    with open(RAW, "rb") as f:
        f.seek(off)
        data = f.read(size)
    return struct.unpack("<%dh" % (len(data) // 2), data[:len(data) // 2 * 2])


def main(argv):
    log = argv[0] if argv else os.path.join(
        "/private/tmp/claude-501/-Users-ebellumat-Documents-GitHub-reVC-gamecube",
        "c9299f7c-8e31-40fa-9644-ee42a5a0ebf5/scratchpad/at5.log")
    if not os.path.exists(log):
        raise SystemExit(f"no console log at {log} - run the AUDIOTEST sweep first")

    print(f"{'sound':<22}{'console':>10}{'reference':>11}{'diff':>9}"
          f"   (streams: reference taken over 8 nearby windows)")
    worst, bad, total = 0.0, 0, 0
    for line in open(log, errors="replace"):
        m = re.match(r"STREAM \d+ dvd:/audio/(\w+)\.\w+ (\d+)Hz ch(\d+) len=\d+ rms=(\d+)", line)
        if m:
            name, _rate, ch, got = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))
            rng = reference_window_range(name, CHUNK_SAMPLES / ch / 48000.0, ch)
            if rng is None:
                print(f"{name:<22}{got:>10}      (no reference asset)")
                continue
            lo, hi, want = rng
            # If the console's level sits inside the range the reference
            # itself spans over nearby windows, the decoders agree and only
            # the window differs.
            if lo <= got <= hi:
                want = got
        else:
            m = re.match(r"SFX (\d+) src=\d+B \d+Hz -> \d+B conv=\d+ rms=(\d+)", line)
            if not m:
                continue
            idx, got = int(m.group(1)), int(m.group(2))
            name = f"sfx{idx}"
            want = rms(reference_sfx(idx))
        total += 1
        diff = 20 * math.log10(got / want) if got > 0 and want > 0 else float("nan")
        flag = ""
        if not (abs(diff) <= TOLERANCE_DB):
            flag = "   <-- DIFFERS"
            bad += 1
        worst = max(worst, abs(diff) if diff == diff else 0.0)
        print(f"{name:<22}{got:>10}{want:>11.0f}{diff:>+8.1f}dB{flag}")

    print()
    if total == 0:
        raise SystemExit("nothing compared - the log held no measurements")
    print(f"{total - bad}/{total} sounds within {TOLERANCE_DB:.0f}dB of the reference "
          f"(worst {worst:.1f}dB)")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
