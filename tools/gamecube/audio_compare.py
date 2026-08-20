#!/usr/bin/env python3
"""Compare the console's DSP output against the game's own samples.

The console side is armed by putting dvd:/audiotest.txt on the card: sampman
then plays a fixed list of bank samples through the sample manager - no
AudioManager, no reflections, no positioning - and parks. With Dolphin's
DumpAudio on, Dump/Audio/*_dspdump.wav contains that sequence and nothing
else.

This script decodes the same samples out of the install's sfx.raw, converts
them exactly the way the console does (linear interpolation to the DSP's own
54MHz/1124 = 48042.7Hz), finds each one inside the dump by cross-correlation,
and reports the error in both time and frequency. If the two disagree, the
numbers say where: a level difference, a rate difference, or spectral energy
the reference does not have (which is what aliasing looks like).

    python3 tools/gamecube/audio_compare.py [dspdump.wav]

With no argument it takes the newest dump in Dolphin's Dump/Audio folder.
"""
import glob
import os
import struct
import sys
import wave

GC_DSP_RATE = 54000000.0 / 1124.0        # 48042.7Hz, libogc's DSP_DEFAULT_FREQ
SDT = "/Users/ebellumat/GTAVC/audio/sfx.sdt"
RAW = "/Users/ebellumat/GTAVC/audio/sfx.raw"
DUMP_DIR = os.path.expanduser("~/Library/Application Support/Dolphin/Dump/Audio")


def sample_entry(index):
    with open(SDT, "rb") as f:
        f.seek(index * 20)
        off, size, freq, _ls, _le = struct.unpack("<IIIIi", f.read(20))
    return off, size, freq


FIR_TAPS, FIR_PHASES = 8, 64


def _fir_table():
    """The same polyphase windowed sinc the console builds (gcBuildFir)."""
    import math
    table = []
    for ph in range(FIR_PHASES):
        frac = ph / FIR_PHASES
        taps, total = [], 0.0
        for t in range(FIR_TAPS):
            x = (t - (FIR_TAPS // 2 - 1)) - frac
            s = 1.0 if abs(x) < 1e-6 else math.sin(math.pi * x) / (math.pi * x)
            w = (0.42
                 - 0.5 * math.cos(2.0 * math.pi * (t + 0.5) / FIR_TAPS)
                 + 0.08 * math.cos(4.0 * math.pi * (t + 0.5) / FIR_TAPS))
            taps.append(s * w)
            total += s * w
        if abs(total) > 1e-4:
            taps = [v / total for v in taps]
        table.append(taps)
    return table


FIR = _fir_table()


def reference(index):
    """The sample as the console should render it: the game's own PCM run
    through the same polyphase windowed sinc, at the DSP's own rate. This has
    to mirror sampman_gamecube.cpp exactly or the comparison measures the
    difference between two scripts instead of the difference that matters."""
    off, size, freq = sample_entry(index)
    with open(RAW, "rb") as f:
        f.seek(off)
        data = f.read(size)
    src = list(struct.unpack("<%dh" % (len(data) // 2), data[: len(data) // 2 * 2]))
    if freq >= GC_DSP_RATE or len(src) < 2:
        return src, freq
    dsp = int(GC_DSP_RATE + 0.5)
    out_n = len(src) * dsp // freq
    step = (freq << 16) // dsp
    n = len(src)
    out, pos = [], 0
    for _ in range(out_n):
        i0 = pos >> 16
        taps = FIR[(pos >> 10) & (FIR_PHASES - 1)]
        acc = 0.0
        for t in range(FIR_TAPS):
            si = i0 + t - (FIR_TAPS // 2 - 1)
            si = 0 if si < 0 else (n - 1 if si >= n else si)
            acc += taps[t] * src[si]
        v = int(acc + (0.5 if acc >= 0 else -0.5))
        out.append(max(-32768, min(32767, v)))
        pos += step
    return out, dsp


def read_dump(path):
    with wave.open(path, "rb") as w:
        n, ch, width, rate = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(n)
    assert width == 2, f"expected 16-bit dump, got {width * 8}-bit"
    all_s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    left = list(all_s[0::ch])
    right = list(all_s[1::ch]) if ch > 1 else left
    return left, right, rate


def best_offset(hay, needle, coarse=8):
    """Cheap normalised cross-correlation; returns (offset, score 0..1)."""
    if len(needle) > len(hay):
        return 0, 0.0
    n = needle[::coarse]
    n_energy = sum(float(v) * v for v in n) ** 0.5 or 1.0
    best, best_score = 0, -1.0
    limit = len(hay) - len(needle)
    stride = max(1, coarse)
    for off in range(0, limit + 1, stride):
        seg = hay[off:off + len(needle):coarse]
        dot = sum(float(a) * b for a, b in zip(seg, n))
        seg_energy = sum(float(v) * v for v in seg) ** 0.5 or 1.0
        score = dot / (seg_energy * n_energy)
        if score > best_score:
            best_score, best = score, off
    return best, best_score


def band_energy(sig, rate, lo, hi):
    """Energy in a band, via a naive Goertzel sweep - no numpy dependency."""
    import math
    total = 0.0
    steps = 24
    for k in range(steps):
        f = lo + (hi - lo) * (k + 0.5) / steps
        w = 2.0 * math.pi * f / rate
        cw, sw = math.cos(w), math.sin(w)
        c = 2.0 * cw
        s1 = s2 = 0.0
        for v in sig:
            s0 = v + c * s1 - s2
            s2, s1 = s1, s0
        total += (s1 * s1 + s2 * s2 - c * s1 * s2)
    return total / steps


def main(argv):
    if argv:
        dump = argv[0]
    else:
        # Dolphin splits the dump: the first file is a fragment captured while
        # the AI is still at its 32kHz power-on rate, and the real output
        # lands in dspdump1.wav at 48kHz. Take the longest from the newest run.
        dumps = glob.glob(os.path.join(DUMP_DIR, "*_dspdump*.wav"))
        if not dumps:
            raise SystemExit(f"no dspdump in {DUMP_DIR} - is DumpAudio on?")
        newest = max(os.path.getmtime(d) for d in dumps)
        recent = [d for d in dumps if newest - os.path.getmtime(d) < 120]
        dump = max(recent, key=os.path.getsize)
    print(f"dump: {os.path.basename(dump)}")
    left, right, rate = read_dump(dump)
    print(f"      {len(left)} frames at {rate}Hz, {len(left)/rate:.1f}s")

    indices = [0, 1, 11, 19]                    # must match gAudioTestSfx
    worst = 0.0
    for idx in indices:
        ref, ref_rate = reference(idx)
        _off, _size, native = sample_entry(idx)
        if len(ref) < 64:
            continue
        off, score = best_offset(left, ref)
        seg = left[off:off + len(ref)]
        peak_ref = max(abs(v) for v in ref) or 1
        peak_seg = max(abs(v) for v in seg) or 1
        gain = peak_seg / peak_ref
        # Aliasing shows up as energy above the source's own Nyquist.
        nyq = native / 2
        if nyq < ref_rate / 2 - 1000:
            alias_ref = band_energy(ref[:4096], ref_rate, nyq + 500, ref_rate / 2 - 500)
            alias_seg = band_energy(seg[:4096], ref_rate, nyq + 500, ref_rate / 2 - 500)
            base_ref = band_energy(ref[:4096], ref_rate, 200, nyq - 500) or 1.0
            base_seg = band_energy(seg[:4096], ref_rate, 200, nyq - 500) or 1.0
            alias = (alias_seg / base_seg) / ((alias_ref / base_ref) or 1e-9)
        else:
            alias = 1.0
        print(f"  sfx {idx:>3}  native {native:>5}Hz  match {score:5.3f}  "
              f"gain {gain:5.2f}x  alias {alias:6.2f}x")
        worst = max(worst, abs(1.0 - score))

    print()
    if worst < 0.1:
        print("console output matches the reference")
    else:
        print("MISMATCH - the dump does not line up with the reference")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
