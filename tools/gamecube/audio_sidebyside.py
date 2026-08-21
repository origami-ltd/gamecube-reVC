#!/usr/bin/env python3
"""Put the console's audio next to the reference build's, sound by sound.

The reference is wasm-vice-city (the PC/OAL path): it plays the ORIGINAL
assets - radio stations are .adf, which is MP3 with every byte XOR'd by 0x22
(CADFFile : CMP3File in src/audio/oal/stream.cpp), mission audio is .mp3,
voice is IMA ADPCM .wav, effects are raw PCM out of sfx.raw. The GameCube
build plays transcodes of those same assets. This decodes the originals the
way the reference does, decodes what the console actually emitted from
Dolphin's DSP dump, and compares them sound by sound.

Run the console side first:

    put dvd:/audiotest.txt on the card, boot, let it park
    python3 tools/gamecube/audio_sidebyside.py

Each item is fed for exactly 2s with a 1s gap, so the dump segments on
silence. (The DSP may finish its already queued blocks after the two seconds.)
Reported per sound: level (RMS, in dB relative to the reference), spectral
centroid (brightness), and level-normalised per-band energy difference. A
band below -50dB of the reference's peak band is omitted: dividing by its
near-zero energy produced spectacular but meaningless false failures.
"""
import glob
import math
import os
import struct
import subprocess
import sys
import wave

GTAVC = "/Users/ebellumat/GTAVC/audio"
SDT = os.path.join(GTAVC, "sfx.sdt")
RAW = os.path.join(GTAVC, "sfx.raw")
DUMP_DIR = os.path.expanduser("~/Library/Application Support/Dolphin/Dump/Audio")

# Must mirror gAudioTestSfx and the stream loop in sampman_gamecube.cpp.
STREAMS = ["wild", "flash", "kchat", "fever", "vrock", "vcpr",
           "espant", "emotion", "wave", "miscom", "city", "water"]
SFX = [0, 1, 11, 19, 33, 37, 43, 154, 291, 320, 321, 322, 323]
BANDS = [(60, 250), (250, 1000), (1000, 4000), (4000, 10000), (10000, 20000)]


def run(cmd):
    return subprocess.run(cmd, capture_output=True).stdout


def dexor(src, dst):
    """.adf is MP3 with every byte XOR'd by 0x22 - what CADFFile undoes."""
    data = open(src, "rb").read()
    open(dst, "wb").write(bytes(b ^ 0x22 for b in data))


def decode_reference(name, tmpdir, rate, seconds=2.0, skip=0.0):
    """Decode an original asset the way the reference build does."""
    for ext, prep in ((".adf", dexor), (".mp3", None), (".wav", None)):
        path = os.path.join(GTAVC, name + ext)
        if not os.path.exists(path):
            continue
        src = path
        if prep:
            src = os.path.join(tmpdir, name + ".mp3")
            if not os.path.exists(src):
                prep(path, src)
        raw = run(["ffmpeg", "-v", "error", "-ss", str(skip), "-t", str(seconds),
                   "-i", src, "-ac", "1", "-ar", str(rate),
                   "-f", "s16le", "-"])
        return list(struct.unpack("<%dh" % (len(raw) // 2), raw[:len(raw) // 2 * 2]))
    return None


def sfx_reference(index, rate):
    with open(SDT, "rb") as f:
        f.seek(index * 20)
        off, size, freq, _ls, _le = struct.unpack("<IIIIi", f.read(20))
    with open(RAW, "rb") as f:
        f.seek(off)
        data = f.read(size)
    src = list(struct.unpack("<%dh" % (len(data) // 2), data[:len(data) // 2 * 2]))
    # The reference plays it at its own rate; resample to the dump's actual
    # output rate (32,028Hz on GameCube and 48kHz on the Wii dev target).
    out, n = [], len(src)
    if n < 2:
        return src
    step = freq / float(rate)
    pos = 0.0
    while pos < n - 1:
        i = int(pos)
        f2 = pos - i
        out.append(int(src[i] + (src[i + 1] - src[i]) * f2))
        pos += step
    return out


def rms(sig):
    if not sig:
        return 0.0
    return math.sqrt(sum(float(v) * v for v in sig) / len(sig))


def goertzel(sig, rate, freq):
    w = 2.0 * math.pi * freq / rate
    c = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for v in sig:
        s0 = v + c * s1 - s2
        s2, s1 = s1, s0
    return s1 * s1 + s2 * s2 - c * s1 * s2


def spectrum(sig, rate):
    """Energy per band, sampled with a few probes per band."""
    out = []
    for lo, hi in BANDS:
        if hi > rate / 2:
            hi = rate / 2 - 100
        if lo >= hi:
            out.append(0.0)
            continue
        total = 0.0
        for k in range(6):
            f = lo * (hi / lo) ** ((k + 0.5) / 6)
            total += goertzel(sig, rate, f)
        out.append(total / 6)
    return out


def centroid(sig, rate):
    bands = spectrum(sig, rate)
    num = den = 0.0
    for (lo, hi), e in zip(BANDS, bands):
        f = math.sqrt(lo * hi)
        num += f * e
        den += e
    return num / den if den else 0.0


def amplitude_db(a, b):
    if a <= 0 or b <= 0:
        return float("nan")
    return 20.0 * math.log10(a / b)


def power_db(a, b):
    """spectrum() returns energy, not amplitude; its ratio is 10*log10."""
    if a <= 0 or b <= 0:
        return float("nan")
    return 10.0 * math.log10(a / b)


def load_dump(path, max_seconds=150):
    with wave.open(path, "rb") as w:
        n, ch, rate = w.getnframes(), w.getnchannels(), w.getframerate()
        raw = w.readframes(min(n, rate * max_seconds))
    s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    mono = [(s[i] + s[i + 1]) // 2 for i in range(0, len(s) - 1, ch)] if ch > 1 else list(s)
    return mono, rate


def envelope(sig, rate, frame_ms=20):
    """RMS per frame. Two different lossy encodes of the same music never
    match sample for sample, but their envelopes do - so this is what the
    stream comparison aligns on."""
    n = max(1, int(rate * frame_ms / 1000))
    return [math.sqrt(sum(float(v) * v for v in sig[i:i + n]) / n)
            for i in range(0, len(sig) - n, n)]


def active_segments(dump, rate, threshold=10.0, join_silence=0.5, pad=0.08):
    """Return active regions in chronological order.

    The old analyser searched the complete recording separately for every
    effect. Repeated engine/noise-like sounds then matched some other item,
    and a valid capture reported 1/25. The self-test has a deliberate
    one-second gap, so chronological segmentation is both simpler and exact.
    """
    frame_ms = 20
    env = envelope(dump, rate, frame_ms)
    active = [i for i, value in enumerate(env) if value > threshold]
    if not active:
        return []
    max_gap = max(1, int(join_silence * 1000 / frame_ms))
    groups = [[active[0]]]
    for index in active[1:]:
        if index - groups[-1][-1] > max_gap:
            groups.append([index])
        else:
            groups[-1].append(index)
    frame = max(1, int(rate * frame_ms / 1000))
    padding = int(rate * pad)
    return [dump[max(0, group[0] * frame - padding):
                 min(len(dump), (group[-1] + 1) * frame + padding)]
            for group in groups]


def find_envelope(dump, ref, rate, frame_ms=20):
    """Locate a stream inside the dump by envelope correlation."""
    ed, er = envelope(dump, rate, frame_ms), envelope(ref, rate, frame_ms)
    if len(er) < 8 or len(ed) <= len(er):
        return dump[:len(ref)], ref, 0.0
    ee = math.sqrt(sum(v * v for v in er)) or 1.0
    best, best_score = 0, -2.0
    for off in range(0, len(ed) - len(er)):
        seg = ed[off:off + len(er)]
        dot = sum(x * y for x, y in zip(seg, er))
        es = math.sqrt(sum(v * v for v in seg)) or 1.0
        sc = dot / (es * ee)
        if sc > best_score:
            best_score, best = sc, off
    fn = max(1, int(rate * frame_ms / 1000))
    a = best * fn
    n = min(len(dump) - a, len(ref))
    return dump[a:a + n], ref[:n], best_score


def find(dump, ref, rate, probe_s=0.6, dec=16):
    """Locate a reference inside the dump by normalised cross-correlation.

    Slicing on the test's nominal cadence looked simpler and was wrong: each
    item also spends time flushing its log to the card, so a fixed grid drifts
    and the short effects land in the gaps. Correlation does not care."""
    probe_n = int(rate * probe_s)
    # Take the loudest part of the reference as the probe - silence correlates
    # with everything.
    if len(ref) > probe_n:
        best_i, best_e = 0, -1.0
        for i in range(0, len(ref) - probe_n, probe_n // 2 or 1):
            e = sum(abs(v) for v in ref[i:i + probe_n:dec])
            if e > best_e:
                best_e, best_i = e, i
        ref_start = best_i
    else:
        ref_start = 0
    probe_span = min(probe_n, len(ref) - ref_start)
    probe = ref[ref_start:ref_start + probe_span:dec]
    ep = math.sqrt(sum(float(v) * v for v in probe)) or 1.0
    # Search on a fine sample grid. A 64-sample stride (the old dec*4) can
    # miss the correct phase entirely for a high-frequency, 26ms effect even
    # when the waveforms are otherwise 99.9% correlated.
    step = max(1, dec // 4)
    best, best_score = 0, -2.0
    limit = len(dump) - probe_span
    for off in range(0, max(1, limit), step):
        seg = dump[off:off + probe_span:dec]
        if len(seg) < len(probe):
            break
        dot = sum(float(x) * y for x, y in zip(seg, probe))
        es = math.sqrt(sum(float(v) * v for v in seg)) or 1.0
        sc = dot / (es * ep)
        if sc > best_score:
            best_score, best = sc, off
    n = min(len(dump) - best, len(ref) - ref_start)
    return dump[best:best + n], ref[ref_start:ref_start + n], best_score


def main():
    dumps = glob.glob(os.path.join(DUMP_DIR, "*_dspdump*.wav"))
    if not dumps:
        raise SystemExit("no DSP dump - arm dvd:/audiotest.txt and boot with DumpAudio on")
    newest = max(os.path.getmtime(d) for d in dumps)
    dump = max([d for d in dumps if newest - os.path.getmtime(d) < 300], key=os.path.getsize)
    print(f"console: {os.path.basename(dump)}")
    dump_pcm, rate = load_dump(dump)
    print(f"         {len(dump_pcm)/rate:.0f}s of console output\n")

    expected = [("stream", n) for n in STREAMS] + [("sfx", i) for i in SFX]
    segments = active_segments(dump_pcm, rate)
    if len(segments) != len(expected):
        print(f"INVALID CAPTURE: found {len(segments)} active regions, "
              f"expected {len(expected)}. No verdict is possible.")
        return 2
    tmpdir = "/private/tmp/claude-501/adf"
    os.makedirs(tmpdir, exist_ok=True)

    print(f"{'sound':<16}{'match':>7}{'level':>9}{'centroid':>17}   bands (console - reference, dB)")
    bad = 0
    compared = 0
    for (kind, ident), segment in zip(expected, segments):
        if kind == "stream":
            ref = decode_reference(ident, tmpdir, rate)
            label = ident
        else:
            ref = sfx_reference(ident, rate)
            label = f"sfx{ident}"
        if not ref:
            print(f"{label:<16}   (no reference asset)")
            continue
        # Effects share their PCM with the reference, so sample correlation
        # is meaningful there. Streams are two different lossy encodes of the
        # same master and never line up sample for sample - align those on
        # the envelope instead.
        if kind == "stream":
            a, b, score = find_envelope(segment, ref, rate)
        else:
            a, b, score = find(segment, ref, rate)
        compared += 1
        lvl = amplitude_db(rms(a), rms(b))
        ca, cb = centroid(a, rate), centroid(b, rate)
        sa, sb = spectrum(a, rate), spectrum(b, rate)
        peak_ref = max(sb, default=0.0)
        meaningful = [x > 0 and y > peak_ref * 1e-5
                      for x, y in zip(sa, sb)]
        # Remove the global gain from every energy band. What remains is the
        # tonal change - precisely the metallic/telephone-quality question.
        band_delta = [power_db(x, y) - lvl if keep else float("nan")
                      for x, y, keep in zip(sa, sb, meaningful)]
        bands = " ".join(f"{delta:+5.1f}" if math.isfinite(delta) else "   --"
                         for delta in band_delta)
        worst = max((abs(delta) for delta in band_delta if math.isfinite(delta)),
                    default=0.0)
        flag = "" if abs(lvl) <= 1.0 and worst <= 3.0 and score > 0.5 else "   <-- DIFFERS"
        if flag:
            bad += 1
        print(f"{label:<16}{score:7.3f}{lvl:+8.1f}dB{ca:8.0f}/{cb:<7.0f}Hz  {bands}{flag}")

    print()
    
    if compared == 0:
        print("NOTHING COMPARED - the dump held no segmented sounds. "
              "A vacuous pass is not a pass.")
        return 2
    print(f"{compared - bad}/{compared} sounds match the reference "
          f"(level within 1dB, every band within 3dB)")
    if compared < len(expected):
        print(f"note: only {compared} of {len(expected)} sounds were found in the dump")
    return 0 if bad == 0 and compared == len(expected) else 1


if __name__ == "__main__":
    sys.exit(main())
